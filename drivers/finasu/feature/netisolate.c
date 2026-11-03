#include <linux/types.h>
#include <linux/byteorder/little_endian.h>
#include <linux/compiler.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/ptrace.h>
#include <linux/slab.h>
#include <linux/socket.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>

#include <net/compat.h>

#include "feature/netisolate.h"
#include "hook/patch_memory.h"
#include "hook/syscall_hook.h"
#include "infra/symbol_resolver.h"
#include "manager/manager_identity.h"

static_assert(sizeof(struct ksu_netisolate_config) == 1040, "netisolate ABI drift");
static_assert(sizeof(struct ksu_netisolate_policies) == 2064, "netisolate policies ABI drift");
static_assert(sizeof(struct ksu_netisolate_event) == 40, "netisolate event ABI drift");
static_assert(sizeof(struct ksu_netisolate_events) == 2576, "netisolate events ABI drift");

#define NI_NATIVE_BASE 0
#define NI_OPERATION_COUNT 5
#define NI_COMPAT_BASE NI_OPERATION_COUNT
#define NI_HOOK_COUNT (NI_OPERATION_COUNT * 2)
#define NI_SYSCALL_LIMIT 1024
#define NI_SENDMMSG_CHECK_MAX 16
#define NI_SUPPORTED_FLAGS                                                                                             \
    (KSU_NETISOLATE_FLAG_STRICT_SOCKET | KSU_NETISOLATE_FLAG_ALLOW_LOOPBACK |                                          \
     KSU_NETISOLATE_FLAG_ALLOW_PRIVATE_NETWORK | KSU_NETISOLATE_FLAG_ALLOW_LOCAL_DISCOVERY)
#define NI_ALLOW_FLAGS                                                                                                 \
    (KSU_NETISOLATE_FLAG_ALLOW_LOOPBACK | KSU_NETISOLATE_FLAG_ALLOW_PRIVATE_NETWORK |                                  \
     KSU_NETISOLATE_FLAG_ALLOW_LOCAL_DISCOVERY)

enum ni_operation {
    NI_CONNECT,
    NI_SENDTO,
    NI_SENDMSG,
    NI_SENDMMSG,
    NI_SOCKET,
};

struct ni_hook {
    syscall_fn_t *table;
    int nr;
    syscall_fn_t original;
    syscall_fn_t replacement;
    bool installed;
};

struct ni_policy {
    u32 uid;
    u32 flags;
};

struct ni_state {
    u32 enabled;
    u32 count;
    struct ni_policy policies[KSU_NETISOLATE_MAX_POLICIES];
};

struct ni_endpoint {
    u8 family;
    u8 address[16];
    u16 port;
};

enum ni_target_class {
    NI_TARGET_NONE,
    NI_TARGET_LOOPBACK,
    NI_TARGET_PRIVATE_NETWORK,
    NI_TARGET_LOCAL_DISCOVERY,
    NI_TARGET_OTHER_IP,
};

struct ni_event_ring {
    u32 head;
    u32 count;
    u64 dropped;
    struct ksu_netisolate_event events[KSU_NETISOLATE_EVENT_MAX];
};

static DEFINE_MUTEX(ni_lifecycle_lock);
static DEFINE_SPINLOCK(ni_state_lock);
static DEFINE_SPINLOCK(ni_event_lock);
static struct ni_state ni_state;
static struct ni_event_ring ni_events;
static struct ni_hook ni_hooks[NI_HOOK_COUNT];
static bool ni_supported;
static bool ni_compat_supported;
static bool ni_stopping;

static const int ni_native_nrs[] = {
    __NR_connect, __NR_sendto, __NR_sendmsg, __NR_sendmmsg, __NR_socket,
};

#if defined(__aarch64__)
static const int ni_compat_nrs[] = { 283, 290, 296, 374, 281 };
#elif defined(__x86_64__)
static const int ni_compat_nrs[] = { 362, 369, 370, 345, 359 };
#endif

