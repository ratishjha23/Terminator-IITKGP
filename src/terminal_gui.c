
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>
#include <strings.h>
#include <dirent.h>

#include "executor.h"
#include "output_buffer.h"
#include "parser.h"
#include "history.h"
#include "multiwatch.h"

#define WINDOW_WIDTH 500
#define WINDOW_HEIGHT 300

#define LINE_HEIGHT 20
#define HISTORY_TOP 40
#define HISTORY_BOTTOM 240
#define PROMPT_Y 280
#define VISIBLE_LINES (((HISTORY_BOTTOM - HISTORY_TOP) / LINE_HEIGHT) + 1)

#define INPUT_SIZE 100
#define MAX_TABS 10


#define TAB_BAR_HEIGHT 30
#define TAB_WIDTH 100

#define MULTIWATCH_REFRESH_SECONDS 2
typedef struct {
    pid_t pid;
    pid_t pgid;
    int output_fd;
    int job_index;
    int active;
    int stopped;
    char command[INPUT_SIZE];

    
} ForegroundProcess;

typedef struct {
    int running;
    int round_active;
    int command_count;

    MultiWatchProcess processes[MULTIWATCH_MAX_COMMANDS];

    char commands[MULTIWATCH_MAX_COMMANDS][INPUT_SIZE];

    int header_printed[MULTIWATCH_MAX_COMMANDS];
    time_t timestamps[MULTIWATCH_MAX_COMMANDS];

    time_t next_refresh_time;
} MultiWatchSession;

MultiWatchSession multiwatch_session = {0};

typedef struct {
    char input[INPUT_SIZE];
    int length;
    int continuing;

    char pending_input[INPUT_SIZE];
    int pending_length;

    char *output;
    int output_capacity;
    int output_length;

    int history_position;
    int scroll_offset;
    int cursor;
    char cwd[4096];
    ForegroundProcess foreground;
    /* Ctrl+R history search state. */
    int history_search_active;
    char history_search_term[INPUT_SIZE];

    char saved_input[INPUT_SIZE];
    int saved_length;
    int saved_cursor;

    /* Tab-completion selection state. */
    int completion_active;
    int completion_word_start;
    int completion_count;
    char completion_matches[INPUT_SIZE][INPUT_SIZE];
} Tab;

Tab tabs[MAX_TABS];
int tab_count = 0;
int active_tab = 0;
int last_status = 0;
/* Mouse selection in the command input line. */
int selection_start = -1;
int selection_end = -1;
int selecting_text = 0;


/* X11 clipboard state. */
Atom clipboard_atom;
Atom utf8_string_atom;
Atom targets_atom;
char *clipboard_text = NULL;
Atom paste_property_atom;


#define MAX_JOBS 100

/* --------------------------------------------------
   CONFIGURABLE KEYBINDINGS
   -------------------------------------------------- */

typedef enum {
    KEY_MOVE_LEFT,
    KEY_MOVE_RIGHT,
    KEY_LINE_START,
    KEY_LINE_END,
    KEY_HISTORY_UP,
    KEY_HISTORY_DOWN,
    KEY_DELETE_TO_START,
    KEY_DELETE_TO_END,
    KEY_DELETE_WORD,
    KEY_CLEAR_SCREEN,
    KEY_EXIT_SHELL,
    KEY_COPY,
    KEY_PASTE,
    KEY_HISTORY_SEARCH,
    KEY_ACTION_COUNT
} KeyAction;

typedef struct {
    KeySym key;
    unsigned int modifiers;
} KeyBinding;

/* Default keybindings. */
KeyBinding keybindings[KEY_ACTION_COUNT] = {
    [KEY_MOVE_LEFT]       = {XK_Left, 0},
    [KEY_MOVE_RIGHT]      = {XK_Right, 0},
    [KEY_LINE_START]      = {XK_a, ControlMask},
    [KEY_LINE_END]        = {XK_e, ControlMask},
    [KEY_HISTORY_UP]      = {XK_Up, 0},
    [KEY_HISTORY_DOWN]    = {XK_Down, 0},
    [KEY_DELETE_TO_START] = {XK_u, ControlMask},
    [KEY_DELETE_TO_END]   = {XK_k, ControlMask},
    [KEY_DELETE_WORD]     = {XK_w, ControlMask},
    [KEY_CLEAR_SCREEN]    = {XK_l, ControlMask},
    [KEY_EXIT_SHELL]      = {XK_d, ControlMask},
    [KEY_COPY]            = {XK_c, ControlMask | ShiftMask},
    [KEY_PASTE]           = {XK_v, ControlMask | ShiftMask},
    [KEY_HISTORY_SEARCH]  = {XK_r, ControlMask}
};

/* Names of actions used in ~/.terminatorrc. */
const char *key_action_names[KEY_ACTION_COUNT] = {
    "move_left",
    "move_right",
    "line_start",
    "line_end",
    "history_up",
    "history_down",
    "delete_to_start",
    "delete_to_end",
    "delete_word",
    "clear_screen",
    "exit_shell",
    "copy",
    "paste",
    "history_search"
};


/* Remove spaces from the beginning and end of a string. */
char *trim_whitespace(char *str)
{
    while (*str == ' ' || *str == '\t') {
        str++;
    }

    char *end = str + strlen(str);

    while (end > str &&
           (end[-1] == ' ' || end[-1] == '\t' ||
            end[-1] == '\r' || end[-1] == '\n')) {
        end--;
    }

    *end = '\0';
    return str;
}


/* Convert a shortcut such as Ctrl+Shift+C into a binding. */
int parse_key_binding(const char *value, KeyBinding *binding)
{
    char copy[128];

    if (strlen(value) >= sizeof(copy)) {
        return 0;
    }

    strcpy(copy, value);

    unsigned int modifiers = 0;
    char *key_name = NULL;
    char *saveptr = NULL;

    char *token = strtok_r(copy, "+", &saveptr);

    while (token != NULL) {
        token = trim_whitespace(token);

        if (strcasecmp(token, "Ctrl") == 0 ||
            strcasecmp(token, "Control") == 0) {
            modifiers |= ControlMask;
        }
        else if (strcasecmp(token, "Shift") == 0) {
            modifiers |= ShiftMask;
        }
        else if (strcasecmp(token, "Alt") == 0) {
            modifiers |= Mod1Mask;
        }
        else {
            if (key_name != NULL) {
                return 0;
            }

            key_name = token;
        }

        token = strtok_r(NULL, "+", &saveptr);
    }

    if (key_name == NULL || key_name[0] == '\0') {
        return 0;
    }

    /* Normalize single alphabetic keys to lowercase. */
    if (strlen(key_name) == 1 &&
        key_name[0] >= 'A' && key_name[0] <= 'Z') {
        key_name[0] = (char)(key_name[0] - 'A' + 'a');
    }

    KeySym key = XStringToKeysym(key_name);

    if (key == NoSymbol) {
        return 0;
    }

    binding->key = key;
    binding->modifiers = modifiers;

    return 1;
}


/* Find an action using its name from the configuration file. */
int find_key_action(const char *name)
{
    for (int i = 0; i < KEY_ACTION_COUNT; i++) {
        if (strcmp(name, key_action_names[i]) == 0) {
            return i;
        }
    }

    return -1;
}


/* Check whether another action already uses this shortcut. */
int key_binding_conflicts(int action, KeyBinding binding)
{
    for (int i = 0; i < KEY_ACTION_COUNT; i++) {
        if (i == action) {
            continue;
        }

        if (keybindings[i].key == binding.key &&
            keybindings[i].modifiers == binding.modifiers) {
            return 1;
        }
    }

    return 0;
}


/* Create the default configuration file on first launch. */
void create_default_keybinding_config(const char *path)
{
    FILE *file = fopen(path, "w");

    if (file == NULL) {
        perror("Could not create ~/.terminatorrc");
        return;
    }

    fprintf(file, "# Terminator keyboard shortcuts\n");
    fprintf(file, "[Keybindings]\n");

    fprintf(file, "move_left = Left\n");
    fprintf(file, "move_right = Right\n");
    fprintf(file, "line_start = Ctrl+A\n");
    fprintf(file, "line_end = Ctrl+E\n");
    fprintf(file, "history_up = Up\n");
    fprintf(file, "history_down = Down\n");
    fprintf(file, "delete_to_start = Ctrl+U\n");
    fprintf(file, "delete_to_end = Ctrl+K\n");
    fprintf(file, "delete_word = Ctrl+W\n");
    fprintf(file, "clear_screen = Ctrl+L\n");
    fprintf(file, "exit_shell = Ctrl+D\n");
    fprintf(file, "copy = Ctrl+Shift+C\n");
    fprintf(file, "paste = Ctrl+Shift+V\n");

    fclose(file);
}


/* Load keyboard shortcuts from ~/.terminatorrc. */
void load_keybindings(void)
{
    const char *home = getenv("HOME");

    if (home == NULL) {
        fprintf(stderr, "HOME environment variable not found.\n");
        return;
    }

    char path[4096];

    snprintf(path, sizeof(path), "%s/.terminatorrc", home);

    FILE *file = fopen(path, "r");

    if (file == NULL) {
        create_default_keybinding_config(path);
        return;
    }

    char line[256];
    int line_number = 0;

    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;

        char *text = trim_whitespace(line);

        /* Ignore blank lines and comments. */
        if (*text == '\0' || *text == '#' || *text == ';') {
            continue;
        }

        /* Ignore section headers. */
        if (*text == '[') {
            continue;
        }

        char *equals = strchr(text, '=');

        if (equals == NULL) {
            fprintf(stderr,
                    "Invalid keybinding at line %d.\n",
                    line_number);
            continue;
        }

        *equals = '\0';

        char *action_name = trim_whitespace(text);
        char *value = trim_whitespace(equals + 1);

        int action = find_key_action(action_name);

        if (action == -1) {
            fprintf(stderr,
                    "Unknown keybinding action: %s\n",
                    action_name);
            continue;
        }

        KeyBinding new_binding;

        if (!parse_key_binding(value, &new_binding)) {
            fprintf(stderr,
                    "Invalid shortcut for %s: %s\n",
                    action_name, value);
            continue;
        }

        if (key_binding_conflicts(action, new_binding)) {
            fprintf(stderr,
                    "Conflicting shortcut for %s: %s\n",
                    action_name, value);
            continue;
        }

        keybindings[action] = new_binding;
    }

    fclose(file);

    printf("Keybindings loaded from %s\n", path);
}

/* Check whether a pressed key matches a configured shortcut. */
int key_matches(KeyAction action, KeySym key,
                unsigned int state)
{
    unsigned int modifiers = state &
        (ControlMask | ShiftMask | Mod1Mask);

    KeySym pressed_key = key;

    /* Treat uppercase and lowercase letters as the same key. */
    if (pressed_key >= XK_A && pressed_key <= XK_Z) {
        pressed_key = pressed_key - XK_A + XK_a;
    }

    return pressed_key == keybindings[action].key &&
           modifiers == keybindings[action].modifiers;
}
typedef struct {
    pid_t pid;
    pid_t pgid;
    int output_fd;
    int tab_index;
    char command[INPUT_SIZE];
    int running;
    int stopped;
} Job;


