#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "fairamp.h"
#include "syscall_wrapper.h"
#include <time.h>


/* ============================ */
/* speedup estimation mechanism */
/* ============================ */
struct speedup_info {
	pid_t pid;
	struct command *comm;
	float IPS_fast;
	float IPS_slow;
	float CPU_util;
	int num_samples_fast;
	int num_samples_slow;
};

/* For parallelsim aware speedup,
 *     speedup = (IPS_ratio*MIN(Util,num_fast_core_f)+MAX(0,Util-num_fast_core_f))/Util
 *                ---------------------------------   --------------------------
 *                       perf on fast core               perf on slow core
 *               ----------------------------------------------------------------  ----
 *                     throughput on fast core                                     throughput on slow core
 */
#define MIN2(A, B) ((A) < (B) ? (A) : (B))
#define MAX2(A, B) ((A) > (B) ? (A) : (B))
/* for single threaded commmarks, this is enough. */
#define GET_SPEEDUP_SINGLE_THREAD(IPS_fast, IPS_slow, CPU_util) \
				((IPS_fast) / (IPS_slow))
#define GET_SPEEDUP_MULTI_THREAD(IPS_fast, IPS_slow, CPU_util) \
		((IPS_fast / IPS_slow) \
				* (MIN2(CPU_util, env.num_fast_core_f) \
						+ MAX2(0, CPU_util - env.num_fast_core_f)) \
				/ CPU_util)
static inline float get_speedup(struct speedup_info *info) {
	if (info->IPS_fast == 0 || info->IPS_slow == 0)
		return 1.0;
	else if (info->comm->num_threads == 1 || info->CPU_util <= 1.0)
		return GET_SPEEDUP_SINGLE_THREAD(info->IPS_fast, info->IPS_slow, info->CPU_util); 
	else
		return GET_SPEEDUP_MULTI_THREAD(info->IPS_fast, info->IPS_slow, info->CPU_util);
}


static inline float absolute(float val) {
	return val >= 0 ? val : -val;
}

