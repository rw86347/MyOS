/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_KOBJECT_H
#define LINUX_KOBJECT_H
/* Linux's kernel objects: what sysfs shows and udev hears about. MyOS has
   no sysfs device tree yet, so an event has no listener. */
enum kobject_action { KOBJ_ADD, KOBJ_REMOVE, KOBJ_CHANGE, KOBJ_MOVE, KOBJ_ONLINE, KOBJ_OFFLINE };
struct kobject { const char *name; };
static inline int kobject_uevent_env(struct kobject *k, enum kobject_action a, char *envp[])
{ (void)k; (void)a; (void)envp; return 0; }
static inline int kobject_uevent(struct kobject *k, enum kobject_action a) { (void)k; (void)a; return 0; }
#endif
