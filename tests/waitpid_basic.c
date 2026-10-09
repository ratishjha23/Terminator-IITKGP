#include<stdio.h>
#include<unistd.h>
#include<sys/types.h>
#include <sys/wait.h>
int main(void){
    pid_t pid=fork();
    if(pid<0){
        perror("fork");
        return 1;
    }
    else if(pid==0){
        printf("Child: I am running...\n");
        sleep(3);
        printf("Child: I am finished\n");
        return 0;
    }
    else{
        int status;
        printf("Parent: waiting for child...\n");
        waitpid(pid,&status,0);
        printf("Parent:child has finished\n");
    }
    return 0;
}