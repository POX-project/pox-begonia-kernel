// SPDX-License-Identifier: GPL-2.0-only

#include "mount_view_filter.h"

#include <linux/dcache.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mount.h>
#include <linux/mutex.h>
#include <linux/path.h>
#include <linux/printk.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/string.h>

#include "feature/kernel_umount.h"
#include "hook/patch_memory.h"
#include "infra/symbol_resolver.h"
#include "policy/allowlist.h"
#include "policy/feature.h"
#include "klog.h" // IWYU pragma: keep
#include "ksu.h"

typedef int (*ksu_mvf_open_fn)(struct inode *, struct file *);
typedef int (*ksu_mvf_show_fn)(struct seq_file *, struct vfsmount *);

struct ksu_mvf_proc_mounts {
    struct mnt_namespace *ns;
    struct path root;
    ksu_mvf_show_fn show;
};

enum ksu_mvf_format {
    KSU_MVF_MOUNTS,
    KSU_MVF_MOUNTINFO,
    KSU_MVF_MOUNTSTATS,
};

struct ksu_mvf_hook {
    const char *name;
    struct file_operations *ops;
    ksu_mvf_show_fn original_show;
    ksu_mvf_show_fn filtered_show;
    ksu_mvf_open_fn *open_slot;
    ksu_mvf_open_fn original_open;
    bool patched;
};

static DEFINE_MUTEX(ksu_mvf_mutex);
static bool ksu_mvf_enabled;

static struct ksu_mvf_hook ksu_mvf_hooks[3];

static int __nocfi ksu_mvf_call_show(ksu_mvf_show_fn show, struct seq_file *m, struct vfsmount *mnt)
{
    return show(m, mnt);
}

static int __nocfi ksu_mvf_call_open(ksu_mvf_open_fn open, struct inode *inode, struct file *file)
{
    return open(inode, file);
}

static bool ksu_mvf_char_equal(char a, char b)
{
    if (a >= 'A' && a <= 'Z')
        a += 'a' - 'A';
    if (b >= 'A' && b <= 'Z')
        b += 'a' - 'A';
    return a == b;
}

static bool ksu_mvf_has_marker(const char *text, size_t len)
{
    static const char *const markers[] = {
        "/adb/", "/data/adb", "kernelsu", "magisk", "zygisk", "/debug_ramdisk",
    };
    size_t i, j, marker_len;

    for (i = 0; i < ARRAY_SIZE(markers); i++) {
        marker_len = strlen(markers[i]);
        if (len < marker_len)
            continue;
        for (j = 0; j + marker_len <= len; j++) {
            size_t k;

            for (k = 0; k < marker_len && ksu_mvf_char_equal(text[j + k], markers[i][k]); k++)
                ;
            if (k == marker_len)
                return true;
        }
    }
    return false;
}

static bool ksu_mvf_has_root_marker(struct vfsmount *mnt)
{
    char path[512];
    char *root;

    if (!mnt || !mnt->mnt_root || IS_ROOT(mnt->mnt_root))
        return false;

    root = dentry_path_raw(mnt->mnt_root, path, sizeof(path));
    return !IS_ERR(root) && ksu_mvf_has_marker(root, strlen(root));
}

static const char *ksu_mvf_token(const char *p, const char *end, unsigned int index, size_t *len)
{
    unsigned int field = 0;

    while (p < end) {
        const char *start;

        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n'))
            p++;
        if (p == end)
            break;
        start = p;
        while (p < end && *p != ' ' && *p != '\t' && *p != '\n')
            p++;
        if (field++ == index) {
            *len = p - start;
            return start;
        }
    }
    return NULL;
}

static const char *ksu_mvf_find(const char *text, size_t len, const char *needle)
{
    size_t needle_len = strlen(needle);
    size_t i;

    if (needle_len > len)
        return NULL;
    for (i = 0; i + needle_len <= len; i++) {
        if (!memcmp(text + i, needle, needle_len))
            return text + i;
    }
    return NULL;
}

