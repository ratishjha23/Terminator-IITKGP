
#ifndef HISTORY_H
#define HISTORY_H

#define HISTORY_SIZE 10000
#define HISTORY_INPUT_SIZE 100

extern char history[HISTORY_SIZE][HISTORY_INPUT_SIZE];
extern int history_count;

void history_add(const char *command);
void history_load(void);
void history_save(void);
int history_search(const char *term, int *matches, int max_matches);

#endif
