#include <assert.h>
#include <stdio.h>
#include "fairamp.h"
#include "syscall_wrapper.h"
#include <math.h>

/******************************************************/
/* Constants and data structures                      */
/******************************************************/
#define MAX_NAME_LEN 256

char *base_str[] = {"fair_share", "slow_core", "fast_core"};
char *criteria_str[] = {"unaware", "manual", "max_perf", "max_fair", "minF", "uniformity", "minF_uniformity"};
char *metric_str[] = {"fairness", "throughput"};

static void set_max_fair_round_slice_fair_share();
static void set_max_fair_round_slice_fast_core();
static void set_max_fair_round_slice_slow_core();

/******************************************************/
/* Global variables                                   */
/******************************************************/

/* the following four variables will not be changed after initialization.
 * Thus, init_set_round_slice takes these values as arguments and saves them. */
static int num_fast_core;
static int num_slow_core;
static struct command *command;
static int num_comm;
static float max_minF;

static struct sched_policy sched_policy;
static struct thread *threads;
static int num_threads;
static int num_active_threads;
static struct fairamp_unit_vruntime *unit_vruntime_info;

static float *perf_threads = NULL;
static float *perf_base = NULL;

static unsigned int *max_perf_fast_round_slice = NULL; /* used only when sched_policy.uniformity > 0 */
static unsigned int *max_perf_slow_round_slice = NULL; /* used only when sched_policy.uniformity > 0 */
static unsigned int *max_fair_fast_round_slice = NULL;
static unsigned int *max_fair_slow_round_slice = NULL;

/******************************************************/
/* Declarations of scheduling policy functions        */
/******************************************************/
void set_round_slice_unaware();
void set_round_slice_manual();
void set_round_slice_max_perf();
void set_round_slice_max_fair();
void set_round_slice_similarity();
void set_round_slice_minF();
void set_round_slice_uniformity();
void set_round_slice_fair();
void set_round_slice_all();

/******************************************************/
/* Predefined schedling policies                      */
/******************************************************/
struct sched_policy DEFAULT_SCHED_POLICY = {"max-fair", set_round_slice_max_fair, set_max_fair_round_slice_fair_share, base_fair_share, c_max_fair, 0, 1.0, 1.0, 0.0};

struct sched_policy predefined_policies[] = {
	{"unaware", set_round_slice_unaware, set_max_fair_round_slice_fair_share, base_fair_share, c_unaware, 0.0, 0.0, 0.0, 0.0},
	{"manual", set_round_slice_manual, set_max_fair_round_slice_fair_share, base_fair_share, c_manual, 0.0, 0.0, 0.0, 0.0},
	{"max_throughput", set_round_slice_max_perf, set_max_fair_round_slice_fair_share, base_fair_share, c_max_perf, 1.0, 0.0, 0.0, 0.0},
	{"max-perf", set_round_slice_max_perf, set_max_fair_round_slice_fair_share, base_fair_share, c_max_perf, 1.0, 0.0, 0.0, 0.0},
	{"complete_fair", set_round_slice_max_fair, set_max_fair_round_slice_fair_share, base_fair_share, c_max_fair, 0, 1.0, 1.0, 0.0},
	{"max-fair", set_round_slice_max_fair, set_max_fair_round_slice_fair_share, base_fair_share, c_max_fair, 0, 1.0, 1.0, 0.0},
	{NULL, },
};

/******************************************************/
/* External functions                                 */
/******************************************************/
inline int is_sched_policy_speedup_aware() {
	return sched_policy.criteria != c_unaware &&
			sched_policy.criteria != c_manual;
}

inline int is_sched_policy_asymmetry_aware() {
	return sched_policy.criteria != c_unaware;
}

inline char *get_sched_policy_name() {
	return sched_policy.name;
}

void list_predefined_policies(const char *header) {
	int i = 0;
	printf("%s", header);
	while (predefined_policies[i].name) {
		printf(" %s", predefined_policies[i].name);
		i++;
	}
	printf("\n");
}


/******************************************************/
/* Macros and helper functionses                      */
/******************************************************/
#define MIN2(A, B) ((A) < (B) ? (A) : (B))

/* return 1 if @str is a string representing a number */
static int is_num_str(char *__str) {
	char *str;	
	int dot_appeared = 0;
	int number_appeared = 0;

	if (!__str) return 0;

	str = __str;

	while (*str != '\0') {
		switch(*str) {
		case '+':
		case '-':
				/* can appear only at the first */
				if (str != __str)
					return 0;
				/* number_appeared = 0, always */
				break;
		case '.':
				if (dot_appeared)
					return 0;
				dot_appeared = 1;
				/* numbers should be followed after dot. */
				number_appeared = 0;
				break;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
				number_appeared = 1;
				break;
		default:
				return 0; /* other characters.... */
		}
		str++;
	}

	return number_appeared;
}

/* 
 * sort the command array by speed up.
 * finished tasks will be the end of the array. 
 * return the number of active tasks. 
 */
int sort_by_speed_up(struct command *command, int num_comm) 
{
	struct command temp;
	int i, j;
	int num_active;
	int max_idx;
	float max_speedup;

	i = 0; /* the last active task */
	j = num_comm; /* the first finished task */
	while (j > 0 && command[j - 1].pid == 0)
		j--;
	
	while (i < num_comm && command[i].pid != 0)
		i++;

	/* no active task. no need to sort. */
	if (j == 0)
		return 0;

	while (i < num_comm && i < j) {
		if (command[i].pid != 0) {
			i++;
			continue;
		}

		j--; /* one finished task is found */
		temp = command[i];
		command[i] = command[j];
		command[j] = temp;
	}

	num_active = i;

	/* now, all finished tasks are inserted at the end of the array. */

	if (!config.periodic_speedup_update) {
		/* common case: already sorted */
		for (i = 0; i < num_active - 1; i++)
			if (command[i].speedup < command[i+1].speedup)
				break;
		if (i == num_active - 1)
			return num_active;
	}

	/* insertion sort */
	for (i = 0; i < num_active; i++) {
		max_idx = i;
		max_speedup = command[i].speedup;
		for (j = i + 1; j < num_active; j++)
			if (max_speedup < command[j].speedup) {
				max_idx = j;
				max_speedup = command[j].speedup;
			}

		if (i != max_idx) {
			temp = command[i];
			command[i] = command[max_idx];
			command[max_idx] = temp;
		}
	}

	return num_active;
}

/******************************************************/
/* Functions for initialization                       */
/******************************************************/

