/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef CRED_H
#define CRED_H

#include <stdint.h>

/*
 * Who a process is: its credentials, as Linux keeps them.
 *
 *   uid   the real user: who started it (login sets it)
 *   euid  the effective user: whose rights it acts with. A setuid program
 *         (passwd, doas) starts with euid = the file's owner.
 *   suid  the saved user: lets a setuid program step down to uid and back
 *   fsuid the user for file access; follows euid (setfsuid changes it alone)
 *
 * The same four for groups, plus up to NGROUPS supplementary groups (the
 * ones /etc/group lists the user in). euid 0 is root: allowed everything.
 * All zeros is root, which is what the first process gets.
 */
#define NGROUPS 32

struct cred {
    uint32_t uid, euid, suid, fsuid;
    uint32_t gid, egid, sgid, fsgid;
    uint32_t ngroups;
    uint32_t groups[NGROUPS];
};

#endif
