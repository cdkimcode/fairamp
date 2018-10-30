CC = gcc
HEADERS = src/error.h src/fairamp.h src/syscall_wrapper.h 
OBJS = src/fairamp.o src/sched_policy.o src/syscall_wrapper.o src/error.o src/ftrace.o src/estimation.o src/set_core.o
SRCS = src/fairamp.c src/sched_policy.c src/syscall_wrapper.c src/error.c src/ftrace.c src/estimation.c src/set_core.c
TARGET = fairamp
CFLAGS = -Wall -g -DCONFIG_TRIO -I../../include/

all: normal quiet

normal:
		$(CC) -DVERBOSE -o fairamp $(SRCS) $(CFLAGS) -lpthread -lm

quiet:
		$(CC) -o fairamp.quiet $(SRCS) $(CFLAGS) -lpthread -lm


dep:
		gccmakedep $(INC) $(SRCS)

objclean:
		rm -f $(OBJS)

clean:
		rm -f $(OBJS) $(TARGET) fairamp.quiet

new:
		$(MAKE) clean
		$(MAKE)

install:
		cp fairamp /usr/bin/
		cp fairamp.quiet /usr/bin/
