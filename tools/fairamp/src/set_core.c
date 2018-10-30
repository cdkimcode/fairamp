#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "fairamp.h"
#include "syscall_wrapper.h"

/* definitions of helper or wrapper functions for main() and update_speedup() */
static void file_read(const char *filename, char *buf, size_t max_len);
static void file_write(const char *filename, const char *buf);

/* core configuration */
char *fast_core_frequency_str;
char *slow_core_frequency_str;
unsigned long fast_core_frequency;
unsigned long slow_core_frequency;

int num_core;
enum core_type *core_type;

static int get_num_cores() {
	FILE *fp = fopen("/proc/cpuinfo", "r");
	char buf[MAX_LINE_LEN];
	int ret = 0;

	while (fgets(buf, MAX_LINE_LEN, fp)) {
		if (strncmp(buf, "processor	:", 11) == 0)
			ret++;
	}

	return ret;
}

/* set @fast_core_frequency_str and @slow_core_frequency_str */
static int set_core_freq() {
	char buf[MAX_LINE_LEN];
	char *tok, *last_tok, *saveptr;
	
	file_read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies",
				buf, MAX_LINE_LEN);
	
	tok = strtok_r(buf, " ", &saveptr);
	last_tok = tok;

	if (last_tok == NULL) {
		fprintf(stderr, "Failed to read scaling_available_frequencies\n");
		goto error;
	}

	fast_core_frequency_str = (char *) malloc(strnlen(last_tok, MAX_LINE_LEN) + 1);
	if (!fast_core_frequency_str)
		goto error;
	stringcopy(fast_core_frequency_str, last_tok, strnlen(last_tok, MAX_LINE_LEN));

	while (tok) {
		last_tok = tok;
		tok = strtok_r(NULL, " ", &saveptr);
	}
	
	if (last_tok == NULL) {
		fprintf(stderr, "Failed to read scaling_available_frequencies\n");
		goto error_fast;
	}
		
	slow_core_frequency_str = (char *) malloc(strnlen(last_tok, MAX_LINE_LEN) + 1);
	if (!slow_core_frequency_str)
		goto error_slow_fast;
	stringcopy(slow_core_frequency_str, last_tok, strnlen(last_tok, MAX_LINE_LEN));

	verbose("fast_core_frequency: %s slow_core_frequency: %s\n", fast_core_frequency_str, slow_core_frequency_str);
	return 0;

error_slow_fast:
	free(slow_core_frequency_str);
error_fast:
	free(fast_core_frequency_str);
error:
	fast_core_frequency_str = NULL;
	slow_core_frequency_str = NULL;
	return -1;
}

int print_core_type(const char *header, enum core_type *__core_type, FILE *fp) {
	enum core_type *type = __core_type ? __core_type : core_type;
	int i, num_fast = 0, num_slow = 0;

	if (!fp)
		fp = stdout;

	if (header)
		fprintf(fp, "%s", header);
	for (i = 0; i < num_core; i++) {
		switch(type[i]) {
		case offline: fprintf(fp, "X"); break;
		case slow_core: fprintf(fp, "S"); num_slow++; break;
		case fast_core: fprintf(fp, "F"); num_fast++; break;
		default: fprintf(fp, "U");
		}
	}
	fprintf(fp, " (fast: %d / slow: %d)\n", num_fast, num_slow); 
	if (__core_type == NULL && ((num_fast != env.num_fast_core) || (num_slow != env.num_slow_core))) {
		fprintf(fp, "ERROR: env is not set properly (fast: %d / slow: %d)\n", env.num_fast_core, env.num_slow_core);
		return -1;
	}
	
	return 0;
}

int init_core_type() {
	int ret;
	num_core = get_num_cores();
	if (num_core == 0) {
		fprintf(stderr, "ERROR: No core detected from /proc/cpuinfo\n");
		return -1;
	}

	core_type = (enum core_type *)calloc(num_core, sizeof(enum core_type));
	if (!core_type)
		return -1;

	ret = set_core_freq();
	if (ret != 0) {
		free(core_type);
		core_type = NULL;
		return -1;
	}
	fast_core_frequency = atol(fast_core_frequency_str);
	slow_core_frequency = atol(slow_core_frequency_str);
	return 0;
}

/* Set @core_type as default core type */
void set_default_core_type(enum core_type *core_type) {
	int i;
	int num_fast = 0;
	
	num_fast = (num_core + 2) / 3;
	for (i = 0; i < num_fast; i++)
		core_type[i] = fast_core;
	for (; i < num_core; i++)
		core_type[i] = slow_core;

	env.num_fast_core = num_fast;
	env.num_slow_core = num_core - num_fast;
	env.num_fast_core_f = (float) env.num_fast_core;

	print_core_type("DEFAULT_CORE_TYPE: ", core_type, NULL);
}

/* read a core configuration and set @core_type appropriately
 * also set global variables, @num_fast_core and @num_slow_core
 * return 0 when any error occurs
 * return 1 otherwise.
 */