/* (legacy) set a global variable @sched_policy based on @name */
int legacy_set_sched_policy(char const *__name) {
	char name[MAX_NAME_LEN];
	int name_len = 0;
	char *tok;
	int i, num_tokens;
	enum {NONE, SIMILARITY, MINF, UNIFORMITY, NUMBER} last_type = NONE;

	/* default policy */
	if (!__name) {
		sched_policy = DEFAULT_SCHED_POLICY;
		goto succeed;
	}

	name_len = strnlen(__name, MAX_NAME_LEN) + 1; /* include terminal null byte */
	if (name_len >= MAX_NAME_LEN) {
		fprintf(stderr, "%s: sched policy name is too long! -> %s\n", __func__, __name);
		return 0;
	}

	/* predefined_policies */
	i = 0;
	for (i = 0; predefined_policies[i].name; i++) {
		if (strncmp(__name, predefined_policies[i].name, MAX_LINE_LEN) == 0) {
			sched_policy = predefined_policies[i];
			goto succeed;
		}
	}

	/* parse and set */
	/* default values */
	sched_policy.name = (char *)malloc(name_len);
	strncpy(sched_policy.name, __name, name_len);
	sched_policy.similarity = 0;
	sched_policy.minF = 0;
	sched_policy.uniformity = 0;

	strncpy(name, __name, MAX_NAME_LEN);
	num_tokens = 0;
	tok = strtok(name, "_");
	while (tok) {
		num_tokens++;

		if (strncmp(tok, "Similarity", MAX_NAME_LEN) == 0 ||
			strncmp(tok, "similarity", MAX_NAME_LEN) == 0 ||
			strncmp(tok, "sim", MAX_NAME_LEN) == 0 ||
			strncmp(tok, "Sim", MAX_NAME_LEN) == 0) {
			last_type = SIMILARITY;
		} else if (strncmp(tok, "QoS", MAX_NAME_LEN) == 0 ||
			strncmp(tok, "MinF", MAX_NAME_LEN) == 0 ||
			strncmp(tok, "minF", MAX_NAME_LEN) == 0 ||
			strncmp(tok, "minf", MAX_NAME_LEN) == 0 ||
			strncmp(tok, "min", MAX_NAME_LEN) == 0) {
			last_type = MINF;
		} else if (strncmp(tok, "Uniformity", MAX_NAME_LEN) == 0 ||
			strncmp(tok, "uniformity", MAX_NAME_LEN) == 0 ||
			strncmp(tok, "Uni", MAX_NAME_LEN) == 0 ||
			strncmp(tok, "uni", MAX_NAME_LEN) == 0) {
			last_type = UNIFORMITY;
		} else if (is_num_str(tok)) {
			if (last_type == SIMILARITY)
				sched_policy.similarity = strtof(tok, NULL);
			else if (last_type == MINF)
				sched_policy.minF = strtof(tok, NULL);
			else if (last_type == UNIFORMITY)
				sched_policy.uniformity = strtof(tok, NULL);
			else /* NONE or NUMBER */
				goto wrong_policy_name;

			last_type = NUMBER;
		} else {
			goto wrong_policy_name;
		}
		
		tok = strtok(NULL, "_");
	}

	if (num_tokens == 0 || num_tokens % 2 > 0)
		goto wrong_policy_name;

	/* if percentage... */
	if (sched_policy.minF > 1) 
		sched_policy.minF /= 100;
	if (sched_policy.uniformity > 1)
		sched_policy.uniformity /= 100;

	/* pick up the fastest function */
	if (sched_policy.minF == 1.0 || sched_policy.uniformity == 1.0)
		sched_policy.func = set_round_slice_max_fair;
	else if (sched_policy.similarity == 0 &&
			sched_policy.minF == 0 &&
			sched_policy.uniformity == 0)
		sched_policy.func = set_round_slice_max_perf;
	/* from here, at least one of the values are not zero */
	else if (sched_policy.minF == 0 && sched_policy.uniformity == 0)
		sched_policy.func = set_round_slice_similarity;
	else if (sched_policy.similarity == 0 && sched_policy.uniformity == 0)
		sched_policy.func = set_round_slice_minF;
	else if (sched_policy.similarity == 0 && sched_policy.minF == 0)
		sched_policy.func = set_round_slice_uniformity;
	/* from here, at least two of the values are not zero */
	else
		sched_policy.func = set_round_slice_fair;

	/* this legacy function does not support setting following values.
	   just set them as defaults. */
	sched_policy.base = base_fair_share;
	sched_policy.throughput = 0;
	sched_policy.set_max_fair_round_slice = set_max_fair_round_slice_fair_share;

succeed:
	if (sched_policy.func == set_round_slice_unaware)
		printf("%s: unaware\n", __func__);
	else if (sched_policy.func == set_round_slice_manual)
		printf("%s: manual\n", __func__);
	else if (sched_policy.func == set_round_slice_max_perf)
		printf("%s: max-perf\n", __func__);
	else if (sched_policy.func == set_round_slice_max_fair)
		printf("%s: max-fair\n", __func__);
	else 
		printf("%s: fair-oriented similarity: %.2f minF: %.2f uniformity %.2f\n", __func__, 
				sched_policy.similarity, sched_policy.minF, sched_policy.uniformity);
	
	return 1;

wrong_policy_name:
	fprintf(stderr, "%s: Wrong sched policy name: %s\n", __func__, __name);
	return 0;
}

void set_sched_policy_name() {
#define MAX_POLICY_NAME_LEN 1024
	/* without error in policy */
	char name[MAX_POLICY_NAME_LEN];
	int len = 0;

	switch(sched_policy.criteria) {
	case c_unaware:
	case c_manual:
	case c_max_perf:
					snprintf(name, MAX_POLICY_NAME_LEN, "%s", criteria_str[sched_policy.criteria]);
	case c_max_fair:
					snprintf(name, MAX_POLICY_NAME_LEN, "%s base: %s", 
									criteria_str[sched_policy.criteria],
									base_str[sched_policy.base]);
					break;
	case c_minF:
	case c_uniformity:
					snprintf(name, MAX_POLICY_NAME_LEN, "%s base: %s %s_target: %.3f", 
									criteria_str[sched_policy.criteria],
									base_str[sched_policy.base],
									sched_policy.throughput != 0
										? "throughput"
										: sched_policy.minF != 0
											? "minF"
											: "uniformity",
									sched_policy.throughput == 0
										? sched_policy.minF + sched_policy.uniformity /* one of two is zero */
										: sched_policy.throughput);
					break;
	case c_minF_uniformity:
					snprintf(name, MAX_POLICY_NAME_LEN, "%s base: %s minF_target: %.3f uniformity_target: %.3f", 
									criteria_str[sched_policy.criteria],
									base_str[sched_policy.base],
									sched_policy.minF,
									sched_policy.uniformity);
					break;
	}

	if (sched_policy.similarity > 0) {
		len = strnlen(name, MAX_POLICY_NAME_LEN);
		snprintf(name + len, MAX_POLICY_NAME_LEN, " similarity: %.3f", sched_policy.similarity);
	}

	len = strnlen(name, MAX_POLICY_NAME_LEN);

	sched_policy.name = (char *) malloc(len + 1);
	stringcopy(sched_policy.name, name, len);
}

/* set a global variable @sched_policy based on @opt_name and @opt_value
 * if @opt_name != NULL, just remember the value.
 * if @opt_name == NULL, finally set @sched_policy. */
