
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

#include "executor.h"
#include "output_buffer.h"
#include "parser.h"
#include "history.h"

#define WINDOW_WIDTH 500
#define WINDOW_HEIGHT 300

#define LINE_HEIGHT 20
#define HISTORY_TOP 40
#define HISTORY_BOTTOM 240
#define PROMPT_Y 280
#define VISIBLE_LINES (((HISTORY_BOTTOM - HISTORY_TOP) / LINE_HEIGHT) + 1)

#define INPUT_SIZE 100
#define MAX_TABS 10


#define TAB_BAR_HEIGHT 30
#define TAB_WIDTH 100

typedef struct {
    char input[INPUT_SIZE];
    int length;

    char *output;
    int output_capacity;
    int output_length;

    int history_position;
    int scroll_offset;

    char cwd[4096];
} Tab;

Tab tabs[MAX_TABS];
int tab_count = 0;
int active_tab = 0;
int last_status = 0;

#define MAX_JOBS 100


typedef struct {
    pid_t pid;
    pid_t pgid;
    char command[INPUT_SIZE];
    int running;
    int stopped;
} Job;


Job jobs[MAX_JOBS];
int job_count = 0;


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

    Tab *tab = &tabs[active_tab];

    if (tab->scroll_offset > max_scroll) {
        tab->scroll_offset = max_scroll;
    }

    int first_line = total_lines - VISIBLE_LINES
                     - tab->scroll_offset;

    if (first_line < 0) {
        first_line = 0;
    }

    int visible_count = total_lines - first_line;

    if (visible_count > VISIBLE_LINES) {
        visible_count = VISIBLE_LINES;
    }

    int starting_y = HISTORY_BOTTOM
                     - (visible_count - 1) * LINE_HEIGHT;

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
                        starting_y
                            + (current_line - first_line) * LINE_HEIGHT,
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

int initialize_tab(int index)
{
    Tab *tab = &tabs[index];

    tab->input[0] = '\0';
    tab->length = 0;

    tab->output_capacity = 4096;
    tab->output_length = 0;
    tab->output = malloc((size_t)tab->output_capacity);

    if (tab->output == NULL) {
        return -1;
    }

    tab->output[0] = '\0';

    tab->history_position = history_count;
    tab->scroll_offset = 0;

    if (getcwd(tab->cwd, sizeof(tab->cwd)) == NULL) {
        tab->cwd[0] = '\0';
    }

    return 0;
}

void close_tab(int index)
{
    if (index < 0 || index >= tab_count) {
        return;
    }

    free(tabs[index].output);

    for (int i = index; i < tab_count - 1; i++) {
        tabs[i] = tabs[i + 1];
    }

    tab_count--;

    if (tab_count == 0) {
        tab_count = 1;
        active_tab = 0;
        initialize_tab(0);
        return;
    }

    if (active_tab >= tab_count) {
        active_tab = tab_count - 1;
    } else if (active_tab > index) {
        active_tab--;
    }
}



void draw_tabs(Display *display, Window window, GC gc)
{
    for (int i = 0; i < tab_count; i++) {
        int x = i * TAB_WIDTH;

        XDrawRectangle(display, window, gc,
                       x, 0, TAB_WIDTH, TAB_BAR_HEIGHT);

        char label[32];
        snprintf(label, sizeof(label), "Tab %d", i + 1);

        XDrawString(display, window, gc,
                    x + 10, 20, label, (int)strlen(label));
        XDrawString(display, window, gc,
            x + TAB_WIDTH - 18, 20, "x", 1);
    }

    XDrawRectangle(display, window, gc,
                   tab_count * TAB_WIDTH, 0,
                   TAB_WIDTH, TAB_BAR_HEIGHT);

    XDrawString(display, window, gc,
                tab_count * TAB_WIDTH + 10, 20,
                "+", 1);
    
}

/* Redraw the complete GUI. */
void draw_input(Display *display, Window window, GC gc,
                const char *input, int length,
                const char *output, int output_length)
{
    XClearWindow(display, window);

    draw_tabs(display, window, gc);

    draw_output(display, window, gc, output, output_length);

    XDrawString(display, window, gc,
                10, PROMPT_Y, "Terminator>", 11);

    XDrawString(display, window, gc,
                120, PROMPT_Y, input, length);

    XFlush(display);
}


