#define _XOPEN_SOURCE 700

#include "parser.h"

#include <string.h>
#include <wordexp.h>
#include <stddef.h>

#define EXPANDED_ARG_SIZE 8192


char *find_pipeline_separator(char *command)
{
    enum { MAX_NESTING = 64 };
    struct frame { int kind; int parens; char saved_quote; } stack[MAX_NESTING];
    int depth = 0, escaped = 0;
    char quote = '\0';

    for (int i = 0; command[i] != '\0'; i++) {
        char c = command[i];
        if (escaped) { escaped = 0; continue; }
        if (c == '\\' && quote != '\'') { escaped = 1; continue; }

        if (quote == '\'') {
            if (c == '\'') quote = '\0';
            continue;
        }
        if (quote == '"') {
            if (c == '"') { quote = '\0'; continue; }
        } else if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }

        if (depth > 0 && stack[depth - 1].kind == 2 &&
            c == 0x60 && quote == '\0') {
            quote = stack[--depth].saved_quote;
            continue;
        }
        if (c == '$' && command[i + 1] == '(') {
            if (depth == MAX_NESTING) return NULL;
            stack[depth++] = (struct frame){1, 1, quote};
            quote = '\0';
            i++;
            continue;
        }
        if (c == 0x60) {
            if (depth == MAX_NESTING) return NULL;
            stack[depth++] = (struct frame){2, 0, quote};
            quote = '\0';
            continue;
        }
        if (quote == '\0' && depth > 0 &&
            stack[depth - 1].kind == 1) {
            if (c == '(') stack[depth - 1].parens++;
            else if (c == ')' && --stack[depth - 1].parens == 0)
                quote = stack[--depth].saved_quote;
        } else if (quote == '\0' && depth == 0 && c == '|') {
            return command + i;
        }
    }
    return NULL;
}


/*
 * Keep substitution results alive after parse_command returns.
 * Separate slots allow multiple pipeline stages to be parsed in sequence.
 */
static char expanded_storage[MAX_PIPE_COMMANDS]
                            [MAX_PIPE_ARGS]
                            [EXPANDED_ARG_SIZE];
static unsigned int expanded_slot;

int parse_command(char *command, char *args[MAX_PIPE_ARGS])
{
    /*
     * Use the system shell-word parser only when command substitution appears.
     * Do not pass WRDE_NOCMD: that flag disables $(...) evaluation.
     */
    if (strpbrk(command, "$\x60") != NULL) {
        wordexp_t words = {0};
        int result = wordexp(command, &words, 0);

        if (result != 0) {
            if (result == WRDE_NOSPACE) {
                wordfree(&words);
            }
            return -1;
        }

        if (words.we_wordc >= MAX_PIPE_ARGS) {
            wordfree(&words);
            return -1;
        }

        unsigned int slot = expanded_slot++ % MAX_PIPE_COMMANDS;

        for (size_t i = 0; i < words.we_wordc; i++) {
            size_t length = strlen(words.we_wordv[i]);

            if (length >= EXPANDED_ARG_SIZE) {
                wordfree(&words);
                return -1;
            }

            memcpy(expanded_storage[slot][i],
                   words.we_wordv[i],
                   length + 1);

            args[i] = expanded_storage[slot][i];
        }

        args[words.we_wordc] = NULL;
        int arg_count = (int)words.we_wordc;
        wordfree(&words);
        return arg_count;
    }

    /* Existing parser path for commands without $(...). */
    int arg_count = 0;
    char *src = command;
    char *dst = command;

    while (*src != '\0') {
        while (*src == ' ' || *src == '\t') {
            src++;
        }

        if (*src == '\0') {
            break;
        }

        if (arg_count >= MAX_PIPE_ARGS - 1) {
            return -1;
        }

        args[arg_count++] = dst;

        char quote = '\0';

        while (*src != '\0') {
            char c = *src++;

            if (quote != '\0') {
                if (c == quote) {
                    quote = '\0';
                } else {
                    *dst++ = c;
                }
            } else if (c == '\'' || c == '"') {
                quote = c;
            } else if (c == ' ' || c == '\t') {
                break;
            } else {
                *dst++ = c;
            }
        }

        if (quote != '\0') {
            return -1;
        }

        *dst++ = '\0';
    }

    args[arg_count] = NULL;
    return arg_count;
}

int parse_redirections(
    char *command,
    char *input_file,
    size_t input_size,
    char *output_file,
    size_t output_size)
{
    char cleaned[8192];
    size_t r = 0, w = 0;

    input_file[0] = '\0';
    output_file[0] = '\0';

    while (command[r] != '\0') {
        char c = command[r];

        if (c == '\'' || c == '"') {
            char quote = c;

            if (w + 1 >= sizeof(cleaned))
                return -1;

            cleaned[w++] = command[r++];

            while (command[r] != '\0') {
                c = command[r++];

                if (w + 1 >= sizeof(cleaned))
                    return -1;

                cleaned[w++] = c;

                if (c == quote)
                    break;
            }

            if (c != quote)
                return -1;

            continue;
        }

        if (c != '<' && c != '>') {
            if (w + 1 >= sizeof(cleaned))
                return -1;

            cleaned[w++] = c;
            r++;
            continue;
        }

        char *filename = (c == '<') ? input_file : output_file;
        size_t filename_size =
            (c == '<') ? input_size : output_size;

        if (filename[0] != '\0')
            return -1;

        r++;

        while (command[r] == ' ' || command[r] == '\t')
            r++;

        if (command[r] == '\0' ||
            command[r] == '<' || command[r] == '>')
            return -1;

        size_t f = 0;
        char quote = '\0';

        while (command[r] != '\0') {
            c = command[r];

            if (quote != '\0') {
                if (c == quote) {
                    quote = '\0';
                    r++;
                    continue;
                }

                if (f + 1 >= filename_size)
                    return -1;

                filename[f++] = c;
                r++;
                continue;
            }

            if (c == '\'' || c == '"') {
                quote = c;
                r++;
                continue;
            }

            if (c == ' ' || c == '\t' ||
                c == '<' || c == '>')
                break;

            if (f + 1 >= filename_size)
                return -1;

            filename[f++] = c;
            r++;
        }

        if (quote != '\0' || f == 0)
            return -1;

        filename[f] = '\0';
    }

    cleaned[w] = '\0';

    size_t start = 0;

    while (cleaned[start] == ' ' || cleaned[start] == '\t')
        start++;

    memmove(command, cleaned + start,
            strlen(cleaned + start) + 1);

    return command[0] == '\0' ? -1 : 0;
}
