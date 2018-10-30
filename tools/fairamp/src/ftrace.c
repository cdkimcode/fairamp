/*=================*/
/* ftrace function */
/*=================*/

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* option: if ftrace is a valid file path, scheduling trace will be saved to the file. */
static FILE *ftrace = NULL;

/* return 0 if ftrace is set properly.
   Otherwise, return -1. */
int set_ftrace(const char *filename) {
	FILE *fp;
	struct stat dummy;
	char path[512];

	/* Check whether the debugfs is mounted. If not, mount it. */
	snprintf(path, 512, "/debug/tracing/trace");
	if (stat("/debug/tracing/trace", &dummy) != 0) {
		int retval;

		if (stat("/debug", &dummy) != 0) {
			mkdir("/debug", 0644);
		}
		retval = system("mount -t debugfs nodev /debug");
		if (retval == -1) {
			fprintf(stderr, "ERROR: system() failed.\n");
			return -1;
		}
		if (WEXITSTATUS(retval) != 0) {
			fprintf(stderr, "ERROR: failed to mount debugfs.\n");
			return -1;
		}
	}

	/* empty the previous trace */
	fp = fopen("/debug/tracing/trace", "w");
	if (fp == NULL) {
		fprintf(stderr, "ERROR: failed to initialize /debug/tracing/trace (open)\n");
		return -1;
	}
	if (fwrite("", 1, 1, fp) != 1) {
		fprintf(stderr, "ERROR: failed to initialize /debug/tracing/trace (write)\n");
		return -1;
	}
	if (fclose(fp) != 0) {
		fprintf(stderr, "ERROR: failed to initialize /debug/tracing/trace (close)\n");
		return -1;
	}
	
	/* extend the ftrace buffer size */
	fp = fopen("/debug/tracing/buffer_size_kb", "w");
	if (fp == NULL) {
		fprintf(stderr, "ERROR: failed to extend ftrace buffer size (open)\n");
		return -1;
	}
	if (fprintf(fp, "%d", 1024 * 64) < 0) {
		fprintf(stderr, "ERROR: failed to extend ftrace buffer size (write)\n");
		return -1;
	}
	if (fclose(fp) != 0) {
		fprintf(stderr, "ERROR: failed to extend ftrace buffer size (close)\n");
		return -1;
	}

	/* open a file to save ftrace */
	ftrace = fopen(filename, "w");
	if (ftrace == NULL) {
		fprintf(stderr, "ERROR: failed to open a file for ftrace. filename: %s errno: %d\n", 
					filename, errno);
		return -1;
	} else {
		printf("ftrace: %s\n", filename);
		return 1;
	}
}

static void __enable_ftrace(int enable) {
	char *str = enable == 1 ? "1" : "0";
	FILE *f;

	f = fopen("/debug/tracing/events/sched/sched_switch/enable", "w");
	if (f == NULL) {
		fprintf(stderr, "ERROR: failed to set /debug/tracing/events/sched/sched_switch/enable <- %d (open)\n", enable);
		return;
	}
	if (fwrite(str, 1, 1, f) != 1) {
		fprintf(stderr, "ERROR: failed to set /debug/tracing/events/sched/sched_switch/enable <- %d (write)\n", enable);
		return;
	}
	if (fclose(f) != 0) {
		fprintf(stderr, "ERROR: failed to set /debug/tracing/events/sched/sched_switch/enable <- %d (close)\n", enable);
		return;
	}
}
void start_ftrace() {
	if (!ftrace)
		return;
	__enable_ftrace(1);	
}

void stop_ftrace() {
	if (!ftrace)
		return;
	__enable_ftrace(0);	
}

void save_ftrace() {
	FILE *f;
	char buf[1024 * 1024];
	size_t bytes = 0, written = 0, total_written = 0, mb = 0, mb_before = 0;

	if (!ftrace)
		return;
	printf("Save ftrace");

	f = fopen("/debug/tracing/trace", "r");
	while (!feof(f)) {
		bytes = fread(buf, 1, 1024 * 1024, f);
		written = fwrite(buf, 1, bytes, ftrace);
		if (written < bytes) {
			fprintf(stderr, "ERROR: failed to save ftrace. (Write %ld bytes but only %ld bytes are written. Total written: %ld.)\n",
						(long int) bytes, (long int) written, (long int) total_written);
		}
		total_written += written;
		mb = total_written >> 20;
		for (; mb_before < mb; mb_before++)
			printf(".");
	}
	fclose(f);
	fclose(ftrace);
	printf("\n");
}
