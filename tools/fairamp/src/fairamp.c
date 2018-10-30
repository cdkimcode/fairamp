#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>
#include <assert.h>
#include <linux/unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <math.h>
#include "error.h"
#include <signal.h>
#include <getopt.h>
#include "fairamp.h"
#include "syscall_wrapper.h"

#define DEFAULT_OUTPUT_FILENAME "output/temp.output"

char *mode_name = "normal";
struct config config = {1, 1, 1, 1, 1}; /* default mode: normal */

int opt_ignore_effi = 0;

int done = 0;

/* data shared between main and update_speedup threads */
struct timespec sched_interval = {2, 0}; /* default value: 2 seconds */
struct environment env = {0, 0, 0, 0, NULL};
static pthread_mutex_t signal_handler_lock = PTHREAD_MUTEX_INITIALIZER;
int measuring_IPS_type_started = 0;

/* variable used by main() and the signal handler */
char *output_filename;

/* mask the available cpus for the configuration */
cpu_set_t __cpumask;
cpu_set_t *cpumask = &__cpumask;

/* definitions of helper or wrapper functions for main() and update_speedup() */
static struct command *parse_comm_file(const char *comm_filename, int *num_comm);
static void sort_by_num(struct command *command, int num_comm);
static void set_cpumask_comm(struct command *command, int num_comm);
static void set_mask_str(char *mask_str, cpu_set_t *mask);
static void usage_comm_file(void);
static void free_command(struct command *command, int num_comm);
static int check_output_filename(const char *output_filename);

/* definitions of cleanup functions */
static void kill_remaining_commands(int __running);
static void close_temp_outputs();
static void merge_temp_outputs(char const *output_filename);
static void delete_temp_outputs(char const *output_filename);

/* ======================== */
/* signal handlers          */
/* ======================== */
static void termination_handler(int signum) {
	if (!pthread_mutex_trylock(&signal_handler_lock))
		return;

	if (signum == -1)
		goto from_set_cpumask_comm;

	switch(signum) {
	case SIGINT:
	case SIGHUP:
	case SIGTERM:
		printf("termination_handler: %s\n", signum == SIGINT ? "SIGINT" :
											signum == SIGHUP ? "SIGHUP" :
															"SIGTERM");
		break;
	default:
		printf("termination_handler: unexpected signal: %d\n", signum);
		return;
	}

	stop_ftrace();
	kill_remaining_commands(-1);
	
	if (measuring_IPS_type_started) 
		stop_measuring_IPS_type(); /* may result in error */

from_set_cpumask_comm:
	close_temp_outputs();
	delete_temp_outputs(output_filename);
	exit(-1);
}
/* refer to http://www.gnu.org/software/libc/manual/html_node/Basic-Signal-Handling.html#Basic-Signal-Handling */
static void register_termination_handler () {
	if (signal(SIGINT, termination_handler) == SIG_IGN)
		signal (SIGINT, SIG_IGN);
	if (signal (SIGHUP, termination_handler) == SIG_IGN)
		signal (SIGHUP, SIG_IGN);
	if (signal (SIGTERM, termination_handler) == SIG_IGN)
		signal (SIGTERM, SIG_IGN);
}

/* ======================== */
/* simple heart beat        */
/* ======================== */
void *periodic_heart_beat(void *data)
{
	unsigned long num_called = 0;

	while (likely(!done)) {
		nanosleep(&sched_interval, NULL);

		if (unlikely(done))
			break;
		
		fprintf(stderr, "update_speedup: called: %ld\n", ++num_called);
		fflush(stderr);
	}

	return (void *)num_called;
}

int set_sched_interval(const char *interval_str) {
/* set @sched_interval */
	unsigned long interval = strtol(interval_str, NULL, 10);

	if (errno != 0) {
		fprintf(stderr, "Fail to convert the given scheduling interval: %s\n", interval_str);
		return -1;
	}

	sched_interval.tv_sec = interval / 1000; /* ms -> sec */
	sched_interval.tv_nsec = (interval % 1000) * 1000000; /* ms -> ns */
	return 1;
}

/* ======================== */
/* CPU usage stat           */
/* ======================== */
struct cpu_usage_stat {
	unsigned long user;
	unsigned long nice;
	unsigned long system;
	unsigned long idle;
	unsigned long iowait;
	unsigned long irq;
	unsigned long softirq;
} cpu_usage_stat;

/* mode: 0 for start, 1 for end */
void get_cpu_usage_stat(int mode) {
//$ cat /proc/stat
//cpu  40778349 14841 205307 1845440400 224071 0 10536 0 0 0
	FILE *fp = fopen("/proc/stat", "r");
	char buf[512], *saveptr, *token;
	unsigned long data[7];
	int i;

	if (fgets(buf, 512, fp) == NULL) {
		memset(&cpu_usage_stat, 0, sizeof(cpu_usage_stat));
		return;
	}

	fclose(fp);

	token = strtok_r(buf, " ", &saveptr); // "cpu"
	for (i = 0; i < 7; i++) {
		token = strtok_r(NULL, " ", &saveptr);
		data[i] = atol(token);
	}

	if (mode == 0) {
		cpu_usage_stat.user = data[0];
		cpu_usage_stat.nice = data[1];
		cpu_usage_stat.system = data[2];
		cpu_usage_stat.idle = data[3];
		cpu_usage_stat.iowait = data[4];
		cpu_usage_stat.irq = data[5];
		cpu_usage_stat.softirq = data[6];
	} else {
		cpu_usage_stat.user = data[0] - cpu_usage_stat.user;
		cpu_usage_stat.nice = data[1] - cpu_usage_stat.nice;
		cpu_usage_stat.system = data[2] - cpu_usage_stat.system;
		cpu_usage_stat.idle = data[3] - cpu_usage_stat.idle;
		cpu_usage_stat.iowait = data[4] - cpu_usage_stat.iowait;
		cpu_usage_stat.irq = data[5] - cpu_usage_stat.irq;
		cpu_usage_stat.softirq = data[6] - cpu_usage_stat.softirq;
	}
}

