#include "history.h"

#include <string.h>

char history[HISTORY_SIZE][100];
int history_count = 0;

/* Add a command, removing the oldest if history is full. */
void history_add(const char *command)
{
    if (history_count < HISTORY_SIZE) {
        strcpy(history[history_count], command);
        history_count++;
    } else {
        for (int i = 1; i < HISTORY_SIZE; i++) {
            strcpy(history[i - 1], history[i]);
        }

        strcpy(history[HISTORY_SIZE - 1], command);
    }
}