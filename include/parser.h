#ifndef PARSER_H
#define PARSER_H

#include "executor.h"

int parse_command(char *command, char *args[MAX_PIPE_ARGS]);
char *find_pipeline_separator(char *command);

#endif