void print_cpu_usage_stat() {
	unsigned long total, user, kernel, idle;

	total = cpu_usage_stat.user  
			+ cpu_usage_stat.nice  
			+ cpu_usage_stat.system
			+ cpu_usage_stat.idle  
			+ cpu_usage_stat.iowait
			+ cpu_usage_stat.irq   
			+ cpu_usage_stat.softirq;

	user = cpu_usage_stat.user  
			+ cpu_usage_stat.nice; 

	kernel = cpu_usage_stat.system  
			+ cpu_usage_stat.irq   
			+ cpu_usage_stat.softirq;

	idle = cpu_usage_stat.idle  
			+ cpu_usage_stat.iowait;

	printf("cpu_usage_stat_raw: %ld %ld %ld %ld %ld %ld %ld\n",
			cpu_usage_stat.user,
			cpu_usage_stat.nice, 
			cpu_usage_stat.system,
			cpu_usage_stat.idle,
			cpu_usage_stat.iowait,
			cpu_usage_stat.irq,
			cpu_usage_stat.softirq);
	printf("cpu_usage_stat_aggr: total: %ld user: %ld kernel: %ld idle: %ld\n",
				total, user, kernel, idle);
	
	printf("cpu_usage_stat: user: %.3f kernel: %.3f idle: %.3f\n",
				(float) user * 100 / total, (float) kernel * 100 / total, (float) idle * 100 / total);

}

/* ======================== */
/* run_a() helper functions */
/* ======================== */

static int __itostrlen(int num) {
	int ret = 0;
	if (num < 0) {
		ret++;
		num = -num;
	}
	while (num > 0) {
		num /= 10;
		ret++;
	}
	return ret;
}

static inline int itostrlen(int num) {
	// few frequent cases
	if (num >= 0) {
		if (num < 10)
			return 1;
		else if (num < 100)
			return 2;
		else if (num < 1000)
			return 3;
	} 
	return __itostrlen(num);
}

char *itostr(int num, int len, char *str) {
	int idx = 0;

	if (len == 0)
		len = itostrlen(num);
	if (str == NULL)
		str = (char *)malloc(len + 1);
	
	if (str == NULL) return NULL;

	idx = len - 1;
	str[len] = '\0';

	if (num == 0) {
		str[0] = '0';
		return str;
	}

	if (num < 0) {
		str[0] = '-';
		num = -num;
	}

	while (num > 0) {
		str[idx--] = '0' + (num % 10);
		num /= 10;
	}
	return str;	
}

/* ======================= */
/* run_a() function        */
/* ======================= */
void run_a(struct command *command) {
	pid_t pid;
	struct fairamp_unit_vruntime info;

	gettimeofday(&command->__begin, 0);

	pid = fork();
	if (pid == 0) {

		pid = getpid();
		/* 'setpgid' is very important to kill this process and children of 
			this process. We will send signal to the group of processes 
			whose pgid is set here. */
		setpgid(pid, pid); 
	
		if (command->speedup < 0) {
			char mask_str[num_core * 2];
			set_mask_str(mask_str, &command->cpumask);
			if (sched_setaffinity(0, sizeof(cpu_set_t), &command->cpumask) != 0) {
				fprintf(stderr, "sched_setaffinity does not work. errno: %d\n",
							 	errno);
				fprintf(stderr, "command->num: %d selected_cpu: %s\n", command->num, mask_str);
				exit(-1);
			} else {
				printf("command(%d) %s pinned to cpu %s\n",
							command->num, command->name, mask_str);
			}
		} else if (config.do_fairamp) {
			info.pid = 0;
			info.unit_fast_vruntime = command->round_slice.fast;
			info.unit_slow_vruntime = command->round_slice.slow;
			set_unit_vruntime(1, &info);
		}
		
		/* handle stdout, stderr */
		if (dup2(command->output, 1) == -1) {
			dup2_error(command->output, 1, command->num, command->name);
			exit(-1);
		}
			
		if (dup2(command->output, 2) == -1) {
			dup2_error(command->output, 2, command->num, command->name);
			exit(-1);
		}

		execvp(command->argv[0], command->argv);

		/* return only when error occurred. */

		execvp_error(pid, command->argv[0]);
		exit(-1);
	} 
	command->pid = pid;
	printf("run(num: %d name: %s pid: %d)\n", command->num, command->name, command->pid);
	return;
}

/* ======================================= */
/* managing options and setting the system */
/* ======================================= */

