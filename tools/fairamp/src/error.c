#include <stdio.h>
#include <sys/types.h>
#include <errno.h>


#define VERBOSE_ERROR

#ifdef VERBOSE_ERROR
void dup2_error(int old_fd, int new_fd, int bench_num, const char *bench_name) {
	printf("error: dup2(%d, %d) for benchmark(%d) %s error: ",
			old_fd, new_fd, bench_num, bench_name);
	switch(errno) {
	case EBADF:		printf("EBADF");		break;
	case EBUSY:		printf("EBUSY");		break;
	case EINTR:		printf("EINTR");		break;
	case EINVAL:	printf("EINVAL");		break;
	case EMFILE:	printf("EMFILE");		break;
	default:		printf("Unknown");		break;
	}
	printf("\nSee the man page of dup2(2)\n");
}

void execvp_error(pid_t pid, const char *comm) {
	printf("[pid: %5d] error when execv(%s, ...) errno: ", pid, comm);
	switch(errno) {
	case E2BIG:			printf("E2BIG");		break;
	case EACCES:		printf("EACCES");		break;
	case EFAULT:		printf("EFAULT");		break;
	case EINVAL:		printf("EINVAL");		break;
	case EIO:			printf("EIO");			break;
	case EISDIR:		printf("EISDIR");		break;
	case ELIBBAD:		printf("ELIBBAD");		break;
	case ELOOP:			printf("ELOOP");		break;
	case EMFILE:		printf("EMFILE");		break;
	case ENAMETOOLONG:	printf("ENAMETOOLONG");	break;
	case ENFILE:		printf("ENFILE");		break;
	case ENOENT:		printf("ENOENT");		break;
	case ENOEXEC:		printf("ENOEXEC");		break;
	case ENOMEM:		printf("ENOMEN");		break;
	case ENOTDIR:		printf("ENOTDIR");		break;
	case EPERM:			printf("EPERM");		break;
	case ETXTBSY:		printf("ETXTBSY");		break;
	default:			printf("Unknown");		break;
	}
	printf("\nSee the man page of execve(2)\n");
}
void wait_error() {
	if (errno == ECHILD) 
		printf("Error: there is no unwaited-for children.\n");
	else if (errno == EINVAL)
		printf("The @options argument was invalid.\n");
	else
		printf("Unknown error\n");
}
#else
inline void dup2_error(int old_fd, int new_fd, int bench_num, const char *bench_name) {}
inline void execv_error(pid_t pid, const char *comm) {}
inline void wait_error() {}
#endif