int run_background(char *args[], const char *command)
{
    if (job_count >= MAX_JOBS) {
        return -1;
    }

    pid_t pid = fork();

    if (pid == -1) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        /* Create a separate process group for this job. */
        if (setpgid(0, 0) == -1) {
            perror("setpgid");
            _exit(1);
        }

        execvp(args[0], args);
        perror("execvp");
        _exit(127);
    }

    /* The parent also attempts to set the child's process group. */
    if (setpgid(pid, pid) == -1) {
        perror("setpgid");
    }

    jobs[job_count].pid = pid;
    jobs[job_count].pgid = pid;

    snprintf(jobs[job_count].command,
             sizeof(jobs[job_count].command),
             "%s", command);

    jobs[job_count].running = 1;
    jobs[job_count].stopped = 0;
    job_count++;

    return (int)pid;
}



void update_job_status(void)
{
    for (int i = 0; i < job_count; i++) {
        if (!jobs[i].running) {
            continue;
        }

        int status;
        pid_t result = waitpid(
            jobs[i].pid,
            &status,
            WNOHANG | WUNTRACED | WCONTINUED
        );

        if (result == jobs[i].pid) {
            if (WIFSTOPPED(status)) {
                jobs[i].stopped = 1;
            }
            else if (WIFCONTINUED(status)) {
                jobs[i].stopped = 0;
            }
            else if (WIFEXITED(status) || WIFSIGNALED(status)) {
                jobs[i].running = 0;
                jobs[i].stopped = 0;
            }
        }
    }
}

int find_job(int job_number)
{
    if (job_number < 1 || job_number > job_count) {
        return -1;
    }

    return job_number - 1;
}