Job jobs[MAX_JOBS];
int job_count = 0;





/* Check whether a command needs another input line. */
int command_needs_more_input(const char *command)
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
            if (depth == MAX_NESTING) return 1;
            stack[depth++] = (struct frame){1, 1, quote};
            quote = '\0';
            i++;
            continue;
        }
        if (c == 0x60) {
            if (depth == MAX_NESTING) return 1;
            stack[depth++] = (struct frame){2, 0, quote};
            quote = '\0';
            continue;
        }
        if (quote == '\0' && depth > 0 &&
            stack[depth - 1].kind == 1) {
            if (c == '(') stack[depth - 1].parens++;
            else if (c == ')' && --stack[depth - 1].parens == 0)
                quote = stack[--depth].saved_quote;
        }
    }
    return quote != '\0' || depth > 0;
}

/* Count the lines currently stored in the output buffer. */
int count_output_lines(const char *output, int output_length)
{
    if (output_length == 0) {
        return 0;
    }

    int lines = 0;

    for (int i = 0; i < output_length; i++) {
        if (output[i] == '\n') {
            lines++;
        }
    }

    if (output[output_length - 1] != '\n') {
        lines++;
    }

    return lines;
}

/* Draw the visible portion of the command output. */
void draw_output(Display *display, Window window, GC gc,
                 const char *output, int output_length)
{
    int total_lines = count_output_lines(output, output_length);

    int max_scroll = total_lines > VISIBLE_LINES
                         ? total_lines - VISIBLE_LINES
                         : 0;

    Tab *tab = &tabs[active_tab];

    if (tab->scroll_offset > max_scroll) {
        tab->scroll_offset = max_scroll;
    }

    int first_line = total_lines - VISIBLE_LINES
                     - tab->scroll_offset;

    if (first_line < 0) {
        first_line = 0;
    }

    int visible_count = total_lines - first_line;

    if (visible_count > VISIBLE_LINES) {
        visible_count = VISIBLE_LINES;
    }

    int starting_y = HISTORY_BOTTOM
                     - (visible_count - 1) * LINE_HEIGHT;

    int current_line = 0;
    int start = 0;

    for (int i = 0; i <= output_length; i++) {
        if (i == output_length || output[i] == '\n') {
            if (current_line >= first_line &&
                current_line < first_line + VISIBLE_LINES) {

                int line_length = i - start;

                if (line_length > 0) {
                    XDrawString(
                        display, window, gc,
                        10,
                        starting_y
                            + (current_line - first_line) * LINE_HEIGHT,
                        output + start,
                        line_length
                    );
                }
            }

            current_line++;
            start = i + 1;
        }
    }
}

int initialize_tab(int index)
{
    Tab *tab = &tabs[index];
    tab->input[0] = '\0';
    tab->length = 0;
    tab->cursor = 0;
    tab->pending_input[0] = '\0';
    tab->pending_length = 0;
    tab->continuing = 0;

    tab->foreground.pid = -1;
    tab->foreground.pgid = -1;
    tab->foreground.output_fd = -1;
    tab->foreground.active = 0;
    tab->foreground.stopped = 0;
    tab->foreground.job_index = -1;
    tab->foreground.command[0] = '\0';

    tab->output_capacity = 4096;
    tab->output_length = 0;
    tab->output = malloc((size_t)tab->output_capacity);

    if (tab->output == NULL) {
        return -1;
    }

    tab->output[0] = '\0';

    tab->history_position = history_count;
    tab->scroll_offset = 0;

    if (getcwd(tab->cwd, sizeof(tab->cwd)) == NULL) {
        tab->cwd[0] = '\0';
    }

    return 0;
}

void close_tab(int index)
{
    if (index < 0 || index >= tab_count) {
        return;
    }

    free(tabs[index].output);

    for (int i = index; i < tab_count - 1; i++) {
        tabs[i] = tabs[i + 1];
    }

    tab_count--;

    if (tab_count == 0) {
        tab_count = 1;
        active_tab = 0;
        initialize_tab(0);
        return;
    }

    if (active_tab >= tab_count) {
        active_tab = tab_count - 1;
    } else if (active_tab > index) {
        active_tab--;
    }
}



void draw_tabs(Display *display, Window window, GC gc)
{
    for (int i = 0; i < tab_count; i++) {
        int x = i * TAB_WIDTH;

        XDrawRectangle(display, window, gc,
                       x, 0, TAB_WIDTH, TAB_BAR_HEIGHT);

        char label[32];
        snprintf(label, sizeof(label), "Tab %d", i + 1);

        XDrawString(display, window, gc,
                    x + 10, 20, label, (int)strlen(label));
        XDrawString(display, window, gc,
            x + TAB_WIDTH - 18, 20, "x", 1);
    }

    XDrawRectangle(display, window, gc,
                   tab_count * TAB_WIDTH, 0,
                   TAB_WIDTH, TAB_BAR_HEIGHT);

    XDrawString(display, window, gc,
                tab_count * TAB_WIDTH + 10, 20,
                "+", 1);
    
}


/* Redraw the complete GUI, including selected input text. */
void draw_input(Display *display, Window window, GC gc,
                const char *input, int length,
                const char *output, int output_length)
{
    XClearWindow(display, window);

    draw_tabs(display, window, gc);
    draw_output(display, window, gc, output, output_length);

    /* Keep the original foreground color for normal text. */
    XGCValues gc_values;
    XGetGCValues(display, gc, GCForeground, &gc_values);
    unsigned long normal_color = gc_values.foreground;

    const char *prompt;

    if (tabs[active_tab].history_search_active) {
        prompt = "Enter search term:";
    }
    else {
        prompt = tabs[active_tab].continuing
                ? ">"
                : "Terminator>";
    }

    XSetForeground(display, gc,
                   BlackPixel(display, DefaultScreen(display)));

    XDrawString(display, window, gc,
                10, PROMPT_Y, prompt, (int)strlen(prompt));

    int input_x;

    if (tabs[active_tab].history_search_active) {
        input_x = 160;
    }
    else {
        input_x = tabs[active_tab].continuing ? 25 : 120;
    }

    int start = selection_start;
    int end = selection_end;

    if (start > end) {
        int temp = start;
        start = end;
        end = temp;
    }

    /* Clamp selection positions to the current input. */
    if (start < 0 || end <= start || end > length) {
        start = -1;
        end = -1;
    }

    if (start >= 0) {
        XFontStruct *font =
            XQueryFont(display, XGContextFromGC(gc));

        if (font != NULL) {
            int prefix_width = XTextWidth(font, input, start);
            int selected_width =
                XTextWidth(font, input + start, end - start);

            /* Draw the selected region's black background. */
            XSetForeground(display, gc,
                           BlackPixel(display, DefaultScreen(display)));

            XFillRectangle(display, window, gc,
                           input_x + prefix_width,
                           PROMPT_Y - font->ascent,
                           (unsigned int)selected_width,
                           (unsigned int)(font->ascent + font->descent));

            /* Normal text before the selection. */
            XDrawString(display, window, gc,
                        input_x, PROMPT_Y, input, start);

            /* Selected text in white. */
            XSetForeground(display, gc,
                           WhitePixel(display, DefaultScreen(display)));

            XDrawString(display, window, gc,
                        input_x + prefix_width, PROMPT_Y,
                        input + start, end - start);

            /* Normal text after the selection. */
            XSetForeground(display, gc,
                           BlackPixel(display, DefaultScreen(display)));

            int suffix_x = input_x +
                XTextWidth(font, input, end);

            XDrawString(display, window, gc,
                        suffix_x, PROMPT_Y,
                        input + end, length - end);

            XFreeFontInfo(NULL, font, 1);
        } else {
            XDrawString(display, window, gc,
                        input_x, PROMPT_Y, input, length);
        }
    } else {
        XDrawString(display, window, gc,
                    input_x, PROMPT_Y, input, length);
    }

    /* Restore the GC foreground for subsequent drawing. */
    XSetForeground(display, gc, normal_color);

    XFlush(display);
}


/* Convert a mouse X coordinate to an input character position. */
int input_position_from_x(Display *display, GC gc,
                          const char *input, int length,
                          int input_x, int mouse_x)
{
    XFontStruct *font = XQueryFont(display, XGContextFromGC(gc));

    if (font == NULL) {
        return length;
    }

    int relative_x = mouse_x - input_x;
    int position = length;

    if (relative_x <= 0) {
        position = 0;
    } else {
        for (int i = 0; i < length; i++) {
            int width = XTextWidth(font, input, i + 1);

            if (relative_x < width) {
                position = i;
                break;
            }
        }
    }

    XFreeFontInfo(NULL, font, 1);
    return position;
}

