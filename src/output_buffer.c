#include "output_buffer.h"

#include <stdlib.h>
#include <string.h>

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