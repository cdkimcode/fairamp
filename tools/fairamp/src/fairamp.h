#ifndef __FAIRAMP_H__
#define __FAIRAMP_H__

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sched.h>
#include <sys/types.h>

/* default value */
#define base_round_slice		30000000U /* 30ms */
#define base_fast_round_slice	       0U
#define base_slow_round_slice	30000000U
#ifndef minimal_round_slice
#define minimal_round_slice      1200000U /* 4% */
#endif

/* constants */
#define MAX_COMM_NAME_LEN 20
#define MAX_LINE_LEN 1024
#define NUM_CPU_TYPES 2

/* macro functions */
#define TIME_DIFF(B,E) ((E.tv_sec - B.tv_sec) + (E.tv_usec - B.tv_usec)*0.000001)
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define pr_err(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)

#ifdef VERBOSE
#define verbose(fmt, ...) printf(fmt, ##__VA_ARGS__)
#define verbose_err(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
#else
#define verbose(fmt, ...) do{}while(0)
#define verbose_err(fmt, ...) do{}while(0)
#endif

/* similar to strncpy(), but put a null-byte after the string.
 * the size of @dst should be equal to or larger than MIN(strlen(@src) + 1 or @max_len + 1)
 */
static inline void stringcopy(char *dst, const char *src, size_t max_len) {
	size_t len;
	for (len = 0; len < max_len && *src != '\0'; len++)
		*dst++ = *src++;
	*dst = '\0';
}

struct config {
int periodic_speedup_update;
int do_fairamp;
int adjust_frequency;
int fast_core_first;
int repeated_run;
} config;

struct timespec sched_interval;

/* data structures */
enum core_type { offline, slow_core, fast_core };
typedef unsigned long long u64;


struct round_slice {
		unsigned int fast;
		unsigned int slow;
};

struct fairamp_threads_info {
	int num;
	pid_t pid;
	long long insts_fast;
	long long insts_slow;
	unsigned long long sum_fast_exec_runtime;
	unsigned long long sum_slow_exec_runtime;
	int err;
};

struct fairamp_unit_vruntime {
	int num;
	pid_t pid;
	unsigned int unit_fast_vruntime;
	unsigned int unit_slow_vruntime;
};

struct command {
	int num; /* updated only by main thread. read only for update_speedup thread */
	pid_t pid; /* updated only by main thread. read only for update_speedup thread */
	pid_t pid_first; /* updated and used only by main thread. */
	char name[MAX_COMM_NAME_LEN]; /* [INFREQUENTLY] used by only main thread */
	char **argv; /* [INFREQUENTLY] used by only main thread */
	int num_threads; /* updated only by main thread. read only for update_speedup thread */
	struct timeval begin; /* [INFREQUENTLY] used by only main thread */
	struct timeval end; /* [INFREQUENTLY] used by only main thread */
	struct timeval __begin; /* [INFREQUENTLY] used by only main thread */
	struct timeval __end; /* [INFREQUENTLY] used by only main thread */
	int output; /* [INFREQUENTLY] used by only main thread */
	int status; /* [INFREQUENTLY] exit status. used by only main thread */
	int finished; /* [INFREQUENTLY] used by only main thread */
	/* variables only for update_speedup thread */
	float speedup; /* not used by main thread after create update_speedup thread */
	struct round_slice round_slice; /* used by only one of main or update_speedup thread */
	cpu_set_t cpumask;
};

/******************************************************/
/* Variable implemented in set_core.c                 */
/******************************************************/
int num_core;
enum core_type *core_type;
char *fast_core_frequency_str;
char *slow_core_frequency_str;
unsigned long fast_core_frequency;
unsigned long slow_core_frequency;
enum core_mode *default_core_mode;


/******************************************************/
/* Functions implemented in fairamp.c                 */
/******************************************************/
struct environment {
	int num_fast_core;
	int num_slow_core;
	float num_fast_core_f;
	int num_comm;
	struct command *command;
};
struct environment env;
int done;
int measuring_IPS_type_started;
void print_commands(struct command *command, int num_comm);

/******************************************************/
/* Functions implemented in set_core.c                */
/******************************************************/
int init_core_type();
int parse_core_config(char const *core_config, enum core_type *core_type);
void set_default_core_type(enum core_type *core_type);
void set_core_type(enum core_type *core_type, cpu_set_t *cpumask);
int print_core_type(const char *header, enum core_type *__core_type, FILE *fp);

/******************************************************/
/* Functions implemented in estimation.c              */
/******************************************************/
void *periodic_update_speedup(void *data);
void *periodic_show_stat(void *data);

/******************************************************/
/* Functions implemented in sched_policy.c            */
/******************************************************/
int init_set_round_slice(struct environment *env);
int set_sched_policy(char const *opt_name, char const *opt_value);
int is_sched_policy_speedup_aware();
int is_sched_policy_asymmetry_aware();
void list_predefined_policies(const char *header);
char *get_sched_policy_name();

int sort_by_speed_up(struct command *command, int num_comm);
void set_round_slice();
void set_round_slice_before_run();

/******************************************************/
/* Constants and data structures for sched_policy.c   */
/******************************************************/
typedef void (*set_round_slice_func)(void);

enum base {base_fair_share, base_slow_core, base_fast_core};
enum criteria {c_unaware, c_manual, c_max_perf, c_max_fair, c_minF, c_uniformity, c_minF_uniformity};
enum metric {m_fairness, m_throughput};

struct sched_policy {
	char *name;
	set_round_slice_func func; /* three types: unaware, manual, fair */
	set_round_slice_func set_max_fair_round_slice; /* different according to base */

	/* for unaware or manual policy, the belows are useless. */
	enum base base;
	enum criteria criteria;
	float throughput;
	float minF;
	float uniformity;
	float similarity;
};

struct thread {
	int idx; /* index for command */
	float speedup;
	struct round_slice round_slice;
};

/******************************************************/
/* Ftrace                                             */
/******************************************************/
int set_ftrace(const char *filename);
void start_ftrace();
void stop_ftrace();
void save_ftrace();
#endif /* __FAIRAMP_H__ */
