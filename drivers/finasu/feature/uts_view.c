#include <linux/build_bug.h>
#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/nsproxy.h>
#include <linux/printk.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/utsname.h>
#include <linux/wait.h>

#include "feature/uts_view.h"
#include "infra/symbol_resolver.h"
#include "ksu.h"
#include "policy/allowlist.h"
#include "policy/feature.h"
#include "selinux/selinux.h"
#include "util.h"

static_assert(sizeof(struct ksu_uts_view_config) == 972, "uts_view ABI drift");

static DEFINE_MUTEX(uts_view_lock);
static DECLARE_WAIT_QUEUE_HEAD(uts_view_callback_waitq);

static struct rw_semaphore *uts_sem_ptr;
static struct uts_namespace *init_uts_ns_ptr;
static struct ksu_uts_view_config template_cfg;
static struct new_utsname global_restore_uts;
static struct new_utsname global_last_applied;
static atomic_t active_callbacks = ATOMIC_INIT(0);
static atomic_t matched_callbacks = ATOMIC_INIT(0);
static atomic_t discarded_callbacks = ATOMIC_INIT(0);
static atomic_t skip_not_app = ATOMIC_INIT(0);
static atomic_t skip_isolated = ATOMIC_INIT(0);
static atomic_t skip_not_target = ATOMIC_INIT(0);
static atomic_t skip_not_zygote = ATOMIC_INIT(0);
static atomic_t skip_private_ns = ATOMIC_INIT(0);
static atomic_t unshare_failed = ATOMIC_INIT(0);
static atomic_t merge_missing_ns = ATOMIC_INIT(0);
static bool feature_enabled;
static u32 global_owned_mask;
static bool selected_active;
static bool supported;
static bool stopping;
static u32 generation;
static u32 last_pid;
static u32 last_uid;

static bool uts_field_terminated(const char field[KSU_UTS_NAME_LEN])
{
    return memchr(field, '\0', KSU_UTS_NAME_LEN) != NULL;
}

static int validate_config(const struct ksu_uts_view_config *config)
{
    u32 i;
    u32 j;

    if (config->field_mask & ~KSU_UTS_FIELD_VALID_MASK)
        return -EINVAL;
    if (config->mode != KSU_UTS_MODE_GLOBAL && config->mode != KSU_UTS_MODE_SELECTED)
        return -EINVAL;
    if (config->target_count > KSU_UTS_MAX_TARGETS)
        return -EINVAL;
    for (i = 0; i < config->target_count; i++) {
        if (config->target_uids[i] < 10000)
            return -EINVAL;
        for (j = 0; j < i; j++) {
            if (config->target_uids[j] == config->target_uids[i])
                return -EINVAL;
        }
    }

    if ((config->field_mask & KSU_UTS_FIELD_SYSNAME) && !uts_field_terminated(config->sysname))
        return -EINVAL;
    if ((config->field_mask & KSU_UTS_FIELD_NODENAME) && !uts_field_terminated(config->nodename))
        return -EINVAL;
    if ((config->field_mask & KSU_UTS_FIELD_RELEASE) && !uts_field_terminated(config->release))
        return -EINVAL;
    if ((config->field_mask & KSU_UTS_FIELD_VERSION) && !uts_field_terminated(config->version))
        return -EINVAL;
    if ((config->field_mask & KSU_UTS_FIELD_MACHINE) && !uts_field_terminated(config->machine))
        return -EINVAL;
    if ((config->field_mask & KSU_UTS_FIELD_DOMAINNAME) && !uts_field_terminated(config->domainname))
        return -EINVAL;

    return 0;
}