static syscall_fn_t ni_original(enum ni_operation op, bool compat)
{
    return ni_hooks[(compat ? NI_COMPAT_BASE : NI_NATIVE_BASE) + op].original;
}

static unsigned long ni_arg(const struct pt_regs *regs, unsigned int index, bool compat)
{
#if defined(__x86_64__)
    if (compat) {
        switch (index) {
        case 0:
            return regs->bx;
        case 1:
            return regs->cx;
        case 2:
            return regs->dx;
        case 3:
            return regs->si;
        case 4:
            return regs->di;
        default:
            return regs->bp;
        }
    }

    switch (index) {
    case 0:
        return regs->di;
    case 1:
        return regs->si;
    case 2:
        return regs->dx;
    case 3:
        return regs->r10;
    case 4:
        return regs->r8;
    default:
        return regs->r9;
    }
#else
    (void)compat;
    if (index >= 6)
        return 0;
    return regs->regs[index];
#endif
}

static void __user *ni_user_ptr(const struct pt_regs *regs, unsigned int index, bool compat)
{
    unsigned long value = ni_arg(regs, index, compat);

#if defined(__x86_64__) || defined(__aarch64__)
    if (compat)
        return compat_ptr((compat_ulong_t)value);
#endif
    return (void __user *)value;
}

static bool ni_policy_match(uid_t uid, enum ni_operation op, u32 *policy_flags)
{
    unsigned long flags;
    u32 i;

    if (op >= NI_OPERATION_COUNT)
        return false;
    if (!READ_ONCE(ni_state.enabled))
        return false;
    if (uid == 0 || is_uid_manager(uid))
        return false;

    spin_lock_irqsave(&ni_state_lock, flags);
    if (ni_state.enabled) {
        for (i = 0; i < ni_state.count; i++) {
            if (ni_state.policies[i].uid == uid) {
                *policy_flags = ni_state.policies[i].flags;
                spin_unlock_irqrestore(&ni_state_lock, flags);
                return true;
            }
        }
    }
    spin_unlock_irqrestore(&ni_state_lock, flags);
    return false;
}

static enum ni_target_class ni_ipv4_target(const struct in_addr *address)
{
    const u8 *bytes = (const u8 *)&address->s_addr;

    if (bytes[0] == 127)
        return NI_TARGET_LOOPBACK;
    if (bytes[0] == 224 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 251)
        return NI_TARGET_LOCAL_DISCOVERY;
    if (bytes[0] == 239 && bytes[1] == 255 && bytes[2] == 255 && bytes[3] == 250)
        return NI_TARGET_LOCAL_DISCOVERY;
    if (bytes[0] == 10 || (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31) ||
        (bytes[0] == 192 && bytes[1] == 168) || (bytes[0] == 169 && bytes[1] == 254))
        return NI_TARGET_PRIVATE_NETWORK;
    return NI_TARGET_OTHER_IP;
}

static enum ni_target_class ni_ipv6_target(const struct in6_addr *address)
{
    const u8 *bytes = address->s6_addr;
    struct in_addr mapped;
    bool zero_prefix = true;
    u32 i;

    for (i = 0; i < 10; i++) {
        if (bytes[i]) {
            zero_prefix = false;
            break;
        }
    }

    if (zero_prefix && !bytes[10] && !bytes[11]) {
        for (i = 12; i < 15; i++) {
            if (bytes[i])
                break;
        }
        if (i == 15 && bytes[15] == 1)
            return NI_TARGET_LOOPBACK;
    }

    if (zero_prefix && bytes[10] == 0xff && bytes[11] == 0xff) {
        memcpy(&mapped, &bytes[12], sizeof(mapped));
        return ni_ipv4_target(&mapped);
    }
    if (bytes[0] == 0xff && bytes[1] == 0x02) {
        for (i = 2; i < 15; i++) {
            if (bytes[i])
                return NI_TARGET_OTHER_IP;
        }
        return bytes[15] == 0xfb ? NI_TARGET_LOCAL_DISCOVERY : NI_TARGET_OTHER_IP;
    }
    if ((bytes[0] & 0xfe) == 0xfc || (bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80))
        return NI_TARGET_PRIVATE_NETWORK;
    return NI_TARGET_OTHER_IP;
}