/* Find files whose names begin with the given word. */
int find_file_matches(const char *cwd,
                      const char *word,
                      char matches[][INPUT_SIZE],
                      int max_matches)
{
    char directory[4096];
    char path_prefix[INPUT_SIZE];

    const char *slash = strrchr(word, '/');
    const char *filename_prefix;

    int prefix_length = 0;

    /* Determine the directory and filename prefix. */
    if (slash != NULL) {

        prefix_length = (int)(slash - word) + 1;

        if (prefix_length >= INPUT_SIZE) {
            return 0;
        }

        memcpy(path_prefix, word, (size_t)prefix_length);
        path_prefix[prefix_length] = '\0';

        filename_prefix = slash + 1;

        /* Absolute path. */
        if (word[0] == '/') {

            int directory_length = (int)(slash - word);

            if (directory_length == 0) {
                strcpy(directory, "/");
            } else {
                memcpy(directory, word,
                       (size_t)directory_length);
                directory[directory_length] = '\0';
            }
        }

        /* Relative path. */
        else {

            int directory_length = (int)(slash - word);

            char relative_directory[INPUT_SIZE];

            memcpy(relative_directory, word,
                   (size_t)directory_length);
            relative_directory[directory_length] = '\0';

            snprintf(directory, sizeof(directory),
                     "%s/%s", cwd, relative_directory);
        }
    }

    /* No slash: search the tab's current directory. */
    else {

        strcpy(directory, cwd);

        path_prefix[0] = '\0';
        filename_prefix = word;
    }

    DIR *dir = opendir(directory);

    if (dir == NULL) {
        return 0;
    }

    int match_count = 0;
    struct dirent *entry;

    size_t search_length = strlen(filename_prefix);

    while ((entry = readdir(dir)) != NULL) {

        if (strncmp(entry->d_name,
                    filename_prefix,
                    search_length) == 0) {

            if (match_count >= max_matches) {
                break;
            }

            int result = snprintf(
                matches[match_count],
                INPUT_SIZE,
                "%s%s",
                path_prefix,
                entry->d_name
            );

            if (result >= 0 && result < INPUT_SIZE) {
                match_count++;
            }
        }
    }

    closedir(dir);

    return match_count;
}
/* Find the longest common prefix among matching filenames. */
int longest_common_prefix(char matches[][INPUT_SIZE],
                          int match_count)
{
    if (match_count <= 0) {
        return 0;
    }

    int length = (int)strlen(matches[0]);

    for (int i = 1; i < match_count; i++) {

        int j = 0;

        while (j < length &&
               matches[0][j] == matches[i][j]) {
            j++;
        }

        length = j;

        if (length == 0) {
            break;
        }
    }

    return length;
}
/* Complete the filename immediately before the cursor. */
void complete_filename(Tab *tab)
{
    char prefix[INPUT_SIZE];
    char matches[INPUT_SIZE][INPUT_SIZE];

    /* Find the beginning of the word before the cursor. */
    int word_start = tab->cursor;

    while (word_start > 0 &&
           tab->input[word_start - 1] != ' ' &&
           tab->input[word_start - 1] != '\t') {
        word_start--;
    }

    int prefix_length = tab->cursor - word_start;

    if (prefix_length == 0) {
        return;
    }

    memcpy(prefix, tab->input + word_start,
           (size_t)prefix_length);
    prefix[prefix_length] = '\0';

    int match_count = find_file_matches(
        tab->cwd,
        prefix,
        matches,
        INPUT_SIZE
    );

    /* No matching filename. */
    if (match_count == 0) {
        return;
    }

    /* One match: complete the filename. */
    if (match_count == 1) {
        int completed_length = (int)strlen(matches[0]);
        int suffix_length = tab->length - tab->cursor;

        if (word_start + completed_length + suffix_length
            >= INPUT_SIZE) {
            return;
        }

        memmove(
            tab->input + word_start + completed_length,
            tab->input + tab->cursor,
            (size_t)suffix_length + 1
        );

        memcpy(
            tab->input + word_start,
            matches[0],
            (size_t)completed_length
        );

        tab->length = word_start + completed_length + suffix_length;
        tab->cursor = word_start + completed_length;

        return;
    }

    /* Multiple matches: complete their common prefix. */
    int common_length = longest_common_prefix(
        matches,
        match_count
    );

    if (common_length > prefix_length) {

        int suffix_length = tab->length - tab->cursor;

        if (word_start + common_length + suffix_length
            >= INPUT_SIZE) {
            return;
        }

        memmove(
            tab->input + word_start + common_length,
            tab->input + tab->cursor,
            (size_t)suffix_length + 1
        );

        memcpy(
            tab->input + word_start,
            matches[0],
            (size_t)common_length
        );

        tab->length = word_start + common_length + suffix_length;
        tab->cursor = word_start + common_length;
    }
    else {
        /* Still ambiguous: remember the matching filenames. */
        tab->completion_active = 1;
        tab->completion_word_start = word_start;
        tab->completion_count = match_count;

        for (int i = 0; i < match_count; i++) {

            strcpy(
                tab->completion_matches[i],
                matches[i]
            );

            char line[INPUT_SIZE + 32];

            snprintf(
                line,
                sizeof(line),
                "%d. %s\n",
                i + 1,
                matches[i]
            );

            append_output(
                &tab->output,
                &tab->output_capacity,
                &tab->output_length,
                line
            );
        }

        tab->scroll_offset = 0;
    }
}
/* Insert clipboard text at the current input cursor. */
void insert_clipboard_text(Tab *tab,
                           const unsigned char *text,
                           unsigned long text_length)
{
    for (unsigned long i = 0;
         i < text_length && tab->length < INPUT_SIZE - 1;
         i++) {

        unsigned char c = text[i];

        /* Keep pasted commands on the current input line. */
        if (c == '\n' || c == '\r') {
            continue;
        }

        /* Ignore control characters other than ordinary text. */
        if (c < ' ' || c == 127) {
            continue;
        }

        memmove(&tab->input[tab->cursor + 1],
                &tab->input[tab->cursor],
                (size_t)(tab->length - tab->cursor + 1));

        tab->input[tab->cursor] = (char)c;
        tab->cursor++;
        tab->length++;
    }
}





int run_background(
    char *args[],
    const char *command,
    const char *input_file,
    const char *output_file
)
{
    
    if (job_count >= MAX_JOBS) {
        return -1;
    }
    jobs[job_count].tab_index = -1;

    pid_t pid = fork();

    if (pid == -1) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        /* Create a separate process group for this job. */
        if (setpgid(0, 0) == -1) {
            perror("setpgid");
            _exit(1);
        }

        /* Apply input redirection. */
        if (input_file != NULL) {
            int fd = open(input_file, O_RDONLY);

            if (fd == -1) {
                perror(input_file);
                _exit(1);
            }

            if (dup2(fd, STDIN_FILENO) == -1) {
                perror("dup2 stdin");
                close(fd);
                _exit(1);
            }

            close(fd);
        }

        /* Apply output redirection. */
        if (output_file != NULL) {
            int fd = open(
                output_file,
                O_WRONLY | O_CREAT | O_TRUNC,
                0666
            );

            if (fd == -1) {
                perror(output_file);
                _exit(1);
            }

            if (dup2(fd, STDOUT_FILENO) == -1) {
                perror("dup2 stdout");
                close(fd);
                _exit(1);
            }

            close(fd);
        }

        execvp(args[0], args);
        perror(args[0]);
        _exit(127);
    }

    /* The parent also attempts to set the child's process group. */
    if (setpgid(pid, pid) == -1) {
        perror("setpgid");
    }

    jobs[job_count].pid = pid;
    jobs[job_count].pgid = pid;
    jobs[job_count].output_fd = -1;

    snprintf(
        jobs[job_count].command,
        sizeof(jobs[job_count].command),
        "%s",
        command
    );

    jobs[job_count].running = 1;
    jobs[job_count].stopped = 0;
    job_count++;

    return (int)pid;
}

int run_foreground_async(
    Tab *tab,
    char *args[],
    const char *command,
    const char *input_file,
    const char *output_file
)
{
    int pipefd[2];

    if (pipe(pipefd) == -1) {
        perror("pipe");
        return -1;
    }

    pid_t pid = fork();

    if (pid == -1) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);

        /* Create a separate process group. */
        if (setpgid(0, 0) == -1) {
            perror("setpgid");
            _exit(1);
        }

        /* Redirect standard error to the output pipe. */
        if (dup2(pipefd[1], STDERR_FILENO) == -1) {
            perror("dup2 stderr");
            _exit(1);
        }

        if (output_file != NULL) {
            int fd = open(
                output_file,
                O_WRONLY | O_CREAT | O_TRUNC,
                0666
            );

            if (fd == -1) {
                perror(output_file);
                _exit(1);
            }

            if (dup2(fd, STDOUT_FILENO) == -1) {
                perror("dup2 stdout");
                close(fd);
                _exit(1);
            }

            close(fd);
        }
        else {
            if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
                perror("dup2 stdout");
                _exit(1);
            }
        }

        close(pipefd[1]);

        if (input_file != NULL) {
            int fd = open(input_file, O_RDONLY);

            if (fd == -1) {
                perror(input_file);
                _exit(1);
            }

            if (dup2(fd, STDIN_FILENO) == -1) {
                perror("dup2 stdin");
                close(fd);
                _exit(1);
            }

            close(fd);
        }

        execvp(args[0], args);

        perror(args[0]);
        _exit(127);
    }

    close(pipefd[1]);

    /* Parent also assigns the process group. */
    if (setpgid(pid, pid) == -1 && errno != EACCES && errno != ESRCH) {
        perror("setpgid");
    }

    /* Make the read end nonblocking. */
    int flags = fcntl(pipefd[0], F_GETFL, 0);

    if (flags == -1 ||
        fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl");
        kill(-pid, SIGKILL);
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        return -1;
    }

    tab->foreground.pid = pid;
    tab->foreground.pgid = pid;
    tab->foreground.output_fd = pipefd[0];
    tab->foreground.active = 1;
    tab->foreground.job_index = -1;
    tab->foreground.stopped = 0;

    snprintf(
        tab->foreground.command,
        sizeof(tab->foreground.command),
        "%s",
        command
    );

    return 0;
}


void update_foreground_output(Tab *tab)
{
    if (tab == NULL) {
        return;
    }
    if (tab->foreground.output_fd != -1){

        char buffer[4097];

        while (1) {
            ssize_t bytes_read = read(
                tab->foreground.output_fd,
                buffer,
                sizeof(buffer)-1
            );

            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';
                int old_length = tab->output_length;

                if (append_output(
                        &tab->output,
                        &tab->output_capacity,
                        &tab->output_length,
                        buffer) == -1) {
                    fprintf(stderr, "Error storing command output.\n");
                    break;
                }

                if (tab->output_length > old_length) {
                    tab->scroll_offset = 0;
                }
            }
            else if (bytes_read == 0) {
                close(tab->foreground.output_fd);
                tab->foreground.output_fd = -1;
                break;
            }
            else {
                if (errno == EINTR) {
                    continue;
                }

                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }

                perror("read foreground output");
                close(tab->foreground.output_fd);
                tab->foreground.output_fd = -1;
                break;
            }
        }
    }

    if (tab->foreground.active) {
        int status;
        pid_t result = waitpid(
            tab->foreground.pid,
            &status,
            WNOHANG | WUNTRACED | WCONTINUED
        );

        if (result == tab->foreground.pid) {
            if (WIFSTOPPED(status)) {
                tab->foreground.stopped = 1;
            }
            else if (WIFCONTINUED(status)) {
                tab->foreground.stopped = 0;
            }
            else if (WIFEXITED(status) || WIFSIGNALED(status)) {
                tab->foreground.active = 0;

                if (WIFEXITED(status)) {
                    last_status = WEXITSTATUS(status);
                }
                else {
                    last_status = 128 + WTERMSIG(status);
                }
            }
        }
    }
}
void update_job_output(Job *job)
{
    if (job == NULL || job->output_fd == -1) {
        return;
    }

    if (job->tab_index < 0 || job->tab_index >= tab_count) {
        close(job->output_fd);
        job->output_fd = -1;
        return;
    }

    Tab *tab = &tabs[job->tab_index];
    char buffer[4097];

    while (1) {
        ssize_t bytes_read = read(
            job->output_fd,
            buffer,
            sizeof(buffer) - 1
        );

        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';

            if (append_output(
                    &tab->output,
                    &tab->output_capacity,
                    &tab->output_length,
                    buffer) == -1) {
                fprintf(stderr, "Error storing background job output.\n");
                break;
            }

            tab->scroll_offset = 0;
        }
        else if (bytes_read == 0) {
            close(job->output_fd);
            job->output_fd = -1;
            break;
        }
        else {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }

            perror("read job output");
            close(job->output_fd);
            job->output_fd = -1;
            break;
        }
    }
}

void update_job_status(void)
{
    for (int i = 0; i < job_count; i++) {
        if (!jobs[i].running) {
            continue;
        }

        int status;
        pid_t result = waitpid(
            jobs[i].pid,
            &status,
            WNOHANG | WUNTRACED | WCONTINUED
        );

        if (result == jobs[i].pid) {
            if (WIFSTOPPED(status)) {
                jobs[i].stopped = 1;
            }
            else if (WIFCONTINUED(status)) {
                jobs[i].stopped = 0;
            }
            else if (WIFEXITED(status) || WIFSIGNALED(status)) {
                jobs[i].running = 0;
                jobs[i].stopped = 0;
            }
        }
    }
}