int set_sched_policy(char const *opt_name, char const *opt_value) {
	static int policy_given = 0, target1_given = 0, target2_given = 0, similarity_given = 0;
	static enum base base = base_fair_share;
	static enum criteria criteria = c_max_fair;
	static enum metric metric = m_fairness;
	static float target1 = -1, target2 = -1, similarity = -1;

	if (opt_name == NULL) {
		if (policy_given)
			return 1; /* nothing to do */

		goto finalize;

	} else if (strcmp(opt_name, "policy") == 0) {
		policy_given = 1;
		return legacy_set_sched_policy(opt_value);

	} else if (strcmp(opt_name, "base") == 0) {
		if (policy_given) goto policy_already_given;

		if (strcmp(opt_value, "fair_share") == 0
					|| strcmp(opt_value, "fair") == 0)
			base = base_fair_share;
		else if (strcmp(opt_value, "slow_core") == 0
					|| strcmp(opt_value, "slow") == 0)
			base = base_slow_core;
		else if (strcmp(opt_value, "fast_core") == 0
					|| strcmp(opt_value, "fast") == 0)
			base = base_fast_core;
		else {
			printf("[ERROR] unknown base: %s", opt_value);
			return -1;
		}

		return 1;

	} else if (strcmp(opt_name, "criteria") == 0) {
		if (policy_given) goto policy_already_given;

		if (strcmp(opt_value, "unaware") == 0
					|| strcmp(opt_value, "Unaware") == 0)
			criteria = c_unaware;
		else if (strcmp(opt_value, "manual") == 0
					|| strcmp(opt_value, "Manual") == 0)
			criteria = c_manual;
		else if (strcmp(opt_value, "max_perf") == 0
					|| strcmp(opt_value, "max-perf") == 0)
			criteria = c_max_perf;
		else if (strcmp(opt_value, "max_fair") == 0
					|| strcmp(opt_value, "max-fair") == 0)
			criteria = c_max_fair;
		else if (strcmp(opt_value, "minFairness") == 0
					|| strcmp(opt_value, "minfairness") == 0
					|| strcmp(opt_value, "minF") == 0)
			criteria = c_minF;
		else if (strcmp(opt_value, "uniformity") == 0
					|| strcmp(opt_value, "Uniformity") == 0
					|| strcmp(opt_value, "uni") == 0
					|| strcmp(opt_value, "Uni") == 0)
			criteria = c_uniformity;
		else if (strcmp(opt_value, "minFairness_uniformity") == 0
					|| strcmp(opt_value, "minFairness_Uniformity") == 0
					|| strcmp(opt_value, "minFairness_uniformity") == 0
					|| strcmp(opt_value, "minfairness_Uniformity") == 0
					|| strcmp(opt_value, "minfairness_uniformity") == 0
					|| strcmp(opt_value, "minF_Uniformity") == 0
					|| strcmp(opt_value, "minF_uniformity") == 0
					|| strcmp(opt_value, "minF_Uni") == 0
					|| strcmp(opt_value, "minF_uni") == 0)
			criteria = c_minF_uniformity;
		else {
			printf("[ERROR] unknown criteria: %s\n", opt_value);
			return -1;
		}

		return 1;

	} else if (strcmp(opt_name, "metric") == 0) {
		if (policy_given) goto policy_already_given;

		if (strcmp(opt_value, "fairness") == 0
					|| strcmp(opt_value, "Fairness") == 0
					|| strcmp(opt_value, "f") == 0
					|| strcmp(opt_value, "F") == 0)
			metric = m_fairness;
		else if (strcmp(opt_value, "throughput") == 0
					|| strcmp(opt_value, "Throughput") == 0
					|| strcmp(opt_value, "t") == 0
					|| strcmp(opt_value, "T") == 0)
			metric = m_throughput;
		else {
			printf("[ERROR] unknown metric: %s\n", opt_value);
			return -1;
		}

		return 1;

	} else if (strcmp(opt_name, "target") == 0) {
		char target1_str[256];
		char target2_str[256];
		char *str;
		int i;
		if (policy_given) goto policy_already_given;

		target2_str[0] = '\0';
		str = target1_str;

		i = 0;
		while (opt_value[i] != '\0') {
			if (opt_value[i] == '_') {
				*str = '\0';
				str = target2_str;
			} else
				*(str++) = opt_value[i];
			i++;
		}
		*str = '\0';

		/* note that the value is percentage. */
		target1 = (float) (atof(target1_str) / 100);
		target1_given = 1;
		if (target2_str[0] != '\0') {
			target2 = (float) (atof(target2_str) / 100);
			target2_given = 1;
		}

		return 1;

	} else if (strcmp(opt_name, "similarity") == 0) {
		if (policy_given) goto policy_already_given;

		similarity = (float) atof(opt_value);
		similarity_given = 1;

		return 1;

	} else {
		return 0; /* not an option for scheduling policy */
	}

finalize:

	/* error cases */
	if (criteria == c_minF_uniformity && metric == m_throughput) {
		printf("[ERROR] throughput metric is not supported with minF_uniformity criteria.\n");
		return 0;
	}
	if ((criteria == c_minF || criteria == c_uniformity || criteria == c_minF_uniformity)
			&& !target1_given) {
		printf("[ERROR] at least one target should be given for criteria minF, uniformity, or minF_uniformity\n");
		return 0;
	}
	if ((criteria == c_unaware || criteria == c_manual || criteria == c_max_fair)
			&& similarity_given) {
		printf("[ERROR] similarity is meaningless with criteria unaware, manual, and max_fair\n");
		return 0;
	}
	if ((target1_given && target1 < 0) || (target2_given && target2 < 0) ) {
		printf("[ERROR] targets should be larger than or equal to 0.\n");
		return 0;
	}
	if (similarity_given && similarity < 0) {
		printf("[ERROR] similarity should be larger than or equal to 0.\n");
		return 0;
	}

	/* setting */
	sched_policy.base = base;
	if (base == base_fair_share)
		sched_policy.set_max_fair_round_slice = set_max_fair_round_slice_fair_share;
	else if (base == base_fast_core)
		sched_policy.set_max_fair_round_slice = set_max_fair_round_slice_fast_core;
	else if (base == base_slow_core)
		sched_policy.set_max_fair_round_slice = set_max_fair_round_slice_slow_core;
	else {
		printf("[WARNING] base is weird. Set it as default: fair_share\n");
		sched_policy.set_max_fair_round_slice = set_max_fair_round_slice_fair_share;
	}

	sched_policy.criteria = criteria;

	if (criteria == c_minF && metric == m_fairness) {
		sched_policy.throughput = 0;
		sched_policy.minF = target1;
		sched_policy.uniformity = 0;
	} else if (criteria == c_minF && metric == m_throughput) {
		sched_policy.throughput = target1;
		sched_policy.minF = 0;
		sched_policy.uniformity = 0;
	} else if (criteria == c_uniformity && metric == m_fairness) {
		sched_policy.throughput = 0;
		sched_policy.minF = 0;
		sched_policy.uniformity = target1;
	} else if (criteria == c_uniformity && metric == m_throughput) {
		sched_policy.throughput = target1;
		sched_policy.minF = 0;
		sched_policy.uniformity = 0;
	} else if (criteria == c_minF_uniformity && metric == m_fairness) {
		if (!target2_given) {
			printf("[ERROR] the second target should be given when criteria is minF_uniformity.\n");
			return 0;
		}
		sched_policy.throughput = 0;
		sched_policy.minF = target1;
		sched_policy.uniformity = target2;
	} else if (criteria == c_max_perf) {
		sched_policy.throughput = 1;
		sched_policy.minF = 0;
		sched_policy.uniformity = 0;
	} else if (criteria == c_max_fair) {
		sched_policy.throughput = 0;
		sched_policy.minF = 1;
		sched_policy.uniformity = 1;
	} else { /* unaware or manual */
		sched_policy.throughput = 0;
		sched_policy.minF = 0;
		sched_policy.uniformity = 0;
	}
	
	if (sched_policy.uniformity > 1.0) {
		printf("[WARNING] uniformity target > 1.0 is meaningless. Cut the value as 1.0.\n");
		sched_policy.uniformity = 1.0;
	}

	if (similarity_given)
		sched_policy.similarity = similarity;

	/* build fast path */
	switch(sched_policy.criteria) {
	case c_unaware:
					sched_policy.func = set_round_slice_unaware;
					break;
	case c_manual:
					sched_policy.func = set_round_slice_manual;
					break;
	case c_max_perf:
					if (sched_policy.similarity > 0)
						sched_policy.func = set_round_slice_similarity;
					else
						sched_policy.func = set_round_slice_max_perf;
					break;
	case c_max_fair:
					sched_policy.func = set_round_slice_max_fair;
					break;
	case c_minF:
	case c_uniformity:
	case c_minF_uniformity:
					/* TODO: build various fast paths */
					sched_policy.func = set_round_slice_all;
					break;
	}
	
	/* set sched_policy name */
	set_sched_policy_name();

	return 1;

policy_already_given:
	printf("[WARNING] sched_policy is already given. %s=%s will be ignored.\n",
			opt_name, opt_value);
	return 1; /* since @opt_name is proper */ 
}