static int ni_sockaddr_target(const void __user *address, unsigned int address_length, struct ni_endpoint *endpoint,
                              enum ni_target_class *target_class)
{
    __u16 family;

    memset(endpoint, 0, sizeof(*endpoint));
    *target_class = NI_TARGET_NONE;
    if (!address)
        return 0;
    if (copy_from_user(&family, address, sizeof(family)))
        return -EFAULT;

    if (family == AF_INET) {
        struct sockaddr_in addr;

        if (address_length < sizeof(addr))
            return 0;
        if (copy_from_user(&addr, address, sizeof(addr)))
            return -EFAULT;
        endpoint->family = AF_INET;
        memcpy(endpoint->address, &addr.sin_addr, sizeof(addr.sin_addr));
        endpoint->port = ntohs(addr.sin_port);
        *target_class = ni_ipv4_target(&addr.sin_addr);
    } else if (family == AF_INET6) {
        struct sockaddr_in6 addr;

        if (address_length < sizeof(addr))
            return 0;
        if (copy_from_user(&addr, address, sizeof(addr)))
            return -EFAULT;
        endpoint->family = AF_INET6;
        memcpy(endpoint->address, &addr.sin6_addr, sizeof(addr.sin6_addr));
        endpoint->port = ntohs(addr.sin6_port);
        *target_class = ni_ipv6_target(&addr.sin6_addr);
    }
    return 0;
}

static bool ni_target_allowed(enum ni_target_class target_class, u32 policy_flags)
{
    switch (target_class) {
    case NI_TARGET_LOOPBACK:
        return policy_flags & KSU_NETISOLATE_FLAG_ALLOW_LOOPBACK;
    case NI_TARGET_PRIVATE_NETWORK:
        return policy_flags & KSU_NETISOLATE_FLAG_ALLOW_PRIVATE_NETWORK;
    case NI_TARGET_LOCAL_DISCOVERY:
        return policy_flags & KSU_NETISOLATE_FLAG_ALLOW_LOCAL_DISCOVERY;
    default:
        return false;
    }
}

static int ni_sendmsg_target(const struct pt_regs *regs, bool compat, u32 policy_flags, struct ni_endpoint *endpoint,
                             bool *target)
{
    void __user *name;
    int name_length;
    enum ni_target_class target_class;
    int ret;

    *target = false;
    if (compat) {
        struct compat_msghdr __user *msg = (struct compat_msghdr __user *)ni_user_ptr(regs, 1, true);
        compat_uptr_t raw_name;

        if (!msg) {
            return 0;
        }
        if (get_user(raw_name, &msg->msg_name) || get_user(name_length, &msg->msg_namelen))
            return -EFAULT;
        name = compat_ptr(raw_name);
    } else {
        struct user_msghdr __user *msg = (struct user_msghdr __user *)ni_user_ptr(regs, 1, false);
        void __user *raw_name = NULL;

        if (!msg) {
            return 0;
        }
        if (get_user(raw_name, &msg->msg_name) || get_user(name_length, &msg->msg_namelen))
            return -EFAULT;
        name = raw_name;
    }

    if (name_length < 0)
        return 0;
    ret = ni_sockaddr_target(name, name_length, endpoint, &target_class);
    if (ret)
        return ret;
    *target = target_class != NI_TARGET_NONE && !ni_target_allowed(target_class, policy_flags);
    return 0;
}