static struct mode {
	char *name;
	struct config config;
} predefined_mode[] = {
					   /* periodic_speedup_update do_fairamp adjust_frequency fast_core_first repeated_run */
	{ "normal",          {1,                      1,         1,               1,              1} },
	{ "static",          {0,                      1,         1,               0,              1} },
	{ "speeduptest",     {1,                      1,         0,               0,              1} },
	{ "wo_overhead",     {0,                      0,         0,               0,              1} },
	{ "overhead_cs",     {0,                      1,         0,               1,              1} },
	{ "overhead_cs_pmu", {1,                      1,         0,               1,              1} },
	{ "pinning",         {0,                      0,         1,               0,              1} },
	{ "unaware",         {0,                      0,         1,               0,              1} }, /* same with 'pinning' */
	{ "repeat",          {0,                      0,         0,               0,              1} },
	{ "no",              {0,                      0,         0,               0,              0} },
	{ NULL,              {0,                      0,         0,               0,              0} },
};

int set_mode(char const *mode_str) {
/* struct config {
int periodic_speedup_update;
int do_fairamp;
int adjust_frequency;
int fast_core_first;
int repeated_run;
} config; */
	int i = 0;
	
	while (predefined_mode[i].name) {
		if (strcmp(mode_str, predefined_mode[i].name) == 0) {
			struct mode *mode = &predefined_mode[i];
			mode_name = mode->name;
			config.periodic_speedup_update = mode->config.periodic_speedup_update;
			config.do_fairamp = mode->config.do_fairamp;
			config.adjust_frequency = mode->config.adjust_frequency;
			config.fast_core_first = mode->config.fast_core_first;
			/* if config.repeated_run == 0, it is already set. So, don't override it. */
			if (config.repeated_run)
				config.repeated_run = mode->config.repeated_run;
			break;
		}
		i++;
	}

	if (predefined_mode[i].name == NULL) {
		printf("[Error] unknown mode: %s\n", mode_str);
		return 0;
	}

	return 1;
}

void check_mode() {
	if (config.periodic_speedup_update && !config.do_fairamp) {
		fprintf(stderr, "[WARNING] periodic_speedup_update can work only with do_fairamp. Automatically set do_fairamp.\n");
		config.do_fairamp = 1;
	}
}

/* refer to http://www.gnu.org/software/libc/manual/html_node/Getopt.html */
int get_options(int argc, char *const *argv, 
	enum core_type *core_type,
	char **comm_filename, char **output_filename)
{
	int mode_given = 0, type_given = 0, comm_given = 0, output_given = 0, ftrace_given = 0, interval_given = 0;
	int policy_given = 0;
	struct option long_options[] =
	{
		{"noeffi", no_argument, &opt_ignore_effi, 1},
		{"norepeat", no_argument, &config.repeated_run, 0},
		{"type", required_argument, NULL, 't'},
		{"policy", required_argument, NULL, 'p'},
		{"base", required_argument, NULL, 0},
		{"criteria", required_argument, NULL, 0},
		{"metric", required_argument, NULL, 0},
		{"target", required_argument, NULL, 0},
		{"similarity", required_argument, NULL, 0},
		{"comm", required_argument, NULL, 'c'},
		{"output", required_argument, NULL, 'o'},
		{"help", no_argument, NULL, 'h'},
		{"stop", no_argument, NULL, 's'},
		{"mode", required_argument, NULL, 'm'},
		{"ftrace", required_argument, NULL, 'f'},
		{"interval", required_argument, NULL, 'i'},
		{0, 0, 0, 0}
	};
	int option_index = 0;
	int c;
	
	while ((c = getopt_long(argc, argv, "t:p:c:o:m:f:i:hs", long_options, &option_index)) != -1) {
		switch(c) {
		case 't':
			/* parse core configuration */
			type_given = parse_core_config(optarg, core_type);
			if (type_given == 0)
				return -1;
			break;
		case 'p':
			policy_given = set_sched_policy("policy", optarg);
			if (policy_given == 0)
				return -1;
			break;
		case 'c':
			*comm_filename = optarg;
			comm_given = 1;
			break;
		case 'o':
			*output_filename = optarg;
			output_given = 1;
			break;
		case 's':
			return 1; /* stop measuring IPS */
		case 'm':
			mode_given = set_mode(optarg);
			if (mode_given == 0)
				return -1;
			break;
		case 'f':
			ftrace_given = set_ftrace(optarg);
			if (ftrace_given < 0)
				return -1;
			break;
		case 'i':
			interval_given = set_sched_interval(optarg);
			if (interval_given < 0)
				return -1;
			break;
		case 0:
			/* If this option set a flag, do nothing else now. */
			if (long_options[option_index].flag != NULL)
				break;
			printf("DEBUG: long options?\n");
			/* options without abbreviation */
			c = set_sched_policy(long_options[option_index].name, optarg);
			if (c == -1)
				return -1; /* error in set_sched_policy() */
			if (c == 1) /* set_sched_policy() got this option. */ 
				break;
			/* now, all options without abbreviation are related to scheduling policy */
			return -1;
		case '?':
			/* getopt_long already printed an error message */
		case 'h':
			/* treat help as an error to show the usage */
		default:
			return -1;
		}
	}
	
	if (comm_given == 0) {
		fprintf(stderr, "error: command file must be given\n");
		return -1;
	}
	
	/* set default values */
	if (type_given == 0) {
		set_default_core_type(core_type);
		env.num_fast_core_f = (float) env.num_fast_core;
	}

	if (output_given == 0)
		*output_filename = DEFAULT_OUTPUT_FILENAME;
	/* mode_given is ignored because the default mode is given. */

	/* case: ./fairamp stop */
	if (optind + 1 == argc && strncmp(argv[optind], "stop", 4) == 0)
		return 1;
	
	if (optind != argc) {
		fprintf(stderr, "error: while parsing options\n" "option:");
		while (optind < argc)
			fprintf(stderr, " %s", argv[optind++]);
		fprintf(stderr, "\n");
		return -1;
	}

	/* finalize the options */
	check_mode();
	if (set_sched_policy(NULL, NULL) == 0)
		return -1;

	/* show the final option */
	printf("mode: %s\n", mode_name); 
	print_core_type("core_type: ", NULL, NULL);
	printf("sched_policy: %s\n", get_sched_policy_name());
	printf("comm_file: %s\n", *comm_filename);
	printf("output_file: %s\n", *output_filename);
	if (opt_ignore_effi)
		printf("efficiency setting will be ignored.\n");
	return 0;
}

