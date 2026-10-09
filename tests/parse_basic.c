#include<stdio.h>
#include<string.h>
int main(void){
    char input[]="ls -1";
    char *token=strtok(input," \t");
    while(token!=NULL){
        printf("Token: %s\n", token);
        token=strtok(NULL," \t");
    }
    return 0;
}