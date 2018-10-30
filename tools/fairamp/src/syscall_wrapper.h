#ifndef __SYSCAL_WRAPPER_H__
#define __SYSCAL_WRAPPER_H__

#include <linux/unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include "fairamp.h"

/* XXX: this only work in x86_64 */
#define __NR_fairamp			313

/* Definition of operations of fairamp system call */
#define SET_FAST_CORE               0
#define SET_SLOW_CORE               1
#define SET_UNIT_VRUNTIME           2
#define GET_THREADS_INFO            3
#define START_MEASURING_IPS_TYPE    4
#define STOP_MEASURING_IPS_TYPE     5
#define CORE_PINNING                6

/* Do not use these functions without fairamp kernel. */
void set_fast_core(int cpu_id);
void set_slow_core(int cpu_id);
void set_unit_vruntime(int num, struct fairamp_unit_vruntime *info);
void get_threads_info(int num, struct fairamp_threads_info *info);
void start_measuring_IPS_type(void);
void stop_measuring_IPS_type(void);
void core_pinning(unsigned long pid, int cpu_id);

#endif /* __SYSCAL_WRAPPER_H__ */