/* ====================== */
/* main() function        */
/* ====================== */

static void usage(void) {
	int i = 0;
	printf("usage: fairamp --help or -h => show this message\n");
	printf("usage: fairamp --stop or stop => stop measuring IPS\n");
	printf("usage: fairamp --comm [command_file] --mode [mode] --type [core_type_config] --policy [policy] --base [base] --criteria [criteria] --metric [metric] --target [target] --output [output_file] {--norepeat}\n");
	printf("usage: fairamp --c [command_file] --m [mode] --t [core_type_config] -p [policy] --base [base] --criteria [criteria] --metric [metric] --target [target] --o [output_file] {--norepeat}\n");
	
	printf("\n");	
	printf("mode:");
	i = 0;
	while (predefined_mode[i].name) {
		printf(" %s", predefined_mode[i].name);
		i++;
	}
	printf("\n");
	list_predefined_policies("policy:");
	printf("       (if policy is given, base, criteria, metric, target, and similarity will be ignored.)\n");
	printf("base: fair_share fair slow_core slow fast_core fast\n"
		   "criteria: unaware manual max_perf max_fair minFairness minF uniformity uni minFairness_uniformity minF_uni\n"
		   "metric: fairness f throughput t\n"
		   "        (throughput cannot be chosen with minF_uni criteria)\n"
		   "target: percentage value for the metric. ex) 130 80 70_80\n"
		   "        (underbar is used for minF_uni criteria and fairness metric)\n"
		   "        (more than 100 can be used for slow-core base)\n"
		   "similarity: threshold in difference of fast core speedups\n"
		   "\n"
		   "Additional options\n"
		   "--ftrace=[ftrace file name] or -f [ftrace file name]: save the trace for scheduling context switch information\n"
		   "--interval=[time in ms] or -i [time in ms]: set the scheduling interval in miniseconds (defautl: 2000ms)\n"
		   "\n");


	printf("NOTE:\n"
		   "1. CPU hotplug is enabled OR all cores should be turned on\n"
		   "2. Detecting hard lockups must be disabled due to confliction on using performance counters.\n"
		   "   You should clear the following kernel option: Kernel hacking -> Kernel debugging -> Detect Hard and Soft Lockups [ ]\n"
		   "3. This version is tested and validated using Ubuntu 12.04.5 and gcc 4.6.3.\n"
		   "   Some problem may occurs on other environments.\n");

	printf("\n");
	printf("For further information, please refer to the following paper.\n"
		   "Changdae Kim and Jaehyuk Huh. Exploring the Design Space of Fair Scheduling Supports for Asymmetric Multicore Systems. IEEE Transactions on Computers, vol. 67, no. 8, pp. 1136-1152, August 2018.\n");
	printf("\n");

	usage_comm_file();
	exit(-1);
}