void *periodic_update_speedup(void *data)
{
	int i = 0;
	unsigned long num_called = 0;
	unsigned long long num_comm = (unsigned long long)data;
	struct command *comm = NULL;
	struct fairamp_threads_info *to_get = NULL;
	int byte_to_get = num_comm * sizeof(struct fairamp_threads_info);
	struct speedup_info *info = NULL;
	float IPS_fast;
	float IPS_slow;
	float CPU_util;
	float full_exec_runtime;
	long ns_interval_times_core = (sched_interval.tv_sec * 1000000000 + sched_interval.tv_nsec) * num_core;
	int nr_running;
	struct fairamp_threads_info me;
	struct command *command = env.command;

	info = (struct speedup_info *)calloc(num_comm, sizeof(struct speedup_info));
	to_get = (struct fairamp_threads_info *)calloc(num_comm, sizeof(struct fairamp_threads_info));
	memset(&me, 0, sizeof(struct fairamp_threads_info));
	me.num = -1;
	/* printf("update_speedup: pid: %d\n", getpid()); */

	/* set performance counters */
	if (is_sched_policy_asymmetry_aware()) {
		measuring_IPS_type_started = 1;
		start_measuring_IPS_type();
	}

	get_threads_info(1, &me);

	while (likely(!done)) {
		nanosleep(&sched_interval, NULL);

		if (unlikely(done))
			break;

		/* even if manual, do the speedup estimation */
		if (!is_sched_policy_asymmetry_aware())
			goto skip_speedup_estimation;
			
		memset(to_get, 0, byte_to_get);	
		nr_running = 0;
		/* TODO: read performance counters and I/O boundness */
		for (i = 0; i <  num_comm; i++) {
			if (command[i].pid > 0) {
				to_get[command[i].num].num = command[i].num;
				to_get[command[i].num].pid = command[i].pid;
				nr_running += command[i].num_threads;
				info[command[i].num].comm = &command[i];
			} else {
				to_get[command[i].num].pid = 0;
				to_get[command[i].num].num = -1;
			}
		}

		if (nr_running <= num_core)
			full_exec_runtime = ns_interval_times_core / num_core;
		else
			full_exec_runtime = ns_interval_times_core / nr_running;
		
		get_threads_info(num_comm, to_get);

		verbose("Got threads info\n");

		for (i = 0; i < num_comm; i++) {
			unsigned long long sum_exec_runtime;
			
			if (to_get[i].pid == 0) {
				info[i].pid = 0;
				continue;
			}

			comm = info[i].comm;

#define MAXIMUM_IPS_RATIO			  4.0
#define INITIAL_SAMPLES				  5
			/* calculate the numbers */
			/* We got a sample only when round_slice >= minimal_round_slice
					since too short running may have lower IPS due to cache cold misses */
			sum_exec_runtime = to_get[i].sum_fast_exec_runtime + to_get[i].sum_slow_exec_runtime;

			IPS_fast = to_get[i].sum_fast_exec_runtime > 0 && comm->round_slice.fast >= minimal_round_slice
						? (float) to_get[i].insts_fast / (float) to_get[i].sum_fast_exec_runtime
						: 0;
			IPS_slow = to_get[i].sum_slow_exec_runtime > 0 && comm->round_slice.slow >= minimal_round_slice
						? (float) to_get[i].insts_slow / (float) to_get[i].sum_slow_exec_runtime
						: 0;
			
			/* XXX: this may not be an appropriate way of measuring CPU utilization, a.k.a., I/O boundness. */
			CPU_util = sum_exec_runtime > 0
						? (float) sum_exec_runtime / (full_exec_runtime * comm->num_threads)
						: 1.0;
			if (unlikely(CPU_util > 1.0 && command[i].num_threads == 1))
				CPU_util = 1.0;
			
			if (likely(config.adjust_frequency)) { /* for speedup test mode, do not drop even if IPS_fast < IPS_slow */
				if ((IPS_fast > 0 && IPS_slow > 0)
					&& (IPS_fast < IPS_slow || IPS_fast > MAXIMUM_IPS_RATIO * IPS_slow)) {
					/* drop the sample in this case */
					verbose("DROP: command%02d %10s pid: %5d IPS_fast: %6.4f IPS_slow: %6.4f\n", 
								i, comm->name, info[i].pid, IPS_fast, IPS_slow);
					IPS_fast = 0;
					IPS_slow = 0;
				}
			}
			if (unlikely(IPS_fast == 0 || IPS_slow == 0))  {
				verbose("NOEX: command%02d %10s pid: %5d IPS_fast: %6.4f IPS_slow: %6.4f\n", 
							i, comm->name, info[i].pid, IPS_fast, IPS_slow);
			}

			/* update the information */
			if (info[i].pid == 0) { /* the first sample */
				info[i].pid = to_get[i].pid;
				info[i].IPS_fast = IPS_fast;
				info[i].IPS_slow = IPS_slow;
				/* info[i].CPU_util = CPU_util; */
				info[i].CPU_util = 1.0;
				//info[i].IPS_fast_last = 0;
				//info[i].IPS_slow_last = 0;
				info[i].num_samples_fast = 0;
				info[i].num_samples_slow = 0;
			} else {
#define WEIGHTED_UPDATE(old, new, WEIGHT_OLD, WEIGHT_NEW) do{ old = ((old) * (WEIGHT_OLD) + (new) * (WEIGHT_NEW)) / ((WEIGHT_OLD) + (WEIGHT_NEW)); }while(0)
				if (IPS_fast > 0) {
					if (info[i].num_samples_fast < INITIAL_SAMPLES) {
						info[i].IPS_fast = (info[i].num_samples_fast * info[i].IPS_fast + IPS_fast)
											/ (info[i].num_samples_fast + 1);
					} else {
						WEIGHTED_UPDATE(info[i].IPS_fast, IPS_fast, 7, 3); /* intended */
						// WEIGHTED_UPDATE(info[i].IPS_fast, IPS_fast, 4, 6);
					}
					info[i].num_samples_fast++;
				}	
				if (IPS_slow > 0) {
					if (info[i].num_samples_slow < INITIAL_SAMPLES) {
						info[i].IPS_slow = (info[i].num_samples_slow * info[i].IPS_slow + IPS_slow)
											/ (info[i].num_samples_slow + 1);
					} else {
						WEIGHTED_UPDATE(info[i].IPS_slow, IPS_slow, 7, 3); /* intended */
						//WEIGHTED_UPDATE(info[i].IPS_slow, IPS_slow, 4, 6); /* previous */
					}
					info[i].num_samples_slow++;
				}
				WEIGHTED_UPDATE(info[i].CPU_util, CPU_util, 7, 3);
#undef WEIGHTED_UPDATE
			}

			/* I CAN'T REMEMBER WHY IPS_*_last VARIABLES EXIST. (maybe... for learning_required?)
			if (   to_get[i].sum_fast_exec_runtime > 0
				&& to_get[i].sum_slow_exec_runtime > 0
				&& info[i].IPS_fast >= info[i].IPS_slow
				&& info[i].IPS_fast <= MAXIMUM_IPS_RATIO * info[i].IPS_slow
				) {
				info[i].IPS_fast_last = info[i].IPS_fast;
				info[i].IPS_slow_last = info[i].IPS_slow;
			} */

			/*if (comm->speedup == 0 || (!comm->learning_required_fast && !comm->learning_required_slow)) {*/
				comm->speedup = get_speedup(info + i);
				if (likely(config.adjust_frequency))
					if (comm->speedup < 1.0) {
						/* fprintf(stderr, "[speedup < 1.0] name: %12s pid: %5d speedup: %6.4f\n", command[num_to_i[i]].name, info[i].pid, command[num_to_i[i]].speedup); */
							comm->speedup = 1.0;
					}
			/*}*/
			
			verbose("INFO: command%02d %10s pid: %5d "
				   "slice: %8d %8d "
				   "fast: %12lld / %12lld slow: %12lld / %12lld util: %12lld / %12lld "
				   "| %6.4f %6.4f %6.4f => %6.4f "
				   "| %6.4f %6.4f %6.4f => %6.4f\n",
				   i, comm->name, info[i].pid,
				   comm->round_slice.fast, comm->round_slice.slow,
				   to_get[i].insts_fast, to_get[i].sum_fast_exec_runtime,
				   to_get[i].insts_slow, to_get[i].sum_slow_exec_runtime,
				   sum_exec_runtime, (long long) full_exec_runtime,
				   IPS_fast, IPS_slow, CPU_util, 
				   comm->num_threads == 1 ? GET_SPEEDUP_SINGLE_THREAD(IPS_fast, IPS_slow, CPU_util)
				   						   : GET_SPEEDUP_MULTI_THREAD(IPS_fast, IPS_slow, CPU_util),
				   info[i].IPS_fast, info[i].IPS_slow, info[i].CPU_util, comm->speedup);
		}

		/* update the fast and slow round slice according to the policy */
		if (is_sched_policy_speedup_aware())
			set_round_slice();

#ifdef VERBOSE
		print_commands(command, num_comm);
#endif

skip_speedup_estimation:	
		num_called++;

#ifdef VERBOSE
		verbose_err("update_speedup: called: %ld\n", num_called);
		fflush(stderr);
#endif
	}

	get_threads_info(1, &me);
	printf("Scheduling_time: %lld num_called: %ld\n", me.sum_fast_exec_runtime + me.sum_slow_exec_runtime, num_called);
		
	fflush(stdout); // fflush stdout once to reduce the overhead
	free(info);
	free(to_get);

	return (void *)num_called;
}