/* initialize some variables */
/* num_*_core can be read from global variables. But, to prevent error... */
int init_set_round_slice(struct environment *env) {
	int i;

	/* save to our static variables */
	command = env->command;
	num_comm = env->num_comm;
	num_fast_core = env->num_fast_core;
	num_slow_core = env->num_slow_core;

	for (i = 0; i < num_comm; i++) {
		num_threads += command[i].num_threads;
	}

	if (num_threads < num_comm) {
		fprintf(stderr, "error: num_threads(%d) < num_comm(%d)\n",
					num_threads, num_comm);
		return -1;
	}
	
	unit_vruntime_info = (struct fairamp_unit_vruntime *) calloc(num_comm, sizeof(struct fairamp_unit_vruntime));
	threads = (struct thread *) calloc(num_threads, sizeof(struct thread));
	perf_threads = (float *) calloc(num_threads, sizeof(float));
	perf_base = (float *) calloc(num_threads, sizeof(float));

	if (sched_policy.uniformity > 0) {
		/* used only in uniformity_fair */
		max_perf_fast_round_slice = (unsigned int *) calloc(num_threads, sizeof(unsigned int));
		max_perf_slow_round_slice = (unsigned int *) calloc(num_threads, sizeof(unsigned int));
	}
		
	max_fair_fast_round_slice = (unsigned int *) calloc(num_threads, sizeof(unsigned int));
	max_fair_slow_round_slice = (unsigned int *) calloc(num_threads, sizeof(unsigned int));

	if (!unit_vruntime_info || !threads || !perf_threads || !perf_base
			|| (sched_policy.uniformity > 0 &&
					(!max_perf_fast_round_slice || !max_perf_slow_round_slice))
			|| (!max_fair_fast_round_slice || !max_fair_slow_round_slice) ) {
		fprintf(stderr, "error: memory allocation failed! (3)\n");
		return -1;
	}
	return 0;
}

/******************************************************/
/* Main function of the scheduling policy             */
/******************************************************/

/* 1. Convert command[] to threads[] according the number of threads of each app.
 * 2. Call the schedling policy function according to the setting by set_sched_policy().
 * 3. Guarantee the minimal round slice for sampling 
 * 4. Convert threads[] to command[].
 * 5. Syscall to set the round_slice values of the task struct in kernel. */

static inline void __command_to_threads() { /* do 1 */
	int i, j;
	int num_active;
	
	num_active = sort_by_speed_up(command, num_comm);

	/* construct @thread */
	num_active_threads = 0;
	for (i = 0; i < num_active; i++) {
		for (j = 0; j < command[i].num_threads; j++) {
			threads[num_active_threads].idx = i;
			threads[num_active_threads].speedup = command[i].speedup;
			//thread[num_active_threads].round_slice = command[i].round_slice;
			num_active_threads++;
		}
		command[i].round_slice.fast = 0;
		command[i].round_slice.slow = 0;
	}
}