int main(int argc, char *argv[])
{
	int i;
	pid_t pid;
	int status;
	int finished = 0;
	int running = 0;
	int try_finished = 0;
	char *comm_filename;
	char filename[MAX_LINE_LEN];
	struct command *command;
	int num_comm = 0;
	pthread_t update_speedup_t;

	/* check whether root user */
	if (geteuid() != 0) {
		printf("ERROR! Please run as root!\n");
		return -1;
	}

	register_termination_handler();

	if (init_core_type()) {
		fprintf(stderr, "Failed to init_core_type()\n");
		return -1;
	}

	status = get_options(argc, argv, core_type,
					&comm_filename, &output_filename);
	if (status == 1) {
		stop_measuring_IPS_type();
		printf("stop measuring IPS\n");
		return 0;
	} else 
			if (status == -1) {
		usage();
		return -1;
	}

	if (check_output_filename(output_filename) < 0)
		return -1; /* error messages are already shown. */
	
	command = parse_comm_file(comm_filename, &num_comm);
	if (command == NULL)
		return -1; /* error messages are already shown. */
	
	env.command = command;
	env.num_comm = num_comm;

	for (i = 0; i < num_comm; i++) {
		command[i].num = i;
		command[i].pid = -1; /* -1 means that this command have not been ever created yet */
		/* don't overwrite the speedup if unaware or manual policy is used. */
		if (config.periodic_speedup_update && is_sched_policy_speedup_aware())
			command[i].speedup = 1.0;
		
		/* open the output file */
		sprintf(filename, "%s.%02d", output_filename, command[i].num);
		command[i].output = open(filename, O_WRONLY | O_CREAT | O_TRUNC, S_IWUSR | S_IRUSR);
		if (command[i].output == -1) {
			printf("error: output file open error: filename: %s error: %d\n", filename, errno);
			return -1;
		}
	}

	if (config.do_fairamp) {
		if (init_set_round_slice(&env))
			return -1; /* error message is already shown */

		/* set round slice according to the commands and scheduling policy */
		/* but, do not set_unit_vruntime() now, since no application is started */
		set_round_slice_before_run();
	}

	sort_by_speed_up(command, num_comm);

#ifdef VERBOSE
	print_commands(command, num_comm);
#endif

	set_core_type(core_type, cpumask);
	/* set @cpumask field of each command */
	set_cpumask_comm(command, num_comm);

	//printf("main: pid: %d\n", getpid());
	if (config.periodic_speedup_update) {
		if (pthread_create(&update_speedup_t, NULL, 
				periodic_update_speedup, (void *) ((unsigned long)num_comm)) < 0) {
			printf("error: pthread_create failed\n");
			return -1;
		}
	}
#ifdef VERBOSE
	else {
		if (config.do_fairamp) {
			if (pthread_create(&update_speedup_t, NULL, 
					periodic_show_stat, (void *) ((unsigned long)num_comm)) < 0) {
				printf("error: pthread_create failed\n");
				return -1;
			}
		} else {
			if (pthread_create(&update_speedup_t, NULL, 
					periodic_heart_beat, (void *) ((unsigned long)num_comm)) < 0) {
				printf("error: pthread_create failed\n");
				return -1;
			}
		}
	}
#endif /* VERBOSE */

	/* start ftrace */
	get_cpu_usage_stat(0);
	start_ftrace();

	/* run the commands */
	for (i = 0; i < num_comm; i++) {
		run_a(&command[i]);
		running++;
	}

	/* wait commands... */
	while (running) {
		try_finished++;
		pid = wait(&status);
		if (unlikely(pid == -1)) { /* error occured */
			wait_error();
			if (errno == ECHILD)
				break;
			continue;
		}

finished_task:
		for (i = 0; i < num_comm; i++)
			if (pid == command[i].pid)
				break;
		
		if (unlikely(i == num_comm))
			pr_err("wait returned but it is not one of the commands (pid: %05d status: %d) try_finished: %d\n", pid, status, try_finished);
		else {
			verbose_err("wait returned with pid: %d status: %d\n", pid, status);
			gettimeofday(&command[i].__end, 0);
			command[i].pid = 0;

			if (!command[i].finished) {
				command[i].pid_first = pid;
				command[i].begin = command[i].__begin;
				command[i].end = command[i].__end;
				command[i].status = status;
				command[i].finished = 1;
				finished++;
				verbose("newly finished command: num: %d name: %s pid: %d time: %-5.3f\n", 
							command[i].num, command[i].name, pid,
							TIME_DIFF(command[i].__begin, command[i].__end));
#ifdef VERBOSE
				print_commands(&command[i], 0);
#endif
			} else {
				verbose("finished command(not newly): num: %d name: %s pid: %d time: %-5.3f\n", 
							command[i].num, command[i].name, pid,
							TIME_DIFF(command[i].__begin, command[i].__end));
			}

			if (finished < num_comm && config.repeated_run) {
				run_a(&command[i]);
				verbose("run again: name: %s pid: %d\n", command[i].name, command[i].pid);
			} else {
				running--;
			}
			verbose(" (running: %d finished: %d)\n", running, finished);
		}

		if (finished == num_comm)
			break;

		if (running) {	
			pid = waitpid(-1, &status, WNOHANG);
			if (pid > 0)
				goto finished_task;
			else if (unlikely(pid == -1)) {
				wait_error();
				if (errno == ECHILD)
					break;
			}
		}

		/* Even if speedup is not updated periodically, update them when a command ends. */
		if (config.do_fairamp && !config.periodic_speedup_update)
			set_round_slice();
	}
	
	/* stop ftrace */
	stop_ftrace();
	get_cpu_usage_stat(1);
	
#ifndef VERBOSE
	/* if VERBOSE, always join the thread. */
	if (config.periodic_speedup_update)
#endif
	{
		done = 1; /* stop the update_speedup() thread */
		verbose_err("join update_speedup_t\n");
		/* sleep twice of scheduling interval */
		nanosleep(&sched_interval, NULL);
		nanosleep(&sched_interval, NULL);
		pthread_join(update_speedup_t, NULL);
	}

	/* kill remaining commands */
	kill_remaining_commands(running);

	/* unset performance counters */
	if (measuring_IPS_type_started) {
		pr_err("unset performance counters\n");
		stop_measuring_IPS_type();
	}

	save_ftrace();

	
	/* show the final results */
	sort_by_num(command, num_comm);
	print_commands(command, num_comm);
	
	close_temp_outputs();
	merge_temp_outputs(output_filename);
	delete_temp_outputs(output_filename);

	free_command(command, num_comm);
	print_cpu_usage_stat();

	return 0;
}

/* ==================== */
/* clean-up functions   */
/* ==================== */

/* kill commands whose pid is not -1.
 * @__running: the remaining commands for double-check.
 *             negative value if you want not to do double-check. 
 */
