#include <sys/types.h>

void dup2_error(int old_fd, int new_fd, int bench_num, const char *bench_name);
void execvp_error(pid_t pid, const char *comm);
void wait_error();
