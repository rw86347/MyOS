/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_CPU_H
#define LINUX_CPU_H
#define num_online_cpus() 1
#define num_possible_cpus() 1
#define smp_processor_id() 0
#define raw_smp_processor_id() 0
#define for_each_possible_cpu(c) for ((c) = 0; (c) < 1; (c)++)
#define for_each_online_cpu(c) for ((c) = 0; (c) < 1; (c)++)
#define cpus_read_lock() do { } while (0)
#define cpus_read_unlock() do { } while (0)
#endif