int read_script_line(FILE *script, char line[INPUT_SIZE])
{
    if (fgets(line, INPUT_SIZE, script) == NULL) {
        return 0;
    }

    line[strcspn(line, "\n")] = '\0';
    return 1;
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

    /* Initialize the first tab. */
    tab_count = 1;
    active_tab = 0;

    if (initialize_tab(0) == -1) {
        fprintf(stderr, "Cannot initialize first tab.\n");
        XFreeGC(display, gc);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return 1;
    }

    Tab *tab = &tabs[active_tab];

    XEvent event;
    int running = 1;

    while (running) {
        XNextEvent(display, &event);

        if (event.type == Expose) {
            tab = &tabs[active_tab];

            draw_input(display, window, gc,
                       tab->input, tab->length,
                       tab->output, tab->output_length);
        }

        else if (event.type == ClientMessage) {
            if ((Atom)event.xclient.data.l[0] == wm_delete) {
                running = 0;
            }
        }

        else if (event.type == ButtonPress) {
            int click_x = event.xbutton.x;
            int click_y = event.xbutton.y;

            /* Handle clicks on the tab bar. */
            if (click_y < TAB_BAR_HEIGHT) {
                int clicked_tab = click_x / TAB_WIDTH;

                if (clicked_tab < tab_count) {
                    int close_x =
                        (clicked_tab + 1) * TAB_WIDTH - 20;

                    if (click_x >= close_x) {
                        close_tab(clicked_tab);
                    }
                    else {
                        active_tab = clicked_tab;

                        if (chdir(tabs[active_tab].cwd) == -1) {
                            /* Keep the current directory if restoration fails. */
                        }
                    }
                }

                else if (clicked_tab == tab_count &&
                         tab_count < MAX_TABS) {
                    int new_index = tab_count;

                    if (initialize_tab(new_index) == 0) {
                        tab_count++;
                        active_tab = new_index;
                    }
                }
            }

            /* Handle mouse-wheel scrolling outside the tab bar. */
            else if (event.xbutton.button == Button4) {
                tab = &tabs[active_tab];

                if (tab->scroll_offset <
                    count_output_lines(tab->output,
                                       tab->output_length)) {
                    tab->scroll_offset++;
                }
            }

            else if (event.xbutton.button == Button5) {
                tab = &tabs[active_tab];

                if (tab->scroll_offset > 0) {
                    tab->scroll_offset--;
                }
            }

            /* Refresh the pointer after switching or closing tabs. */
            tab = &tabs[active_tab];

            draw_input(display, window, gc,
                       tab->input, tab->length,
                       tab->output, tab->output_length);
        }

        else if (event.type == KeyPress) {
            tab = &tabs[active_tab];

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
                if (tab->length == 0) {
                    append_output(&tab->output,
                                  &tab->output_capacity,
                                  &tab->output_length,
                                  "Terminator>\n");

                    tab->scroll_offset = 0;
                }

                if (tab->length > 0) {
                    tab->input[tab->length] = '\0';
                    char command[INPUT_SIZE];
                    int command_length = 0;

                    for (int i = 0; tab->input[i] != '\0'; i++) {
                        if (tab->input[i] == '$' &&
                            tab->input[i + 1] == '?') {
                            char status_text[12];

                            snprintf(status_text, sizeof(status_text),
                            "%d", last_status);

                            int status_length = (int)strlen(status_text);

                            if (command_length + status_length >= INPUT_SIZE) {
                                break;
                            }

                            strcpy(&command[command_length], status_text);
                            command_length += status_length;
                            i++;
                        }
                        else {
                            if (command_length >= INPUT_SIZE - 1) {
                                break;
                            }

                            command[command_length++] = tab->input[i];
                        }   
                    }

                    command[command_length] = '\0';


                    int has_pipeline = strchr(command, '|') != NULL;
                        
                    int background = 0;
                    int background_length = (int)strlen(command);
                    
                    while (background_length > 0 &&
                            (command[background_length - 1] == ' ' ||
                            command[background_length - 1] == '\t')) {
                        command[--background_length] = '\0';
                    }
                    
                    if (background_length > 0 &&
                        command[background_length - 1] == '&') {
                        background = 1;
                        command[--background_length] = '\0';
                    
                        while (background_length > 0 &&
                                (command[background_length - 1] == ' ' ||
                                command[background_length - 1] == '\t')) {
                            command[--background_length] = '\0';
                        }
                    }
                        

                    history_add(tab->input);
                    tab->history_position = history_count;

                    char command_line[INPUT_SIZE + 20];

                    snprintf(command_line, sizeof(command_line),
                             "Terminator> %s\n", tab->input);

                    if (append_output(&tab->output,
                                      &tab->output_capacity,
                                      &tab->output_length,
                                      command_line) == -1) {
                        running = 0;
                        continue;
                    }

                    tab->scroll_offset = 0;

                    if (has_pipeline) {
                        char *commands[MAX_PIPE_COMMANDS][MAX_PIPE_ARGS] = {
                            {NULL}
                        };

                        int command_count = 0;
                        int parse_error = 0;

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
                                &tab->output,
                                &tab->output_capacity,
                                &tab->output_length,
                                "Error: invalid pipeline or too many "
                                "commands/arguments.\n"
                            );
                        }
                        
                        else {
                            last_status = run_pipeline(
                            commands, command_count,
                            &tab->output,
                            &tab->output_capacity,
                            &tab->output_length
                            );

                            if (last_status == -1) {
                                append_output(
                                    &tab->output,
                                    &tab->output_capacity,
                                    &tab->output_length,
                                    "Error executing pipeline.\n"
                                );

                            last_status = 1;
                            }
                        }
                    }

                    else {
                        char *args[10];
                        int arg_count = 0;

                        char *token = strtok(command, " \t");

                        while (token != NULL && arg_count < 9) {
                            args[arg_count++] = token;
                            token = strtok(NULL, " \t");
                        }

                        args[arg_count] = NULL;

                        if (token != NULL) {
                            append_output(
                                &tab->output,
                                &tab->output_capacity,
                                &tab->output_length,
                                "Error: too many arguments.\n"
                            );
                        }

                        else if (arg_count > 0) {
                            if (strcmp(args[0], "cd") == 0) {
                                const char *directory;

                                if (args[1] == NULL) {
                                    directory = getenv("HOME");
                                }
                                else {
                                    directory = args[1];
                                }

                                if (directory == NULL) {
                                    append_output(
                                        &tab->output,
                                        &tab->output_capacity,
                                        &tab->output_length,
                                        "Error: HOME is not set.\n"
                                    );
                                }

                                else if (chdir(directory) == -1) {
                                    char error_message[INPUT_SIZE + 100];

                                    snprintf(
                                        error_message,
                                        sizeof(error_message),
                                        "cd: %s: %s\n",
                                        directory, strerror(errno)
                                    );

                                    append_output(
                                        &tab->output,
                                        &tab->output_capacity,
                                        &tab->output_length,
                                        error_message
                                    );
                                }

                                else {
                                    if (getcwd(tab->cwd,
                                               sizeof(tab->cwd)) == NULL) {
                                        tab->cwd[0] = '\0';
                                    }
                                }
                            }
                            
                            else if (strcmp(args[0], "source") == 0) {
                                if (args[1] == NULL) {
                                    append_output(&tab->output,
                                                &tab->output_capacity,
                                                &tab->output_length,
                                                "source: missing filename\n");
                                    last_status = 1;
                                } else {
                                    FILE *script = fopen(args[1], "r");

                                    if (script == NULL) {
                                        char error_message[INPUT_SIZE + 100];

                                        snprintf(error_message, sizeof(error_message),
                                                "source: %s: %s\n",
                                                args[1], strerror(errno));

                                        append_output(&tab->output,
                                                    &tab->output_capacity,
                                                    &tab->output_length,
                                                    error_message);
                                        last_status = 1;
                                    } else {
                                        char line[INPUT_SIZE];

                                        while (read_script_line(script, line)) {
                                            if (line[0] == '\0') {
                                                continue;
                                            }

                                            char *script_args[10];
                                            int count = 0;

                                            char *token = strtok(line, " \t");

                                            while (token != NULL && count < 9) {
                                                script_args[count++] = token;
                                                token = strtok(NULL, " \t");
                                            }

                                            script_args[count] = NULL;

                                            if (count == 0) {
                                                continue;
                                            }

                                            if (strcmp(script_args[0], "cd") == 0) {
                                                const char *directory =
                                                    script_args[1] != NULL
                                                    ? script_args[1] : getenv("HOME");

                                                if (directory == NULL || chdir(directory) == -1) {
                                                    append_output(&tab->output,
                                                                &tab->output_capacity,
                                                                &tab->output_length,
                                                                "source: cd failed\n");
                                                    last_status = 1;
                                                } else {
                                                    if (getcwd(tab->cwd, sizeof(tab->cwd)) == NULL) {
                                                        tab->cwd[0] = '\0';
                                                    }
                                                    last_status = 0;
                                                }
                                            } else if (strcmp(script_args[0], "exit") == 0) {
                                                running = 0;
                                                break;
                                            } else {
                                                last_status = run_command(
                                                    script_args,
                                                    &tab->output,
                                                    &tab->output_capacity,
                                                    &tab->output_length
                                                );

                                                if (last_status == -1) {
                                                    last_status = 1;
                                                    append_output(&tab->output,
                                                                &tab->output_capacity,
                                                                &tab->output_length,
                                                                "source: command execution failed\n");
                                                }
                                            }
                                        }

                                        fclose(script);
                                    }
                                }
                            }
                            else if (strcmp(args[0], "history") == 0) {
                                for (int i = 0; i < history_count; i++) {
                                    char history_line[120];

                                    snprintf(history_line, sizeof(history_line),
                                            "%d  %s\n", i + 1, history[i]);

                                    if (append_output(&tab->output,
                                                    &tab->output_capacity,
                                                    &tab->output_length,
                                                    history_line) == -1) {
                                        last_status = 1;
                                        break;
                                    }
                                }

                                last_status = 0;
                            }                            
                            else if (strcmp(args[0], "jobs") == 0) {
                                update_job_status();

                                for (int i = 0; i < job_count; i++) {
                                    char job_line[INPUT_SIZE + 100];
                                    const char *state;

                                    if (!jobs[i].running) {
                                        state = "Done";
                                    } else if (jobs[i].stopped) {
                                        state = "Stopped";
                                    } else {
                                        state = "Running";
                                    }

                                    snprintf(job_line, sizeof(job_line),
                                            "[%d] %s %s\n",
                                            i + 1, state, jobs[i].command);

                                    if (append_output(&tab->output,
                                                    &tab->output_capacity,
                                                    &tab->output_length,
                                                    job_line) == -1) {
                                        last_status = 1;
                                        break;
                                    }
                                }
                                if (last_status != 1) {
                                    last_status = 0;
                                }
                            }
                            
                            else if (strcmp(args[0], "bg") == 0) {
                                int job_number = job_count;

                                if (args[1] != NULL) {
                                    job_number = atoi(args[1]);
                                }

                                int index = find_job(job_number);

                                if (index == -1 || !jobs[index].running) {
                                    append_output(&tab->output,
                                                &tab->output_capacity,
                                                &tab->output_length,
                                                "bg: invalid or finished job\n");
                                    last_status = 1;
                                } else if (kill(-jobs[index].pgid, SIGCONT) == -1) {
                                    perror("bg: SIGCONT");
                                    last_status = 1;
                                } else {
                                    jobs[index].stopped = 0;
                                    jobs[index].running = 1;

                                    char message[INPUT_SIZE + 100];
                                    snprintf(message, sizeof(message),
                                            "[%d] Running %s\n",
                                            job_number, jobs[index].command);

                                    append_output(&tab->output,
                                                &tab->output_capacity,
                                                &tab->output_length,
                                                message);
                                    last_status = 0;
                                }
                            }
                            
                            else if (strcmp(args[0], "fg") == 0) {
                                int job_number = job_count;

                                if (args[1] != NULL) {
                                    job_number = atoi(args[1]);
                                }

                                int index = find_job(job_number);

                                if (index == -1 || !jobs[index].running) {
                                    append_output(&tab->output,
                                                &tab->output_capacity,
                                                &tab->output_length,
                                                "fg: invalid or finished job\n");
                                    last_status = 1;
                                } else {
                                    if (kill(-jobs[index].pgid, SIGCONT) == -1) {
                                        perror("fg: SIGCONT");
                                        last_status = 1;
                                    } else {
                                        jobs[index].stopped = 0;

                                        int status;
                                        pid_t result;

                                        do {
                                            result = waitpid(jobs[index].pid,
                                                            &status,
                                                            WUNTRACED);
                                        } while (result == -1 && errno == EINTR);

                                        if (result == -1) {
                                            perror("fg: waitpid");
                                            last_status = 1;
                                        } else if (WIFSTOPPED(status)) {
                                            jobs[index].stopped = 1;
                                            last_status = 128 + WSTOPSIG(status);
                                        } else {
                                            jobs[index].running = 0;
                                            jobs[index].stopped = 0;

                                            if (WIFEXITED(status)) {
                                                last_status = WEXITSTATUS(status);
                                            } else if (WIFSIGNALED(status)) {
                                                last_status = 128 + WTERMSIG(status);
                                            } else {
                                                last_status = 1;
                                            }
                                        }
                                    }
                                }
                            }

                            else if (strcmp(args[0], "exit") == 0) {
                                running = 0;
                            }                            
                            else {
                                if (background) {
                                    int pid = run_background(args, tab->input);

                                    if (pid == -1) {
                                        last_status = 1;

                                        append_output(
                                            &tab->output,
                                            &tab->output_capacity,
                                            &tab->output_length,
                                            "Error starting background command.\n"
                                        );
                                    }
                                    else {
                                        last_status = 0;

                                        char message[64];
                                        snprintf(message, sizeof(message),
                                                "[%d] Started\n", pid);

                                        append_output(
                                            &tab->output,
                                            &tab->output_capacity,
                                            &tab->output_length,
                                            message
                                        );
                                    }
                                }
                                else {
                                    last_status = run_command(
                                        args,
                                        &tab->output,
                                        &tab->output_capacity,
                                        &tab->output_length
                                    );

                                    if (last_status == -1) {
                                        append_output(
                                            &tab->output,
                                            &tab->output_capacity,
                                            &tab->output_length,
                                            "Error executing command.\n"
                                        );

                                        last_status = 1;
                                    }
                                }
                            }
                        }
                    }

                    tab->length = 0;
                    tab->input[0] = '\0';
                    tab->history_position = history_count;
                }
            }

            /* Backspace: delete one character. */
            else if (key == XK_BackSpace || key == XK_Delete) {
                if (tab->length > 0) {
                    tab->length--;
                    tab->input[tab->length] = '\0';
                }
            }

            /* Up arrow: previous command. */
            else if (key == XK_Up) {
                if (tab->history_position > 0) {
                    tab->history_position--;

                    strcpy(tab->input,
                           history[tab->history_position]);

                    tab->length = (int)strlen(tab->input);
                }
            }

            /* Down arrow: next command. */
            else if (key == XK_Down) {
                if (tab->history_position < history_count - 1) {
                    tab->history_position++;

                    strcpy(tab->input,
                           history[tab->history_position]);

                    tab->length = (int)strlen(tab->input);
                }
                else {
                    tab->history_position = history_count;
                    tab->length = 0;
                    tab->input[0] = '\0';
                }
            }

            /* Ctrl+L: clear displayed output. */
            else if ((key == XK_l || key == XK_L) &&
                     (event.xkey.state & ControlMask)) {
                tab->output_length = 0;
                tab->output[0] = '\0';
                tab->scroll_offset = 0;
            }

            /* Ctrl+D: exit when the input is empty. */
            else if (key == XK_d &&
                     (event.xkey.state & ControlMask) &&
                     tab->length == 0) {
                running = 0;
            }

            /* Append ordinary typed characters. */
            else if (key_length > 0 &&
                     !(event.xkey.state & ControlMask) &&
                     tab->length < INPUT_SIZE - 1) {
                for (int i = 0;
                     i < key_length &&
                     tab->length < INPUT_SIZE - 1;
                     i++) {
                    if (key_text[i] >= ' ' && key_text[i] != 127) {
                        tab->input[tab->length++] = key_text[i];
                    }
                }

                tab->input[tab->length] = '\0';
            }

            draw_input(display, window, gc,
                       tab->input, tab->length,
                       tab->output, tab->output_length);
        }
    }

    /* Free every tab's output buffer. */
    for (int i = 0; i < tab_count; i++) {
        free(tabs[i].output);
    }

    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    return 0;
}
