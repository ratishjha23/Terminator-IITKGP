#include<stdio.h>
#include <string.h>
int main(void){
    char input[100];
    printf("Enter a line: ");
    if(fgets(input,sizeof(input),stdin)==NULL){
        printf("No input\n");
        return 1;
    }
    else{
        input[strcspn(input,"\n")]='\0';
        printf("The input is %s\n",input);
        return 0;
    }
}