static inline void __guarantee_minimal_round_slice() { /* do 3 */
	int i;
	/* variables for minimal round slice */
	int steal_fast_round_slice = 0;
	int steal_slow_round_slice = 0;
	unsigned int donor_fast_round_slice = 0;
	unsigned int donor_slow_round_slice = 0;
	unsigned int amount = 0;
	
	if (!is_sched_policy_speedup_aware() ||
			!config.periodic_speedup_update)
		return;
	
	/* guarantee minimal round slice for each command */
	for (i = 0; i < num_active_threads; i++) {
		//fprintf(stderr, "before_guarantee: thread: %d fast_round: %12d slow_round: %12d", i , threads[i].round_slice.fast, threads[i].round_slice.slow);
		if (threads[i].round_slice.fast < minimal_round_slice) {
			amount = minimal_round_slice - threads[i].round_slice.fast;
			threads[i].round_slice.fast += amount;	
			threads[i].round_slice.slow -= amount;	
			
			steal_fast_round_slice += amount;
			steal_slow_round_slice -= amount;
			//fprintf(stderr, " refill_fast: %12d steal_fast: %21d steal_slow: %21d\n", amount, steal_fast_round_slice, steal_slow_round_slice);
		} else if (threads[i].round_slice.slow < minimal_round_slice) {
			amount = minimal_round_slice - threads[i].round_slice.slow;
			threads[i].round_slice.fast -= amount;	
			threads[i].round_slice.slow += amount;	
			
			steal_fast_round_slice -= amount;
			steal_slow_round_slice += amount;
			//fprintf(stderr, " refill_slow: %12d steal_fast: %21d steal_slow: %21d\n", amount, steal_fast_round_slice, steal_slow_round_slice);
		} 
	}


	if (steal_fast_round_slice > 0) {
		for (i = 0; i < num_active_threads; i++) {
			if (threads[i].round_slice.fast > minimal_round_slice)
				donor_fast_round_slice += threads[i].round_slice.fast - minimal_round_slice;
		}
		
		//fprintf(stderr, "steal_fast_round_slice: %12d donor_fast_round_slice: %12d\n", steal_fast_round_slice, donor_fast_round_slice);
		assert(steal_fast_round_slice < (int) donor_fast_round_slice);
	
		donor_fast_round_slice /= 1000; /* scaling for division and preventing overflow */
		for (i = 0; i < num_active_threads; i++) {
			if (threads[i].round_slice.fast > minimal_round_slice) {
				amount = (threads[i].round_slice.fast - minimal_round_slice) / donor_fast_round_slice ;
				amount = (unsigned int) (steal_fast_round_slice * amount) / 1000;
				//fprintf(stderr, "donation_of_fast: thread: %d amount: %d = %d * (%d - %d) / %d\n", i, amount, steal_fast_round_slice, threads[i].round_slice.fast, minimal_round_slice, donor_fast_round_slice);
				threads[i].round_slice.fast -= amount; 
				threads[i].round_slice.slow += amount;
			}
		}
	} else if (steal_slow_round_slice > 0) {
		for (i = 0; i < num_active_threads; i++) {
			if (threads[i].round_slice.slow > minimal_round_slice)
				donor_slow_round_slice += threads[i].round_slice.slow - minimal_round_slice;
		}
	
		//fprintf(stderr, "steal_slow_round_slice: %12d donor_slow_round_slice: %12d\n", steal_slow_round_slice, donor_slow_round_slice);
		assert(steal_slow_round_slice < (int) donor_slow_round_slice);
		
		donor_slow_round_slice /= 1000; /* scaling for division and preventing overflow */
		for (i = 0; i < num_active_threads; i++) {
			if (threads[i].round_slice.slow > minimal_round_slice) {
				amount = (threads[i].round_slice.slow - minimal_round_slice) / donor_slow_round_slice;
				amount = (unsigned int) (steal_slow_round_slice * amount) / 1000;
				//fprintf(stderr, "donation_of_slow: thread: %d amount: %d = %d * (%d - %d) / %d\n", i, amount, steal_slow_round_slice, threads[i].round_slice.slow, minimal_round_slice, donor_slow_round_slice);
				threads[i].round_slice.fast += amount; 
				threads[i].round_slice.slow -= amount;
			}
		}
	}
}

static inline void __threads_to_command() { /* do 4 */
	int i, j;
	for (i = 0; i < num_threads; i++) {
		j = threads[i].idx;
		command[j].round_slice.fast += threads[i].round_slice.fast;
		command[j].round_slice.slow += threads[i].round_slice.slow;
	}
	
	for (i = 0; i < num_comm; i++) {
		command[i].round_slice.fast /= command[i].num_threads;
		command[i].round_slice.slow /= command[i].num_threads;
		unit_vruntime_info[i].num = command[i].num;
		unit_vruntime_info[i].pid = command[i].pid;
		unit_vruntime_info[i].unit_fast_vruntime = command[i].round_slice.fast; /* XXX */
		unit_vruntime_info[i].unit_slow_vruntime = command[i].round_slice.slow; /* XXX */
	}
}

static inline void __set_round_slice_before_run() {
	__command_to_threads();
	sched_policy.func(); /* parameters are passed as global variables */

	__guarantee_minimal_round_slice();
	__threads_to_command();
}

void set_round_slice() {
	__set_round_slice_before_run();
	set_unit_vruntime(num_comm, unit_vruntime_info);
}

void set_round_slice_before_run() {
	int i;
	/* assume that all commands are active.
	   This is required for static mode. */
	for (i = 0; i < num_comm; i++)
		/* note that sort_by_speedup() distinguishes active commands with pid == 0 */
		if (command[i].pid == 0)
			command[i].pid = -10; 

	__set_round_slice_before_run();

	for (i = 0; i < num_comm; i++)
		if (command[i].pid == - 10)
			command[i].pid = 0; 
}

/******************************************************/
/* Calculating metrics                                */
/******************************************************/
static inline void print_perf(const char *name, float *perf) {
	int i;
	printf("%s: name: %s num_active_threads: %d\n", __func__, name, num_active_threads);
	for (i = 0; i < num_active_threads; i++)
		printf("%s: %s[%d] %.3f\n", __func__, name, i, perf[i]);
}

static inline void print_perf_one_line(const char *name, float *perf) {
	int i;
	printf("%s:", name);
	for (i = 0; i < num_active_threads; i++)
		printf(" %.3f", perf[i]);
	printf("\n");
}

static inline void calculate_perf(float *perf) {
	int i;
	for (i = 0; i < num_active_threads; i++)
		perf[i] = threads[i].speedup * threads[i].round_slice.fast + threads[i].round_slice.slow;
}

static inline float calculate_throughput(float *perf, float *perf_base) {
	float throughput = 0.0;
	int i;
	for (i = 0; i < num_active_threads; i++)
		throughput += perf[i] / perf_base[i];
	return num_active_threads ? throughput / num_active_threads : 0;
}

static inline float calculate_minF(float *perf_threads, float *perf_base) {
	int i;
	float fairness;
	if (num_active_threads == 0)
		return 1.0;

	fairness = perf_threads[0] / perf_base[0];
	for (i = 1; i < num_active_threads; i++)
		fairness = MIN2(fairness, perf_threads[i] / perf_base[i]);
	/* the final value may exceeds 1.0 if base is slow core */
	return fairness;
}

static inline float calculate_uniformity(float *perf_threads, float *perf_base)
{
	int i;
	float throughput, avg = 0.0, square_avg = 0.0;
	if (!num_active_threads)
		return 1;
	
	for (i = 0; i < num_active_threads; i++) {
		throughput = perf_threads[i] / perf_base[i];
		avg += throughput;
		square_avg += throughput*throughput;
	}

	avg /= num_active_threads;
	square_avg /= num_active_threads;

	/* to solve the floating number accuracy problem */
	return (square_avg > avg * avg) ? 1 - sqrtf(square_avg - avg * avg) / avg
									: 1;
}

/******************************************************/
/* max-fair functions with various bases              */
/******************************************************/
/* max-fair functions should calculate perf_base */
static void set_max_fair_round_slice_fair_share() {
	int i;
	unsigned int fast_round_slice;
	unsigned int slow_round_slice;
	
	/* Note that fast-core-first is not used in static modes,
	 * and static modes call this function very infrequently. */
	if (unlikely(!config.fast_core_first)) {
		fast_round_slice = base_round_slice * num_fast_core / (num_fast_core + num_slow_core);
		slow_round_slice = base_round_slice * num_slow_core / (num_fast_core + num_slow_core);
	}
	/* fast core first */
	else if (num_active_threads < num_fast_core) {
		fast_round_slice = base_round_slice;
		slow_round_slice = 0UL;
	} else if (num_active_threads < num_fast_core + num_slow_core) {
		fast_round_slice = base_round_slice * num_fast_core / num_active_threads;
		slow_round_slice = base_round_slice - fast_round_slice;
	} else { /* normal case */
		fast_round_slice = base_round_slice * num_fast_core / (num_fast_core + num_slow_core);
		slow_round_slice = base_round_slice * num_slow_core / (num_fast_core + num_slow_core);
	}

	for (i = 0; i < num_active_threads; i++) {
		max_fair_fast_round_slice[i] = fast_round_slice;
		max_fair_slow_round_slice[i] = slow_round_slice;
		perf_base[i] = threads[i].speedup * fast_round_slice + slow_round_slice;
	}

	max_minF = 1.0;
}