static int ni_sendmmsg_target(const struct pt_regs *regs, bool compat, u32 policy_flags, struct ni_endpoint *endpoint,
                              bool *target)
{
    unsigned long base = ni_arg(regs, 1, compat);
    unsigned long vlen = ni_arg(regs, 2, compat);
    unsigned long count = min_t(unsigned long, vlen, NI_SENDMMSG_CHECK_MAX);
    unsigned long i;

    *target = false;
    if (!base || !count)
        return 0;

    for (i = 0; i < count; i++) {
        void __user *name;
        enum ni_target_class target_class;
        int name_length;
        int ret;

        if (compat) {
            unsigned long offset = i * sizeof(struct compat_mmsghdr);
            struct compat_mmsghdr __user *entry;
            compat_uptr_t raw_name;

            if (offset > ULONG_MAX - base)
                return -EFAULT;
            entry = (struct compat_mmsghdr __user *)(base + offset);
            if (get_user(raw_name, &entry->msg_hdr.msg_name) || get_user(name_length, &entry->msg_hdr.msg_namelen))
                return -EFAULT;
            name = compat_ptr(raw_name);
        } else {
            unsigned long offset = i * sizeof(struct mmsghdr);
            struct mmsghdr __user *entry;
            void __user *raw_name = NULL;

            if (offset > ULONG_MAX - base)
                return -EFAULT;
            entry = (struct mmsghdr __user *)(base + offset);
            if (get_user(raw_name, &entry->msg_hdr.msg_name) || get_user(name_length, &entry->msg_hdr.msg_namelen))
                return -EFAULT;
            name = raw_name;
        }

        if (!name)
            continue;
        if (name_length < 0)
            continue;
        ret = ni_sockaddr_target(name, name_length, endpoint, &target_class);
        if (ret)
            return ret;
        if (target_class != NI_TARGET_NONE && !ni_target_allowed(target_class, policy_flags)) {
            *target = true;
            return 0;
        }
    }
    return 0;
}

static bool __nocfi ni_should_refuse(const struct pt_regs *regs, enum ni_operation op, bool compat,
                                     struct ni_endpoint *endpoint)
{
    u32 policy_flags = 0;
    enum ni_target_class target_class;
    bool target = false;

    memset(endpoint, 0, sizeof(*endpoint));
    if (!ni_policy_match(current_uid().val, op, &policy_flags))
        return false;

    switch (op) {
    case NI_CONNECT:
        if (ni_sockaddr_target(ni_user_ptr(regs, 1, compat), ni_arg(regs, 2, compat), endpoint, &target_class))
            return false;
        target = target_class != NI_TARGET_NONE && !ni_target_allowed(target_class, policy_flags);
        break;
    case NI_SENDTO:
        if (ni_sockaddr_target(ni_user_ptr(regs, 4, compat), ni_arg(regs, 5, compat), endpoint, &target_class))
            return false;
        target = target_class != NI_TARGET_NONE && !ni_target_allowed(target_class, policy_flags);
        break;
    case NI_SENDMSG:
        if (ni_sendmsg_target(regs, compat, policy_flags, endpoint, &target))
            return false;
        break;
    case NI_SENDMMSG:
        if (ni_sendmmsg_target(regs, compat, policy_flags, endpoint, &target))
            return false;
        break;
    case NI_SOCKET:
        if (!(policy_flags & KSU_NETISOLATE_FLAG_STRICT_SOCKET))
            return false;
        endpoint->family = ni_arg(regs, 0, compat);
        target = endpoint->family == AF_INET || endpoint->family == AF_INET6;
        break;
    }
    return target;
}

static void ni_record_event(u32 uid, enum ni_operation op, const struct ni_endpoint *endpoint)
{
    struct ksu_netisolate_event event = {
        .timestamp_ns = ktime_get_ns(),
        .uid = uid,
        .port = endpoint->port,
        .family = endpoint->family,
        .operation = op,
    };
    unsigned long flags;
    u32 slot;

    memcpy(event.address, endpoint->address, sizeof(event.address));
    spin_lock_irqsave(&ni_event_lock, flags);
    if (READ_ONCE(ni_stopping)) {
        spin_unlock_irqrestore(&ni_event_lock, flags);
        return;
    }
    slot = ni_events.head;
    ni_events.events[slot] = event;
    ni_events.head = (slot + 1) % KSU_NETISOLATE_EVENT_MAX;
    if (ni_events.count < KSU_NETISOLATE_EVENT_MAX)
        ni_events.count++;
    else
        ni_events.dropped++;
    spin_unlock_irqrestore(&ni_event_lock, flags);
}