#define KSU_UTS_UPDATE_FIELD(bit, field)                                                                               \
    do {                                                                                                               \
        if (new_mask & (bit)) {                                                                                        \
            if (!(old_mask & (bit)) ||                                                                                 \
                strncmp(active_name->field, global_last_applied.field, sizeof(active_name->field)))                    \
                strscpy(global_restore_uts.field, active_name->field, sizeof(global_restore_uts.field));               \
            strscpy(active_name->field, tmpl->field, sizeof(active_name->field));                                      \
            strscpy(global_last_applied.field, tmpl->field, sizeof(global_last_applied.field));                        \
        } else if ((old_mask & (bit)) &&                                                                               \
                   !strncmp(active_name->field, global_last_applied.field, sizeof(active_name->field))) {              \
            strscpy(active_name->field, global_restore_uts.field, sizeof(active_name->field));                         \
        }                                                                                                              \
    } while (0)

static void apply_global_locked(const struct ksu_uts_view_config *tmpl)
{
    struct new_utsname *active_name = &init_uts_ns_ptr->name;
    u32 old_mask = global_owned_mask;
    u32 new_mask = tmpl->field_mask & KSU_UTS_FIELD_VALID_MASK;

    down_write(uts_sem_ptr);
    KSU_UTS_UPDATE_FIELD(KSU_UTS_FIELD_SYSNAME, sysname);
    KSU_UTS_UPDATE_FIELD(KSU_UTS_FIELD_NODENAME, nodename);
    KSU_UTS_UPDATE_FIELD(KSU_UTS_FIELD_RELEASE, release);
    KSU_UTS_UPDATE_FIELD(KSU_UTS_FIELD_VERSION, version);
    KSU_UTS_UPDATE_FIELD(KSU_UTS_FIELD_MACHINE, machine);
    KSU_UTS_UPDATE_FIELD(KSU_UTS_FIELD_DOMAINNAME, domainname);
    global_owned_mask = new_mask;
    up_write(uts_sem_ptr);
}

#undef KSU_UTS_UPDATE_FIELD

static void restore_global_locked(void)
{
    struct ksu_uts_view_config empty = {
        .mode = KSU_UTS_MODE_GLOBAL,
    };

    apply_global_locked(&empty);
    memset(&global_restore_uts, 0, sizeof(global_restore_uts));
    memset(&global_last_applied, 0, sizeof(global_last_applied));
}

static bool uts_view_active_locked(void)
{
    return supported && feature_enabled && !stopping && template_cfg.field_mask != 0 &&
           (template_cfg.mode & KSU_UTS_MODE_VALID_MASK);
}

static void refresh_active_locked(void)
{
    WRITE_ONCE(selected_active, uts_view_active_locked() && (template_cfg.mode & KSU_UTS_MODE_SELECTED));
}

static u32 current_state_locked(void)
{
    u32 state = 0;

    if (supported && !stopping)
        state |= KSU_UTS_STATE_SUPPORTED;
    if (uts_view_active_locked())
        state |= KSU_UTS_STATE_ACTIVE;
    if (READ_ONCE(selected_active) && atomic_read(&matched_callbacks) > 0)
        state |= KSU_UTS_STATE_OBSERVED;
    return state;
}

static void reset_observation_locked(void)
{
    generation++;
    if (!generation)
        generation++;
    atomic_set(&matched_callbacks, 0);
    atomic_set(&discarded_callbacks, 0);
    atomic_set(&skip_not_app, 0);
    atomic_set(&skip_isolated, 0);
    atomic_set(&skip_not_target, 0);
    atomic_set(&skip_not_zygote, 0);
    atomic_set(&skip_private_ns, 0);
    atomic_set(&unshare_failed, 0);
    atomic_set(&merge_missing_ns, 0);
    last_pid = 0;
    last_uid = 0;
}

static int uts_view_feature_get(u64 *value)
{
    mutex_lock(&uts_view_lock);
    if (!supported || stopping) {
        mutex_unlock(&uts_view_lock);
        return -ENODEV;
    }
    *value = feature_enabled ? 1 : 0;
    mutex_unlock(&uts_view_lock);
    return 0;
}