static void set_max_fair_round_slice_slow_core() {
	int i;
	int max_minF_calculate = 0, force_retry;
	int num_fast_only = 0;
	int num_small_speedup = 0;
	int total_fast_round_slice;
	float Hsum = 0;
	float speedup[num_active_threads], H[num_active_threads];

	for (i = 0; i < num_active_threads; i++) {
		speedup[i] = threads[i].speedup;
		H[i] = 1.0 / (threads[i].speedup - 1.0);
		perf_base[i] = base_round_slice;
	}

retry:
	num_small_speedup = 0;
	num_fast_only = 0;
	force_retry = 0;
	Hsum = 0;
	
	/* calculate Hsum */
	for (i = 0; i < num_active_threads; i++) {
		if (speedup[i] > 1)
			Hsum += H[i];
		else if (speedup[i] < 0)
			num_fast_only++;
		else
			num_small_speedup++;
	}

	max_minF = (float) num_fast_core / Hsum + 1;

	/* num_fast_only >= num_fast_core implies  num_small_speedup <= num_slow_core,
		since num_threads == num_fast_core + num_slow_core */
	num_small_speedup = num_small_speedup > num_slow_core
							? num_small_speedup - num_slow_core 
							: 0;
	total_fast_round_slice = (num_fast_core - num_fast_only - num_small_speedup) * base_round_slice;

	if (unlikely(total_fast_round_slice <= 0)) {
		/* corner case: fair share for fast only threads,
						fast only if num_small_speedup - num_slow_core > 0,
						slow only for other threads */
		for (i = 0; i < num_active_threads; i++) {
			if (speedup[i] < 0) {
				max_fair_fast_round_slice[i] = num_fast_only < num_fast_core 
												? base_round_slice
												: base_round_slice * num_fast_core / num_fast_only;
				max_fair_slow_round_slice[i] = base_round_slice - max_fair_fast_round_slice[i];
			} else if (speedup[i] <= 1.0 && num_small_speedup > 0) {
				max_fair_fast_round_slice[i] = base_round_slice;
				max_fair_slow_round_slice[i] = 0;
				num_small_speedup--;
			} else {
				max_fair_fast_round_slice[i] = 0;
				max_fair_slow_round_slice[i] = base_round_slice;
			}
		}
		goto out; /* return; */ 
	}
	
	for (i = 0; i < num_active_threads; i++) {
		if (speedup[i] < 0) {
			max_fair_fast_round_slice[i] = base_round_slice;
			max_fair_slow_round_slice[i] = 0;
		} else if (speedup[i] <= 1) {
			max_fair_fast_round_slice[i] = num_small_speedup > 0 ? base_round_slice : 0;
			max_fair_slow_round_slice[i] = num_small_speedup > 0 ? 0 : base_round_slice;
			num_small_speedup--;
		} else {
			/* f_i = {1 / (e_i - 1)} / {Sum_j (1 / (e_j))} * F */
			max_fair_fast_round_slice[i] = (float) total_fast_round_slice * H[i] / Hsum;
			max_fair_slow_round_slice[i] = base_round_slice - max_fair_fast_round_slice[i];
			
			if (unlikely(max_fair_fast_round_slice[i] > base_round_slice)) {
				force_retry = 1;
				max_minF_calculate = 1;
				speedup[i] = -1.0;
			} 
		}
	}
	
out:

	if (unlikely(force_retry))
		goto retry;

	/* re-calculate max_minF for corner cases */
	if (max_minF_calculate) {
		float perf;
		for (i = 0; i < num_active_threads; i++) {
			if (speedup[i] == -1.0) {
				perf = threads[i].speedup * threads[i].round_slice.fast + threads[i].round_slice.slow;
				if (perf / perf_base[i] < max_minF)
					max_minF = perf / perf_base[i];
			}
		}
	}

	printf("%s: max_minF: %.2f Hsum: %6.4f\n", __func__, max_minF, Hsum);
}

static void set_max_fair_round_slice_fast_core() {
	int i;
	int num_small_speedup, num_fast_only, force_retry, max_minF_calculate = 0;
	int total_fast_round_slice;
	unsigned int temp;
	float Hsum, Msum;
	float speedup[num_active_threads], H[num_active_threads], M[num_active_threads];

	for (i = 0; i < num_active_threads; i++) {
		speedup[i] = threads[i].speedup;
		H[i] = 1.0 / (threads[i].speedup - 1.0);
		M[i] = threads[i].speedup / (threads[i].speedup - 1.0);
		perf_base[i] = threads[i].speedup * base_round_slice;
	}

retry:
	num_small_speedup = 0;
	num_fast_only = 0;
	force_retry = 0;
	Hsum = 0;
	Msum = 0;
	/* calculate Hsum */
	for (i = 0; i < num_active_threads; i++) {
		if (speedup[i] > 1) {
			Hsum += H[i];
			Msum += M[i];
		} else if (speedup[i] < 0)
			num_fast_only++;
		else
			num_small_speedup++;
	}
	
	max_minF = (num_fast_core + Hsum) / Msum;

	/* num_fast_only >= num_fast_core implies  num_small_speedup <= num_slow_core,
		since num_threads == num_fast_core + num_slow_core */
	num_small_speedup = num_small_speedup > num_slow_core
							? num_small_speedup - num_slow_core 
							: 0;
	total_fast_round_slice = (num_fast_core - num_fast_only - num_small_speedup) * base_round_slice;

	if (unlikely(total_fast_round_slice <= 0)) {
		/* corner case: fair share for fast only threads,
						fast only if num_small_speedup - num_slow_core > 0,
						slow only for other threads */
		for (i = 0; i < num_active_threads; i++) {
			if (speedup[i] < 0) {
				max_fair_fast_round_slice[i] = num_fast_only < num_fast_core 
												? base_round_slice
												: base_round_slice * num_fast_core / num_fast_only;
				max_fair_slow_round_slice[i] = base_round_slice - max_fair_fast_round_slice[i];
			} else if (speedup[i] <= 1.0 && num_small_speedup > 0) {
				max_fair_fast_round_slice[i] = base_round_slice;
				max_fair_slow_round_slice[i] = 0;
				num_small_speedup--;
			} else {
				max_fair_fast_round_slice[i] = 0;
				max_fair_slow_round_slice[i] = base_round_slice;
			}
		}
		goto out; /* return; */ 
	}
	
	for (i = 0; i < num_active_threads; i++) {
		if (speedup[i] < 0) {
			max_fair_fast_round_slice[i] = base_round_slice;
			max_fair_slow_round_slice[i] = 0;
		} else if (speedup[i] <= 1.0) {
			max_fair_fast_round_slice[i] = num_small_speedup > 0 ? base_round_slice : 0;
			max_fair_slow_round_slice[i] = num_small_speedup > 0 ? 0 : base_round_slice;
			num_small_speedup--;
		} else {
			/* H_i = 1 / (e_i - 1.0)
			   M_i = e_i / (e_i - 1.0)
			   f_i = M_i / {Sum_j M_j} * F - H_i + M_i / {Sum_j M_j} * {Sum_j H_j} */
			/* max_fair_fast_round_slice[i] = ((float) total_fast_round_slice) * M[i] / Msum 
											- ((float) base_round_slice) * H[i] 
											+ ((float) base_round_slice) * M[i] * Hsum / Msum;
			*/	
			max_fair_fast_round_slice[i] = ((float) total_fast_round_slice) * M[i] / Msum 
											+ ((float) base_round_slice) * M[i] * Hsum / Msum;
			temp = ((float) base_round_slice) * H[i];
			if (likely(max_fair_fast_round_slice[i] > temp)) {
				max_fair_fast_round_slice[i] -= temp;
				
				if (unlikely(max_fair_fast_round_slice[i] > base_round_slice)) {
					force_retry = 1;
					max_minF_calculate = 1;
					speedup[i] = -1.0;
				} 
			} else {
				max_fair_fast_round_slice[i] = 0;
				force_retry = 1;
				max_minF_calculate = 1;
				speedup[i] = 1.0; /* small speedup */
			}


			max_fair_slow_round_slice[i] = base_round_slice - max_fair_fast_round_slice[i];
		}
	}

out:

	if (unlikely(force_retry))
		goto retry;

	/* re-calculate max_minF for corner cases */
	if (max_minF_calculate) {
		float perf;
		for (i = 0; i < num_active_threads; i++) {
			if (speedup[i] <= 1.0) {
				perf = threads[i].speedup * threads[i].round_slice.fast + threads[i].round_slice.slow;
				if (perf / perf_base[i] < max_minF)
					max_minF = perf / perf_base[i];
			}
		}
	}

	/* debug */
	printf("%s: max_minF: %.2f\n", __func__, max_minF);
	printf("%s: Hsum: %6.4f Msum: %6.4f num_small: %2d total_fast: %16d\n", __func__, Hsum, Msum, num_small_speedup, total_fast_round_slice);
}



