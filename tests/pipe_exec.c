#include<stdio.h>
#include<unistd.h>
#include<stdlib.h>
#include<sys/wait.h>
#include <errno.h>
int main(void){
    int pipefd[2];
    if(pipe(pipefd)==-1){
        perror("pipe");
        return 1;
    }
    pid_t pid=fork();
    if(pid<0){
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return 1;
    }   
    if(pid == 0){
        close(pipefd[0]);
        if(dup2(pipefd[1],STDOUT_FILENO)==-1){
            perror("dup2");
            _exit(1);
        }
        if (dup2(pipefd[1], 2) == -1) {
            perror("dup2 stderr");
            _exit(1);
        }
        close(pipefd[1]);
        char *args[] = {"pwd", NULL};
        execvp(args[0],args);
        perror("execvp");
        _exit(127);
    } 
    else{
        close(pipefd[1]);
        char buffer[256];
        ssize_t bytes_read;
        while(1){
            bytes_read=read(pipefd[0],buffer,sizeof(buffer));
            if(bytes_read>0){
                ssize_t total_written = 0;
                while (total_written < bytes_read) {
                    ssize_t n = write(STDOUT_FILENO,
                            buffer + total_written,
                            bytes_read - total_written);
                    if (n > 0) {
                        total_written += n;
                    }
                    else if(n == -1 && errno == EINTR){
                        continue;
                    }
                    else{
                        perror("write");
                        break;
                    }

                }
                if(total_written < bytes_read){
                    break;
                }
            }
            else if (bytes_read == 0) {
                    break;
            }
            else if (errno == EINTR) {
                continue;
            }
            else {
                perror("read");
                break;
            }
        }
        int status;
        if (waitpid(pid, &status, 0) == -1) {
            perror("waitpid");
            return 1;
        }

        if (WIFEXITED(status)) {
            printf("\nChild exit status: %d\n", WEXITSTATUS(status));
        } else {
            printf("\nChild did not exit normally.\n");
        }
    }
    return 0;
}
