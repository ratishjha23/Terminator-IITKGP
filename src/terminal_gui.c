
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define WINDOW_WIDTH 500
#define WINDOW_HEIGHT 300

#define LINE_HEIGHT 20
#define HISTORY_TOP 40
#define HISTORY_BOTTOM 240
#define PROMPT_Y 280
#define VISIBLE_LINES (((HISTORY_BOTTOM - HISTORY_TOP) / LINE_HEIGHT) + 1)

#define MAX_PIPE_COMMANDS 10
#define MAX_PIPE_ARGS 10
#define INPUT_SIZE 100

char history[100][INPUT_SIZE];
int history_count = 0;
int history_position = 0;
int scroll_offset = 0;

/* Add text to the dynamically growing output buffer. */
int append_output(char **output, int *output_capacity,
                  int *output_length, const char *text)
{
    int text_length = (int)strlen(text);

    if (*output_length + text_length + 1 > *output_capacity) {
        int new_capacity = *output_capacity;

        while (*output_length + text_length + 1 > new_capacity) {
            if (new_capacity > 100000000 / 2) {
                return -1;
            }
            new_capacity *= 2;
        }

        char *new_output = realloc(*output, (size_t)new_capacity);

        if (new_output == NULL) {
            return -1;
        }

        *output = new_output;
        *output_capacity = new_capacity;
    }

    memcpy(*output + *output_length, text, (size_t)text_length);

    *output_length += text_length;
    (*output)[*output_length] = '\0';

    return 0;
}

/* Count the lines currently stored in the output buffer. */
int count_output_lines(const char *output, int output_length)
{
    if (output_length == 0) {
        return 0;
    }

    int lines = 0;

    for (int i = 0; i < output_length; i++) {
        if (output[i] == '\n') {
            lines++;
        }
    }

    if (output[output_length - 1] != '\n') {
        lines++;
    }

    return lines;
}


/* Draw the visible portion of the command output. */
void draw_output(Display *display, Window window, GC gc,
                 const char *output, int output_length)
{
    int total_lines = count_output_lines(output, output_length);

    int max_scroll = total_lines > VISIBLE_LINES
                         ? total_lines - VISIBLE_LINES
                         : 0;

    if (scroll_offset > max_scroll) {
        scroll_offset = max_scroll;
    }

    int first_line = total_lines - VISIBLE_LINES - scroll_offset;

    if (first_line < 0) {
        first_line = 0;
    }

    /* Count how many lines will actually be displayed. */
    int visible_count = total_lines - first_line;

    if (visible_count > VISIBLE_LINES) {
        visible_count = VISIBLE_LINES;
    }

    /*
     * Short output starts near the bottom.
     * Longer output fills the available area upward.
     */
    int starting_y = HISTORY_BOTTOM -
                     (visible_count - 1) * LINE_HEIGHT;

    int current_line = 0;
    int start = 0;

    for (int i = 0; i <= output_length; i++) {
        if (i == output_length || output[i] == '\n') {
            if (current_line >= first_line &&
                current_line < first_line + VISIBLE_LINES) {

                int line_length = i - start;

                if (line_length > 0) {
                    XDrawString(
                        display, window, gc,
                        10,
                        starting_y +
                            (current_line - first_line) * LINE_HEIGHT,
                        output + start,
                        line_length
                    );
                }
            }

            current_line++;
            start = i + 1;
        }
    }
}

/* Redraw the complete GUI. */
void draw_input(Display *display, Window window, GC gc,
                const char *input, int length,
                const char *output, int output_length)
{
    XClearWindow(display, window);

    draw_output(display, window, gc, output, output_length);

    XDrawString(display, window, gc,
                10, PROMPT_Y, "Terminator>", 11);

    XDrawString(display, window, gc,
                120, PROMPT_Y, input, length);

    XFlush(display);
}

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

    return 0;
}

/*
 * Split one pipeline stage into arguments.
 *
 * Example:
 * "wc -l" becomes {"wc", "-l", NULL}.
 */
