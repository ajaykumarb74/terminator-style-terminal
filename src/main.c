
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_TABS 4
#define BUFFER_SIZE 16000
#define INPUT_SIZE 1024
#define COMMAND_SIZE 4096
#define HISTORY_SIZE 1000
#define MAX_TOKENS 256
#define MAX_ARGS 64
#define MAX_PIPELINE 32
#define LINE_HEIGHT 20

typedef struct {
    char output[BUFFER_SIZE];
    char input[INPUT_SIZE];
    int input_length;

    char command[COMMAND_SIZE];
    int command_length;
    int continuation;

    char history[HISTORY_SIZE][INPUT_SIZE];
    int history_count;
} Tab;

typedef struct {
    char value[INPUT_SIZE];
    int type;
} Token;

enum {
    TOKEN_WORD,
    TOKEN_PIPE,
    TOKEN_INPUT,
    TOKEN_OUTPUT
};

Display *display;
Window window;
GC gc;
XFontStruct *font;

Tab tabs[MAX_TABS];
int tab_count = 1;
int active_tab = 0;
int running = 1;
int last_status = 0;

void append_output(Tab *tab, const char *text) {
    size_t used = strlen(tab->output);

    if (used >= BUFFER_SIZE - 1) return;

    size_t available = BUFFER_SIZE - used - 1;
    strncat(tab->output, text, available);
}

void save_history(Tab *tab, const char *command) {
    if (command[0] == '\0') return;

    if (tab->history_count < HISTORY_SIZE) {
        snprintf(tab->history[tab->history_count],
                 INPUT_SIZE, "%s", command);
        tab->history_count++;
    } else {
        memmove(tab->history, tab->history + 1,
                (HISTORY_SIZE - 1) * sizeof(tab->history[0]));

        snprintf(tab->history[HISTORY_SIZE - 1],
                 INPUT_SIZE, "%s", command);
    }
}

/*
 * Split a command into words while respecting single and
 * double quotes. Recognize |, < and > as separate tokens.
 *
 * Return 0 on success and -1 on a syntax/size error.
 */
int tokenize(const char *command, Token tokens[], int *token_count) {
    int count = 0;
    int i = 0;

    while (command[i] != '\0') {
        while (isspace((unsigned char)command[i])) i++;

        if (command[i] == '\0') break;

        if (count >= MAX_TOKENS) return -1;

        if (command[i] == '|' ||
            command[i] == '<' ||
            command[i] == '>') {
            tokens[count].value[0] = command[i];
            tokens[count].value[1] = '\0';

            if (command[i] == '|') tokens[count].type = TOKEN_PIPE;
            if (command[i] == '<') tokens[count].type = TOKEN_INPUT;
            if (command[i] == '>') tokens[count].type = TOKEN_OUTPUT;

            count++;
            i++;
            continue;
        }

        int j = 0;
        char quote = '\0';

        while (command[i] != '\0') {
            char c = command[i];

            if (quote == '\0' && isspace((unsigned char)c)) break;

            if (quote == '\0' && (c == '\'' || c == '"')) {
                quote = c;
                i++;
                continue;
            }

            if (quote != '\0' && c == quote) {
                quote = '\0';
                i++;
                continue;
            }

            if (c == '\\' && quote != '\'') {
                if (command[i + 1] == '\0') return -1;

                i++;
                c = command[i];
            }

            if (j >= INPUT_SIZE - 1) return -1;

            tokens[count].value[j++] = c;
            i++;
        }

        if (quote != '\0') return -1;

        tokens[count].value[j] = '\0';
        tokens[count].type = TOKEN_WORD;
        count++;
    }

    *token_count = count;
    return 0;
}

void draw_window(void) {
    XClearWindow(display, window);

    char heading[256] = "";
    char part[64];

    for (int i = 0; i < tab_count; i++) {
        snprintf(part, sizeof(part), " [%d]%s ",
                 i + 1, i == active_tab ? "*" : "");

        if (strlen(heading) + strlen(part) < sizeof(heading) - 1) {
            strcat(heading, part);
        }
    }

    XDrawString(display, window, gc, 10, 22,
                heading, (int)strlen(heading));

    Tab *tab = &tabs[active_tab];

    XWindowAttributes attributes;
    XGetWindowAttributes(display, window, &attributes);

    int max_lines = (attributes.height - 75) / LINE_HEIGHT;
    if (max_lines < 1) max_lines = 1;

    char *copy = malloc(strlen(tab->output) + 1);

    if (copy != NULL) {
        strcpy(copy, tab->output);

        char *lines[512];
        int count = 0;
        char *line = strtok(copy, "\n");

        while (line != NULL && count < 512) {
            lines[count++] = line;
            line = strtok(NULL, "\n");
        }

        int start = count > max_lines ? count - max_lines : 0;

        for (int i = start; i < count; i++) {
            XDrawString(display, window, gc, 10,
                        48 + (i - start) * LINE_HEIGHT,
                        lines[i], (int)strlen(lines[i]));
        }

        free(copy);
    }

    int prompt_y = attributes.height - 20;

    const char *prompt = tab->continuation
        ? "> "
        : "user@terminator> ";

    XDrawString(display, window, gc, 10, prompt_y,
                prompt, (int)strlen(prompt));

    XDrawString(display, window, gc, 180, prompt_y,
                tab->input, tab->input_length);

    XFlush(display);
}

