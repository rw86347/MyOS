/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_I386_FPU_H
#define ARCH_I386_FPU_H

#include <stdint.h>

/* Bytes needed to save the FPU/SSE registers (FXSAVE format; must be
   16-byte aligned). */
#define FPU_STATE_SIZE 512

/* Turn on the FPU and SSE and remember the clean reset state. Call once,
   early. */
void fpu_init(void);

/* Save / load all FPU and SSE registers. */
void fpu_save(void *state);
void fpu_restore(const void *state);

/* Fill `state` with a freshly reset FPU (what a new program starts with). */
void fpu_clean_state(void *state);

#endif