/* just heart-beat */
void *periodic_show_stat(void *data)
{
	unsigned long num_called = 0;
	unsigned long long num_comm = (unsigned long long)data;
	int i;
	struct command *comm[num_comm];
	struct command *command = env.command;
	struct fairamp_threads_info *to_get = NULL;
	int byte_to_get = num_comm * sizeof(struct fairamp_threads_info);
	struct fairamp_threads_info me;
	
	to_get = (struct fairamp_threads_info *)calloc(num_comm, sizeof(struct fairamp_threads_info));
	memset(&me, 0, sizeof(struct fairamp_threads_info));
	me.num = -1;
	
	get_threads_info(1, &me);

	while (likely(!done)) {
		nanosleep(&sched_interval, NULL);

		if (unlikely(done))
			break;
		
		
		memset(to_get, 0, byte_to_get);	
		for (i = 0; i <  num_comm; i++) {
			if (command[i].pid > 0) {
				to_get[command[i].num].num = command[i].num;
				to_get[command[i].num].pid = command[i].pid;
				comm[command[i].num] = &command[i];
			} else {
				to_get[command[i].num].pid = 0;
				to_get[command[i].num].num = -1;
				comm[command[i].num] = NULL;
			}
		}
		get_threads_info(num_comm, to_get);
		printf("Got threads info\n");

		for (i = 0; i < num_comm; i++) {
			
			if (to_get[i].pid == 0) 
				continue;
			
			printf("INFO: command%02d %10s pid: %5d "
				   "slice: %8d %8d "
				   "fast_exec: %12lld slow_exec: %12lld\n",
				   i, comm[i]->name, comm[i]->pid,
				   comm[i]->round_slice.fast, comm[i]->round_slice.slow,
				   to_get[i].sum_fast_exec_runtime,
				   to_get[i].sum_slow_exec_runtime
				   );
		}
		print_commands(command, num_comm);
	
		fflush(stdout);
		fprintf(stderr, "update_speedup: called: %ld\n", ++num_called);
		fflush(stderr);
	}
	
	get_threads_info(1, &me);
	printf("Scheduling_time: %lld num_called: %ld\n", me.sum_fast_exec_runtime + me.sum_slow_exec_runtime, num_called);
		
	fflush(stdout); // fflush stdout once to reduce the overhead

	return (void *)num_called;
}
