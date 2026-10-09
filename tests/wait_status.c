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
        return 10;
    }
    else{
        int status;
        printf("Parent: waiting for child...\n");
        pid_t result = waitpid(pid,&status,0);
        if (result == -1)
        {
            perror("waitpid");
            return 1;
        }

        printf("Parent: waitpid returned PID = %d\n", result);

        if (WIFEXITED(status))
        {
            printf("Parent: child exited normally\n");
            printf("Parent: child's exit status = %d\n",
                   WEXITSTATUS(status));
        }
        
    }
    return 0;
}