#define NI_WRAPPER(name, op, compat)                                                                                   \
    static long __nocfi name(const struct pt_regs *regs)                                                               \
    {                                                                                                                  \
        struct ni_endpoint endpoint;                                                                                   \
        if (ni_should_refuse(regs, op, compat, &endpoint)) {                                                           \
            ni_record_event(current_uid().val, op, &endpoint);                                                         \
            return op == NI_SOCKET ? -EACCES : -ECONNREFUSED;                                                          \
        }                                                                                                              \
        return ni_original(op, compat)(regs);                                                                          \
    }

NI_WRAPPER(ni_connect, NI_CONNECT, false)
NI_WRAPPER(ni_sendto, NI_SENDTO, false)
NI_WRAPPER(ni_sendmsg, NI_SENDMSG, false)
NI_WRAPPER(ni_sendmmsg, NI_SENDMMSG, false)
NI_WRAPPER(ni_socket, NI_SOCKET, false)

#if defined(__aarch64__) || defined(__x86_64__)
NI_WRAPPER(ni_connect_compat, NI_CONNECT, true)
NI_WRAPPER(ni_sendto_compat, NI_SENDTO, true)
NI_WRAPPER(ni_sendmsg_compat, NI_SENDMSG, true)
NI_WRAPPER(ni_sendmmsg_compat, NI_SENDMMSG, true)
NI_WRAPPER(ni_socket_compat, NI_SOCKET, true)
#endif

#undef NI_WRAPPER

static const syscall_fn_t ni_native_replacements[] = {
    ni_connect, ni_sendto, ni_sendmsg, ni_sendmmsg, ni_socket,
};

#if defined(__aarch64__) || defined(__x86_64__)
static const syscall_fn_t ni_compat_replacements[] = {
    ni_connect_compat, ni_sendto_compat, ni_sendmsg_compat, ni_sendmmsg_compat, ni_socket_compat,
};
#endif

static void ni_fill_native_hooks(void)
{
    u32 i;

    for (i = 0; i < ARRAY_SIZE(ni_native_nrs); i++) {
        struct ni_hook *hook = &ni_hooks[NI_NATIVE_BASE + i];

        hook->table = ksu_syscall_table;
        hook->nr = ni_native_nrs[i];
        hook->replacement = ni_native_replacements[i];
        if (hook->table && hook->nr >= 0 && hook->nr < NI_SYSCALL_LIMIT)
            hook->original = READ_ONCE(hook->table[hook->nr]);
    }
}

static bool ni_native_hooks_valid(void)
{
    u32 i;

    for (i = 0; i < ARRAY_SIZE(ni_native_replacements); i++) {
        struct ni_hook *hook = &ni_hooks[NI_NATIVE_BASE + i];

        if (!hook->table || hook->nr < 0 || hook->nr >= NI_SYSCALL_LIMIT || !hook->original)
            return false;
    }
    return true;
}

static void ni_fill_compat_hooks(void)
{
#if defined(__aarch64__)
    const char *table_name = "compat_sys_call_table";
    const char *ni_name = "__arm64_sys_ni_syscall";
#elif defined(__x86_64__)
    const char *table_name = "ia32_sys_call_table";
    const char *ni_name = "__ia32_sys_ni_syscall";
#else
    return;
#endif
    syscall_fn_t *table;
    syscall_fn_t ni_syscall;
    u32 i;

    table = (syscall_fn_t *)ksu_resolve_symbol_for_functable_hook(table_name);
    ni_syscall = (syscall_fn_t)ksu_resolve_symbol_for_functable_hook(ni_name);
    if (!table || !ni_syscall)
        return;

    for (i = 0; i < ARRAY_SIZE(ni_compat_nrs); i++) {
        struct ni_hook *hook = &ni_hooks[NI_COMPAT_BASE + i];

        hook->table = table;
        hook->nr = ni_compat_nrs[i];
#if defined(__aarch64__) || defined(__x86_64__)
        hook->replacement = ni_compat_replacements[i];
#endif
        if (hook->nr >= 0 && hook->nr < NI_SYSCALL_LIMIT)
            hook->original = READ_ONCE(hook->table[hook->nr]);
        if (!hook->original || hook->original == ni_syscall) {
            int nr = hook->nr;

            memset(&ni_hooks[NI_COMPAT_BASE], 0, sizeof(ni_hooks[NI_COMPAT_BASE]) * ARRAY_SIZE(ni_compat_nrs));
            pr_info("netisolate: compat table unavailable entry=%d nr=%d\n", i, nr);
            return;
        }
    }
    ni_compat_supported = true;
}

