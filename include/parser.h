#ifndef PARSER_H
#define PARSER_H

#include "executor.h"
#include <stddef.h>

int parse_command(char *command, char *args[MAX_PIPE_ARGS]);
char *find_pipeline_separator(char *command);
int parse_redirections(
    char *command,
    char *input_file,
    size_t input_size,
    char *output_file,
    size_t output_size
);

#endif