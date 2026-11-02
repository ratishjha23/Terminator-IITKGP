
#include "multiwatch.h"

#include <stdio.h>
#include <sys/wait.h>


int main(void) {
    MultiWatchProcess processes[2] = {
        {.pid = -1, .output_fd = -1, .command = NULL, .finished = 1},
        {.pid = -1, .output_fd = -1, .command = NULL, .finished = 1}
    };

    if (multiwatch_start(
            &processes[0],
            "echo FIRST; sleep 1; echo FIRST_DONE") == -1) {
        perror("multiwatch_start");
        return 1;
    }

    if (multiwatch_start(
            &processes[1],
            "echo SECOND; sleep 1; echo SECOND_DONE") == -1) {
        perror("multiwatch_start");
        multiwatch_stop(processes, 2);
        return 1;
    }

    char buffer[MULTIWATCH_BUFFER_SIZE];
    int remaining = 2;

    while (remaining > 0) {
        int process_index = -1;

        int bytes_read = multiwatch_read(
            processes,
            2,
            buffer,
            sizeof(buffer),
            &process_index
        );

        if (bytes_read < 0) {
            perror("multiwatch_read");
            multiwatch_stop(processes, 2);
            return 1;
        }

        if (bytes_read > 0) {
            printf("[%s] %s",
                   processes[process_index].command,
                   buffer);
            fflush(stdout);
        }

        for (int i = 0; i < 2; i++) {
            if (processes[i].finished && processes[i].pid > 0) {
                while (waitpid(processes[i].pid, NULL, 0) == -1) {
                    /* Retry if interrupted by a signal. */
                    break;
                }

                processes[i].pid = -1;
                remaining--;
            }
        }
    }

    multiwatch_stop(processes, 2);
    return 0;
}
