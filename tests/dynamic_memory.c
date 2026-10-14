
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    char *output = malloc(16);

    if (output == NULL)
    {
        perror("malloc");
        return 1;
    }

    strcpy(output, "Hello");
    printf("Before resize: %s\n", output);

    char *temp = realloc(output, 32);

    if (temp == NULL)
    {
        perror("realloc");
        free(output);
        return 1;
    }

    output = temp;

    strcat(output, " Terminator");
    printf("After resize: %s\n", output);

    free(output);

    return 0;
}