int parse_core_config(char const *core_config, enum core_type *core_type) {
	int i;
	
	if (strlen(core_config) != num_core) {
		printf("error: %ld number of core types are specified.\n"
			   "       BUT, you should specify %d cores' type.\n"
			   "       core_config: %s\n",
			   (long int) strlen(core_config), num_core, core_config);
		return 0;
	}

	for (i = 0; i < num_core; i++) {
		switch(core_config[i]) {
		case '0':
		case 'S':
		case 's':
			core_type[i] = slow_core;
			env.num_slow_core++;
			break;
		case '1':
		case 'F':
		case 'f':
			core_type[i] = fast_core;
			env.num_fast_core++;
			break;
		case 'X':
		case 'x':
			core_type[i] = offline;
			break;
		default:
			printf("error: core setting error: %s\n"
				   "                           ", core_config);
			while(i-- > 0) printf(" ");
			printf("^\n");
			return 0;
		}
	}

	env.num_fast_core_f = (float) env.num_fast_core;
	return 1;
}
	
/* set core type according to the setting */
/* adjust the frequency of cores according to the type */
/* set the global variable, @cpumask */
void set_core_type(enum core_type *core_type, cpu_set_t *cpumask)
{
	int i;
	int hotplug = 0;
	char line[MAX_LINE_LEN];
	char filename[MAX_LINE_LEN];
			
	/* check whether hotplugging is available */
	if (access("/sys/devices/system/cpu/cpu0/online", R_OK | W_OK) == 0)
		hotplug = 1;
	

	CPU_ZERO(cpumask);
	for (i = 0; i < num_core; i++) {
		const char *frequency;
		int updated = 0;

		if (core_type[i] == offline) {
			CPU_CLR(i, cpumask); /* unmark the core */
			if (!hotplug || i == 0) {
				/* sched_setaffinity prevents using cpu0 */
				/* do nothing */
				printf("cpu%d: offline by affinity\n", i);
			} else {
				sprintf(filename, "/sys/devices/system/cpu/cpu%d/online", i);
				file_read(filename, line, MAX_LINE_LEN);
				while (strcmp("0", line) != 0) {
					if (updated == 1) {
						printf("error: plugging off cpu%d fails\n", i);
						sleep(1);
					}
					file_write(filename, "0");
					file_read(filename, line, MAX_LINE_LEN);
					updated = 1;
				}
				
				printf("cpu%d: offline\n", i);
			}

		} else { /* core_type[i] == fast_core or slow_core */
			CPU_SET(i, cpumask); /* mark the core to use */
			if (hotplug && i != 0) { /* cpu0 is not hotpluggable by default */
				sprintf(filename, "/sys/devices/system/cpu/cpu%d/online", i);
				file_read(filename, line, MAX_LINE_LEN);
				while (strcmp("1", line) != 0) {
					if (updated == 1) {
						printf("error: plugging on cpu%d fails\n", i);
						sleep(1);
					}
					file_write(filename, "1");
					file_read(filename, line, MAX_LINE_LEN);
					updated = 1;
				}
			}

			if (config.do_fairamp) {
				if (core_type[i] == fast_core && is_sched_policy_asymmetry_aware())
					set_fast_core(i);
				else
					set_slow_core(i);
			}

			if (config.adjust_frequency) {
				frequency = core_type[i] == fast_core ? fast_core_frequency_str : slow_core_frequency_str;
	retry3:
				sprintf(filename, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", i);
				file_read(filename, line, MAX_LINE_LEN);
				if (strcmp("userspace", line) != 0) {
					file_write(filename, "userspace");
					updated = 1;
				}
				sprintf(filename, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", i);
				file_read(filename, line, MAX_LINE_LEN);
				if (strcmp(frequency, line) != 0) {
					file_write(filename, frequency);
					updated = 1;
				}
				sprintf(filename, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq", i);
				file_read(filename, line, MAX_LINE_LEN);
				if (strcmp(frequency, line) != 0) {
					file_write(filename, frequency);
					updated = 1;
				}
				sprintf(filename, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
				file_read(filename, line, MAX_LINE_LEN);
				if (strcmp(frequency, line) != 0) {
					printf("error: [cpu%d] frequency is not adjusted as we want. (We want %s but actually %s)\n", i, frequency, line);
					sleep(1); /* do not change @updated since we sleep here. */
					goto retry3;
				}

				printf("cpu%d: %s cur_freq: %s\n", i, core_type[i] == fast_core ? "fast core" : "slow core", line);
			}
		}

		if (config.adjust_frequency && updated)
			sleep(1);
	}
	printf("num_active_cores: %d\n", CPU_COUNT(cpumask));
	if (sched_setaffinity(0, sizeof(cpu_set_t), cpumask) != 0) {
		printf("sched_setaffinity does not work. errno: %d\n",
					 errno);
		exit(-1);
	}
	sleep(1);

	return;
}

/*===========================*/
/* file processing functions */
/*===========================*/
static inline void rstrip(char *buf, size_t max_len) {
	size_t len = strnlen(buf, max_len) - 1;
	while (len > 0 && (buf[len] == '\0' || buf[len] == '\n' || buf[len] == ' ')) {
		buf[len] = '\0';
		len--;
	}
}

static inline void file_read(const char *filename, char *buf, size_t max_len)
{
	FILE *fp = fopen(filename, "r");
	fgets(buf, max_len, fp);
	rstrip(buf, max_len);
	fclose(fp);
}

static inline void file_write(const char *filename, const char *buf)
{
	FILE *fp = fopen(filename, "w");
	printf("DEBUG: write [%s] on [%s] fp: %p\n", buf, filename, fp);
	if (fwrite(buf, strlen(buf), 1, fp) != 1) {
		fprintf(stderr, "ERROR: failed to write [%s] on [%s]\n", buf, filename);
	}
	fclose(fp);
}
