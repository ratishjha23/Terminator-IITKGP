#include<stdio.h>
#include<unistd.h>
#include<sys/types.h>
int main(){
    pid_t pid=fork();
    if(pid<0){
        perror("fork");
        return 1;
    }
    else if(pid==0){
        printf("I am the child my pid is %d\n",getpid());
    }
    else{
        printf("I am the parent my pid is %d\n",getpid());
        printf("My child pid is %d\n",pid);
    }
    return 0;
}