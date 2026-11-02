
#ifndef MULTIWATCH_H
#define MULTIWATCH_H

#include <sys/types.h>

#define MULTIWATCH_MAX_COMMANDS 10
#define MULTIWATCH_BUFFER_SIZE 4096

typedef struct {
    pid_t pid;
    int output_fd;
    char *command;
    int finished;
} MultiWatchProcess;

/*
 * Start one command and capture its output through a pipe.
 * Returns 0 on success and -1 on failure.
 */
int multiwatch_start(
    MultiWatchProcess *process,
    const char *command
);

/*
 * Read output from active commands without busy-waiting.
 * Returns the number of bytes read, 0 if no bytes are available,
 * or -1 on error.
 */
 
 int multiwatch_read(
     MultiWatchProcess processes[],
     int process_count,
     char *buffer,
     int buffer_size,
     int *process_index
 );
 

/* Stop all active commands and release their resources. */
void multiwatch_stop(
    MultiWatchProcess processes[],
    int process_count
);


/* Return the number of commands that have not finished. */
int multiwatch_active_count(
    MultiWatchProcess processes[],
    int process_count
);

#endif
