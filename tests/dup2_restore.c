#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main(void)
{
    int saved_stdout = dup(STDOUT_FILENO);

    if (saved_stdout == -1) {
        perror("dup");
        return 1;
    }

    int fd = open("dup2_output.txt",
                  O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd == -1) {
        perror("open");
        close(saved_stdout);
        return 1;
    }

    if (dup2(fd, STDOUT_FILENO) == -1) {
        perror("dup2");
        close(fd);
        close(saved_stdout);
        return 1;
    }

    close(fd);

    printf("This goes into the file.\n");
    fflush(stdout);

    if (dup2(saved_stdout, STDOUT_FILENO) == -1) {
        perror("restore stdout");
        close(saved_stdout);
        return 1;
    }

    close(saved_stdout);

    printf("This goes into the terminal.\n");

    return 0;
}