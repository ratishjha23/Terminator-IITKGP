#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>

int main(void){
    int pipefd[2];
    if(pipe(pipefd)==-1){
        perror("pipe");
        return 1;
    }
    pid_t pid=fork();
    if(pid==-1){
        perror("fork");
        return 1;
    }
    if(pid==0){
        close(pipefd[0]);
        const char *message="Hello from the other side\n";
        write(pipefd[1],message,strlen(message));
        close(pipefd[1]);
        return 0;
    }
    else{
        close(pipefd[1]);
        char buffer[100];
        ssize_t bytes= read(pipefd[0],buffer,sizeof(buffer)-1);
        if(bytes>0){
            buffer[bytes]='\0';
            printf("Message recived is %s",buffer);
        }
        close(pipefd[0]);
        waitpid(pid,NULL,0);
    }
    return 0;
}