#include <asm/barrier.h>
#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/string.h>

#include "feature/module_blacklist.h"
#include "infra/symbol_resolver.h"
#include "klog.h" // IWYU pragma: keep

// Use the native module loader blacklist, without adding syscall hooks.
// Static storage is required because built-in params may precede slab init.
// Developer only: EPERM is fatal to first stage init for modules listed in
// modules.load / modules.load.recovery but absent from modules.blocklist.
static char block_modules[256];

static int block_modules_param_set(const char *val, const struct kernel_param *kp)
{
    if (!val || !val[0])
        return 0;
    if (strscpy(block_modules, val, sizeof(block_modules)) < 0)
        pr_err("module_blacklist: list truncated; some modules will not be blocked\n");
    return 0;
}

static const struct kernel_param_ops block_modules_param_ops = {
    .flags = KERNEL_PARAM_OPS_FL_NOARG,
    .set = block_modules_param_set,
};
module_param_cb(block_modules, &block_modules_param_ops, NULL, 0);

void __init ksu_module_blacklist_init(void)
{
    char **native_blacklist;
    const char *existing;
    char *merged;

    if (!block_modules[0])
        return;
    native_blacklist = (char **)find_kernel_symbol_exact("module_blacklist");
    if (!native_blacklist) {
        pr_warn("module_blacklist: symbol not found, cannot block modules\n");
        return;
    }
    existing = *native_blacklist;
    merged =
        kasprintf(GFP_KERNEL, "%s%s%s", existing ? existing : "", existing && existing[0] ? "," : "", block_modules);
    if (!merged) {
        pr_err("module_blacklist: cannot allocate blacklist\n");
        return;
    }
    smp_store_release(native_blacklist, merged);
    kick_all_cpus_sync();
    pr_info("module_blacklist: blocking modules: %s\n", merged);
}

void __exit ksu_module_blacklist_exit(void)
{
    // Keep the merged heap allocation installed for the remainder of this boot.
    // The native reader is lockless: restoring a possibly NULL pointer or
    // freeing the string can race a module load. Nothing points into our module.
}