static const char *ksu_mvf_mountpoint(const char *text, size_t len, enum ksu_mvf_format format, size_t *field_len)
{
    const char *end = text + len;

    switch (format) {
    case KSU_MVF_MOUNTS:
        return ksu_mvf_token(text, end, 1, field_len);
    case KSU_MVF_MOUNTINFO:
        return ksu_mvf_token(text, end, 4, field_len);
    case KSU_MVF_MOUNTSTATS: {
        const char *mounted_on = ksu_mvf_find(text, len, " mounted on ");

        if (!mounted_on)
            return NULL;
        return ksu_mvf_token(mounted_on + strlen(" mounted on "), end, 0, field_len);
    }
    }
    return NULL;
}

static int ksu_mvf_octal(char c)
{
    if (c >= '0' && c <= '7')
        return c - '0';
    return -1;
}

static bool ksu_mvf_path_equal(const char *field, size_t field_len, const char *path)
{
    size_t i = 0, j = 0;

    while (i < field_len) {
        char c = field[i++];

        if (c == '\\' && i + 2 < field_len) {
            int a = ksu_mvf_octal(field[i]);
            int b = ksu_mvf_octal(field[i + 1]);
            int d = ksu_mvf_octal(field[i + 2]);

            if (a >= 0 && b >= 0 && d >= 0) {
                c = (char)((a << 6) | (b << 3) | d);
                i += 3;
            }
        }
        if (path[j++] != c)
            return false;
    }
    return path[j] == '\0';
}

static bool ksu_mvf_in_umount_list(const char *mountpoint, size_t len)
{
    struct mount_entry *entry;
    bool found = false;

    // The show callback runs under namespace_sem(R); the umount path takes
    // mount_list_lock(R) then namespace_sem(W). Blocking here would complete
    // that cycle once a mount_list writer queues, so skip on contention.
    if (!down_read_trylock(&mount_list_lock))
        return false;

    list_for_each_entry (entry, &mount_list, list) {
        if (entry->umountable && ksu_mvf_path_equal(mountpoint, len, entry->umountable)) {
            found = true;
            break;
        }
    }
    up_read(&mount_list_lock);
    return found;
}

static bool ksu_mvf_record_hidden(struct seq_file *m, size_t start, enum ksu_mvf_format format, struct vfsmount *mnt)
{
    const char *text = m->buf + start;
    const char *mountpoint;
    size_t len = m->count - start;
    size_t mountpoint_len;

    if (ksu_mvf_has_marker(text, len))
        return true;

    mountpoint = ksu_mvf_mountpoint(text, len, format, &mountpoint_len);
    if (mountpoint && ksu_mvf_in_umount_list(mountpoint, mountpoint_len))
        return true;

    return ksu_mvf_has_root_marker(mnt);
}

static int ksu_mvf_show(struct seq_file *m, struct vfsmount *mnt, enum ksu_mvf_format format, ksu_mvf_show_fn original)
{
    size_t start = m->count;
    int ret = ksu_mvf_call_show(original, m, mnt);

    if (ret || seq_has_overflowed(m))
        return ret;
    if (m->count >= start && ksu_mvf_record_hidden(m, start, format, mnt))
        m->count = start;
    return 0;
}

static int ksu_mvf_show_mounts(struct seq_file *m, struct vfsmount *mnt)
{
    return ksu_mvf_show(m, mnt, KSU_MVF_MOUNTS, ksu_mvf_hooks[0].original_show);
}

static int ksu_mvf_show_mountinfo(struct seq_file *m, struct vfsmount *mnt)
{
    return ksu_mvf_show(m, mnt, KSU_MVF_MOUNTINFO, ksu_mvf_hooks[1].original_show);
}

static int ksu_mvf_show_mountstats(struct seq_file *m, struct vfsmount *mnt)
{
    return ksu_mvf_show(m, mnt, KSU_MVF_MOUNTSTATS, ksu_mvf_hooks[2].original_show);
}