static int uts_view_feature_set(u64 value)
{
    bool enable = value != 0;
    bool global_mode;
    int ret = 0;

    mutex_lock(&uts_view_lock);
    if (!supported || stopping) {
        ret = -ENODEV;
        goto out;
    }
    if (enable && template_cfg.field_mask == 0) {
        ret = -ENODATA;
        goto out;
    }
    if (enable && (template_cfg.mode & KSU_UTS_MODE_SELECTED) && template_cfg.target_count == 0) {
        ret = -ENODATA;
        goto out;
    }

    global_mode = template_cfg.mode & KSU_UTS_MODE_GLOBAL;
    if (global_mode && enable && !feature_enabled)
        apply_global_locked(&template_cfg);
    else if (global_mode && !enable && (feature_enabled || global_owned_mask))
        restore_global_locked();

    feature_enabled = enable;
    reset_observation_locked();
    refresh_active_locked();
    pr_info("uts_view: event=%s mode=%s mask=0x%x targets=%u uid=%d\n", enable ? "enable" : "disable",
            (template_cfg.mode & KSU_UTS_MODE_GLOBAL) ? "global" : "selected", template_cfg.field_mask,
            template_cfg.target_count, current_uid().val);
out:
    mutex_unlock(&uts_view_lock);
    return ret;
}

static const struct ksu_feature_handler uts_view_feature_handler = {
    .feature_id = KSU_FEATURE_UTS_VIEW,
    .name = "uts_view",
    .get_handler = uts_view_feature_get,
    .set_handler = uts_view_feature_set,
};

int __init ksu_uts_view_init(void)
{
    static const char *const names[] = { "uts_sem", "init_uts_ns" };
    unsigned long addrs[2] = { 0 };
    int ret;

    ret = ksu_lookup_kernel_symbols(names, addrs, ARRAY_SIZE(addrs));
    if (ret < 0)
        return ret;
    if (!addrs[0] || !addrs[1]) {
        pr_info("uts_view: event=init result=missing-symbol uts_sem=%d init_uts_ns=%d\n", addrs[0] != 0, addrs[1] != 0);
        return -ENODEV;
    }

    mutex_lock(&uts_view_lock);
    if (supported || stopping) {
        mutex_unlock(&uts_view_lock);
        return -EBUSY;
    }
    uts_sem_ptr = (struct rw_semaphore *)addrs[0];
    init_uts_ns_ptr = (struct uts_namespace *)addrs[1];
    memset(&template_cfg, 0, sizeof(template_cfg));
    memset(&global_restore_uts, 0, sizeof(global_restore_uts));
    memset(&global_last_applied, 0, sizeof(global_last_applied));
    global_owned_mask = 0;
    feature_enabled = false;
    selected_active = false;
    generation = 0;
    reset_observation_locked();
    supported = true;
    mutex_unlock(&uts_view_lock);

    ret = ksu_register_feature_handler(&uts_view_feature_handler);
    if (ret) {
        mutex_lock(&uts_view_lock);
        supported = false;
        uts_sem_ptr = NULL;
        init_uts_ns_ptr = NULL;
        mutex_unlock(&uts_view_lock);
        return ret;
    }

    pr_info("uts_view: event=init result=ok uts_sem=1 init_uts_ns=1\n");
    return 0;
}

void ksu_uts_view_exit(void)
{
    mutex_lock(&uts_view_lock);
    stopping = true;
    WRITE_ONCE(selected_active, false);
    mutex_unlock(&uts_view_lock);

    wait_event(uts_view_callback_waitq, atomic_read(&active_callbacks) == 0);

    ksu_unregister_feature_handler(KSU_FEATURE_UTS_VIEW);

    mutex_lock(&uts_view_lock);
    if (uts_sem_ptr && init_uts_ns_ptr && global_owned_mask)
        restore_global_locked();
    memset(&template_cfg, 0, sizeof(template_cfg));
    memset(&global_restore_uts, 0, sizeof(global_restore_uts));
    memset(&global_last_applied, 0, sizeof(global_last_applied));
    global_owned_mask = 0;
    feature_enabled = false;
    supported = false;
    uts_sem_ptr = NULL;
    init_uts_ns_ptr = NULL;
    mutex_unlock(&uts_view_lock);
}

