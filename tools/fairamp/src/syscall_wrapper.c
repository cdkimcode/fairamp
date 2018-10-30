#include <linux/unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include "fairamp.h"
#include "syscall_wrapper.h"

inline void set_fast_core(int cpu_id) {
	int error;
	error = syscall(__NR_fairamp, SET_FAST_CORE, cpu_id, 0, NULL);
	if (error)
		printf("Error: %d while set fast core %d\n", error, cpu_id);
	return;
}

inline void set_slow_core(int cpu_id) {
	int error;
	error = syscall(__NR_fairamp, SET_SLOW_CORE, cpu_id, 0, NULL);
	if (error)
		printf("Error: %d while set slow core %d\n", error, cpu_id);
	return;
}

inline void set_unit_vruntime(int num, struct fairamp_unit_vruntime *info) {
	int error;
	error = syscall(__NR_fairamp, SET_UNIT_VRUNTIME, 0, num, info);
	if (error != num)
		printf("Error: %d while set unit_vruntime of %d threads\n", error, num);
	return;
}

inline void get_threads_info(int num, struct fairamp_threads_info *info) {
	int error;
	error = syscall(__NR_fairamp, GET_THREADS_INFO, 0, num, info);
	if (error != num)
		printf("Error: %d while get %d threads info\n", error, num);
	return;
}

inline void start_measuring_IPS_type(void) {
	int error;
	error = syscall(__NR_fairamp, START_MEASURING_IPS_TYPE, 0, 0, NULL);
	if (error)
		printf("Error: %d while start measuring IPS type\n", error);
	return;
}

inline void stop_measuring_IPS_type(void) {
	int error;
	error = syscall(__NR_fairamp, STOP_MEASURING_IPS_TYPE, 0, 0, NULL);
	if (error)
		printf("Error: %d while stop measuring IPS type\n", error);
	return;
}

inline void core_pinning(unsigned long pid, int cpu_id) {
	int error;
	error = syscall(__NR_fairamp, CORE_PINNING, cpu_id, pid, NULL);
	if (error)
		printf("Error: %d while pinning to core %d\n", error, cpu_id);
	return;
}

/*inline void turn_on_debugging() {
	int error;
	error = syscall(__NR_fairamp, SET_FAIRAMP_DEBUGGING_MODE, 1, 0, NULL);
	if (error)
		printf("Error: %d while turn on debugging mode\n", error);
	return;
}

inline void turn_off_debugging() {
	int error;
	error = syscall(__NR_fairamp, SET_FAIRAMP_DEBUGGING_MODE, 0, 0, NULL);
	if (error)
		printf("Error: %d while turn on debugging mode\n", error);
	return;
}*/
