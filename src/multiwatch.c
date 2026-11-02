
#include "multiwatch.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>



int multiwatch_start(
    MultiWatchProcess *process,
    const char *command
) {
    int pipe_fd[2];

    if (process == NULL || command == NULL || command[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    if (pipe(pipe_fd) == -1) {
        perror("multiWatch: pipe");
        return -1;
    }

    pid_t pid = fork();

    if (pid == -1) {
        perror("multiWatch: fork");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return -1;
    }

    if (pid == 0) {
        /* Give this command its own process group. */
        if (setpgid(0, 0) == -1) {
            perror("multiWatch: setpgid");
            _exit(127);
        }

        close(pipe_fd[0]);

        if (dup2(pipe_fd[1], STDOUT_FILENO) == -1) {
            perror("multiWatch: dup2");
            _exit(127);
        }

        close(pipe_fd[1]);

        execl("/bin/sh", "sh", "-c", command, (char *)NULL);

        perror("multiWatch: execl");
        _exit(127);
    }

    /* The parent also attempts to establish the process group. */
    if (setpgid(pid, pid) == -1 &&
        errno != EACCES && errno != ESRCH) {
        perror("multiWatch: setpgid");
    }

    close(pipe_fd[1]);

    int flags = fcntl(pipe_fd[0], F_GETFL);

    if (flags == -1 ||
        fcntl(pipe_fd[0], F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("multiWatch: fcntl");
        close(pipe_fd[0]);
        kill(-pid, SIGTERM);

        while (waitpid(pid, NULL, 0) == -1 && errno == EINTR) {
        }

        return -1;
    }

    process->pid = pid;
    process->output_fd = pipe_fd[0];
    process->command = strdup(command);
    process->finished = 0;

    if (process->command == NULL) {
        close(process->output_fd);
        process->output_fd = -1;

        kill(-pid, SIGTERM);

        while (waitpid(pid, NULL, 0) == -1 && errno == EINTR) {
        }

        process->pid = -1;
        process->finished = 1;
        return -1;
    }

    return 0;
}



int multiwatch_read(
    MultiWatchProcess processes[],
    int process_count,
    char *buffer,
    int buffer_size,
    int *process_index
) {
    if (processes == NULL || buffer == NULL ||
        process_index == NULL || process_count <= 0 ||
        buffer_size < 2) {
        errno = EINVAL;
        return -1;
    }

    if (process_count > MULTIWATCH_MAX_COMMANDS) {
        process_count = MULTIWATCH_MAX_COMMANDS;
    }

    struct pollfd poll_fds[MULTIWATCH_MAX_COMMANDS];
    int process_indices[MULTIWATCH_MAX_COMMANDS];
    int active_count = 0;

    for (int i = 0; i < process_count; i++) {
        if (processes[i].finished || processes[i].output_fd < 0) {
            continue;
        }

        poll_fds[active_count].fd = processes[i].output_fd;
        poll_fds[active_count].events = POLLIN;
        poll_fds[active_count].revents = 0;
        process_indices[active_count] = i;
        active_count++;
    }

    if (active_count == 0) {
        return 0;
    }

    int ready = poll(poll_fds, (nfds_t)active_count, 0);

    if (ready == -1) {
        if (errno == EINTR) {
            return 0;
        }

        perror("multiWatch: poll");
        return -1;
    }

    for (int i = 0; i < active_count; i++) {
        if (!(poll_fds[i].revents &
              (POLLIN | POLLHUP | POLLERR))) {
            continue;
        }

        int index = process_indices[i];

        ssize_t bytes_read = read(
            processes[index].output_fd,
            buffer,
            (size_t)(buffer_size - 1)
        );

        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            *process_index = index;
            return (int)bytes_read;
        }

        if (bytes_read == 0) {
            close(processes[index].output_fd);
            processes[index].output_fd = -1;
            processes[index].finished = 1;
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK ||
            errno == EINTR) {
            continue;
        }

        perror("multiWatch: read");
        close(processes[index].output_fd);
        processes[index].output_fd = -1;
        processes[index].finished = 1;
    }

    return 0;
}



void multiwatch_stop(
    MultiWatchProcess processes[],
    int process_count
) {
    if (processes == NULL || process_count <= 0) {
        return;
    }

    if (process_count > MULTIWATCH_MAX_COMMANDS) {
        process_count = MULTIWATCH_MAX_COMMANDS;
    }

    /* Terminate every active command process group first. */
    for (int i = 0; i < process_count; i++) {
        if (processes[i].pid > 0 && !processes[i].finished) {
            if (kill(-processes[i].pid, SIGTERM) == -1 &&
                errno != ESRCH) {
                perror("multiWatch: kill");
            }
        }
    }

    /* Reap children and release resources. */
    for (int i = 0; i < process_count; i++) {
        if (processes[i].pid > 0) {
            while (waitpid(processes[i].pid, NULL, 0) == -1) {
                if (errno == EINTR) {
                    continue;
                }

                if (errno != ECHILD) {
                    perror("multiWatch: waitpid");
                }

                break;
            }
        }

        if (processes[i].output_fd >= 0) {
            close(processes[i].output_fd);
            processes[i].output_fd = -1;
        }

        free(processes[i].command);
        processes[i].command = NULL;

        processes[i].pid = -1;
        processes[i].finished = 1;
    }
}

int multiwatch_active_count(
    MultiWatchProcess processes[],
    int process_count
) {
    if (processes == NULL || process_count <= 0) {
        return 0;
    }

    if (process_count > MULTIWATCH_MAX_COMMANDS) {
        process_count = MULTIWATCH_MAX_COMMANDS;
    }

    int active = 0;

    for (int i = 0; i < process_count; i++) {
        if (!processes[i].finished && processes[i].output_fd >= 0) {
            active++;
        }
    }

    return active;
}
