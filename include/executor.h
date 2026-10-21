#ifndef EXECUTOR_H
#define EXECUTOR_H

#define MAX_PIPE_COMMANDS 10
#define MAX_PIPE_ARGS 10

int run_command(char *args[],
                char **output,
                int *output_capacity,
                int *output_length);

int run_pipeline(
    char *commands[][MAX_PIPE_ARGS],
    int command_count,
    const char *input_file,
    const char *output_file,
    char **output,
    int *output_capacity,
    int *output_length
);

int run_command_redirected(
    char *args[],
    const char *input_file,
    const char *output_file,
    char **output,
    int *output_capacity,
    int *output_length
);

#endif