#ifndef LINUX_MODULE_H
#define LINUX_MODULE_H

/* MyOS has no loadable modules: a driver is compiled in, and its
   module_init function is called once at boot by name (the driver list
   that brings it up). These macros make a Linux driver's module glue
   compile and do nothing else. */
#include <linux/compiler.h>

struct module;
#define THIS_MODULE            ((struct module *)0)
#define MODULE_LICENSE(x)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_FIRMWARE(x)
#define MODULE_DEVICE_TABLE(t, n)
#define MODULE_ALIAS(x)
#define MODULE_IMPORT_NS(x)
#define MODULE_VERSION(x)
#define MODULE_PARM_DESC(n, d)
#define module_param(n, t, p)
#define module_param_named(n, v, t, p)
#define module_param_array(n, t, nump, p)
#define module_init(fn)        int __init_##fn(void) { return fn(); }
#define module_exit(fn)        void __exit_##fn(void) { fn(); }
#define try_module_get(m)      1
#define request_module(...)          (-ENOENT)   /* every driver is built in */
#define request_module_nowait(...)   ({ 0; })
#define module_put(m)          do { } while (0)
#define __module_get(m)        do { } while (0)
#define EXPORT_SYMBOL(s)
#define EXPORT_SYMBOL_GPL(s)
#define EXPORT_SYMBOL_IF_KUNIT(s)
#define EXPORT_SYMBOL_NS(s, ns)
#define EXPORT_SYMBOL_NS_GPL(s, ns)
#define VISIBLE_IF_KUNIT       static

#endif
