#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_COMMANDS 10

int main(void)
{
    char *commands[MAX_COMMANDS][4] = {
        {"cat", "file.txt", NULL},
        {"sort", NULL},
        {"head", "-5", NULL}
    };

    int command_count = 3;
    int pipes[MAX_COMMANDS - 1][2];

    for (int i = 0; i < command_count - 1; i++) {
        if (pipe(pipes[i]) == -1) {
            perror("pipe");
            return 1;
        }
    }
    pid_t pids[MAX_COMMANDS];

    for (int i = 0; i < command_count; i++) {
        pids[i] = fork();

        if (pids[i] == -1) {
            perror("fork");
            return 1;
        }

        if (pids[i] == 0) {
            if (i > 0) {
                if (dup2(pipes[i - 1][0], STDIN_FILENO) == -1) {
                    perror("dup2 input");
                    _exit(1);
                }
            }
            if (i < command_count - 1) {
                if (dup2(pipes[i][1], STDOUT_FILENO) == -1) {
                    perror("dup2 output");
                    _exit(1);
                }
            }
            for (int j = 0; j < command_count - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            execvp(commands[i][0], commands[i]);
            perror("execvp");
            _exit(127);
        }
    }
    for (int i = 0; i < command_count - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    for (int i = 0; i < command_count; i++) {
        waitpid(pids[i], NULL, 0);
    }   
    return 0;
}