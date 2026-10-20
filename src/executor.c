#include "executor.h"
#include "output_buffer.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
/* Execute one command and capture its stdout and stderr. */
int run_command(char *args[],
                char **output, int *output_capacity,
                int *output_length)
{
    int output_pipe[2];

    if (pipe(output_pipe) == -1) {
        perror("pipe");
        return -1;
    }

    pid_t pid = fork();

    if (pid == -1) {
        perror("fork");
        close(output_pipe[0]);
        close(output_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        close(output_pipe[0]);

        if (dup2(output_pipe[1], STDOUT_FILENO) == -1 ||
            dup2(output_pipe[1], STDERR_FILENO) == -1) {
            _exit(1);
        }

        close(output_pipe[1]);

        execvp(args[0], args);

        perror("execvp");
        _exit(127);
    }

    close(output_pipe[1]);

    char buffer[257];
    ssize_t bytes_read;

    while (1) {
        bytes_read = read(output_pipe[0], buffer, sizeof(buffer) - 1);

        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';

            if (append_output(output, output_capacity,
                              output_length, buffer) == -1) {
                close(output_pipe[0]);
                waitpid(pid, NULL, 0);
                return -1;
            }
        } else if (bytes_read == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            perror("read");
            close(output_pipe[0]);
            waitpid(pid, NULL, 0);
            return -1;
        }
    }

    close(output_pipe[0]);

    
    int status;

    while (waitpid(pid, &status, 0) == -1) {
        if (errno == EINTR) {
            continue;
        }

        perror("waitpid");
        return -1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return 1;

}
/*
 * Execute a pipeline containing two or more commands.
 *
 * Example:
 * ls | sort | uniq | wc -l
 */
int run_pipeline(char *commands[][MAX_PIPE_ARGS], int command_count,
                 char **output, int *output_capacity,
                 int *output_length)
{
    int pipes[MAX_PIPE_COMMANDS - 1][2];
    int output_pipe[2];
    pid_t pids[MAX_PIPE_COMMANDS];
    int started = 0;

    if (command_count < 2 || command_count > MAX_PIPE_COMMANDS) {
        return -1;
    }

    /* Create the pipes connecting the commands. */
    for (int i = 0; i < command_count - 1; i++) {
        if (pipe(pipes[i]) == -1) {
            perror("pipe");

            for (int j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            return -1;
        }
    }

    /* This pipe sends the final output to the GUI. */
    if (pipe(output_pipe) == -1) {
        perror("output pipe");

        for (int i = 0; i < command_count - 1; i++) {
            close(pipes[i][0]);
            close(pipes[i][1]);
        }

        return -1;
    }

    /* Start one child process for every command. */
    for (int i = 0; i < command_count; i++) {
        pids[i] = fork();

        if (pids[i] == -1) {
            perror("fork");

            for (int j = 0; j < command_count - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            close(output_pipe[0]);
            close(output_pipe[1]);

            for (int j = 0; j < started; j++) {
                kill(pids[j], SIGTERM);
            }

            for (int j = 0; j < started; j++) {
                while (waitpid(pids[j], NULL, 0) == -1 &&
                       errno == EINTR) {
                }
            }

            return -1;
        }

        if (pids[i] == 0) {
            /* Read input from the previous command. */
            if (i > 0) {
                if (dup2(pipes[i - 1][0], STDIN_FILENO) == -1) {
                    _exit(1);
                }
            }

            /* Send output to the next command. */
            if (i < command_count - 1) {
                if (dup2(pipes[i][1], STDOUT_FILENO) == -1) {
                    _exit(1);
                }
            } else {
                /* The last command sends output to the GUI. */
                if (dup2(output_pipe[1], STDOUT_FILENO) == -1) {
                    _exit(1);
                }
            }

            /* Capture errors from every command in the GUI. */
            if (dup2(output_pipe[1], STDERR_FILENO) == -1) {
                _exit(1);
            }

            /* Close original descriptors after dup2(). */
            for (int j = 0; j < command_count - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            close(output_pipe[0]);
            close(output_pipe[1]);

            execvp(commands[i][0], commands[i]);

            perror("execvp");
            _exit(127);
        }

        started++;
    }

    /* The parent does not use the pipeline's internal pipes. */
    for (int i = 0; i < command_count - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    close(output_pipe[1]);

    /* Read the final output without assuming it fits in one buffer. */
    char buffer[257];
    ssize_t bytes_read;
    int read_error = 0;

    while (1) {
        bytes_read = read(output_pipe[0], buffer, sizeof(buffer) - 1);

        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';

            if (append_output(output, output_capacity,
                              output_length, buffer) == -1) {
                read_error = 1;
                break;
            }
        } else if (bytes_read == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            perror("read");
            read_error = 1;
            break;
        }
    }

    close(output_pipe[0]);

    if (read_error) {
        for (int i = 0; i < command_count; i++) {
            kill(pids[i], SIGTERM);
        }
    }

    
    int wait_error = 0;
    int last_status = 0;

    for (int i = 0; i < command_count; i++) {
        int status;

        while (waitpid(pids[i], &status, 0) == -1) {
            if (errno == EINTR) {
                continue;
            }

            perror("waitpid");
            wait_error = 1;
            status = -1;
            break;
        }

        if (i == command_count - 1) {
            last_status = status;
        }
    }

    if (read_error || wait_error) {
        return -1;
    }

    if (WIFEXITED(last_status)) {
        return WEXITSTATUS(last_status);
    }

    if (WIFSIGNALED(last_status)) {
        return 128 + WTERMSIG(last_status);
    }

    return 1;

}

int run_command_redirected(
    char *args[],
    const char *input_file,
    const char *output_file,
    char **output,
    int *output_capacity,
    int *output_length
)
{
    int pipefd[2];

    if (pipe(pipefd) == -1) {
        perror("pipe");
        return -1;
    }

    pid_t pid = fork();

    if (pid == -1) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);

        /* Capture stderr for display in the GUI. */
        if (dup2(pipefd[1], STDERR_FILENO) == -1) {
            _exit(1);
        }

        /* Handle input redirection. */
        if (input_file != NULL) {
            int fd = open(input_file, O_RDONLY);

            if (fd == -1) {
                perror(input_file);
                _exit(1);
            }

            if (dup2(fd, STDIN_FILENO) == -1) {
                perror("dup2 stdin");
                close(fd);
                _exit(1);
            }

            close(fd);
        }

        /* Handle output redirection OR capture stdout. */
        if (output_file != NULL) {
            int fd = open(
                output_file,
                O_WRONLY | O_CREAT | O_TRUNC,
                0666
            );

            if (fd == -1) {
                perror(output_file);
                _exit(1);
            }

            if (dup2(fd, STDOUT_FILENO) == -1) {
                perror("dup2 stdout");
                close(fd);
                _exit(1);
            }

            close(fd);
        } else {
            if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
                _exit(1);
            }
        }

        /* IMPORTANT: close the pipe only AFTER both dup2 calls. */
        close(pipefd[1]);

        execvp(args[0], args);

        perror(args[0]);
        _exit(127);
    }

    close(pipefd[1]);

    char buffer[4096];
    ssize_t bytes_read;
    int read_error = 0;

    for (;;) {
        bytes_read = read(pipefd[0], buffer, sizeof(buffer));

        if (bytes_read > 0) {
            char chunk[4097];

            memcpy(chunk, buffer, (size_t)bytes_read);
            chunk[bytes_read] = '\0';

            if (append_output(
                    output,
                    output_capacity,
                    output_length,
                    chunk
                ) == -1) {
                read_error = 1;
                break;
            }
        } else if (bytes_read == -1 && errno == EINTR) {
            continue;
        } else {
            if (bytes_read == -1) {
                perror("read");
                read_error = 1;
            }
            break;
        }
    }

    close(pipefd[0]);

    int status;

    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            perror("waitpid");
            return -1;
        }
    }

    if (read_error) {
        return -1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return 1;
}
