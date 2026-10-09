#include<stdio.h>
#include<unistd.h>
#include<sys/types.h>
int main(){
    char *args[]={"ls","-1",NULL};
    pid_t pid=fork();
    if(pid<0){
        perror("fork");
        return 1;
    }
    else if(pid==0){
        printf("Child:executing ls\n");
        execvp("ls",args);
        perror("execvp");
        return 1;
    }
    else{
        printf("I am the parent my pid is %d\n",getpid());
        printf("Parent:My child pid is %d\n",pid);
    }
    return 0;
}