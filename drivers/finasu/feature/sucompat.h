#ifndef __KSU_H_SUCOMPAT
#define __KSU_H_SUCOMPAT
#include <asm/ptrace.h>
#include <linux/jump_label.h>
#include <linux/types.h>

#ifdef CONFIG_KSU_SUSFS
extern struct static_key_true ksu_su_compat_enabled;

static inline bool ksu_su_compat_is_enabled(void)
{
    return static_branch_likely(&ksu_su_compat_enabled);
}
#else
extern bool ksu_su_compat_enabled;

static inline bool ksu_su_compat_is_enabled(void)
{
    return ksu_su_compat_enabled;
}
#endif

void ksu_sucompat_init(void);
void ksu_sucompat_exit(void);

#ifdef CONFIG_KSU_SUSFS
void ksu_sucompat_mark_no_su(void);
#endif

// Handler functions exported for hook_manager
long ksu_handle_faccessat_sucompat(int orig_nr, struct pt_regs *regs);
long ksu_handle_stat_sucompat(int orig_nr, struct pt_regs *regs);
long ksu_handle_execve_sucompat(const char __user **filename_user, int orig_nr, struct pt_regs *regs);
long ksu_handle_execveat_sucompat_regs(const char __user **filename_user, int orig_nr, struct pt_regs *regs);

#endif