static void kill_remaining_commands(int __running) {
	int i, running = 0;
	pid_t pid;
	int status;
	struct command *command = env.command;
	int num_comm = env.num_comm;

	/* count the number of commands to be killed */
	for (i = 0; i < num_comm; i++) {
		if (command[i].pid > 0)
			running++;
	}

	/* Double-check (Do not this if __running < 0) */
	if (__running >= 0 && running != __running) {
		pr_err("ERROR: call kill_remaining_commands(%d), but %d commands are running.\n",
				__running, running);
	}

	pr_err("now try to kill %d processes\n", running);
	while(running) {
		for (i = 0; i < num_comm; i++) {
			if (command[i].pid > 0) {
				pr_err("send kill to name: %s pid: %d\n", command[i].name, command[i].pid);
				/* we use the negative value to kill all processes invoked for the command */
				/*kill(-command[i].pid, SIGKILL);*/
				if (kill(-command[i].pid, SIGKILL) != 0)
					pr_err("kill error: errno: %d\n", errno);
			}
		}

		pr_err("sleep 1 seconds\n");

		sleep(1);

		pr_err("wake up and wait... (running: %d)\n", running);
		for (i = 0; i < env.num_comm; i++) {
			if (command[i].pid > 0) { 
				pr_err("waitpid pid: %d\n", command[i].pid);
				pid = waitpid(command[i].pid, &status, WNOHANG);
				pr_err("waitpid return: %d status %d\n", pid, status);
				if (pid == command[i].pid) {
					running--;
					pr_err("killed command: name: %s pid: %d (running: %d)\n", command[i].name, command[i].pid, running);
					command[i].pid = 0;
				} else if (unlikely(pid == -1)) {
					wait_error();
					if (errno == ECHILD)
						break;
				}
			}
		}

	}
}

static int check_output_filename(const char *output_filename) {
	int i = strlen(output_filename);
	char *output_dirname = (char *)malloc(i + 1);
	int ret = 0;

printf("%s: filename: %s\n", __func__, output_filename);
	/* get the dirname from @output_filename */
	stringcopy(output_dirname, output_filename, i);
	i--;
	while (i > 0 && output_dirname[i] != '/')
		i--;
	
	if (i == 0 && output_dirname[0] == '/') { /* root directory */
		if (output_dirname[1] == '\0')
			goto error_no_filename;
		output_dirname[1] = '\0';
	} else if (i == 0) { /* the current directory */
		if (output_dirname[0] == '\0')
			goto error_no_filename;
		output_dirname[0] = '.';
		output_dirname[1] = '\0';
	} else {
		if (output_dirname[i + 1] == '\0')
			goto error_no_filename;
		output_dirname[i] = '\0';
	}
printf("%s: dirname: %s\n", __func__, output_dirname);
	
	/* check the existance of output_dirname. if not, try to make it. */	
	if (access(output_dirname, R_OK) != 0) {
		char *ptr = output_dirname;
		char save;

		do {
			while (*ptr != '/' && *ptr != '\0')
				ptr++;

			save = *ptr;
			*ptr = '\0';
printf("%s: check dir: %s\n", __func__, output_dirname);
			if (access(output_dirname, R_OK) == 0) {
printf("%s: check dir: %s exist\n", __func__, output_dirname);
				if (access(output_dirname, W_OK) != 0) {
					fprintf(stderr, "ERROR: we have no permission to write on directory [%s]\n", output_dirname); 
					ret = -EACCES;
					goto end;
				}
			} else {
printf("%s: check dir: %s not exist\n", __func__, output_dirname);
				if (mkdir(output_dirname, 00666) != 0) {
					fprintf(stderr, "ERROR: we failed to create directory [%s]\n", output_dirname); 
					ret = -EACCES;
					goto end;
				}
			}
			*ptr = save;
		} while (*ptr++ != '\0');
	}

	/* check whether we have a permission to write on the directory */
	if (access(output_dirname, W_OK) != 0) {
		fprintf(stderr, "ERROR: we have no permission to write on directory [%s]\n", output_dirname); 
		ret = -EACCES;
		goto end;
	}

	/* finally, check whether we have a permission to write on the file */
	if (access(output_filename, R_OK) != 0)
		goto end; /* the file doest not exist. we will make it. */

	if (access(output_filename, W_OK) != 0) {
		fprintf(stderr, "ERROR: we have no permission to write on file [%s]\n", output_filename); 
		ret = -EACCES;
	} else {
		printf("WARNING: output file [%s] already exists.\n", output_filename);
	}
	

end:
	free(output_dirname);
	return ret;

error_no_filename:
	fprintf(stderr, "ERROR: output_filename should be given as a filename, not a directory name\n");
	fprintf(stderr, "ERROR: given output_filename: %s\n", output_filename);
	ret = -EISDIR;
	goto end;
}

static void close_temp_outputs() {
	int i;

	pr_err("close temp outputs of commands\n");
	for (i = 0; i < env.num_comm; i++)
		/* ignore the return value of close() for safety in signal handler */
		close(env.command[i].output);
}

static void merge_temp_outputs(char const *output_filename) {
	int i;
	char filename[MAX_LINE_LEN];
	FILE *fp;
	FILE *comm_fp;
	char buf[1024];

	/* merge output files of commands */
	pr_err("merge output files of commands\n");
	stringcopy(filename, output_filename, MAX_LINE_LEN);
	fp = fopen(filename, "w");
	if (!fp) {
		pr_err("ERROR: while opening %s\n", filename);
		return;
	}

	for (i = 0; i < env.num_comm; i++) {
		sprintf(filename, "%s.%02d", output_filename, env.command[i].num);
		comm_fp = fopen(filename, "r");
		if (!comm_fp) {
			pr_err("ERROR: while opening %s to merge output of command(%d) %s\n", 
						filename, env.command[i].num, env.command[i].name);
			return;
		}


		fprintf(fp, "output%02d==============================================================================\n", env.command[i].num);
		
		while (fgets(buf, 1024, comm_fp) != NULL)
			fputs(buf, fp);
		fclose(comm_fp);
	}
	fclose(fp);
}

