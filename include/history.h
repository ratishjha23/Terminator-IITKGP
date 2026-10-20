
#ifndef HISTORY_H
#define HISTORY_H

#define HISTORY_SIZE 100
#define HISTORY_INPUT_SIZE 100

extern char history[HISTORY_SIZE][HISTORY_INPUT_SIZE];
extern int history_count;

void history_add(const char *command);

#endif