static int ni_patch(const struct ni_hook *hook, syscall_fn_t replacement)
{
    if (!hook->table || hook->nr < 0 || hook->nr >= NI_SYSCALL_LIMIT)
        return -EINVAL;
    return ksu_patch_text(&hook->table[hook->nr], &replacement, sizeof(replacement), KSU_PATCH_TEXT_FLUSH_DCACHE);
}

static int ni_uninstall_hooks_locked(void);

static int ni_install_hooks_locked(void)
{
    u32 i;

    for (i = 0; i < NI_HOOK_COUNT; i++) {
        struct ni_hook *hook = &ni_hooks[i];
        int ret;

        if (!hook->table || hook->installed)
            continue;
        ret = ni_patch(hook, hook->replacement);
        if (ret) {
            pr_err("netisolate: patch failed table=%s nr=%d result=%d\n",
                   hook->table == ksu_syscall_table ? "native" : "compat", hook->nr, ret);
            break;
        }
        hook->installed = true;
    }
    if (i == NI_HOOK_COUNT)
        return 0;

    {
        int rollback = ni_uninstall_hooks_locked();

        if (rollback)
            pr_err("netisolate: install rollback failed result=%d\n", rollback);
    }
    return -EIO;
}

static int ni_uninstall_hooks_locked(void)
{
    int ret = 0;
    int i;

    for (i = NI_HOOK_COUNT - 1; i >= 0; i--) {
        struct ni_hook *hook = &ni_hooks[i];
        int err;

        if (!hook->table || !hook->installed)
            continue;
        err = ni_patch(hook, hook->original);
        if (err) {
            pr_err("netisolate: restore failed table=%s nr=%d result=%d\n",
                   hook->table == ksu_syscall_table ? "native" : "compat", hook->nr, err);
            ret = err;
            continue;
        }
        hook->installed = false;
    }
    return ret;
}

static void ni_publish_locked(const struct ksu_netisolate_policies *config)
{
    unsigned long flags;
    u32 i;

    spin_lock_irqsave(&ni_state_lock, flags);
    ni_state.count = config->count;
    for (i = 0; i < config->count; i++) {
        ni_state.policies[i].uid = config->policies[i].uid;
        ni_state.policies[i].flags = config->policies[i].flags;
    }
    ni_state.enabled = config->enabled ? 1 : 0;
    spin_unlock_irqrestore(&ni_state_lock, flags);
}

static int ni_validate_config(const struct ksu_netisolate_config *config)
{
    u32 policy_flags;
    u32 i;
    u32 j;

    if (config->reserved & ~NI_SUPPORTED_FLAGS)
        return -EINVAL;
    policy_flags = config->reserved & KSU_NETISOLATE_FLAG_MASK;
    if ((policy_flags & KSU_NETISOLATE_FLAG_STRICT_SOCKET) && (policy_flags & NI_ALLOW_FLAGS))
        return -EINVAL;
    if (config->count > KSU_NETISOLATE_MAX_UIDS)
        return -EINVAL;
    if (config->enabled && !config->count)
        return -EINVAL;
    for (i = 0; i < config->count; i++) {
        if (!config->uids[i])
            return -EINVAL;
        for (j = 0; j < i; j++) {
            if (config->uids[j] == config->uids[i])
                return -EINVAL;
        }
    }
    return 0;
}

