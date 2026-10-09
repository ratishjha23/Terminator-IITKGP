#include<stdio.h>
#include<unistd.h>
int main(void){
    printf("Before execvp\n");
    char *args[]={"ls","-1",NULL};
    execvp("ls",args);
    perror("execvp");
    return 1;
}