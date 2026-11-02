
#include "multiwatch.h"

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

int main(void) {
    MultiWatchProcess process = {
        .pid = -1,
        .output_fd = -1,
        .command = NULL,
        .finished = 1
    };

    if (multiwatch_start(&process, "sleep 30") == -1) {
        perror("multiwatch_start");
        return 1;
    }

    pid_t child_pid = process.pid;

    printf("Started command with PID %ld\n", (long)child_pid);
    printf("Stopping command...\n");
    fflush(stdout);

    multiwatch_stop(&process, 1);

    int status;
    pid_t result = waitpid(child_pid, &status, WNOHANG);

    if (result == -1 && errno == ECHILD) {
        printf("PASS: child process was reaped\n");
    } else if (result == 0) {
        printf("FAIL: child process is still running\n");
        return 1;
    } else {
        printf("Child process has exited\n");
    }

    return 0;
}
