
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#define WINDOW_WIDTH 500
#define WINDOW_HEIGHT 300
#define LINE_HEIGHT 20
#define HISTORY_TOP 40
#define HISTORY_BOTTOM 240
#define PROMPT_Y 280
#define VISIBLE_LINES (((HISTORY_BOTTOM - HISTORY_TOP) / LINE_HEIGHT) + 1)

int append_output(char **output, int *output_capacity,
                  int *output_length, const char *text){
    int text_length = (int)strlen(text);

    if (*output_length + text_length + 1 > *output_capacity){
        int new_capacity = *output_capacity * 2;

        while (*output_length + text_length + 1 > new_capacity){
            new_capacity *= 2;
        }

        char *temp = realloc(*output, (size_t)new_capacity);

        if (temp == NULL){
            perror("realloc");
            return -1;
        }

        *output = temp;
        *output_capacity = new_capacity;
    }

    memcpy(*output + *output_length, text, (size_t)text_length);
    *output_length += text_length;
    (*output)[*output_length] = '\0';

    return 0;
}

int count_output_lines(char *output, int output_length){
    int lines = 0;

    for (int i = 0; i < output_length; i++){
        if (output[i] == '\n'){
            lines++;
        }
    }

    if (output_length > 0 && output[output_length - 1] != '\n'){
        lines++;
    }

    return lines;
}

void draw_output(Display *display, Window window,
                 char *output, int output_length, int scroll_offset){
    int total_lines = count_output_lines(output, output_length);

    int first_line = total_lines - VISIBLE_LINES - scroll_offset;

    if (first_line < 0){
        first_line = 0;
    }

    int last_line = first_line + VISIBLE_LINES;

    if (last_line > total_lines){
        last_line = total_lines;
    }

    int visible_count = last_line - first_line;
    int y = HISTORY_BOTTOM - (visible_count - 1) * LINE_HEIGHT;

    int start = 0;
    int line_number = 0;

    while (start < output_length && line_number < last_line){
        int end = start;

        while (end < output_length && output[end] != '\n'){
            end++;
        }

        if (line_number >= first_line){
            XDrawString(
                display,
                window,
                DefaultGC(display, 0),
                20,
                y,
                output + start,
                end - start
            );

            y += LINE_HEIGHT;
        }

        line_number++;
        start = end + 1;
    }
}

void draw_input(Display *display, Window window, char *input, int length,
                char *output, int output_length, int scroll_offset){
    XClearWindow(display, window);

    XDrawString(
        display,
        window,
        DefaultGC(display, 0),
        20,
        PROMPT_Y,
        "Terminator>",
        12
    );

    XDrawString(
        display,
        window,
        DefaultGC(display, 0),
        120,
        PROMPT_Y,
        input,
        length
    );

    draw_output(display, window, output, output_length, scroll_offset);
}

int run_command(char *args[], char **output,
                int *output_capacity, int *output_length){
    int pipefd[2];

    if (pipe(pipefd) == -1){
        perror("pipe");
        return -1;
    }

    pid_t pid = fork();

    if (pid == -1){
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0){
        close(pipefd[0]);

        if (dup2(pipefd[1], STDOUT_FILENO) == -1){
            perror("dup2 stdout");
            _exit(1);
        }

        if (dup2(pipefd[1], STDERR_FILENO) == -1){
            perror("dup2 stderr");
            _exit(1);
        }

        close(pipefd[1]);

        execvp(args[0], args);

        perror("execvp");
        _exit(127);
    }
    else{
        close(pipefd[1]);

        char buffer[256];
        ssize_t bytes_read;

        while (1){
            bytes_read = read(pipefd[0], buffer, sizeof(buffer));

            if (bytes_read > 0){
                if (*output_length + bytes_read + 1 > *output_capacity){
                    int new_capacity = *output_capacity * 2;

                    while (*output_length + bytes_read + 1 > new_capacity){
                        new_capacity *= 2;
                    }

                    char *temp = realloc(*output, (size_t)new_capacity);

                    if (temp == NULL){
                        perror("realloc");
                        close(pipefd[0]);
                        waitpid(pid, NULL, 0);
                        return -1;
                    }

                    *output = temp;
                    *output_capacity = new_capacity;
                }

                memcpy(*output + *output_length, buffer, (size_t)bytes_read);
                *output_length += (int)bytes_read;
                (*output)[*output_length] = '\0';
            }
            else if (bytes_read == 0){
                break;
            }
            else if (errno == EINTR){
                continue;
            }
            else{
                perror("read");
                close(pipefd[0]);
                waitpid(pid, NULL, 0);
                return -1;
            }
        }

        close(pipefd[0]);

        int status;

        if (waitpid(pid, &status, 0) == -1){
            perror("waitpid");
            return -1;
        }
    }

    return 0;
}

