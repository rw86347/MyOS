/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_STDDEF_H
#define LINUX_STDDEF_H
#include <stddef.h>
/* The offset just past a member: how much of a struct a buffer must hold
   to contain that member. */
#define offsetofend(T, m)    (offsetof(T, m) + sizeof(((T *)0)->m))
#define sizeof_field(T, m)   sizeof(((T *)0)->m)
/* A flexible array allowed where C forbids one (in a union, or alone). */
#define DECLARE_FLEX_ARRAY(T, name) struct { struct { } __empty_##name; T name[]; }
/* A group of members that can be reached both directly and as one
   struct (to memset or copy them together). */
#define __struct_group(TAG, NAME, ATTRS, MEMBERS...) \
    union { struct { MEMBERS } ATTRS; struct TAG { MEMBERS } ATTRS NAME; } ATTRS
#define struct_group(NAME, MEMBERS...)          __struct_group(/* no tag */, NAME, /* no attrs */, MEMBERS)
#define struct_group_attr(NAME, ATTRS, MEMBERS...) __struct_group(/* no tag */, NAME, ATTRS, MEMBERS)
#define struct_group_tagged(TAG, NAME, MEMBERS...) __struct_group(TAG, NAME, /* no attrs */, MEMBERS)
#endif
