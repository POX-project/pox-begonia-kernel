#ifndef __KSU_H_SELINUX_HIDE
#define __KSU_H_SELINUX_HIDE

#include <linux/types.h>

struct file;

extern bool ksu_selinux_hide_enabled;
extern bool ksu_selinux_hide_running;

ssize_t my_write_context(struct file *file, char *buf, size_t size);
ssize_t my_write_access(struct file *file, char *buf, size_t size);

void ksu_selinux_hide_init();
void ksu_selinux_hide_exit();
void ksu_selinux_hide_drop_backup_if_unused();
void ksu_selinux_hide_handle_second_stage();
void ksu_selinux_hide_handle_post_fs_data();

#endif