int find_job(int job_number)
{
    if (job_number < 1 || job_number > job_count) {
        return -1;
    }

    return job_number - 1;
}


int read_script_line(FILE *script, char line[INPUT_SIZE])
{
    if (fgets(line, INPUT_SIZE, script) == NULL) {
        return 0;
    }

    line[strcspn(line, "\n")] = '\0';
    return 1;
}
static int parse_multiwatch_commands(
    const char *input,
    char commands[][INPUT_SIZE],
    int *command_count
) {
    if (input == NULL || commands == NULL || command_count == NULL) {
        return -1;
    }

    *command_count = 0;

    const char *p = input;

    while (*p == ' ' || *p == '\t') {
        p++;
    }

    const char keyword[] = "multiWatch";
    size_t keyword_length = sizeof(keyword) - 1;

    if (strncmp(p, keyword, keyword_length) != 0) {
        return -1;
    }

    p += keyword_length;

    if (*p != ' ' && *p != '\t' && *p != '[') {
        return -1;
    }

    while (*p == ' ' || *p == '\t') {
        p++;
    }

    if (*p != '[') {
        return -1;
    }

    p++;

    while (*p == ' ' || *p == '\t') {
        p++;
    }

    while (*p != '\0' && *p != ']') {
        if (*command_count >= MULTIWATCH_MAX_COMMANDS) {
            return -1;
        }

        if (*p != '"') {
            return -1;
        }

        p++;

        int length = 0;

        while (*p != '\0' && *p != '"') {
            if (*p == '\\' && p[1] == '"') {
                p++;
            }

            if (length >= INPUT_SIZE - 1) {
                return -1;
            }

            commands[*command_count][length++] = *p++;
        }

        if (*p != '"' || length == 0) {
            return -1;
        }

        commands[*command_count][length] = '\0';
        (*command_count)++;

        p++;

        while (*p == ' ' || *p == '\t') {
            p++;
        }

        if (*p == ',') {
            p++;

            while (*p == ' ' || *p == '\t') {
                p++;
            }
        } else if (*p != ']') {
            return -1;
        }
    }

    if (*p != ']' || *command_count == 0) {
        return -1;
    }

    p++;

    while (*p == ' ' || *p == '\t') {
        p++;
    }

    if (*p != '\0' && *p != '\n') {
        return -1;
    }

    return 0;
}
static int multiwatch_start_round(void)
{
    int count = multiwatch_session.command_count;

    for (int i = 0; i < count; i++) {
        multiwatch_session.processes[i].pid = -1;
        multiwatch_session.processes[i].output_fd = -1;
        multiwatch_session.processes[i].command = NULL;
        multiwatch_session.processes[i].finished = 1;
    }

    for (int i = 0; i < count; i++) {
        if (multiwatch_start(
                &multiwatch_session.processes[i],
                multiwatch_session.commands[i]) == -1) {

            multiwatch_stop(
                multiwatch_session.processes,
                count
            );

            multiwatch_session.round_active = 0;
            return -1;
        }

        multiwatch_session.header_printed[i] = 0;
        multiwatch_session.timestamps[i] = time(NULL);
    }

    multiwatch_session.round_active = 1;
    return 0;
}
static void multiwatch_update(Tab *tab)
{
    if (!multiwatch_session.running || tab == NULL) {
        return;
    }

    /* Start a new round when the refresh time arrives. */
    if (!multiwatch_session.round_active) {
        if (time(NULL) < multiwatch_session.next_refresh_time) {
            return;
        }

        if (multiwatch_start_round() == -1) {
            append_output(
                &tab->output,
                &tab->output_capacity,
                &tab->output_length,
                "multiWatch: failed to start commands.\n"
            );

            multiwatch_session.running = 0;
            return;
        }
    }

    char buffer[MULTIWATCH_BUFFER_SIZE];

    /*
     * Read several ready chunks per update, without waiting
     * for any command to produce output.
     */
    for (int attempt = 0;
         attempt < MULTIWATCH_MAX_COMMANDS;
         attempt++) {

        int process_index = -1;

        int bytes_read = multiwatch_read(
            multiwatch_session.processes,
            multiwatch_session.command_count,
            buffer,
            sizeof(buffer),
            &process_index
        );

        if (bytes_read < 0) {
            append_output(
                &tab->output,
                &tab->output_capacity,
                &tab->output_length,
                "multiWatch: error reading command output.\n"
            );

            multiwatch_stop(
                multiwatch_session.processes,
                multiwatch_session.command_count
            );

            multiwatch_session.round_active = 0;
            multiwatch_session.running = 0;
            return;
        }

        if (bytes_read == 0) {
            break;
        }

        if (process_index < 0 ||
            process_index >= multiwatch_session.command_count) {
            continue;
        }

        int i = process_index;

        if (multiwatch_session.header_printed[i] == 0) {
            char header[INPUT_SIZE + 100];

            snprintf(
                header,
                sizeof(header),
                "\"%s\", %lld:\n"
                "----------------------------------------------------\n",
                multiwatch_session.commands[i],
                (long long)multiwatch_session.timestamps[i]
            );

            append_output(
                &tab->output,
                &tab->output_capacity,
                &tab->output_length,
                header
            );

            multiwatch_session.header_printed[i] = 1;
        }

        append_output(
            &tab->output,
            &tab->output_capacity,
            &tab->output_length,
            buffer
        );
    }

    /*
     * Close output blocks for commands whose pipes reached EOF.
     * State 2 means that the block has already been closed.
     */
    for (int i = 0;
         i < multiwatch_session.command_count;
         i++) {

        if (multiwatch_session.processes[i].finished &&
            multiwatch_session.header_printed[i] != 2) {

            if (multiwatch_session.header_printed[i] == 0) {
                char header[INPUT_SIZE + 100];

                snprintf(
                    header,
                    sizeof(header),
                    "\"%s\", %lld:\n"
                    "----------------------------------------------------\n",
                    multiwatch_session.commands[i],
                    (long long)multiwatch_session.timestamps[i]
                );

                append_output(
                    &tab->output,
                    &tab->output_capacity,
                    &tab->output_length,
                    header
                );
            }

            append_output(
                &tab->output,
                &tab->output_capacity,
                &tab->output_length,
                "\n----------------------------------------------------\n"
            );

            multiwatch_session.header_printed[i] = 2;
        }
    }

    /* Start the next round two seconds after all commands finish. */
    if (multiwatch_active_count(
            multiwatch_session.processes,
            multiwatch_session.command_count) == 0) {

        multiwatch_stop(
            multiwatch_session.processes,
            multiwatch_session.command_count
        );

        multiwatch_session.round_active = 0;
        multiwatch_session.next_refresh_time = time(NULL) + 2;
    }
}
int main(void)
{
    /* Load user-configured keyboard shortcuts. */
    load_keybindings();
    history_load();
    Display *display = XOpenDisplay(NULL);

    if (display == NULL) {
        fprintf(stderr, "Cannot open X display.\n");
        return 1;
    }
    
    /* Initialize clipboard atoms used by X11. */
    clipboard_atom = XInternAtom(display, "CLIPBOARD", False);
    utf8_string_atom = XInternAtom(display, "UTF8_STRING", False);
    targets_atom = XInternAtom(display, "TARGETS", False);
    paste_property_atom =
    XInternAtom(display, "TERMINATOR_PASTE", False);


    int screen = DefaultScreen(display);

    Window window = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        100, 100,
        WINDOW_WIDTH, WINDOW_HEIGHT,
        1,
        BlackPixel(display, screen),
        WhitePixel(display, screen)
    );

    XStoreName(display, window, "Terminator - Custom Terminal");

    XSelectInput(display, window,
        ExposureMask |
        StructureNotifyMask |
        KeyPressMask |
        ButtonPressMask |
        ButtonReleaseMask |
        PointerMotionMask);
    
    Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wm_delete, 1);

    XMapWindow(display, window);

    GC gc = XCreateGC(display, window, 0, NULL);

    /* Initialize the first tab. */
    tab_count = 1;
    active_tab = 0;

    if (initialize_tab(0) == -1) {
        fprintf(stderr, "Cannot initialize first tab.\n");
        XFreeGC(display, gc);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return 1;
    }

    Tab *tab = &tabs[active_tab];

    XEvent event;
    int running = 1;

    while (running) {
        /* Poll X11 and all foreground command output pipes. */
        if (XPending(display) == 0) {
            struct pollfd poll_fds[MAX_TABS + MAX_JOBS + 1];
            int fd_tabs[MAX_TABS + MAX_JOBS + 1];
            int fd_jobs[MAX_TABS + MAX_JOBS + 1];
            int fd_count = 1;

            poll_fds[0].fd = ConnectionNumber(display);
            poll_fds[0].events = POLLIN;
            poll_fds[0].revents = 0;
            fd_tabs[0] = -1;
            fd_jobs[0] = -1;
            for (int i = 0; i < tab_count; i++) {
                if (tabs[i].foreground.output_fd != -1) {
                    poll_fds[fd_count].fd =
                        tabs[i].foreground.output_fd;

                    poll_fds[fd_count].events = POLLIN;
                    poll_fds[fd_count].revents = 0;
                    fd_tabs[fd_count] = i;
                    fd_jobs[fd_count] = -1;

                    fd_count++;
                }
            }
            /* Add output pipes belonging to suspended/resumed jobs. */
            for (int i = 0; i < job_count; i++) {
                if (jobs[i].output_fd != -1) {
                    poll_fds[fd_count].fd = jobs[i].output_fd;
                    poll_fds[fd_count].events = POLLIN;
                    poll_fds[fd_count].revents = 0;

                    fd_tabs[fd_count] = -1;
                    fd_jobs[fd_count] = i;

                    fd_count++;
                }
            }

            int has_foreground_process = 0;

            for (int i = 0; i < tab_count; i++) {
                if (tabs[i].foreground.active) {
                    has_foreground_process = 1;
                    break;
                }
            }

            int timeout = (multiwatch_session.running ||
                        has_foreground_process) ? 100 : -1;

            int ready = poll(poll_fds, fd_count, timeout);

            if (ready == -1) {
                if (errno == EINTR) {
                    continue;
                }

                perror("poll: event loop");
                running = 0;
                break;
            }

            /* Read output from whichever foreground pipes are ready. */
            for (int i = 1; i < fd_count; i++) {
                if (poll_fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                    int index = fd_tabs[i];

                    if (fd_jobs[i] >= 0 && fd_jobs[i] < job_count) {
                        update_job_output(&jobs[fd_jobs[i]]);
                    }
                    else if (index >= 0 && index < tab_count) {
                        update_foreground_output(&tabs[index]);
                    }
                }
            }
            /* Recheck foreground processes whose output pipes are closed. */
            for (int i = 0; i < tab_count; i++) {
                if (tabs[i].foreground.active &&
                    tabs[i].foreground.output_fd == -1) {

                    update_foreground_output(&tabs[i]);
                }
            }

            /* Update multiWatch even when no keyboard event arrives. */
            if (multiwatch_session.running) {
                multiwatch_update(&tabs[active_tab]);
            }

            /* If no X11 event is waiting, redraw and poll again. */
            if (XPending(display) == 0) {
                tab = &tabs[active_tab];

                draw_input(
                    display,
                    window,
                    gc,
                    tab->input,
                    tab->length,
                    tab->output,
                    tab->output_length
                );

                XFlush(display);
                continue;
            }
        }
    
        XNextEvent(display, &event);

        if (event.type == Expose) {
            tab = &tabs[active_tab];

            draw_input(display, window, gc,
                       tab->input, tab->length,
                       tab->output, tab->output_length);
        }
        
        else if (event.type == SelectionNotify) {
            if (event.xselection.selection == clipboard_atom &&
                event.xselection.target == utf8_string_atom &&
                event.xselection.property != None) {

                Atom actual_type;
                int actual_format;
                unsigned long item_count;
                unsigned long bytes_after;
                unsigned char *data = NULL;

                int result = XGetWindowProperty(
                    display,
                    window,
                    paste_property_atom,
                    0,
                    4096,
                    True,
                    AnyPropertyType,
                    &actual_type,
                    &actual_format,
                    &item_count,
                    &bytes_after,
                    &data
                );

                if (result == Success &&
                    data != NULL &&
                    actual_format == 8 &&
                    (actual_type == utf8_string_atom ||
                    actual_type == XA_STRING)) {

                    tab = &tabs[active_tab];

                    insert_clipboard_text(tab, data, item_count);

                    draw_input(display, window, gc,
                            tab->input, tab->length,
                            tab->output, tab->output_length);
                }

                if (data != NULL) {
                    XFree(data);
                }
            }
        }

        
        else if (event.type == SelectionRequest) {
            XSelectionRequestEvent *request = &event.xselectionrequest;
            XSelectionEvent response = {0};

            response.type = SelectionNotify;
            response.display = request->display;
            response.requestor = request->requestor;
            response.selection = request->selection;
            response.target = request->target;
            response.time = request->time;
            response.property = None;

            if (request->selection == clipboard_atom &&
                clipboard_text != NULL) {

                Atom property = request->property;

                if (property == None) {
                    property = request->target;
                }

                if (request->target == targets_atom) {
                    Atom supported[] = {
                        targets_atom,
                        utf8_string_atom,
                        XA_STRING
                    };

                    XChangeProperty(
                        display,
                        request->requestor,
                        property,
                        XA_ATOM,
                        32,
                        PropModeReplace,
                        (unsigned char *)supported,
                        3
                    );

                    response.property = property;
                } else if (request->target == utf8_string_atom ||
                        request->target == XA_STRING) {

                    XChangeProperty(
                        display,
                        request->requestor,
                        property,
                        request->target,
                        8,
                        PropModeReplace,
                        (unsigned char *)clipboard_text,
                        (int)strlen(clipboard_text)
                    );

                    response.property = property;
                }
            }

            XSendEvent(
                display, request->requestor, False, 0,
                (XEvent *)&response
            );

            XFlush(display);
        }


        else if (event.type == ClientMessage) {
            if ((Atom)event.xclient.data.l[0] == wm_delete) {
                running = 0;
            }
        }

        else if (event.type == ButtonPress) {
            int click_x = event.xbutton.x;
            int click_y = event.xbutton.y;

            /* Handle clicks on the tab bar. */
            if (click_y < TAB_BAR_HEIGHT) {
                int clicked_tab = click_x / TAB_WIDTH;

                if (clicked_tab < tab_count) {
                    int close_x =
                        (clicked_tab + 1) * TAB_WIDTH - 20;

                    if (click_x >= close_x) {
                        close_tab(clicked_tab);
                    }
                    else {
                        active_tab = clicked_tab;

                        if (chdir(tabs[active_tab].cwd) == -1) {
                            /* Keep the current directory if restoration fails. */
                        }
                    }
                }

                else if (clicked_tab == tab_count &&
                         tab_count < MAX_TABS) {
                    int new_index = tab_count;

                    if (initialize_tab(new_index) == 0) {
                        tab_count++;
                        active_tab = new_index;
                    }
                }
            }

            /* Handle mouse-wheel scrolling outside the tab bar. */
            else if (event.xbutton.button == Button4) {
                tab = &tabs[active_tab];

                if (tab->scroll_offset <
                    count_output_lines(tab->output,
                                       tab->output_length)) {
                    tab->scroll_offset++;
                }
            }

            else if (event.xbutton.button == Button5) {
                tab = &tabs[active_tab];

                if (tab->scroll_offset > 0) {
                    tab->scroll_offset--;
                }
            }
            
/* Start selecting text in the input line with the left mouse button. */
            else if (event.xbutton.button == Button1 &&
                    click_y >= PROMPT_Y - 15 &&
                    click_y <= PROMPT_Y + 5) {
                tab = &tabs[active_tab];

                int input_x = tab->continuing ? 25 : 120;

                selection_start = input_position_from_x(
                    display, gc, tab->input, tab->length,
                    input_x, click_x
                );

                selection_end = selection_start;
                selecting_text = 1;
            }



            /* Refresh the pointer after switching or closing tabs. */
            tab = &tabs[active_tab];

            draw_input(display, window, gc,
                       tab->input, tab->length,
                       tab->output, tab->output_length);
        }
        
        else if (event.type == MotionNotify && selecting_text) {
            tab = &tabs[active_tab];

            int input_x = tab->continuing ? 25 : 120;

            selection_end = input_position_from_x(
                display, gc, tab->input, tab->length,
                input_x, event.xmotion.x
            );

            draw_input(display, window, gc,
                    tab->input, tab->length,
                    tab->output, tab->output_length);
        }
        
        else if (event.type == ButtonRelease &&
                event.xbutton.button == Button1 &&
                selecting_text) {
            tab = &tabs[active_tab];

            int input_x = tab->continuing ? 25 : 120;

            selection_end = input_position_from_x(
                display, gc, tab->input, tab->length,
                input_x, event.xbutton.x
            );

            selecting_text = 0;

            draw_input(display, window, gc,
                    tab->input, tab->length,
                    tab->output, tab->output_length);
        }
        else if (event.type == KeyPress) {
            tab = &tabs[active_tab];

            KeySym key;
            char key_text[32];

            int key_length = XLookupString(
                &event.xkey,
                key_text,
                sizeof(key_text),
                &key,
                NULL
            );
            
            /* Handle keyboard input while Ctrl+R search is active. */
            if (tab->history_search_active) {
            
                /* Escape: cancel search and restore previous input. */
                if (key == XK_Escape) {
            
                    strcpy(tab->input, tab->saved_input);
                    tab->length = tab->saved_length;
                    tab->cursor = tab->saved_cursor;
            
                    tab->history_search_active = 0;
                }
            
                /* Backspace: remove the last character from search term. */
                else if (key == XK_BackSpace) {
            
                    if (tab->length > 0) {
                        tab->length--;
                        tab->input[tab->length] = '\0';
                        tab->cursor = tab->length;
                    }
            
                    strcpy(tab->history_search_term, tab->input);
                }
            
                /* Enter: perform the history search. */
                else if (key == XK_Return || key == XK_KP_Enter) {
            
                    int matches[HISTORY_SIZE];
            
                    int match_count = history_search(
                        tab->history_search_term,
                        matches,
                        HISTORY_SIZE
                    );
            
                    if (match_count == 0) {
            
                        append_output(
                            &tab->output,
                            &tab->output_capacity,
                            &tab->output_length,
                            "No match for search term in history\n"
                        );
                    }
                    else {
            
                        for (int i = 0; i < match_count; i++) {
            
                            char line[HISTORY_INPUT_SIZE + 2];
            
                            snprintf(
                                line,
                                sizeof(line),
                                "%s\n",
                                history[matches[i]]
                            );
            
                            append_output(
                                &tab->output,
                                &tab->output_capacity,
                                &tab->output_length,
                                line
                            );
                        }
                    }
            
                    /* Restore the command that was being typed before Ctrl+R. */
                    strcpy(tab->input, tab->saved_input);
                    tab->length = tab->saved_length;
                    tab->cursor = tab->saved_cursor;
            
                    tab->history_search_active = 0;
                }
            
                /* Add a character to the search term. */
                else if (key_length > 0 &&
                         tab->length + key_length < INPUT_SIZE) {
            
                    int valid_text = 1;
            
                    for (int i = 0; i < key_length; i++) {
                        if (key_text[i] < 32 || key_text[i] > 126) {
                            valid_text = 0;
                            break;
                        }
                    }
            
                    if (valid_text) {
            
                        for (int i = 0; i < key_length; i++) {
                            tab->input[tab->length++] = key_text[i];
                        }
            
                        tab->input[tab->length] = '\0';
                        tab->cursor = tab->length;
            
                        strcpy(tab->history_search_term, tab->input);
                    }
                }
            
                draw_input(
                    display,
                    window,
                    gc,
                    tab->input,
                    tab->length,
                    tab->output,
                    tab->output_length
                );
            
                XFlush(display);
                continue;
            }
            /* Tab: complete the filename before the cursor. */
            if (key == XK_Tab) {
                /* Clear any previous completion choices. */
                tab->completion_active = 0;
                tab->completion_count = 0;

                complete_filename(tab);


                draw_input(
                    display,
                    window,
                    gc,
                    tab->input,
                    tab->length,
                    tab->output,
                    tab->output_length
                );

                XFlush(display);
                continue;
            }
            /* Select a filename using its displayed number. */
            if (tab->completion_active) {

                if (key >= XK_1 && key <= XK_9) {

                    int choice = (int)(key - XK_1);

                    if (choice < tab->completion_count) {

                        char selected[INPUT_SIZE];

                        strcpy(
                            selected,
                            tab->completion_matches[choice]
                        );

                        int word_start = tab->completion_word_start;
                        int old_end = tab->cursor;

                        int selected_length = (int)strlen(selected);
                        int suffix_length = tab->length - old_end;

                        if (word_start + selected_length + suffix_length
                            < INPUT_SIZE) {

                            /* Preserve everything after the cursor. */
                            memmove(
                                tab->input + word_start + selected_length,
                                tab->input + old_end,
                                (size_t)suffix_length + 1
                            );

                            /* Insert the selected filename. */
                            memcpy(
                                tab->input + word_start,
                                selected,
                                (size_t)selected_length
                            );

                            tab->length =
                                word_start + selected_length + suffix_length;

                            tab->cursor = word_start + selected_length;
                        }

                        tab->completion_active = 0;
                        tab->completion_count = 0;

                        draw_input(
                            display,
                            window,
                            gc,
                            tab->input,
                            tab->length,
                            tab->output,
                            tab->output_length
                        );

                        XFlush(display);
                        continue;
                    }
                }

                /* Any other key cancels the pending selection. */
                tab->completion_active = 0;
                tab->completion_count = 0;
            }
            
            /* Configurable paste shortcut. */
            if (key_matches(KEY_PASTE, key, event.xkey.state)) {

                XConvertSelection(
                    display,
                    clipboard_atom,
                    utf8_string_atom,
                    paste_property_atom,
                    window,
                    CurrentTime
                );

                XFlush(display);
                continue;
            }

            
            /* Configurable copy shortcut. */
            if (key_matches(KEY_COPY, key, event.xkey.state)) {

                int start = selection_start;
                int end = selection_end;

                if (start > end) {
                    int temp = start;
                    start = end;
                    end = temp;
                }

                if (start >= 0 && end > start &&
                    end <= tab->length) {

                    size_t text_length = (size_t)(end - start);
                    char *new_text = malloc(text_length + 1);

                    if (new_text != NULL) {
                        memcpy(new_text, tab->input + start, text_length);
                        new_text[text_length] = '\0';

                        free(clipboard_text);
                        clipboard_text = new_text;

                        XSetSelectionOwner(
                            display, clipboard_atom, window, CurrentTime
                        );

                        XFlush(display);
                    }
                }

                continue;
            }
            /* Ctrl+R: start history search. */
            if (key_matches(KEY_HISTORY_SEARCH, key, event.xkey.state)) {

                if (!tab->history_search_active) {

                    /* Preserve the current command input. */
                    strcpy(tab->saved_input, tab->input);
                    tab->saved_length = tab->length;
                    tab->saved_cursor = tab->cursor;

                    /* Start with an empty search term. */
                    tab->history_search_term[0] = '\0';
                    tab->history_search_active = 1;

                    tab->input[0] = '\0';
                    tab->length = 0;
                    tab->cursor = 0;
                }

                draw_input(
                    display,
                    window,
                    gc,
                    tab->input,
                    tab->length,
                    tab->output,
                    tab->output_length
                );

                XFlush(display);
                continue;
            }
            /* Handle Ctrl+Z: suspend the foreground command. */
            if ((event.xkey.state & ControlMask) &&
                (key == XK_z || key == XK_Z)) {

                if (tab->foreground.active &&
                    !tab->foreground.stopped &&
                    tab->foreground.pgid > 0) {

                    if (tab->foreground.job_index == -1 &&
                            job_count >= MAX_JOBS)  {
                        append_output(
                            &tab->output,
                            &tab->output_capacity,
                            &tab->output_length,
                            "Cannot suspend: job table is full.\n"
                        );
                    }
                    else if (kill(-tab->foreground.pgid, SIGTSTP) == -1) {
                        perror("Ctrl+Z: SIGTSTP");
                    }
                    else {
                        int status;
                        pid_t result;

                        do {
                            result = waitpid(
                                tab->foreground.pid,
                                &status,
                                WUNTRACED
                            );
                        } while (result == -1 && errno == EINTR);

                        if (result == tab->foreground.pid &&
                            WIFSTOPPED(status)) {
                            int job_index = tab->foreground.job_index;

                            /* Create a new job only if this process has no previous job entry. */
                            if (job_index == -1) {
                                job_index = job_count;
                                job_count++;
                            }
                            
                            int job_number = job_index + 1;
                            
                            jobs[job_index].pid = tab->foreground.pid;
                            jobs[job_index].pgid = tab->foreground.pgid;
                            jobs[job_index].output_fd = tab->foreground.output_fd;
                            jobs[job_index].tab_index = active_tab;
                            
                            snprintf(
                                jobs[job_index].command,
                                sizeof(jobs[job_index].command),
                                "%s",
                                tab->foreground.command
                            );
                            
                            jobs[job_index].running = 1;
                            jobs[job_index].stopped = 1;

                            char message[INPUT_SIZE + 100];

                            snprintf(
                                message,
                                sizeof(message),
                                "\n^Z\n[%d]+ Stopped %s\n",
                                job_number,
                                tab->foreground.command
                            );

                            append_output(
                                &tab->output,
                                &tab->output_capacity,
                                &tab->output_length,
                                message
                            );

                            /* Release the foreground process slot. */
                            tab->foreground.pid = -1;
                            tab->foreground.pgid = -1;
                            tab->foreground.output_fd = -1;
                            tab->foreground.active = 0;
                            tab->foreground.stopped = 0;
                            tab->foreground.job_index = -1;
                            tab->foreground.command[0] = '\0';

                            tab->scroll_offset = 0;
                        }
                        else {
                            append_output(
                                &tab->output,
                                &tab->output_capacity,
                                &tab->output_length,
                                "Error: could not suspend foreground command.\n"
                            );
                        }
                    }
                }

                draw_input(
                    display,
                    window,
                    gc,
                    tab->input,
                    tab->length,
                    tab->output,
                    tab->output_length
                );

                XFlush(display);
                continue;
            }

            /* Handle Ctrl+C. */
            if ((event.xkey.state & ControlMask) &&
                (key == XK_c || key == XK_C)) {

                /* Stop the foreground command, if one is running. */
                if (tab->foreground.active &&
                    !tab->foreground.stopped &&
                    tab->foreground.pgid > 0) {

                    if (kill(-tab->foreground.pgid, SIGINT) == 0) {

                        append_output(
                            &tab->output,
                            &tab->output_capacity,
                            &tab->output_length,
                            "\n^C\n"
                        );

                        tab->scroll_offset = 0;
                    }
                }

                /* Preserve the existing MultiWatch behaviour. */
                if (multiwatch_session.running) {

                    multiwatch_stop(
                        multiwatch_session.processes,
                        multiwatch_session.command_count
                    );

                    multiwatch_session.running = 0;
                    multiwatch_session.round_active = 0;
                    multiwatch_session.command_count = 0;

                    append_output(
                        &tab->output,
                        &tab->output_capacity,
                        &tab->output_length,
                        "\nmultiWatch stopped.\n"
                    );

                    tab->scroll_offset = 0;
                }

                draw_input(
                    display,
                    window,
                    gc,
                    tab->input,
                    tab->length,
                    tab->output,
                    tab->output_length
                );

                XFlush(display);

                continue;
            }

            /* Enter: execute the command. */
            if (key == XK_Return || key == XK_KP_Enter) {
                if (tab->foreground.active) {
                    append_output(
                        &tab->output,
                        &tab->output_capacity,
                        &tab->output_length,
                        "A foreground command is already running.\n"
                    );
                
                    draw_input(
                        display,
                        window,
                        gc,
                        tab->input,
                        tab->length,
                        tab->output,
                        tab->output_length
                    );
                
                    continue;
                }
                
                /* Handle multiline commands before normal execution. */
                if (tab->length > 0) {
                    tab->input[tab->length] = '\0';
                }

                /* Case 1: A trailing backslash continues the command. */
                if (tab->length > 0 &&
                    tab->input[tab->length - 1] == '\\') {

                    int line_length = tab->length - 1;

                    if (tab->pending_length + line_length >= INPUT_SIZE) {
                        append_output(&tab->output,
                                    &tab->output_capacity,
                                    &tab->output_length,
                                    "Terminator: command too long.\n");

                        tab->pending_input[0] = '\0';
                        tab->pending_length = 0;
                        tab->continuing = 0;
                        tab->length = 0;
                        tab->cursor = 0;
                        tab->input[0] = '\0';
                    }
                    else {
                        memcpy(tab->pending_input + tab->pending_length,
                            tab->input, (size_t)line_length);

                        tab->pending_length += line_length;
                        tab->pending_input[tab->pending_length] = '\0';

                        tab->continuing = 1;
                        tab->length = 0;
                        tab->input[0] = '\0';
                    }

                    draw_input(display, window, gc,
                            tab->input, tab->length,
                            tab->output, tab->output_length);

                    continue;
                }

                /* Case 2: Join the current line to previously saved lines. */
                if (tab->continuing) {
                    if (tab->pending_length + tab->length >= INPUT_SIZE) {
                        append_output(&tab->output,
                                    &tab->output_capacity,
                                    &tab->output_length,
                                    "Terminator: command too long.\n");

                        tab->pending_input[0] = '\0';
                        tab->pending_length = 0;
                        tab->continuing = 0;
                        tab->length = 0;
                        tab->cursor = 0;
                        tab->input[0] = '\0';

                        draw_input(display, window, gc,
                                tab->input, tab->length,
                                tab->output, tab->output_length);

                        continue;
                    }

                    memcpy(tab->pending_input + tab->pending_length,
                        tab->input, (size_t)tab->length);

                    tab->pending_length += tab->length;
                    tab->pending_input[tab->pending_length] = '\0';

                    memcpy(tab->input, tab->pending_input,
                        (size_t)tab->pending_length + 1);

                    tab->length = tab->pending_length;

                    tab->pending_input[0] = '\0';
                    tab->pending_length = 0;
                    tab->continuing = 0;
                }

                /* Case 3: An unfinished quote needs another line. */
                if (tab->length > 0 &&
                    command_needs_more_input(tab->input)) {

                    if (tab->length + 1 >= INPUT_SIZE) {
                        append_output(&tab->output,
                                    &tab->output_capacity,
                                    &tab->output_length,
                                    "Terminator: command too long.\n");

                        tab->length = 0;
                        tab->cursor = 0;
                        tab->input[0] = '\0';
                        tab->pending_length = 0;
                        tab->pending_input[0] = '\0';
                        tab->continuing = 0;
                    }
                    else {
                        memcpy(tab->pending_input, tab->input,
                            (size_t)tab->length);

                        tab->pending_length = tab->length;
                        tab->pending_input[tab->pending_length++] = '\n';
                        tab->pending_input[tab->pending_length] = '\0';

                        tab->continuing = 1;
                        tab->length = 0;
                        tab->input[0] = '\0';
                    }

                    draw_input(display, window, gc,
                            tab->input, tab->length,
                            tab->output, tab->output_length);

                    continue;
                }

                if (tab->length == 0) {
                    append_output(&tab->output,
                                  &tab->output_capacity,
                                  &tab->output_length,
                                  "Terminator>\n");

                    tab->scroll_offset = 0;
                }

                if (tab->length > 0) {
                    tab->input[tab->length] = '\0';
                    char command[INPUT_SIZE];
                    int command_length = 0;

                    char parameter_quote = '\0';
                    int parameter_escaped = 0;

                    for (int i = 0; tab->input[i] != '\0'; i++) {
                        char c = tab->input[i];

                        if (!parameter_escaped &&
                            parameter_quote != '\'' &&
                            c == '$' && tab->input[i + 1] == '?') {
                            char status_text[12];
                            snprintf(status_text, sizeof(status_text),
                                     "%d", last_status);
                            int status_length = (int)strlen(status_text);

                            if (command_length + status_length >= INPUT_SIZE) {
                                break;
                            }

                            memcpy(&command[command_length], status_text,
                                   (size_t)status_length);
                            command_length += status_length;
                            i++;
                            continue;
                        }

                        if (command_length >= INPUT_SIZE - 1) {
                            break;
                        }

                        command[command_length++] = c;

                        if (parameter_escaped) {
                            parameter_escaped = 0;
                        } else if (c == '\\' && parameter_quote != '\'') {
                            parameter_escaped = 1;
                        } else if (parameter_quote == c) {
                            parameter_quote = '\0';
                        } else if (parameter_quote == '\0' &&
                                   (c == '\'' || c == '"')) {
                            parameter_quote = c;
                        }
                    }

                    command[command_length] = '\0';
                    
                                                        
                    /* Handle the multiWatch command before normal command parsing. */
                    if (strncmp(command, "multiWatch", 10) == 0 &&
                        (command[10] == ' ' || command[10] == '\t' ||
                        command[10] == '[')) {

                            /* Display and store the original multiWatch command. */
                            history_add(tab->input);
                            tab->history_position = history_count;
                            
                            char multiwatch_command_line[INPUT_SIZE + 20];
                            
                            snprintf(multiwatch_command_line,
                                     sizeof(multiwatch_command_line),
                                     "Terminator> %s\n", tab->input);
                            
                            append_output(&tab->output,
                                          &tab->output_capacity,
                                          &tab->output_length,
                                          multiwatch_command_line);

                        int parsed_count = 0;

                        char parsed_commands[MULTIWATCH_MAX_COMMANDS][INPUT_SIZE];

                        if (multiwatch_session.running) {
                            append_output(
                                &tab->output,
                                &tab->output_capacity,
                                &tab->output_length,
                                "multiWatch: a monitoring session is already running.\n"
                            );
                        }
                        else if (parse_multiwatch_commands(
                                    command,
                                    parsed_commands,
                                    &parsed_count) != 0) {

                            append_output(
                                &tab->output,
                                &tab->output_capacity,
                                &tab->output_length,
                                "Usage: multiWatch [\"cmd1\", \"cmd2\", ...]\n"
                            );
                        }
                        else {
                            multiwatch_session.command_count = parsed_count;

                            for (int i = 0; i < parsed_count; i++) {
                                snprintf(
                                    multiwatch_session.commands[i],
                                    sizeof(multiwatch_session.commands[i]),
                                    "%s",
                                    parsed_commands[i]
                                );
                            }

                            multiwatch_session.running = 1;
                            multiwatch_session.round_active = 0;
                            multiwatch_session.next_refresh_time = time(NULL);

                            append_output(
                                &tab->output,
                                &tab->output_capacity,
                                &tab->output_length,
                                "multiWatch: commands accepted; monitoring will start shortly.\n"
                            );
                        }

                        tab->length = 0;
                        tab->cursor = 0;
                        tab->input[0] = '\0';
                        tab->history_position = history_count;

                        draw_input(
                            display,
                            window,
                            gc,
                            tab->input,
                            tab->length,
                            tab->output,
                            tab->output_length
                        );

                        continue;
                    }
                    int has_pipeline = find_pipeline_separator(command) != NULL;
                    
                        
                    int background = 0;
                    int background_length = (int)strlen(command);
                    
                    while (background_length > 0 &&
                            (command[background_length - 1] == ' ' ||
                            command[background_length - 1] == '\t')) {
                        command[--background_length] = '\0';
                    }
                    
                    if (background_length > 0 &&
                        command[background_length - 1] == '&') {
                        background = 1;
                        command[--background_length] = '\0';
                    
                        while (background_length > 0 &&
                                (command[background_length - 1] == ' ' ||
                                command[background_length - 1] == '\t')) {
                            command[--background_length] = '\0';
                        }
                    }
                        

                    history_add(tab->input);
                    tab->history_position = history_count;

                    char command_line[INPUT_SIZE + 20];

                    snprintf(command_line, sizeof(command_line),
                             "Terminator> %s\n", tab->input);

                    if (append_output(&tab->output,
                                      &tab->output_capacity,
                                      &tab->output_length,
                                      command_line) == -1) {
                        running = 0;
                        continue;
                    }

                    tab->scroll_offset = 0;

                    if (has_pipeline) {
                        char *commands[MAX_PIPE_COMMANDS][MAX_PIPE_ARGS] = {
                            {NULL}
                        };

                        int command_count = 0;
                        int parse_error = 0;

                        char *stage = command;
                        char *separator;

                        char input_file[INPUT_SIZE] = "";
                        char output_file[INPUT_SIZE] = "";

                        int redirection_status = parse_redirections(
                            command,
                            input_file, sizeof(input_file),
                            output_file, sizeof(output_file)
                        );

                        if (redirection_status != 0) {
                            parse_error = 1;
                        }

                        while (!parse_error) {
                            separator = find_pipeline_separator(stage);

                            if (separator != NULL) {
                                *separator = '\0';
                            }

                            if (command_count >= MAX_PIPE_COMMANDS) {
                                parse_error = 1;
                                break;
                            }

                            int arg_count = parse_command(
                                stage, commands[command_count]
                            );

                            if (arg_count <= 0) {
                                parse_error = 1;
                                break;
                            }

                            command_count++;

                            if (separator == NULL) {
                                break;
                            }

                            stage = separator + 1;
                        }

                        if (command_count < 2) {
                            parse_error = 1;
                        }

                        if (parse_error) {
                            append_output(
                                &tab->output,
                                &tab->output_capacity,
                                &tab->output_length,
                                "Error: invalid pipeline or too many "
                                "commands/arguments.\n"
                            );
                        }
                        
                        else {
                            last_status = run_pipeline(
                                commands,
                                command_count,
                                input_file[0] != '\0' ? input_file : NULL,
                                output_file[0] != '\0' ? output_file : NULL,
                                &tab->output,
                                &tab->output_capacity,
                                &tab->output_length
                            );

                            if (last_status == -1) {
                                append_output(
                                    &tab->output,
                                    &tab->output_capacity,
                                    &tab->output_length,
                                    "Error executing pipeline.\n"
                                );

                            last_status = 1;
                            }
                        }
                    }

                    else {
                        char *args[MAX_PIPE_ARGS];

                        char input_file[INPUT_SIZE] = "";
                        char output_file[INPUT_SIZE] = "";

                        int redirection_status = parse_redirections(
                            command,
                            input_file, sizeof(input_file),
                            output_file, sizeof(output_file)
                        );

                        int arg_count = (redirection_status == 0)
                            ? parse_command(command, args)
                            : -1;
                        if (arg_count < 0) {
                            append_output(
                                &tab->output,
                                &tab->output_capacity,
                                &tab->output_length,
                                "Error: invalid command or too many arguments.\n"
                            );
                        }

                        
                        else if (arg_count > 0) {
                            if (strcmp(args[0], "cd") == 0) {
                                const char *directory;

                                if (args[1] == NULL) {
                                    directory = getenv("HOME");
                                }
                                else {
                                    directory = args[1];
                                }

                                if (directory == NULL) {
                                    append_output(
                                        &tab->output,
                                        &tab->output_capacity,
                                        &tab->output_length,
                                        "Error: HOME is not set.\n"
                                    );
                                }

                                else if (chdir(directory) == -1) {
                                    char error_message[INPUT_SIZE + 100];

                                    snprintf(
                                        error_message,
                                        sizeof(error_message),
                                        "cd: %s: %s\n",
                                        directory, strerror(errno)
                                    );

                                    append_output(
                                        &tab->output,
                                        &tab->output_capacity,
                                        &tab->output_length,
                                        error_message
                                    );
                                }

                                else {
                                    if (getcwd(tab->cwd,
                                               sizeof(tab->cwd)) == NULL) {
                                        tab->cwd[0] = '\0';
                                    }
                                }
                            }
                            
                            else if (strcmp(args[0], "source") == 0) {
                                if (args[1] == NULL) {
                                    append_output(&tab->output,
                                                &tab->output_capacity,
                                                &tab->output_length,
                                                "source: missing filename\n");
                                    last_status = 1;
                                } else {
                                    FILE *script = fopen(args[1], "r");

                                    if (script == NULL) {
                                        char error_message[INPUT_SIZE + 100];

                                        snprintf(error_message, sizeof(error_message),
                                                "source: %s: %s\n",
                                                args[1], strerror(errno));

                                        append_output(&tab->output,
                                                    &tab->output_capacity,
                                                    &tab->output_length,
                                                    error_message);
                                        last_status = 1;
                                    } else {
                                        char line[INPUT_SIZE];

                                        while (read_script_line(script, line)) {
                                            if (line[0] == '\0') {
                                                continue;
                                            }

                                            char *script_args[10];
                                            int count = 0;

                                            char *token = strtok(line, " \t");

                                            while (token != NULL && count < 9) {
                                                script_args[count++] = token;
                                                token = strtok(NULL, " \t");
                                            }

                                            script_args[count] = NULL;

                                            if (count == 0) {
                                                continue;
                                            }

                                            if (strcmp(script_args[0], "cd") == 0) {
                                                const char *directory =
                                                    script_args[1] != NULL
                                                    ? script_args[1] : getenv("HOME");

                                                if (directory == NULL || chdir(directory) == -1) {
                                                    append_output(&tab->output,
                                                                &tab->output_capacity,
                                                                &tab->output_length,
                                                                "source: cd failed\n");
                                                    last_status = 1;
                                                } else {
                                                    if (getcwd(tab->cwd, sizeof(tab->cwd)) == NULL) {
                                                        tab->cwd[0] = '\0';
                                                    }
                                                    last_status = 0;
                                                }
                                            } else if (strcmp(script_args[0], "exit") == 0) {
                                                running = 0;
                                                break;
                                            } else {
                                                last_status = run_command(
                                                    script_args,
                                                    &tab->output,
                                                    &tab->output_capacity,
                                                    &tab->output_length
                                                );
                                                if (last_status == -1) {
                                                    last_status = 1;
                                                    append_output(&tab->output,
                                                                &tab->output_capacity,
                                                                &tab->output_length,
                                                                "source: command execution failed\n");
                                                }
                                            }
                                        }

                                        fclose(script);
                                    }
                                }
                            }
                            else if (strcmp(args[0], "history") == 0) {
                                int start = history_count > 1000 ? history_count - 1000 : 0;
                            
                                for (int i = start; i < history_count; i++) {
                                    char history_line[120];
                            
                                    snprintf(history_line, sizeof(history_line),
                                            "%d  %s\n", i + 1, history[i]);
                            
                                    if (append_output(&tab->output,
                                                    &tab->output_capacity,
                                                    &tab->output_length,
                                                    history_line) == -1) {
                                        last_status = 1;
                                        break;
                                    }
                                }
                            
                                last_status = 0;
                            }                          
                            else if (strcmp(args[0], "jobs") == 0) {
                                update_job_status();

                                for (int i = 0; i < job_count; i++) {
                                    char job_line[INPUT_SIZE + 100];
                                    const char *state;

                                    if (!jobs[i].running) {
                                        state = "Done";
                                    } else if (jobs[i].stopped) {
                                        state = "Stopped";
                                    } else {
                                        state = "Running";
                                    }

                                    snprintf(job_line, sizeof(job_line),
                                            "[%d] %s %s\n",
                                            i + 1, state, jobs[i].command);

                                    if (append_output(&tab->output,
                                                    &tab->output_capacity,
                                                    &tab->output_length,
                                                    job_line) == -1) {
                                        last_status = 1;
                                        break;
                                    }
                                }
                                if (last_status != 1) {
                                    last_status = 0;
                                }
                            }
                            
                            else if (strcmp(args[0], "bg") == 0) {
                                int job_number = job_count;
                            
                                if (args[1] != NULL) {
                                    if (args[1][0] == '%') {
                                        job_number = atoi(args[1] + 1);
                                    } else {
                                        job_number = atoi(args[1]);
                                    }
                                }
                            
                                int index = find_job(job_number);

                                if (index == -1 || !jobs[index].running) {
                                    append_output(&tab->output,
                                                &tab->output_capacity,
                                                &tab->output_length,
                                                "bg: invalid or finished job\n");
                                    last_status = 1;
                                } else if (kill(-jobs[index].pgid, SIGCONT) == -1) {
                                    perror("bg: SIGCONT");
                                    last_status = 1;
                                } else {
                                    jobs[index].stopped = 0;
                                    jobs[index].running = 1;

                                    char message[INPUT_SIZE + 100];
                                    snprintf(message, sizeof(message),
                                            "[%d] Running %s\n",
                                            job_number, jobs[index].command);

                                    append_output(&tab->output,
                                                &tab->output_capacity,
                                                &tab->output_length,
                                                message);
                                    last_status = 0;
                                }
                            }
                            else if (strcmp(args[0], "fg") == 0) {

                                update_job_status();
                            
                                int job_number = job_count;
                            
                                if (args[1] != NULL) {
                                    job_number = atoi(args[1]);
                                }
                            
                                int index = find_job(job_number);
                            
                                if (index == -1 || !jobs[index].running) {
                            
                                    append_output(
                                        &tab->output,
                                        &tab->output_capacity,
                                        &tab->output_length,
                                        "fg: invalid or finished job\n"
                                    );
                            
                                    last_status = 1;
                                }
                                else if (tab->foreground.active ||
                                         tab->foreground.output_fd != -1) {
                            
                                    append_output(
                                        &tab->output,
                                        &tab->output_capacity,
                                        &tab->output_length,
                                        "fg: another foreground command is active\n"
                                    );
                            
                                    last_status = 1;
                                }
                                else if (kill(-jobs[index].pgid, SIGCONT) == -1) {
                            
                                    perror("fg: SIGCONT");
                                    last_status = 1;
                                }
                                else {
                            
                                    /* Transfer the job back to the foreground tab. */
                                    tab->foreground.pid = jobs[index].pid;
                                    tab->foreground.pgid = jobs[index].pgid;
                                    tab->foreground.output_fd = jobs[index].output_fd;
                                    tab->foreground.active = 1;
                                    tab->foreground.stopped = 0;
                                    tab->foreground.job_index = index;
                            
                                    snprintf(
                                        tab->foreground.command,
                                        sizeof(tab->foreground.command),
                                        "%s",
                                        jobs[index].command
                                    );
                            
                                    /* The job is now managed as a foreground process. */
                                    jobs[index].output_fd = -1;
                                    jobs[index].running = 0;
                                    jobs[index].stopped = 0;
                            
                                    last_status = 0;
                                }
                            }

                            else if (strcmp(args[0], "exit") == 0) {
                                running = 0;
                            }                            
                            else {
                                if (background) {
                                    int pid = run_background(
                                        args,
                                        tab->input,
                                        input_file[0] != '\0' ? input_file : NULL,
                                        output_file[0] != '\0' ? output_file : NULL
                                    );

                                    if (pid == -1) {
                                        last_status = 1;

                                        append_output(
                                            &tab->output,
                                            &tab->output_capacity,
                                            &tab->output_length,
                                            "Error starting background command.\n"
                                        );
                                    }
                                    else {
                                        last_status = 0;

                                        char message[64];
                                        snprintf(message, sizeof(message),
                                                "[%d] Started\n", pid);

                                        append_output(
                                            &tab->output,
                                            &tab->output_capacity,
                                            &tab->output_length,
                                            message
                                        );
                                    }
                                }
                                else {
                                    int result = run_foreground_async(
                                        tab,
                                        args,
                                        tab->input,
                                        input_file[0] != '\0' ? input_file : NULL,
                                        output_file[0] != '\0' ? output_file : NULL
                                    );
                                
                                    if (result == -1) {
                                        append_output(
                                            &tab->output,
                                            &tab->output_capacity,
                                            &tab->output_length,
                                            "Error starting foreground command.\n"
                                        );
                                
                                        last_status = 1;
                                    }
                                    else {
                                        last_status = 0;
                                    }
                                }
                            }
                        }
                    }

                    tab->length = 0;
                    tab->cursor = 0;
                    tab->input[0] = '\0';
                    tab->history_position = history_count;
                }
            }

            
            /* Move cursor left. */
            else if (key_matches(KEY_MOVE_LEFT, key, event.xkey.state)) {
                if (tab->cursor > 0) {
                    tab->cursor--;
                }
            }

            /* Move cursor right. */
            else if (key_matches(KEY_MOVE_RIGHT, key, event.xkey.state)) {
                if (tab->cursor < tab->length) {
                    tab->cursor++;
                }
            }

            /* Move cursor to beginning of input. */
            else if (key_matches(KEY_LINE_START, key, event.xkey.state)) {
                tab->cursor = 0;
            }

            /* Move cursor to end of input. */
            else if (key_matches(KEY_LINE_END, key, event.xkey.state)) {
                tab->cursor = tab->length;
            }

            /* Backspace: delete the character before the cursor. */
            else if (key == XK_BackSpace) {
                if (tab->cursor > 0) {
                    memmove(&tab->input[tab->cursor - 1],
                            &tab->input[tab->cursor],
                            (size_t)(tab->length - tab->cursor + 1));

                    tab->cursor--;
                    tab->length--;
                }
            }

            /* Delete: delete the character at the cursor. */
            else if (key == XK_Delete) {
                if (tab->cursor < tab->length) {
                    memmove(&tab->input[tab->cursor],
                            &tab->input[tab->cursor + 1],
                            (size_t)(tab->length - tab->cursor));
                    tab->length--;
                }
            }

            /* Previous command in history. */
            else if (key_matches(KEY_HISTORY_UP, key, event.xkey.state)) {
                if (tab->history_position > 0) {
                    tab->history_position--;

                    strcpy(tab->input, history[tab->history_position]);
                    tab->length = (int)strlen(tab->input);
                    tab->cursor = tab->length;
                }
            }

            /* Next command in history. */
            else if (key_matches(KEY_HISTORY_DOWN, key, event.xkey.state)) {
                if (tab->history_position < history_count - 1) {
                    tab->history_position++;

                    strcpy(tab->input, history[tab->history_position]);
                    tab->length = (int)strlen(tab->input);
                    tab->cursor = tab->length;
                } else {
                    tab->history_position = history_count;
                    tab->length = 0;
                    tab->cursor = 0;
                    tab->input[0] = '\0';
                }
            }
            
            /* Ctrl+U: delete from the beginning to the cursor. */
            else if (key_matches(KEY_DELETE_TO_START, key, event.xkey.state)) {
                memmove(tab->input,
                        &tab->input[tab->cursor],
                        (size_t)(tab->length - tab->cursor + 1));

                tab->length -= tab->cursor;
                tab->cursor = 0;
            }

            /* Ctrl+K: delete from the cursor to the end. */
            else if (key_matches(KEY_DELETE_TO_END, key, event.xkey.state)) {
                tab->length = tab->cursor;
                tab->input[tab->length] = '\0';
            }

            /* Ctrl+W: delete the previous word. */
            else if (key_matches(KEY_DELETE_WORD, key, event.xkey.state)) {
                int start = tab->cursor;

                /* Skip whitespace immediately before the cursor. */
                while (start > 0 &&
                    (tab->input[start - 1] == ' ' ||
                        tab->input[start - 1] == '\t')) {
                    start--;
                }

                /* Skip the previous word. */
                while (start > 0 &&
                    tab->input[start - 1] != ' ' &&
                    tab->input[start - 1] != '\t') {
                    start--;
                }

                memmove(&tab->input[start],
                        &tab->input[tab->cursor],
                        (size_t)(tab->length - tab->cursor + 1));

                tab->length -= tab->cursor - start;
                tab->cursor = start;
            }


            /* Clear displayed output. */
            else if (key_matches(KEY_CLEAR_SCREEN, key, event.xkey.state)) {
                tab->output_length = 0;
                tab->output[0] = '\0';
                tab->scroll_offset = 0;
            }

            /* Exit when the input is empty. */
            else if (key_matches(KEY_EXIT_SHELL, key, event.xkey.state) &&
                    tab->length == 0) {
                running = 0;
            }


            /* Insert ordinary typed characters at the cursor. */
            else if (key_length > 0 &&
                    !(event.xkey.state & ControlMask) &&
                    tab->length + key_length < INPUT_SIZE) {
                for (int i = 0; i < key_length; i++) {
                    if (key_text[i] >= ' ' && key_text[i] != 127 &&
                        tab->length < INPUT_SIZE - 1) {
                        memmove(&tab->input[tab->cursor + 1],
                                &tab->input[tab->cursor],
                                (size_t)(tab->length - tab->cursor + 1));

                        tab->input[tab->cursor] = key_text[i];
                        tab->cursor++;
                        tab->length++;
                    }
                }
            }


            /* Up arrow: previous command. */
            else if (key == XK_Up) {
                if (tab->history_position > 0) {
                    tab->history_position--;

                    strcpy(tab->input,
                           history[tab->history_position]);

                    tab->length = (int)strlen(tab->input);
                }
            }

            /* Down arrow: next command. */
            else if (key == XK_Down) {
                if (tab->history_position < history_count - 1) {
                    tab->history_position++;

                    strcpy(tab->input,
                           history[tab->history_position]);

                    tab->length = (int)strlen(tab->input);
                }
                else {
                    tab->history_position = history_count;
                    tab->length = 0;
                    tab->input[0] = '\0';
                }
            }

            /* Ctrl+L: clear displayed output. */
            else if ((key == XK_l || key == XK_L) &&
                     (event.xkey.state & ControlMask)) {
                tab->output_length = 0;
                tab->output[0] = '\0';
                tab->scroll_offset = 0;
            }

            /* Ctrl+D: exit when the input is empty. */
            else if (key == XK_d &&
                     (event.xkey.state & ControlMask) &&
                     tab->length == 0) {
                running = 0;
            }

            /* Append ordinary typed characters. */
            else if (key_length > 0 &&
                     !(event.xkey.state & ControlMask) &&
                     tab->length < INPUT_SIZE - 1) {
                for (int i = 0;
                     i < key_length &&
                     tab->length < INPUT_SIZE - 1;
                     i++) {
                    if (key_text[i] >= ' ' && key_text[i] != 127) {
                        tab->input[tab->length++] = key_text[i];
                    }
                }

                tab->input[tab->length] = '\0';
            }

            draw_input(display, window, gc,
                       tab->input, tab->length,
                       tab->output, tab->output_length);
        }
        if (multiwatch_session.running) {
            tab = &tabs[active_tab];
            multiwatch_update(tab);

            draw_input(
                display,
                window,
                gc,
                tab->input,
                tab->length,
                tab->output,
                tab->output_length
            );

XFlush(display);
        
            XFlush(display);
        }
    }

    /* Free every tab's output buffer. */
    for (int i = 0; i < tab_count; i++) {
        free(tabs[i].output);
    }

    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    return 0;
}