int parse_command(char *command, char *args[MAX_PIPE_ARGS])
{
    int arg_count = 0;
    char *saveptr = NULL;

    char *token = strtok_r(command, " \t", &saveptr);

    while (token != NULL && arg_count < MAX_PIPE_ARGS - 1) {
        args[arg_count++] = token;
        token = strtok_r(NULL, " \t", &saveptr);
    }

    if (token != NULL) {
        return -1;
    }

    args[arg_count] = NULL;

    return arg_count;
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

    for (int i = 0; i < command_count; i++) {
        while (waitpid(pids[i], NULL, 0) == -1) {
            if (errno == EINTR) {
                continue;
            }

            perror("waitpid");
            wait_error = 1;
            break;
        }
    }

    return (read_error || wait_error) ? -1 : 0;
}

int main(void)
{
    Display *display = XOpenDisplay(NULL);

    if (display == NULL) {
        fprintf(stderr, "Cannot open X display.\n");
        return 1;
    }

    int screen = DefaultScreen(display);

    Window window = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        100, 100,
        WINDOW_WIDTH, WINDOW_HEIGHT,
        1,
        BlackPixel(display, screen),
        WhitePixel(display, screen)
    );

    XStoreName(display, window, "Terminator - Custom Terminal");

    XSelectInput(display, window,
                 ExposureMask |
                 StructureNotifyMask |
                 KeyPressMask |
                 ButtonPressMask);

    Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wm_delete, 1);

    XMapWindow(display, window);

    GC gc = XCreateGC(display, window, 0, NULL);

    char input[INPUT_SIZE] = {0};
    int length = 0;

    int output_capacity = 4096;
    int output_length = 0;

    char *output = malloc((size_t)output_capacity);

    if (output == NULL) {
        fprintf(stderr, "Cannot allocate output buffer.\n");
        XFreeGC(display, gc);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return 1;
    }

    output[0] = '\0';

    XEvent event;
    int running = 1;

    while (running) {
        XNextEvent(display, &event);

        if (event.type == Expose) {
            draw_input(display, window, gc,
                       input, length, output, output_length);
        }

        else if (event.type == ClientMessage) {
            if ((Atom)event.xclient.data.l[0] == wm_delete) {
                running = 0;
            }
        }

        else if (event.type == ButtonPress) {
            if (event.xbutton.button == Button4) {
                if (scroll_offset < count_output_lines(output, output_length)) {
                    scroll_offset++;
                }
            } else if (event.xbutton.button == Button5) {
                if (scroll_offset > 0) {
                    scroll_offset--;
                }
            }

            draw_input(display, window, gc,
                       input, length, output, output_length);
        }

        else if (event.type == KeyPress) {
            KeySym key;
            char key_text[32];

            int key_length = XLookupString(
                &event.xkey,
                key_text,
                sizeof(key_text),
                &key,
                NULL
            );

            /* Enter: execute the command. */
            if (key == XK_Return || key == XK_KP_Enter) {
                if (length == 0) {
                    append_output(&output, &output_capacity,
                      &output_length, "Terminator>\n");

                    scroll_offset = 0;
                }
                if (length > 0) {
                    input[length] = '\0';

                    char command[INPUT_SIZE];
                    strcpy(command, input);

                    int has_pipeline = strchr(command, '|') != NULL;

                    /* Save non-empty commands in history. */
                    if (history_count < 100) {
                        strcpy(history[history_count], input);
                        history_count++;
                    } else {
                        for (int i = 1; i < 100; i++) {
                            strcpy(history[i - 1], history[i]);
                        }

                        strcpy(history[99], input);
                    }

                    history_position = history_count;

                    char command_line[INPUT_SIZE + 20];

                    snprintf(command_line, sizeof(command_line),
                             "Terminator> %s\n", input);

                    if (append_output(&output, &output_capacity,
                                      &output_length, command_line) == -1) {
                        running = 0;
                        continue;
                    }

                    scroll_offset = 0;

                    if (has_pipeline) {
                        char *commands[MAX_PIPE_COMMANDS][MAX_PIPE_ARGS] = {
                            {NULL}
                        };

                        int command_count = 0;
                        int parse_error = 0;

                        /*
                         * Split stages manually so empty stages such as
                         * "ls || wc" can be detected.
                         */
                        char *stage = command;
                        char *separator;

                        while (1) {
                            separator = strchr(stage, '|');

                            if (separator != NULL) {
                                *separator = '\0';
                            }

                            if (command_count >= MAX_PIPE_COMMANDS) {
                                parse_error = 1;
                                break;
                            }

                            int arg_count = parse_command(
                                stage, commands[command_count]
                            );

                            if (arg_count <= 0) {
                                parse_error = 1;
                                break;
                            }

                            command_count++;

                            if (separator == NULL) {
                                break;
                            }

                            stage = separator + 1;
                        }

                        if (command_count < 2) {
                            parse_error = 1;
                        }

                        if (parse_error) {
                            append_output(
                                &output, &output_capacity, &output_length,
                                "Error: invalid pipeline or too many "
                                "commands/arguments.\n"
                            );
                        } else {
                            if (run_pipeline(commands, command_count,
                                             &output, &output_capacity,
                                             &output_length) == -1) {
                                append_output(
                                    &output, &output_capacity,
                                    &output_length,
                                    "Error executing pipeline.\n"
                                );
                            }
                        }
                    }

                    else {
                        char *args[10];
                        int arg_count = 0;
                        char *saveptr = NULL;

                        char *token = strtok_r(command, " \t", &saveptr);

                        while (token != NULL && arg_count < 9) {
                            args[arg_count++] = token;
                            token = strtok_r(NULL, " \t", &saveptr);
                        }

                        args[arg_count] = NULL;

                        if (token != NULL) {
                            append_output(
                                &output, &output_capacity, &output_length,
                                "Error: too many arguments.\n"
                            );
                        }

                        else if (arg_count > 0) {
                            if (strcmp(args[0], "cd") == 0) {
                                const char *directory;

                                if (args[1] == NULL) {
                                    directory = getenv("HOME");
                                } else {
                                    directory = args[1];
                                }

                                if (directory == NULL) {
                                    append_output(
                                        &output, &output_capacity,
                                        &output_length,
                                        "Error: HOME is not set.\n"
                                    );
                                } else if (chdir(directory) == -1) {
                                    char error_message[INPUT_SIZE + 100];

                                    snprintf(
                                        error_message,
                                        sizeof(error_message),
                                        "cd: %s: %s\n",
                                        directory, strerror(errno)
                                    );

                                    append_output(
                                        &output, &output_capacity,
                                        &output_length, error_message
                                    );
                                }
                            }

                            else if (strcmp(args[0], "exit") == 0) {
                                running = 0;
                            }

                            else {
                                if (run_command(args,
                                                &output, &output_capacity,
                                                &output_length) == -1) {
                                    append_output(
                                        &output, &output_capacity,
                                        &output_length,
                                        "Error executing command.\n"
                                    );
                                }
                            }
                        }
                    }

                    length = 0;
                    input[0] = '\0';
                    history_position = history_count;
                }

                draw_input(display, window, gc,
                           input, length, output, output_length);
            }

            /* Backspace: delete one character. */
            else if (key == XK_BackSpace || key == XK_Delete) {
                if (length > 0) {
                    length--;
                    input[length] = '\0';
                }
            }

            /* Up arrow: previous command. */
            else if (key == XK_Up) {
                if (history_position > 0) {
                    history_position--;

                    strcpy(input, history[history_position]);
                    length = (int)strlen(input);
                }
            }

            /* Down arrow: next command. */
            else if (key == XK_Down) {
                if (history_position < history_count - 1) {
                    history_position++;

                    strcpy(input, history[history_position]);
                    length = (int)strlen(input);
                } else {
                    history_position = history_count;
                    length = 0;
                    input[0] = '\0';
                }
            }

            /* Ctrl+L: clear displayed output. */
            else if ((key == XK_l || key == XK_L) &&
                     (event.xkey.state & ControlMask)) {
                output_length = 0;
                output[0] = '\0';
                scroll_offset = 0;
            }

            /* Ctrl+D: exit when the input is empty. */
            else if (key == XK_d &&
                     (event.xkey.state & ControlMask) &&
                     length == 0) {
                running = 0;
            }

            /* Append ordinary typed characters. */
            else if (key_length > 0 &&
                     !(event.xkey.state & ControlMask) &&
                     length < INPUT_SIZE - 1) {
                for (int i = 0; i < key_length &&
                                length < INPUT_SIZE - 1; i++) {
                    if (key_text[i] >= ' ' && key_text[i] != 127) {
                        input[length++] = key_text[i];
                    }
                }

                input[length] = '\0';
            }

            draw_input(display, window, gc,
                       input, length, output, output_length);
        }
    }

    free(output);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    return 0;
}