static int ni_validate_policies(const struct ksu_netisolate_policies *config)
{
    u32 flags;
    u32 i;
    u32 j;

    if (config->supported_flags || config->reserved)
        return -EINVAL;
    if (config->count > KSU_NETISOLATE_MAX_POLICIES)
        return -EINVAL;
    if (config->enabled && !config->count)
        return -EINVAL;
    for (i = 0; i < config->count; i++) {
        flags = config->policies[i].flags;
        if (!config->policies[i].uid || flags & ~NI_SUPPORTED_FLAGS)
            return -EINVAL;
        if ((flags & KSU_NETISOLATE_FLAG_STRICT_SOCKET) && (flags & NI_ALLOW_FLAGS))
            return -EINVAL;
        for (j = 0; j < i; j++) {
            if (config->policies[j].uid == config->policies[i].uid)
                return -EINVAL;
        }
    }
    return 0;
}

static void ni_legacy_to_policies(const struct ksu_netisolate_config *legacy, struct ksu_netisolate_policies *policies)
{
    u32 flags = legacy->reserved & KSU_NETISOLATE_FLAG_MASK;
    u32 i;

    memset(policies, 0, sizeof(*policies));
    policies->enabled = legacy->enabled;
    policies->count = legacy->count;
    for (i = 0; i < legacy->count; i++) {
        policies->policies[i].uid = legacy->uids[i];
        policies->policies[i].flags = flags;
    }
}

static void ni_clear_events_locked(void)
{
    unsigned long flags;

    spin_lock_irqsave(&ni_event_lock, flags);
    memset(&ni_events, 0, sizeof(ni_events));
    spin_unlock_irqrestore(&ni_event_lock, flags);
}

int __init ksu_netisolate_init(void)
{
    mutex_lock(&ni_lifecycle_lock);
    if (ni_supported) {
        mutex_unlock(&ni_lifecycle_lock);
        return -EBUSY;
    }
    memset(ni_hooks, 0, sizeof(ni_hooks));
    memset(&ni_state, 0, sizeof(ni_state));
    ni_clear_events_locked();
    ni_compat_supported = false;
    ni_stopping = false;
    ni_fill_native_hooks();
    if (!ni_native_hooks_valid()) {
        mutex_unlock(&ni_lifecycle_lock);
        return -ENODEV;
    }
    ni_fill_compat_hooks();
    ni_supported = true;
    mutex_unlock(&ni_lifecycle_lock);
    pr_info("netisolate: event=init result=ok compat=%u flags=0x%x\n", ni_compat_supported, NI_SUPPORTED_FLAGS);
    return 0;
}

void ksu_netisolate_exit(void)
{
    mutex_lock(&ni_lifecycle_lock);
    WRITE_ONCE(ni_stopping, true);
    ni_publish_locked(&(struct ksu_netisolate_policies){});
    {
        int ret = ni_uninstall_hooks_locked();

        if (ret)
            pr_err("netisolate: exit restore incomplete result=%d\n", ret);
    }
    ni_supported = false;
    ni_compat_supported = false;
    ni_clear_events_locked();
    mutex_unlock(&ni_lifecycle_lock);
}

int ksu_netisolate_get_config(struct ksu_netisolate_config *config)
{
    unsigned long flags;
    u32 common_flags = 0;
    u32 i;

    if (!config)
        return -EINVAL;
    mutex_lock(&ni_lifecycle_lock);
    if (!ni_supported || ni_stopping) {
        mutex_unlock(&ni_lifecycle_lock);
        return -EOPNOTSUPP;
    }
    memset(config, 0, sizeof(*config));
    spin_lock_irqsave(&ni_state_lock, flags);
    config->enabled = ni_state.enabled;
    config->count = ni_state.count;
    if (ni_state.count)
        common_flags = ni_state.policies[0].flags;
    for (i = 0; i < ni_state.count; i++) {
        config->uids[i] = ni_state.policies[i].uid;
        common_flags &= ni_state.policies[i].flags;
    }
    config->reserved = common_flags & KSU_NETISOLATE_FLAG_MASK;
    spin_unlock_irqrestore(&ni_state_lock, flags);
    config->compat_supported = ni_compat_supported;
    config->reserved |= (NI_SUPPORTED_FLAGS & KSU_NETISOLATE_FLAG_MASK) << KSU_NETISOLATE_SUPPORTED_SHIFT;
    mutex_unlock(&ni_lifecycle_lock);
    return 0;
}

