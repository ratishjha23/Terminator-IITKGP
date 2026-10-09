#include<stdio.h>
#include<string.h>
int main(void){
    char input[100];
    printf("Enter the line\n");
    if(fgets(input,sizeof(input),stdin)==NULL){
        printf("NO input\n");
        return 1;
    }
    input[strcspn(input,"\n")]='\0';
    char *token=strtok(input," \t");
    char *args[10];
    int count =0;
    while(token!=NULL && count < 9){
        args[count++]=token;
        token=strtok(NULL," \t");
    }
    args[count]=NULL;
    for(int i=0;args[i]!=NULL;i++){
        printf("args[%d] = %s\n",i,args[i]);
    }
    return 0;
}