int ksu_uts_view_get_config(struct ksu_uts_view_config *config)
{
    if (!config)
        return -EINVAL;

    mutex_lock(&uts_view_lock);
    if (!supported || stopping) {
        mutex_unlock(&uts_view_lock);
        return -ENODEV;
    }
    *config = template_cfg;
    config->state = current_state_locked();
    config->generation = generation;
    config->matched = atomic_read(&matched_callbacks);
    config->discarded = atomic_read(&discarded_callbacks);
    config->last_pid = last_pid;
    config->last_uid = last_uid;
    config->skip_not_app = atomic_read(&skip_not_app);
    config->skip_isolated = atomic_read(&skip_isolated);
    config->skip_not_target = atomic_read(&skip_not_target);
    config->skip_not_zygote = atomic_read(&skip_not_zygote);
    config->skip_private_ns = atomic_read(&skip_private_ns);
    config->unshare_failed = atomic_read(&unshare_failed);
    config->merge_missing_ns = atomic_read(&merge_missing_ns);
    mutex_unlock(&uts_view_lock);
    return 0;
}

int ksu_uts_view_set_config(const struct ksu_uts_view_config *config)
{
    struct ksu_uts_view_config next;
    bool was_global;
    bool will_global;
    int ret;

    if (!config)
        return -EINVAL;

    next = *config;
    if (!next.mode)
        next.mode = KSU_UTS_MODE_SELECTED;
    next.state = 0;
    next.generation = 0;
    next.matched = 0;
    next.discarded = 0;
    next.last_pid = 0;
    next.last_uid = 0;
    next.skip_not_app = 0;
    next.skip_isolated = 0;
    next.skip_not_target = 0;
    next.skip_not_zygote = 0;
    next.skip_private_ns = 0;
    next.reserved0 = 0;
    next.unshare_failed = 0;
    next.merge_missing_ns = 0;
    ret = validate_config(&next);
    if (ret)
        return ret;

    mutex_lock(&uts_view_lock);
    if (!supported || stopping) {
        ret = -ENODEV;
        goto out;
    }
    was_global = uts_view_active_locked() && (template_cfg.mode & KSU_UTS_MODE_GLOBAL);
    will_global = feature_enabled && !stopping && next.field_mask && (next.mode & KSU_UTS_MODE_GLOBAL);
    if (was_global && !will_global)
        restore_global_locked();
    template_cfg = next;
    if (will_global)
        apply_global_locked(&template_cfg);
    reset_observation_locked();
    refresh_active_locked();
    pr_info("uts_view: event=set mode=%s mask=0x%x targets=%u uid=%d\n",
            (template_cfg.mode & KSU_UTS_MODE_GLOBAL) ? "global" : "selected", template_cfg.field_mask,
            template_cfg.target_count, current_uid().val);
out:
    mutex_unlock(&uts_view_lock);
    return ret;
}

static bool callback_enter(void)
{
    bool entered = false;

    mutex_lock(&uts_view_lock);
    if (!stopping) {
        atomic_inc(&active_callbacks);
        entered = true;
    }
    mutex_unlock(&uts_view_lock);
    return entered;
}

static bool is_selected_target(uid_t uid)
{
    u32 i;

    for (i = 0; i < template_cfg.target_count; i++) {
        if (template_cfg.target_uids[i] == uid)
            return true;
    }
    return false;
}

static void callback_exit(void)
{
    if (atomic_dec_and_test(&active_callbacks))
        wake_up(&uts_view_callback_waitq);
}