/******************************************************/
/* Core function for the scheduling policy            */
/* (All of these are inline functions for speed.)     */
/******************************************************/
static inline void __set_round_slice_unaware() {
	int i;
	
	for (i = 0; i < num_active_threads; i++) {
		threads[i].round_slice.fast = 0ULL;
		threads[i].round_slice.slow = base_round_slice;
	}
}

static inline void __set_round_slice_manual() {
	int i;
	for (i = 0; i < num_active_threads; i++) {
		threads[i].round_slice.fast = base_round_slice * threads[i].speedup;
		threads[i].round_slice.slow = base_round_slice - threads[i].round_slice.fast;
	}
}

static inline void __set_round_slice_max_fair_set() {
	int i;
	for (i = 0; i < num_active_threads; i++) {
		threads[i].round_slice.fast = max_fair_fast_round_slice[i];
		threads[i].round_slice.slow = max_fair_slow_round_slice[i];
	}
}

static inline void __set_round_slice_max_fair() {
	sched_policy.set_max_fair_round_slice();
	__set_round_slice_max_fair_set();
}

static inline void __set_round_slice_max_perf() {
	int i;

	for (i = 0; i < num_active_threads && i < num_fast_core; i++) {
		threads[i].round_slice.fast = base_round_slice;
		threads[i].round_slice.slow = 0UL;
	}
	
	for ( ; i < num_active_threads; i++) {
		threads[i].round_slice.fast = 0UL;
		threads[i].round_slice.slow = base_round_slice;
	}
}

/* perf_base must be set before call this function */
static inline void __set_round_slice_minF() {
	int amount;
	int i;
	int remaining_fast_round_slice = num_fast_core * base_round_slice;

	if (unlikely(sched_policy.minF >= max_minF)) {
		printf("%s: minF(%.2f) >= max_minF(%.2f)\n", __func__, sched_policy.minF, max_minF);
		//__set_round_slice_max_fair(); /* always, __set_round_slice_max_fair() is called right before */
		return;
	}

	for (i = 0; i < num_active_threads; i++) {
		/* command[i].speedup * amount + (base_round_slice - amount) >= QoS * perf_base[i]
		 * (command[i].speedup - 1) * amount + base_round_slice >= QoS * perf_base[i]
		 * amount >= (QoS * perf_base[i] - base_round_slice ) / (command[i].speedup - 1)
		 */

		amount = (int) (sched_policy.minF * perf_base[i] - base_round_slice) / (threads[i].speedup - 1);
		amount = amount >= 0 ?  amount : 0;

		remaining_fast_round_slice -= amount;

		threads[i].round_slice.fast = amount;
		threads[i].round_slice.slow = base_round_slice - amount;
	}

	/* Note that the application list is sorted by speedup */
	for (i = 0; i < num_active_threads; i++) {
		if (remaining_fast_round_slice <= 0)
			break;

		amount = MIN2(base_round_slice - threads[i].round_slice.fast, remaining_fast_round_slice);
		threads[i].round_slice.fast += amount;
		threads[i].round_slice.slow -= amount;
		remaining_fast_round_slice -= amount;
	}
}

/* perf_fair must be set before call this function */
/* threads[*].round_slice.* must be set before calling this function */
static inline void __set_round_slice_similarity() {
	unsigned int total_fast_round_slice;
	unsigned int total_slow_round_slice;
	int start, i = 0, num;

	while (i < num_active_threads) {
		if (threads[i].round_slice.fast > max_fair_fast_round_slice[i]) {
			start = i++;
			total_fast_round_slice = threads[start].round_slice.fast;
			total_slow_round_slice = threads[start].round_slice.slow;

			while (i < num_active_threads &&
					threads[start].speedup - threads[i].speedup <= sched_policy.similarity) {
				total_fast_round_slice += threads[i].round_slice.fast;
				total_slow_round_slice += threads[i].round_slice.slow;
				i++;
			}

			num = i - start;

			/* ignore the very small remainder */
			for (; start < i; start++) {
				threads[start].round_slice.fast = total_fast_round_slice / num;
				threads[start].round_slice.slow = total_slow_round_slice / num;
			}
		} else
			i++;
	}
}

