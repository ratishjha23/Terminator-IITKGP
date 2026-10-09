#include<stdio.h>
#include<unistd.h>
int main(){
    printf("my pid=%d\n",getpid());
    printf("parent pid=%d\n",getppid());
    return 0;
}