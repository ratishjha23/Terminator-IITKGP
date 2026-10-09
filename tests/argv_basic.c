
#include <stdio.h>
#include <string.h>

int main(void)
{
    char input[] = "ls -l";
    char *args[10];
    int count = 0;

    char *token = strtok(input, " \t");

    while (token != NULL && count < 9)
    {
        args[count] = token;
        count++;

        token = strtok(NULL, " \t");
    }

    args[count] = NULL;

    for (int i = 0; args[i] != NULL; i++)
    {
        printf("args[%d] = %s\n", i, args[i]);
    }

    printf("args[%d] = NULL\n", count);

    return 0;
}
