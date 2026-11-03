#ifndef __KSU_FEATURE_UTS_VIEW_H
#define __KSU_FEATURE_UTS_VIEW_H

#include <linux/types.h>

#include "uapi/uts_view.h"

int __init ksu_uts_view_init(void);
void ksu_uts_view_exit(void);

int ksu_uts_view_get_config(struct ksu_uts_view_config *config);
int ksu_uts_view_set_config(const struct ksu_uts_view_config *config);

void ksu_uts_view_on_setresuid(uid_t old_uid, uid_t new_uid);

#endif // __KSU_FEATURE_UTS_VIEW_H
