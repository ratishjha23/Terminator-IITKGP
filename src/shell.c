#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "shell.h"

void shell_run(void)
{
    char input[100];
    int last_status = 0;

    while (1)
    {
        printf("Terminator> ");
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin) == NULL)
        {
            if (ferror(stdin))
            {
                perror("input");
            }

            printf("\nShell terminated.\n");
            break;
        }

        if (strchr(input, '\n') == NULL)
        {
            int ch;

            while ((ch = getchar()) != '\n' && ch != EOF)
            {
                /* Discard the remaining characters. */
            }

            if (ch == EOF && ferror(stdin))
            {
                perror("input");
                break;
            }

            if (ch == EOF)
            {
                printf("\nShell terminated.\n");
                break;
            }

            fprintf(stderr, "Error: input line too long.\n");
            continue;
        }

        input[strcspn(input, "\n")] = '\0';

        char *token = strtok(input, " \t");
        char *args[10];
        int count = 0;

        while (token != NULL && count < 9)
        {
            args[count++] = token;
            token = strtok(NULL, " \t");
        }

        args[count] = NULL;

        if (token != NULL)
        {
            fprintf(stderr,
                    "Error: too many arguments (maximum 9).\n");
            continue;
        }

        if (count == 0)
        {
            continue;
        }

        if (strcmp(args[0], "cd") == 0)
        {
            const char *path = args[1];

            if (path == NULL)
            {
                fprintf(stderr, "Usage: cd <directory>\n");
                continue;
            }

            if (chdir(path) == -1)
            {
                perror("cd");
            }

            continue;
        }

        if (strcmp(args[0], "exit") == 0)
        {
            printf("Shell terminated.\n");
            break;
        }

        pid_t pid = fork();

        if (pid < 0)
        {
            perror("fork");
            last_status = 1;
            continue;
        }
        else if (pid == 0)
        {
            execvp(args[0], args);
            perror("execvp");
            _exit(127);
        }
        else
        {
            int status;

            if (waitpid(pid, &status, 0) == -1)
            {
                perror("waitpid");
                last_status = 1;
                continue;
            }

            if (WIFEXITED(status))
            {
                last_status = WEXITSTATUS(status);
                printf("Exit status: %d\n", last_status);
            }
            else
            {
                last_status = 1;
                printf("Command did not exit normally.\n");
            }
        }
    }
}