static bool ksu_mvf_should_filter(void)
{
    uid_t uid = current_uid().val;
    bool isolated = is_isolated_process(uid);

    return (is_appuid(uid) || isolated) && (isolated || ksu_uid_should_umount(uid));
}

static int ksu_mvf_open(struct inode *inode, struct file *file, unsigned int index)
{
    struct ksu_mvf_hook *hook = &ksu_mvf_hooks[index];
    struct ksu_mvf_proc_mounts *mounts;
    struct seq_file *m;
    int ret;

    ret = ksu_mvf_call_open(hook->original_open, inode, file);
    if (ret)
        return ret;
    if (!smp_load_acquire(&ksu_mvf_enabled) || !file->private_data)
        return 0;

    m = file->private_data;
    mounts = m->private;
    if (!mounts || READ_ONCE(mounts->show) != hook->original_show) {
        pr_warn_once("mount_view_filter: proc_mounts layout changed for %s\n", hook->name);
        return 0;
    }
    if (ksu_mvf_should_filter())
        WRITE_ONCE(mounts->show, hook->filtered_show);
    return 0;
}

static int ksu_mvf_open_mounts(struct inode *inode, struct file *file)
{
    return ksu_mvf_open(inode, file, 0);
}

static int ksu_mvf_open_mountinfo(struct inode *inode, struct file *file)
{
    return ksu_mvf_open(inode, file, 1);
}

static int ksu_mvf_open_mountstats(struct inode *inode, struct file *file)
{
    return ksu_mvf_open(inode, file, 2);
}

static ksu_mvf_open_fn ksu_mvf_open_wrapper(unsigned int index)
{
    static const ksu_mvf_open_fn wrappers[] = {
        ksu_mvf_open_mounts,
        ksu_mvf_open_mountinfo,
        ksu_mvf_open_mountstats,
    };

    return wrappers[index];
}

static int ksu_mvf_install_hooks(void)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(ksu_mvf_hooks); i++) {
        struct ksu_mvf_hook *hook = &ksu_mvf_hooks[i];
        ksu_mvf_open_fn wrapper = ksu_mvf_open_wrapper(i);
        int ret;

        if (hook->patched)
            continue;
        hook->original_open = READ_ONCE(*hook->open_slot);
        if (!hook->original_open) {
            pr_err("mount_view_filter: %s has no open handler\n", hook->name);
            return -ENOSYS;
        }
        ret = ksu_patch_text(hook->open_slot, &wrapper, sizeof(wrapper), KSU_PATCH_TEXT_FLUSH_DCACHE);
        if (ret) {
            pr_err("mount_view_filter: failed to hook %s: %d\n", hook->name, ret);
            return ret;
        }
        hook->patched = true;
    }
    return 0;
}

static int ksu_mvf_restore_hooks(void)
{
    int error = 0;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(ksu_mvf_hooks); i++) {
        struct ksu_mvf_hook *hook = &ksu_mvf_hooks[i];
        int ret;

        if (!hook->patched)
            continue;
        if (READ_ONCE(*hook->open_slot) != ksu_mvf_open_wrapper(i)) {
            pr_warn("mount_view_filter: %s open slot was replaced externally\n", hook->name);
            hook->patched = false;
            continue;
        }
        ret = ksu_patch_text(hook->open_slot, &hook->original_open, sizeof(hook->original_open),
                             KSU_PATCH_TEXT_FLUSH_DCACHE);
        if (ret) {
            pr_err("mount_view_filter: failed to restore %s: %d\n", hook->name, ret);
            error = error ?: ret;
            continue;
        }
        hook->patched = false;
    }
    return error;
}

