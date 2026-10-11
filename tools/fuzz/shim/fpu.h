/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/* fpu.h for the host fuzz build: just enough for the real thread.h, which
   the filesystem code needs for the wait-queue types. No FPU state is ever
   saved or restored here -- nothing in the fuzz build switches threads. */
#ifndef FUZZ_SHIM_FPU_H
#define FUZZ_SHIM_FPU_H

#define FPU_STATE_SIZE 512

#endif