static void delete_temp_outputs(char const *output_filename) {
	int i;
	char filename[MAX_LINE_LEN];
	/* delete the merged output files */
	for (i = 0; i < env.num_comm; i++) {
		sprintf(filename, "%s.%02d", output_filename, env.command[i].num);
		remove(filename); /* delete the merged file */
	}
}

static void free_command(struct command *command, int num_comm) {
	int i;
	for (i = 0; i < num_comm; i++) {
		if (command[i].argv) {
			if (command[i].argv[0])
				free(command[i].argv[0]); /* all arguments are saved in a consecutive region */
			free(command[i].argv);
		}
	}
	free(command);
}

/* ======================================= */
/* parse_comm_file() and related functions */
/* ======================================= */
static void usage_comm_file(void) {
	printf(
"Command File Format\n"
"----------------------------------------------------------------------------------\n"
"1: #comment only if the first character is '#'\n"
"2: speedup: [speedup] num: [num_thread] cmd: [execution command]\n"
"3: speedup: [speedup] cmd: [execution command]\n"
"4: num: [number of threads] cmd: [execution command]\n"
"5: cmd: [command to execute]\n"
"...\n"
"\n"
"@cmd: NOTE that this must be the last element and this cannot be omitted.\n"
"      All words after 'cmd:' are treated as the execution command.\n"
"@speedup: The offline speedup value.\n"
"          This can be omitted. The default value is 1.0.\n"
"          If this is -1, the program is run as core pinning mode\n"
"@num_thread: The number of threads of the program. (integer)\n"
"             This can be omitted. By default, 1. (single-threaded)\n"
"             Note that the support for multi-threaded application is very limited.\n"
"             If the program is not embarrasingly parallel,\n"
"             the scheduling policy may not work properly.\n"
"----------------------------------------------------------------------------------\n"
);
}

#define is_empty_char(c) ((c) == ' ' || (c) == '\t') /* there will be no '\n' since we delete it */
static int __parse_store_argv(struct command *command, char *__line, char *start) {
	char *ptr;
	char *cmd;
	int num_argv = 0, len;

	for (ptr = start; *ptr != '\0'; ptr++)
		if (*ptr == '\n') {
			*ptr = '\0';
			break;
		}

	while (*start == '\0' || is_empty_char(*start))
		start++;

	cmd = (char *)malloc(strnlen(start, MAX_LINE_LEN) + 1); /* to include null-byte */
	if (!cmd)
		return -1;

	stringcopy(cmd, start, MAX_LINE_LEN);

	ptr = start;
	num_argv = 0;
	while (*ptr != '\0') {
		num_argv++;
		while (!is_empty_char(*ptr) && *ptr != '\0')
			ptr++;
		while (is_empty_char(*ptr) && *ptr != '\0')
			ptr++;
	}

	command->argv = (char **)calloc(num_argv + 1, sizeof(char *));
	if (!command->argv)	
		return -1;

	ptr = cmd;
	num_argv = 0;
	while (*ptr != '\0') {
		command->argv[num_argv++] = ptr;
		while (!is_empty_char(*ptr) && *ptr != '\0')
			ptr++;
		if (*ptr == '\0')
			break;
		*ptr = '\0';
		ptr++;
		while (is_empty_char(*ptr) && *ptr != '\0')
			ptr++;
	}
	command->argv[num_argv] = NULL;

	len = strnlen(command->argv[0], MAX_COMM_NAME_LEN);
	if (len < MAX_COMM_NAME_LEN - 1)
		stringcopy(command->name, start, MAX_COMM_NAME_LEN - 1);
	else
		stringcopy(command->name, command->argv[0] + (len - MAX_COMM_NAME_LEN), MAX_COMM_NAME_LEN - 1);

	return 0;
}

static int __parse_comm_file(struct command *command, char *__line) {
	char *tok, *prev_tok, *saveptr, *endptr;
	static char line[MAX_LINE_LEN];

	stringcopy(line, __line, MAX_LINE_LEN);
	line[MAX_LINE_LEN - 1] = '\0';

	/* set default value */
	command->speedup = 1.0;
	command->num_threads = 1;

	/* tokenizing... */
	prev_tok = line;
	tok = strtok_r(line, " ", &saveptr);
	while (tok) {
		if (strncmp(tok, "speedup:", 8) == 0) {
			prev_tok = tok;
			tok = strtok_r(NULL, " ", &saveptr);
			if (!tok) goto error;

			command->speedup = strtof(tok, &endptr);

			if (endptr == tok) {
				prev_tok = tok;
				goto error;
			} else if (command->speedup == HUGE_VALF || command->speedup == -HUGE_VALF) {
				prev_tok = tok;
				goto error;
			} else if (command->speedup == 0 && errno == ERANGE) {
				prev_tok = tok;
				goto error;
			}
		} else if (strncmp(tok, "num:", 4) == 0) {
			prev_tok = tok;
			tok = strtok_r(NULL, " ", &saveptr);
			if (!tok) goto error;

			command->num_threads = atoi(tok);

		} else if (strncmp(tok, "cmd:", 4) == 0) {
			/* process after this loop */
			break;
		} else 
			goto error;

		prev_tok = tok;
		tok = strtok_r(NULL, " ", &saveptr);
	}

	if (tok == NULL) { /* "cmd:" is not appeared */
		fprintf(stderr, "error while parsing command file: no command is found\n");
		fprintf(stderr, "line: %s\n", __line);
		fprintf(stderr, "\n");
		usage_comm_file();
		return -1;
	}

	while (*tok != '\0')
		tok++;
	tok++;

	return __parse_store_argv(command, __line, tok);

error:
	saveptr = line;
	for (saveptr = line; saveptr < prev_tok; saveptr++) {
		*saveptr = ' ';
	}
	*saveptr++ = '^';
	*saveptr = '\0';
	fprintf(stderr, "error while parsing command file\n");
	fprintf(stderr, "line: %s\n", __line);
	fprintf(stderr, "      %s (error around here)\n", line);
	fprintf(stderr, "\n");
	usage_comm_file();
	return -1;
}