static void merge_template(struct new_utsname *dst, const struct ksu_uts_view_config *tmpl)
{
    if (tmpl->field_mask & KSU_UTS_FIELD_SYSNAME)
        strscpy(dst->sysname, tmpl->sysname, sizeof(dst->sysname));
    if (tmpl->field_mask & KSU_UTS_FIELD_NODENAME)
        strscpy(dst->nodename, tmpl->nodename, sizeof(dst->nodename));
    if (tmpl->field_mask & KSU_UTS_FIELD_RELEASE)
        strscpy(dst->release, tmpl->release, sizeof(dst->release));
    if (tmpl->field_mask & KSU_UTS_FIELD_VERSION)
        strscpy(dst->version, tmpl->version, sizeof(dst->version));
    if (tmpl->field_mask & KSU_UTS_FIELD_MACHINE)
        strscpy(dst->machine, tmpl->machine, sizeof(dst->machine));
    if (tmpl->field_mask & KSU_UTS_FIELD_DOMAINNAME)
        strscpy(dst->domainname, tmpl->domainname, sizeof(dst->domainname));
}

void ksu_uts_view_on_setresuid(uid_t old_uid, uid_t new_uid)
{
    struct ksu_uts_view_config snapshot;
    const struct cred *saved;
    u32 snapshot_generation;
    long ret;

    (void)old_uid;
    if (!READ_ONCE(selected_active))
        return;
    if (is_isolated_process(new_uid)) {
        atomic_inc(&skip_isolated);
        return;
    }
    if (!is_appuid(new_uid)) {
        atomic_inc(&skip_not_app);
        return;
    }
    if (!is_zygote(current_cred())) {
        atomic_inc(&skip_not_zygote);
        return;
    }
    if (!current || (current->flags & PF_KTHREAD) || !current->nsproxy || !current->nsproxy->uts_ns)
        return;
    if (!callback_enter())
        return;

    mutex_lock(&uts_view_lock);
    if (stopping || !feature_enabled || template_cfg.field_mask == 0 || !uts_sem_ptr || !init_uts_ns_ptr || !ksu_cred) {
        mutex_unlock(&uts_view_lock);
        goto out;
    }
    if (!is_selected_target(new_uid)) {
        atomic_inc(&skip_not_target);
        mutex_unlock(&uts_view_lock);
        goto out;
    }
    if (current->nsproxy->uts_ns != init_uts_ns_ptr) {
        atomic_inc(&skip_private_ns);
        mutex_unlock(&uts_view_lock);
        goto out;
    }
    snapshot = template_cfg;
    snapshot_generation = generation;
    mutex_unlock(&uts_view_lock);

    saved = override_creds(ksu_cred);
    ret = ksyscall(unshare, CLONE_NEWUTS);
    revert_creds(saved);
    if (ret) {
        atomic_inc(&unshare_failed);
        pr_warn("uts_view: event=apply-fail pid=%d comm=\"%s\" uid=%d stage=unshare result=%ld\n", current->pid,
                current->comm, new_uid, ret);
        goto out;
    }

    mutex_lock(&uts_view_lock);
    if (!stopping && feature_enabled && READ_ONCE(selected_active) && generation == snapshot_generation &&
        snapshot.field_mask != 0) {
        if (!current->nsproxy || !current->nsproxy->uts_ns || current->nsproxy->uts_ns == init_uts_ns_ptr) {
            atomic_inc(&merge_missing_ns);
            goto unlock;
        }
        down_write(uts_sem_ptr);
        merge_template(&current->nsproxy->uts_ns->name, &snapshot);
        up_write(uts_sem_ptr);
        last_pid = current->pid;
        last_uid = new_uid;
        if (atomic_inc_return(&matched_callbacks) == 1)
            pr_info("uts_view: event=observed-first pid=%d comm=\"%s\" uid=%d generation=%u\n", current->pid,
                    current->comm, new_uid, generation);
        else
            pr_debug("uts_view: event=observed pid=%d comm=\"%s\" uid=%d generation=%u\n", current->pid, current->comm,
                     new_uid, generation);
    } else {
        atomic_inc(&discarded_callbacks);
    }
unlock:
    mutex_unlock(&uts_view_lock);

out:
    callback_exit();
}