/* Check whether quotes are still open or a line ends in \. */
int needs_continuation(const char *text) {
    char quote = '\0';
    int escaped = 0;
    int length = (int)strlen(text);

    for (int i = 0; i < length; i++) {
        char c = text[i];

        if (escaped) {
            escaped = 0;
            continue;
        }

        if (c == '\\' && quote != '\'') {
            escaped = 1;
            continue;
        }

        if (quote == '\0' && (c == '\'' || c == '"')) {
            quote = c;
        } else if (quote != '\0' && c == quote) {
            quote = '\0';
        }
    }

    return quote != '\0' || escaped;
}

/* Run a pipeline of external commands. */
int execute_pipeline(Tab *tab, Token tokens[], int count) {
    char *args[MAX_PIPELINE][MAX_ARGS];
    char *input_files[MAX_PIPELINE];
    char *output_files[MAX_PIPELINE];
    int stages = 1;
    int argc[MAX_PIPELINE] = {0};

    memset(input_files, 0, sizeof(input_files));
    memset(output_files, 0, sizeof(output_files));

    for (int i = 0; i < count; i++) {
        if (tokens[i].type == TOKEN_PIPE) {
            stages++;

            if (stages > MAX_PIPELINE) {
	                append_output(tab, "terminator: too many pipeline stages\n");
                return 2;
            }
        }
    }

    int stage = 0;

    for (int i = 0; i < count; i++) {
        if (tokens[i].type == TOKEN_PIPE) {
            if (argc[stage] == 0) {
                append_output(tab, "terminator: empty pipeline stage\n");
                return 2;
            }

            stage++;
            continue;
        }

        if (tokens[i].type == TOKEN_INPUT ||
            tokens[i].type == TOKEN_OUTPUT) {
            int is_input = tokens[i].type == TOKEN_INPUT;

            if (i + 1 >= count ||
                tokens[i + 1].type != TOKEN_WORD) {
                append_output(tab, "terminator: missing redirection filename\n");
                return 2;
            }

            if (is_input) {
                input_files[stage] = tokens[++i].value;
            } else {
                output_files[stage] = tokens[++i].value;
            }

            continue;
        }

        if (argc[stage] >= MAX_ARGS - 1) {
            append_output(tab, "terminator: too many arguments\n");
            return 2;
        }

        args[stage][argc[stage]++] = tokens[i].value;
    }

    if (argc[stage] == 0) {
        append_output(tab, "terminator: empty command\n");
        return 2;
    }

    for (int s = 0; s < stages; s++) {
        args[s][argc[s]] = NULL;
    }

    int output_pipe[2];

    if (pipe(output_pipe) == -1) {
        append_output(tab, "terminator: pipe failed\n");
        return 1;
    }

    int previous_read = -1;
    pid_t pids[MAX_PIPELINE];

    for (int s = 0; s < stages; s++) {
        int next_pipe[2] = {-1, -1};

        if (s < stages - 1 && pipe(next_pipe) == -1) {
            append_output(tab, "terminator: pipe failed\n");

            if (previous_read != -1) close(previous_read);
            close(output_pipe[0]);
            close(output_pipe[1]);

            return 1;
        }

        pid_t pid = fork();

        if (pid == -1) {
            append_output(tab, "terminator: fork failed\n");

            if (previous_read != -1) close(previous_read);
            if (next_pipe[0] != -1) close(next_pipe[0]);
            if (next_pipe[1] != -1) close(next_pipe[1]);

            close(output_pipe[0]);
            close(output_pipe[1]);

            return 1;
        }

        if (pid == 0) {
            if (previous_read != -1) {
                dup2(previous_read, STDIN_FILENO);
            }

            if (s < stages - 1) {
                dup2(next_pipe[1], STDOUT_FILENO);
            } else {
                dup2(output_pipe[1], STDOUT_FILENO);
            }

            dup2(output_pipe[1], STDERR_FILENO);

            if (previous_read != -1) close(previous_read);
            if (next_pipe[0] != -1) close(next_pipe[0]);
            if (next_pipe[1] != -1) close(next_pipe[1]);

            close(output_pipe[0]);
            close(output_pipe[1]);

            if (input_files[s] != NULL) {
                int fd = open(input_files[s], O_RDONLY);

                if (fd == -1) {
                    perror(input_files[s]);
                    _exit(1);
                }

                dup2(fd, STDIN_FILENO);
                close(fd);
            }

            if (output_files[s] != NULL) {
                int fd = open(output_files[s],
                              O_WRONLY | O_CREAT | O_TRUNC, 0644);

                if (fd == -1) {
                    perror(output_files[s]);
                    _exit(1);
                }

                dup2(fd, STDOUT_FILENO);
                close(fd);
            }

            execvp(args[s][0], args[s]);

            perror(args[s][0]);
            _exit(127);
        }

        pids[s] = pid;

        if (previous_read != -1) close(previous_read);
        if (next_pipe[1] != -1) close(next_pipe[1]);

        previous_read = next_pipe[0];
    }

    if (previous_read != -1) close(previous_read);
    close(output_pipe[1]);

    char buffer[512];
    ssize_t bytes;
while ((bytes = read(output_pipe[0], buffer, sizeof(buffer))) > 0) {
    size_t used = strlen(tab->output);

    if (used < BUFFER_SIZE - 1) {
        size_t available = BUFFER_SIZE - used - 1;
        size_t amount = (size_t)bytes;

        if (amount > available) {
            amount = available;
        }

        memcpy(tab->output + used, buffer, amount);
        tab->output[used + amount] = '\0';
    }
}
    close(output_pipe[0]);

    int result = 0;

    for (int s = 0; s < stages; s++) {
        int status;

        if (waitpid(pids[s], &status, 0) == -1) {
            result = 1;
        } else if (s == stages - 1) {
            if (WIFEXITED(status)) {
                result = WEXITSTATUS(status);
            } else {
                result = 1;
            }
        }
    }

    return result;
}