static struct command *parse_comm_file(const char *comm_filename, int *num_comm_ptr) {
	struct command *command;
	char line[MAX_LINE_LEN];
	int num_comm = 0;
	FILE *fp;
	int ret;

	/* get num_comm */
	fp = fopen(comm_filename, "r");
	if (!fp) {
		printf("error: while reading %s\n", comm_filename);
		return NULL;
	}

	while (fgets(line, MAX_LINE_LEN, fp)) {
		if (line[0] == '#')
			continue;
		num_comm++;
	}
	rewind(fp);

	command = (struct command *)calloc(num_comm, sizeof(struct command));
	if (!command) {
		printf("error: memory allocation failed!\n");
		return NULL;
	}

	num_comm = 0;
	while (fgets(line, MAX_LINE_LEN, fp)) {
		if (line[0] == '#')
			continue;
		ret = __parse_comm_file(&command[num_comm], line);
		if (ret) {
			free_command(command, num_comm + 1);
			return NULL;
		}
		num_comm++;
	}
	
	*num_comm_ptr = num_comm;
	printf("num_comm: %d\n", *num_comm_ptr);
	return command;
}

static void set_cpumask_comm(struct command *command, int num_comm)
{
	int i = -1;
	int cpu = 0;
	int remain_thread = 0;
	cpu_set_t *mymask = NULL;
	char mask_str[num_core * 2];
	set_mask_str(mask_str, cpumask);

	for (cpu = 0; cpu < num_core; cpu++) {
		if (CPU_ISSET(cpu, cpumask) == 0)
			continue;

		if (remain_thread == 0) { /* load the next command */
			i++;
			if (i == num_comm)
				break;

			mymask = &(command[i].cpumask);
			CPU_ZERO(mymask);
			remain_thread = command[i].num_threads;
			assert(remain_thread > 0);
		}

		CPU_SET(cpu, mymask);
		remain_thread--;
	}

	/* if #cpus < #threads, check the mode and exit. */
	if (remain_thread > 0 || i < num_comm - 1) {
		cpu = i; /* back up for the message */
		for (i = 0; i < num_comm; i++)
			if (command[i].speedup < 0)
				break;
		if (i < num_comm) {
			fprintf(stderr, "ERROR: commands want to be pinned, but #cpus < #threads.\n");
			fprintf(stderr, "ERROR: remain_thread: %d i: %d\n", remain_thread, cpu);
			termination_handler(-1);
		}
	}
}

static void set_mask_str(char *mask_str, cpu_set_t *mask) {
	int idx = 0;
	int cpu = 0;

	for (cpu = 0; cpu < num_core; cpu++) {
		if (CPU_ISSET(cpu, mask)) {
			if (idx > 0)
				mask_str[idx++] = ',';
			mask_str[idx++] = '0' + cpu;
		}
	}

	if (idx == 0) { /* no cpu is set */
		mask_str[idx++] = 'X';
	}
	mask_str[idx] = '\0';
}

/* ======================================= */
/* print_command() and related functions */
/* ======================================= */
void __print_command(struct command *command)
{
	printf("%-2d %-20s %7.3f %8d %16d %16d %5d %9.3f\n",
				command->num,
				command->name,
				command->speedup,
				command->num_threads,
				command->round_slice.fast,
				command->round_slice.slow,
				command->pid,
				command->finished ? TIME_DIFF(command->begin, command->end) : -1);
}

void print_commands(struct command *command, int num_comm)
{
	int i;

	if (num_comm)
		printf("Command Table %d\n", num_comm);

	printf("%-2s %-20s %7s %8s %16s %16s %5s %9s\n",
				"id", "name", "speedup", "#threads", 
				"fast_round_slice", "slow_round_slice", 
				"pid", "time");
	
	if (num_comm == 0) {
		__print_command(command);
		return;
	}
	printf("========================================================="
			"=================================="
			"================\n");

	for (i = 0; i < num_comm; i++)
		__print_command(&command[i]);
}

void sort_by_num(struct command *command, int num_comm)
{
	int i, j;
	struct command temp;

	for (i = 0; i < num_comm; i++) {
		if (command[i].num == i)
			continue;

		for (j = i + 1; j < num_comm; j++)
			if (command[j].num == i)
				break;

		assert(j < num_comm);

		temp = command[i];
		command[i] = command[j];
		command[j] = temp;
	}
}
