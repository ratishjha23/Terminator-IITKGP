#include <stdio.h>
#include <unistd.h>

int main(void){
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) == NULL){
        perror("getcwd");
        return 1;
    }
    printf("Before: %s\n", cwd);
    if (chdir("/tmp") == -1){
        perror("chdir");
        return 1;
    }
    if (getcwd(cwd, sizeof(cwd)) == NULL){
        perror("getcwd");
        return 1;
    }
    printf("After: %s\n", cwd);
    return 0;
}