int ksu_netisolate_set_config(const struct ksu_netisolate_config *config)
{
    struct ksu_netisolate_policies *policies;
    int ret;

    if (!config)
        return -EINVAL;
    ret = ni_validate_config(config);
    if (ret)
        return ret;
    policies = kzalloc(sizeof(*policies), GFP_KERNEL);
    if (!policies)
        return -ENOMEM;
    ni_legacy_to_policies(config, policies);
    ret = ksu_netisolate_set_policies(policies);
    kfree(policies);
    return ret;
}

int ksu_netisolate_get_policies(struct ksu_netisolate_policies *config)
{
    unsigned long flags;
    u32 i;

    if (!config)
        return -EINVAL;
    mutex_lock(&ni_lifecycle_lock);
    if (!ni_supported || ni_stopping) {
        mutex_unlock(&ni_lifecycle_lock);
        return -EOPNOTSUPP;
    }
    memset(config, 0, sizeof(*config));
    spin_lock_irqsave(&ni_state_lock, flags);
    config->enabled = ni_state.enabled;
    config->count = ni_state.count;
    for (i = 0; i < ni_state.count; i++) {
        config->policies[i].uid = ni_state.policies[i].uid;
        config->policies[i].flags = ni_state.policies[i].flags;
    }
    spin_unlock_irqrestore(&ni_state_lock, flags);
    config->supported_flags = NI_SUPPORTED_FLAGS;
    mutex_unlock(&ni_lifecycle_lock);
    return 0;
}

int ksu_netisolate_set_policies(const struct ksu_netisolate_policies *config)
{
    int ret;

    if (!config)
        return -EINVAL;
    ret = ni_validate_policies(config);
    if (ret)
        return ret;

    mutex_lock(&ni_lifecycle_lock);
    if (!ni_supported || ni_stopping) {
        ret = -EOPNOTSUPP;
        goto out;
    }
    if (config->enabled) {
        ret = ni_install_hooks_locked();
        if (ret)
            goto out;
        ni_publish_locked(config);
    } else {
        ni_publish_locked(config);
        ret = ni_uninstall_hooks_locked();
    }
out:
    mutex_unlock(&ni_lifecycle_lock);
    return ret;
}

int ksu_netisolate_get_events(struct ksu_netisolate_events *events)
{
    unsigned long flags;
    u32 first;
    u32 i;

    if (!events)
        return -EINVAL;
    mutex_lock(&ni_lifecycle_lock);
    if (!ni_supported || ni_stopping) {
        mutex_unlock(&ni_lifecycle_lock);
        return -EOPNOTSUPP;
    }
    memset(events, 0, sizeof(*events));
    spin_lock_irqsave(&ni_event_lock, flags);
    events->count = ni_events.count;
    events->capacity = KSU_NETISOLATE_EVENT_MAX;
    events->dropped = ni_events.dropped;
    first = (ni_events.head + KSU_NETISOLATE_EVENT_MAX - ni_events.count) % KSU_NETISOLATE_EVENT_MAX;
    for (i = 0; i < ni_events.count; i++)
        events->events[i] = ni_events.events[(first + i) % KSU_NETISOLATE_EVENT_MAX];
    spin_unlock_irqrestore(&ni_event_lock, flags);
    mutex_unlock(&ni_lifecycle_lock);
    return 0;
}

int ksu_netisolate_clear_events(void)
{
    int ret = 0;

    mutex_lock(&ni_lifecycle_lock);
    if (!ni_supported || ni_stopping)
        ret = -EOPNOTSUPP;
    else
        ni_clear_events_locked();
    mutex_unlock(&ni_lifecycle_lock);
    return ret;
}