/* threads[*].round_slice.* must be set before calling this function */
static inline void __set_round_slice_uniformity() {
	int i = 0;
	float uniformity, uniformity_init;
	int alpha = 100; /* 100 * ratio of time the scheduler runs following max-perf policy */
	int alpha_init = -1; /* -1 means uniformity_init > target_uniformity */
	
	calculate_perf(perf_threads);
	uniformity = calculate_uniformity(perf_threads, perf_base);
	uniformity_init = uniformity;
	/* uniformity gained from max-perf exceeds the target */
	if (uniformity >= sched_policy.uniformity)
		goto uniformity_done;

	/* sched_policy.uniformity <= 1.0 here. (see set_sched_policy())
	 * Thus, uniformity < 1.0 also. see the above if-statement. */
	alpha = (int)((1.0 - sched_policy.uniformity) / (1.0 - uniformity) * 100);
	alpha_init = alpha;
	for (i = 0; i < num_active_threads; i++) {
		max_perf_fast_round_slice[i] = threads[i].round_slice.fast;
		max_perf_slow_round_slice[i] = threads[i].round_slice.slow;
	}
	
	while (uniformity < sched_policy.uniformity && alpha >= 0) {
		for (i = 0; i < num_active_threads; i++) {
			/* XXX: If base_round_slice > 30000000, worry about overflow here. */
			threads[i].round_slice.fast = max_perf_fast_round_slice[i] * alpha 
											+ max_fair_fast_round_slice[i] * (100 - alpha);
			threads[i].round_slice.fast = (threads[i].round_slice.fast + 50) / 100; /* scale and round up */
			threads[i].round_slice.slow = base_round_slice - threads[i].round_slice.fast;
		}

		calculate_perf(perf_threads);
		uniformity = calculate_uniformity(perf_threads, perf_base);
		//printf("%s: uniformity alpha=%4lld uniformity=%.3f target=%.3f\n", __func__, alpha, uniformity, sched_policy.uniformity);
		if (uniformity >= sched_policy.uniformity)
			break;
		alpha -= 1; /* since alpha is rough estimation, search the appropriate value */
	}

	if (alpha < 0) /* this means overflow */
		__set_round_slice_max_fair();

uniformity_done:
	printf("%s: alpha: %3d alpha_init: %3d uniformity: %.3f uniformity_init: %.3f\n", 
			__func__, alpha, alpha_init, uniformity, uniformity_init);
}

/* perf_base must be set before call this function */
static inline void __set_round_slice_minF_thru() {
	float target_throughput; /* absolute value of the target throughput. */
	float minF_upper; /* upper bound of minF: does not achieves the target throughput. */
	float minF_lower; /* lower bound of minF: achieves the target throughput. */
	float throughput; /* temp */
	unsigned int num_called = 0;

	/* MAX_FAIR */
	/* note that __set_round_slice_max_fair() is called right before. */ 
	calculate_perf(perf_threads);	
	throughput = calculate_throughput(perf_threads, perf_base); /* lower bound of throughput */
	minF_upper = max_minF; /* max_minF was calculated in __set_round_slice_max_fair() */
	if (unlikely(sched_policy.throughput == 0.0)) return;

	/* MAX_PERF */
	__set_round_slice_max_perf();
	calculate_perf(perf_threads);
	target_throughput = calculate_throughput(perf_threads, perf_base); /* upper bound of throughput */
	minF_lower = calculate_minF(perf_threads, perf_base);
	if (unlikely(sched_policy.throughput == 1.0)) return;

	/* up to now,
		target_throughput = throughput_upper
		throughput = throughput_lower */
	printf("%s: throughput: %.3f ~ %.3f minF: %.3f ~ %.3f\n", 
				__func__, throughput, target_throughput, minF_lower, minF_upper);

	if (unlikely(throughput >= target_throughput)) {
		__set_round_slice_max_fair_set();
		return;
	}

	target_throughput = throughput + sched_policy.throughput * (target_throughput - throughput);

#define ABS(x) ((x) >= 0 ? (x) : -(x))
	sched_policy.minF = minF_upper; /* if minF_upper - minF_lower < 0.005 already,
										this makes that round slices are set based on minF_lower. */

	/* Invariant: minF_lower achieves the target throughput,
				while minF_upper does not achieve the target throughput. */
	while (ABS(minF_upper - minF_lower) >= 0.005) {
		num_called++;

		sched_policy.minF = (minF_lower + minF_upper) / 2;
		__set_round_slice_minF();
		calculate_perf(perf_threads);
		throughput = calculate_throughput(perf_threads, perf_base);

		if (throughput >= target_throughput) {
			minF_lower = sched_policy.minF;
		} else {
			minF_upper = sched_policy.minF;
		}
	}

	if (sched_policy.minF == minF_upper) {
		sched_policy.minF = minF_lower;
		num_called++;
		__set_round_slice_minF();
	}

	printf("%s: minF: %.3f target_throughput: %.3f num_called: %d\n", 
				__func__, sched_policy.minF, target_throughput, num_called);
}

/* threads[*].round_slice.* must be set before calling this function */
static inline void __set_round_slice_uniformity_thru() {
	/* set based on the throughput */
}

/******************************************************/
/* Scheduling policy functions                        */
/* (Mostly, just use the core functions)              */
/******************************************************/
void set_round_slice_unaware() {
	__set_round_slice_unaware();
}

void set_round_slice_manual() {
	__set_round_slice_manual();
}

void set_round_slice_max_fair() {
	__set_round_slice_max_fair();
}

void set_round_slice_max_perf() {
	__set_round_slice_max_perf();
}

void set_round_slice_minF() {
	__set_round_slice_max_fair();
	__set_round_slice_minF();
}

void set_round_slice_similarity() {
	__set_round_slice_max_perf();
	__set_round_slice_similarity();
}

void set_round_slice_uniformity() {
	__set_round_slice_max_fair();
	__set_round_slice_max_perf();
	__set_round_slice_uniformity();
}

/* [legacy] sim_fair + min_fair + uni_fair */
void set_round_slice_fair() {
	/* Note that at least one of minF and uniformity are not zero. see set_sched_policy(). */
	__set_round_slice_max_fair();

	/* 1. consider minF */
	if (sched_policy.minF == 0)
		__set_round_slice_max_perf();
	else
		__set_round_slice_minF();

	/* 2. consider similarity => does not hurt minF */
	if (sched_policy.similarity > 0)
		__set_round_slice_similarity();

	/* 3. consider uniformity => does not hurt minF and similarity
	 *    (Note that if *_round_slice of two apps are same, 
	 *     after uni-fair alg, the values will be same) */
	if (sched_policy.uniformity > 0)
	 	__set_round_slice_uniformity();
}

void set_round_slice_all() {
	__set_round_slice_max_fair();

	/* 1. consider minF */
	if (sched_policy.criteria != c_minF && sched_policy.criteria != c_minF_uniformity)
		__set_round_slice_max_perf();
	else if (sched_policy.criteria == c_minF && sched_policy.throughput > 0) {
		__set_round_slice_minF_thru();
		return;
	} else
		__set_round_slice_minF();

	/* 2. consider similarity => does not hurt minF */
	if (sched_policy.similarity > 0)
		__set_round_slice_similarity();

	/* 3. consider uniformity => does not hurt minF and similarity
	 *    (Note that if *_round_slice of two apps are same, 
	 *     after uni-fair alg, the values will be same) */
	if (sched_policy.criteria == c_uniformity || sched_policy.criteria == c_minF_uniformity) {
		if (sched_policy.throughput > 0)
	 		__set_round_slice_uniformity_thru();
		else
	 		__set_round_slice_uniformity();
	}
}
