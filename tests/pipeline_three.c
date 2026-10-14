
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

int main(void)
{
    int pipe1[2];
    int pipe2[2];

    if (pipe(pipe1) == -1 || pipe(pipe2) == -1) {
        perror("pipe");
        return 1;
    }

    /* Child 1: cat file.txt */
    pid_t first = fork();

    if (first == -1) {
        perror("fork");
        return 1;
    }

    if (first == 0) {
        close(pipe1[0]);
        close(pipe2[0]);
        close(pipe2[1]);

        if (dup2(pipe1[1], STDOUT_FILENO) == -1) {
            perror("dup2");
            _exit(1);
        }

        close(pipe1[1]);

        char *args[] = {"cat", "file.txt", NULL};
        execvp(args[0], args);
        perror("execvp cat");
        _exit(127);
    }

    /* Child 2: sort */
    pid_t second = fork();

    if (second == -1) {
        perror("fork");
        close(pipe1[0]);
        close(pipe1[1]);
        close(pipe2[0]);
        close(pipe2[1]);
        waitpid(first, NULL, 0);
        return 1;
    }

    if (second == 0) {
        close(pipe1[1]);
        close(pipe2[0]);

        if (dup2(pipe1[0], STDIN_FILENO) == -1 ||
            dup2(pipe2[1], STDOUT_FILENO) == -1) {
            perror("dup2");
            _exit(1);
        }

        close(pipe1[0]);
        close(pipe2[1]);

        char *args[] = {"sort", NULL};
        execvp(args[0], args);
        perror("execvp sort");
        _exit(127);
    }

    /* Child 3: head -5 */
    pid_t third = fork();

    if (third == -1) {
        perror("fork");
        close(pipe1[0]);
        close(pipe1[1]);
        close(pipe2[0]);
        close(pipe2[1]);
        waitpid(first, NULL, 0);
        waitpid(second, NULL, 0);
        return 1;
    }

    if (third == 0) {
        close(pipe1[0]);
        close(pipe1[1]);
        close(pipe2[1]);

        if (dup2(pipe2[0], STDIN_FILENO) == -1) {
            perror("dup2");
            _exit(1);
        }

        close(pipe2[0]);

        char *args[] = {"head", "-5", NULL};
        execvp(args[0], args);
        perror("execvp head");
        _exit(127);
    }

    /* Parent: close every pipe descriptor */
    close(pipe1[0]);
    close(pipe1[1]);
    close(pipe2[0]);
    close(pipe2[1]);

    waitpid(first, NULL, 0);
    waitpid(second, NULL, 0);
    waitpid(third, NULL, 0);

    return 0;
}
