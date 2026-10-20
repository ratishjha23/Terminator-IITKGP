#ifndef OUTPUT_BUFFER_H
#define OUTPUT_BUFFER_H

int append_output(char **output,
                  int *output_capacity,
                  int *output_length,
                  const char *text);

#endif