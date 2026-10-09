#include<stdio.h>
#include<unistd.h>
#include<sys/types.h>
#include<sys/wait.h>
int main(void){
    pid_t pid=fork();
    if(pid<0){
        perror("fork");
        return 1;
    }
    else if(pid==0){
        char *args[]={"ls","-1",NULL};
        printf("Child executing ls\n");
        execvp(args[0],args);
        perror("execvp");
        _exit(1);
    }
    else{
        int status;
        printf("Parent: waiting for child %d...\n", pid);
        pid_t result=waitpid(pid,&status,0);
        if(result==-1){
            perror("waitpid");
            return 1;
        }
        if(WIFEXITED(status)){
            printf("Parent:child terminated with status %d\n",WEXITSTATUS(status));
        }
        return 0;
    }
}