void execute_command(Tab *tab) {
    if (tab->input_length == 0) return;

    char line[INPUT_SIZE];

    snprintf(line, sizeof(line), "%s", tab->input);

    append_output(tab, tab->continuation ? "> " : "user@terminator> ");
    append_output(tab, line);
    append_output(tab, "\n");

    if (tab->command_length > 0 &&
        tab->command_length < COMMAND_SIZE - 1) {
        tab->command[tab->command_length++] = '\n';
    }

    size_t line_length = strlen(line);

    if (tab->command_length + (int)line_length >= COMMAND_SIZE) {
        append_output(tab, "terminator: command too long\n");
        tab->command_length = 0;
        tab->command[0] = '\0';
        tab->continuation = 0;
        goto clear_input;
    }

    memcpy(tab->command + tab->command_length, line, line_length);
    tab->command_length += (int)line_length;
    tab->command[tab->command_length] = '\0';

    if (needs_continuation(tab->command)) {
        tab->continuation = 1;
        goto clear_input;
    }

    tab->continuation = 0;
    save_history(tab, tab->command);

    Token tokens[MAX_TOKENS];
    int count = 0;

    if (tokenize(tab->command, tokens, &count) == -1) {
        append_output(tab, "terminator: invalid or incomplete command\n");
        last_status = 2;
        goto reset_command;
    }

    if (count == 0) goto reset_command;

    int has_pipe = 0;
    int has_redirect = 0;

    for (int i = 0; i < count; i++) {
        if (tokens[i].type == TOKEN_PIPE) has_pipe = 1;
        if (tokens[i].type == TOKEN_INPUT ||
            tokens[i].type == TOKEN_OUTPUT) has_redirect = 1;
    }

    if (!has_pipe && !has_redirect &&
        strcmp(tokens[0].value, "exit") == 0) {
        running = 0;
        goto reset_command;
    }

    if (!has_pipe && !has_redirect &&
        strcmp(tokens[0].value, "cd") == 0) {
        const char *path = count > 1
            ? tokens[1].value
            : getenv("HOME");

        if (path == NULL || chdir(path) == -1) {
            append_output(tab, "terminator: cd: cannot change directory\n");
            last_status = 1;
        } else {
            last_status = 0;
        }

        goto reset_command;
    }

    if (!has_pipe && !has_redirect &&
        strcmp(tokens[0].value, "history") == 0) {
        char text[INPUT_SIZE + 32];

        for (int i = 0; i < tab->history_count; i++) {
            snprintf(text, sizeof(text), "%4d  %s\n",
                     i + 1, tab->history[i]);

            append_output(tab, text);
        }

        last_status = 0;
        goto reset_command;
    }

    if (!has_pipe && !has_redirect &&
        strcmp(tokens[0].value, "clear") == 0) {
        tab->output[0] = '\0';
        last_status = 0;
        goto reset_command;
    }

    if (!has_pipe && !has_redirect &&
        strcmp(tokens[0].value, "echo") == 0) {
        for (int i = 1; i < count; i++) {
            if (strcmp(tokens[i].value, "$?") == 0) {
                char status_text[32];
                snprintf(status_text, sizeof(status_text), "%d", last_status);
                append_output(tab, status_text);
            } else {
                append_output(tab, tokens[i].value);
            }

            if (i + 1 < count) append_output(tab, " ");
        }

        append_output(tab, "\n");
        last_status = 0;
        goto reset_command;
    }

    last_status = execute_pipeline(tab, tokens, count);

reset_command:
    tab->command_length = 0;
    tab->command[0] = '\0';

clear_input:
    tab->input_length = 0;
    tab->input[0] = '\0';
}

