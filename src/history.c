#include "history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char history[HISTORY_SIZE][100];
int history_count = 0;

/* Get the path of the history file. */
static int get_history_file_path(char *path, size_t size)
{
    const char *home = getenv("HOME");

    if (home == NULL) {
        return 0;
    }

    int result = snprintf(path, size, "%s/.terminator_history", home);

    return result >= 0 && (size_t)result < size;
}

/* Save all commands to the history file. */
void history_save(void)
{
    char path[4096];

    if (!get_history_file_path(path, sizeof(path))) {
        return;
    }

    FILE *file = fopen(path, "w");

    if (file == NULL) {
        perror("history_save");
        return;
    }

    for (int i = 0; i < history_count; i++) {
        fprintf(file, "%s\n", history[i]);
    }

    fclose(file);
}


/* Load previously saved commands from the history file. */
void history_load(void)
{
    char path[4096];

    if (!get_history_file_path(path, sizeof(path))) {
        return;
    }

    FILE *file = fopen(path, "r");

    if (file == NULL) {
        return;
    }

    history_count = 0;

    char line[HISTORY_INPUT_SIZE + 2];

    while (fgets(line, sizeof(line), file) != NULL) {

        line[strcspn(line, "\r\n")] = '\0';

        if (history_count < HISTORY_SIZE) {
            strcpy(history[history_count], line);
            history_count++;
        } else {
            for (int i = 1; i < HISTORY_SIZE; i++) {
                strcpy(history[i - 1], history[i]);
            }

            strcpy(history[HISTORY_SIZE - 1], line);
        }
    }

    fclose(file);
}
/* Find the longest common substring length of two strings. */
static int longest_common_substring(const char *a, const char *b)
{
    int previous[HISTORY_INPUT_SIZE + 1] = {0};
    int current[HISTORY_INPUT_SIZE + 1] = {0};

    int best = 0;

    for (int i = 1; a[i - 1] != '\0'; i++) {
        for (int j = 1; b[j - 1] != '\0'; j++) {

            if (a[i - 1] == b[j - 1]) {
                current[j] = previous[j - 1] + 1;

                if (current[j] > best) {
                    best = current[j];
                }
            } else {
                current[j] = 0;
            }
        }

        for (int j = 0; j <= HISTORY_INPUT_SIZE; j++) {
            previous[j] = current[j];
            current[j] = 0;
        }
    }

    return best;
}


/* Search history using exact match first, then longest common substring. */
int history_search(const char *term, int *matches, int max_matches)
{
    if (term == NULL || term[0] == '\0' ||
        matches == NULL || max_matches <= 0) {
        return 0;
    }

    /* First, search for the most recent exact match. */
    for (int i = history_count - 1; i >= 0; i--) {
        if (strcmp(history[i], term) == 0) {
            matches[0] = i;
            return 1;
        }
    }

    /* Otherwise, find commands with the longest common substring. */
    int best_length = 2;
    int match_count = 0;

    for (int i = 0; i < history_count; i++) {
        int length = longest_common_substring(history[i], term);

        if (length > best_length) {
            best_length = length;
            match_count = 0;
        }

        if (length == best_length && match_count < max_matches) {
            matches[match_count] = i;
            match_count++;
        }
    }

    return match_count;
}
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
    history_save();
}