static int ksu_mvf_resolve_symbols(void)
{
    static const char *const names[] = {
        "show_vfsmnt",
        "show_mountinfo",
        "show_vfsstat",
        "proc_mounts_operations",
        "proc_mountinfo_operations",
        "proc_mountstats_operations",
    };
    unsigned long addrs[ARRAY_SIZE(names)];
    int found = ksu_lookup_kernel_symbols(names, addrs, ARRAY_SIZE(names));
    unsigned int i;

    if (found != ARRAY_SIZE(names)) {
        pr_err("mount_view_filter: symbol lookup failed: %d/%zu\n", found, ARRAY_SIZE(names));
        return found < 0 ? found : -ENOSYS;
    }

    for (i = 0; i < ARRAY_SIZE(names); i++) {
        if (!addrs[i]) {
            pr_err("mount_view_filter: missing symbol %s\n", names[i]);
            return -ENOSYS;
        }
    }

    ksu_mvf_hooks[0].name = names[0];
    ksu_mvf_hooks[0].original_show = (ksu_mvf_show_fn)addrs[0];
    ksu_mvf_hooks[0].filtered_show = ksu_mvf_show_mounts;
    ksu_mvf_hooks[0].ops = (struct file_operations *)addrs[3];
    ksu_mvf_hooks[0].open_slot = &ksu_mvf_hooks[0].ops->open;

    ksu_mvf_hooks[1].name = names[1];
    ksu_mvf_hooks[1].original_show = (ksu_mvf_show_fn)addrs[1];
    ksu_mvf_hooks[1].filtered_show = ksu_mvf_show_mountinfo;
    ksu_mvf_hooks[1].ops = (struct file_operations *)addrs[4];
    ksu_mvf_hooks[1].open_slot = &ksu_mvf_hooks[1].ops->open;

    ksu_mvf_hooks[2].name = names[2];
    ksu_mvf_hooks[2].original_show = (ksu_mvf_show_fn)addrs[2];
    ksu_mvf_hooks[2].filtered_show = ksu_mvf_show_mountstats;
    ksu_mvf_hooks[2].ops = (struct file_operations *)addrs[5];
    ksu_mvf_hooks[2].open_slot = &ksu_mvf_hooks[2].ops->open;
    return 0;
}

static int ksu_mvf_feature_get(u64 *value)
{
    *value = READ_ONCE(ksu_mvf_enabled) ? 1 : 0;
    return 0;
}

static int ksu_mvf_feature_set(u64 value)
{
    bool enable = value != 0;
    int ret = 0;

    mutex_lock(&ksu_mvf_mutex);
    if (enable) {
        if (!smp_load_acquire(&ksu_mvf_enabled)) {
            ret = ksu_mvf_resolve_symbols();
            if (!ret)
                ret = ksu_mvf_install_hooks();
            if (!ret)
                smp_store_release(&ksu_mvf_enabled, true);
            else {
                ksu_mvf_restore_hooks();
                synchronize_rcu();
            }
        }
    } else if (smp_load_acquire(&ksu_mvf_enabled)) {
        smp_store_release(&ksu_mvf_enabled, false);
        ret = ksu_mvf_restore_hooks();
        synchronize_rcu();
    }
    mutex_unlock(&ksu_mvf_mutex);
    return ret;
}

static const struct ksu_feature_handler ksu_mvf_handler = {
    .feature_id = KSU_FEATURE_MOUNT_VIEW_FILTER,
    .name = "mount_view_filter",
    .get_handler = ksu_mvf_feature_get,
    .set_handler = ksu_mvf_feature_set,
};

void __init ksu_mvf_init(void)
{
    if (ksu_register_feature_handler(&ksu_mvf_handler))
        pr_err("mount_view_filter: failed to register feature handler\n");
}

void __exit ksu_mvf_exit(void)
{
    mutex_lock(&ksu_mvf_mutex);
    if (smp_load_acquire(&ksu_mvf_enabled)) {
        smp_store_release(&ksu_mvf_enabled, false);
        ksu_mvf_restore_hooks();
        synchronize_rcu();
    }
    mutex_unlock(&ksu_mvf_mutex);
    ksu_unregister_feature_handler(KSU_FEATURE_MOUNT_VIEW_FILTER);
}