int main(void) {
    display = XOpenDisplay(NULL);

    if (display == NULL) {
        fprintf(stderr, "Cannot open X11 display.\n");
        return 1;
    }

    int screen = DefaultScreen(display);

    window = XCreateSimpleWindow(
        display, RootWindow(display, screen),
        100, 100, 900, 600, 1,
        BlackPixel(display, screen),
        WhitePixel(display, screen)
    );

    XStoreName(display, window, "Terminator - Custom Terminal");

    XSelectInput(display, window,
                 ExposureMask | KeyPressMask | StructureNotifyMask);

    gc = XCreateGC(display, window, 0, NULL);

    font = XLoadQueryFont(display, "fixed");

    if (font != NULL) {
        XSetFont(display, gc, font->fid);
    }

    XMapWindow(display, window);

    append_output(&tabs[0],
                  "Terminator: Custom Terminal\n"
                  "Basic quoting, redirection and pipelines\n"
                  "Ctrl+T: new tab; Ctrl+W: close tab; F1-F4: switch\n\n");

    while (running) {
        XEvent event;
        XNextEvent(display, &event);

        if (event.type == Expose) {
            draw_window();
        }

        if (event.type == DestroyNotify) {
            running = 0;
        }

        if (event.type != KeyPress) continue;

        char text[32];
        KeySym key;
        XKeyEvent *key_event = &event.xkey;

        int length = XLookupString(key_event, text,
                                   sizeof(text), &key, NULL);

        Tab *tab = &tabs[active_tab];

        if ((key_event->state & ControlMask) &&
            (key == XK_t || key == XK_T)) {
            if (tab_count < MAX_TABS) {
                active_tab = tab_count++;
                memset(&tabs[active_tab], 0, sizeof(Tab));
                append_output(&tabs[active_tab], "New tab opened.\n\n");
            }
        } else if ((key_event->state & ControlMask) &&
                   (key == XK_w || key == XK_W)) {
            if (tab_count > 1) {
                for (int i = active_tab; i < tab_count - 1; i++) {
                    tabs[i] = tabs[i + 1];
                }

                tab_count--;

                if (active_tab >= tab_count) {
                    active_tab = tab_count - 1;
                }
            } else {
                running = 0;
            }
        } else if (key >= XK_F1 && key <= XK_F4) {
            int selected = (int)(key - XK_F1);
            if (selected < tab_count) active_tab = selected;
        } else if (key == XK_Return || key == XK_KP_Enter) {
            execute_command(tab);
        } else if (key == XK_BackSpace) {
            if (tab->input_length > 0) {
                tab->input_length--;
                tab->input[tab->input_length] = '\0';
            }
        } else if (key == XK_Escape) {
            tab->input_length = 0;
            tab->input[0] = '\0';
        } else if (length > 0 &&
                   text[0] >= 32 && text[0] <= 126) {
            if (tab->input_length < INPUT_SIZE - 1) {
                tab->input[tab->input_length++] = text[0];
                tab->input[tab->input_length] = '\0';
            }
        }

        draw_window();
    }

    XFreeGC(display, gc);

    if (font != NULL) XFreeFont(display, font);

    XCloseDisplay(display);
    return 0;
}