int main(void){
    Display *display = XOpenDisplay(NULL);

    if (display == NULL){
        printf("Unable to connect to X11\n");
        return 1;
    }

    Window window = XCreateSimpleWindow(
        display,
        DefaultRootWindow(display),
        100,
        100,
        WINDOW_WIDTH,
        WINDOW_HEIGHT,
        1,
        BlackPixel(display, 0),
        WhitePixel(display, 0)
    );

    Atom delete_window = XInternAtom(
        display,
        "WM_DELETE_WINDOW",
        False
    );

    XSetWMProtocols(display, window, &delete_window, 1);

    XSelectInput(display, window,
                 ExposureMask | StructureNotifyMask | KeyPressMask | ButtonPressMask);

    XMapWindow(display, window);

    char input[100] = "";
    int length = 0;
    char history[100][100];
    int history_count = 0;
    int history_position = 0;

    int output_capacity = 4096;
    char *output = malloc((size_t)output_capacity);

    if (output == NULL){
        perror("malloc");
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return 1;
    }

    output[0] = '\0';

    int output_length = 0;

    /*
       scroll_offset = 0 means show the latest history.
       Larger values move backward into older history.
    */
    int scroll_offset = 0;

    while (1){
        XEvent event;

        XNextEvent(display, &event);

        if (event.type == Expose){
            draw_input(display, window, input, length,
                       output, output_length, scroll_offset);
        }
        if (event.type == ButtonPress){
            int total_lines = count_output_lines(output, output_length);
            int max_offset = total_lines - VISIBLE_LINES;

            if (max_offset < 0){
                max_offset = 0;
            }   

            if (event.xbutton.button == Button4){
                if (scroll_offset < max_offset){
                    scroll_offset++;
                }
            }
            else if (event.xbutton.button == Button5){
                if (scroll_offset > 0){
                    scroll_offset--;
                }
            }

            draw_input(display, window, input, length,output, output_length, scroll_offset);
        }

        if (event.type == KeyPress){
            int count = 0;
            KeySym keysym;
            char text[32];

            count = XLookupString(
                &event.xkey,
                text,
                sizeof(text),
                &keysym,
                NULL
            );
            
            if (keysym == XK_Up){
                if (history_position > 0){
                    history_position--;

                    strcpy(input, history[history_position]);
                    length = (int)strlen(input);
                }
            }
            else if (keysym == XK_Down){
                if (history_position < history_count){
                    history_position++;

                    if (history_position == history_count){
                        input[0] = '\0';
                        length = 0;
                    }
                    else{
                        strcpy(input, history[history_position]);
                        length = (int)strlen(input);
                    }
                }
            }
            else if (keysym == XK_Return || keysym == XK_KP_Enter){
                if (length > 0){
                    char command[100];
                    strcpy(command, input);
                    if (history_count < 100){
                        strcpy(history[history_count], input);
                        history_count++;
                    }

                    history_position = history_count;
                

                    char *args[10];
                    int arg_count = 0;
                    char *token = strtok(command, " \t");

                    while (token != NULL && arg_count < 9){
                        args[arg_count++] = token;
                        token = strtok(NULL, " \t");
                    }

                    args[arg_count] = NULL;

                    if (token != NULL){
                        append_output(&output, &output_capacity,
                                      &output_length,
                                      "Error: too many arguments.\n");
                    }
                    else if (arg_count > 0){
                        char command_line[120];

                        snprintf(command_line, sizeof(command_line),
                                 "Terminator> %s\n", input);

                        if (append_output(&output, &output_capacity,
                                          &output_length, command_line) == -1){
                            input[0] = '\0';
                            length = 0;

                            draw_input(display, window, input, length,
                                       output, output_length, scroll_offset);
                            continue;
                        }

                        if (strcmp(args[0], "cd") == 0){
                            char *directory = args[1];

                            if (directory == NULL){
                                directory = getenv("HOME");
                            }

                            if (directory == NULL){
                                append_output(&output, &output_capacity,
                                              &output_length,
                                              "cd: HOME not set\n");
                            }
                            else if (chdir(directory) == -1){
                                char error_message[200];

                                snprintf(error_message, sizeof(error_message),
                                         "cd: %s\n", strerror(errno));

                                append_output(&output, &output_capacity,
                                              &output_length, error_message);
                            }
                        }
                        else if (run_command(args, &output,
                                             &output_capacity,
                                             &output_length) == -1){
                            append_output(&output, &output_capacity,
                                          &output_length,
                                          "Error executing command.\n");
                        }
                    }
                }
                else{
                    append_output(&output, &output_capacity,&output_length, "Terminator>\n");
                }


                input[0] = '\0';
                length = 0;

                /* Return to the latest history after each command. */
                scroll_offset = 0;
            }
            else if (keysym == XK_BackSpace){
                if (length > 0){
                    length--;
                    input[length] = '\0';
                }
            }
            else if (count > 0 && length + count < (int)sizeof(input)){
                for (int i = 0; i < count; i++){
                    input[length] = text[i];
                    length++;
                }

                input[length] = '\0';
            }

            draw_input(display, window, input, length,
                       output, output_length, scroll_offset);
        }

        if (event.type == ClientMessage &&
            (Atom)event.xclient.data.l[0] == delete_window){
            break;
        }
    }

    free(output);

    XDestroyWindow(display, window);
    XCloseDisplay(display);

    return 0;
}
