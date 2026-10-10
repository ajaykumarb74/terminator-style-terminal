
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_TABS 4
#define BUFFER_SIZE 16000
#define INPUT_SIZE 1024
#define HISTORY_SIZE 1000
#define LINE_HEIGHT 20

typedef struct {
    char output[BUFFER_SIZE];
    char input[INPUT_SIZE];
    int input_length;
    char history[HISTORY_SIZE][INPUT_SIZE];
    int history_count;
} Tab;

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

    if (used >= BUFFER_SIZE - 1) {
        return;
    }

    size_t available = BUFFER_SIZE - used - 1;
    strncat(tab->output, text, available);
}

void save_history(Tab *tab, const char *command) {
    if (command[0] == '\0') {
        return;
    }

    if (tab->history_count < HISTORY_SIZE) {
        snprintf(tab->history[tab->history_count],
                 INPUT_SIZE, "%s", command);
        tab->history_count++;
    } else {
        memmove(
            tab->history,
            tab->history + 1,
            (HISTORY_SIZE - 1) * sizeof(tab->history[0])
        );

        snprintf(tab->history[HISTORY_SIZE - 1],
                 INPUT_SIZE, "%s", command);
    }
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
    if (max_lines < 1) {
        max_lines = 1;
    }

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

    XDrawString(display, window, gc, 10, prompt_y,
                "user@terminator> ", 17);

    XDrawString(display, window, gc, 180, prompt_y,
                tab->input, tab->input_length);

    XFlush(display);
}

void execute_command(Tab *tab) {
    if (tab->input_length == 0) {
        return;
    }

    char original[INPUT_SIZE];
    snprintf(original, sizeof(original), "%s", tab->input);

    append_output(tab, "user@terminator> ");
    append_output(tab, original);
    append_output(tab, "\n");

    save_history(tab, original);

    char command[INPUT_SIZE];
    snprintf(command, sizeof(command), "%s", original);

    char *args[128];
    int argc = 0;

    char *token = strtok(command, " \t");

    while (token != NULL && argc < 127) {
        args[argc++] = token;
        token = strtok(NULL, " \t");
    }

    args[argc] = NULL;

    if (argc == 0) {
        goto clear_input;
    }

    if (strcmp(args[0], "exit") == 0) {
        running = 0;
        goto clear_input;
    }

    if (strcmp(args[0], "cd") == 0) {
        const char *path;

        if (argc == 1) {
            path = getenv("HOME");
        } else {
            path = args[1];
        }

        if (path == NULL || chdir(path) == -1) {
            append_output(tab, "terminator: cd: cannot change directory\n");
            last_status = 1;
        } else {
            last_status = 0;
        }

        goto clear_input;
    }

    if (strcmp(args[0], "history") == 0) {
        char line[INPUT_SIZE + 32];

        for (int i = 0; i < tab->history_count; i++) {
            snprintf(line, sizeof(line), "%4d  %s\n",
                     i + 1, tab->history[i]);

            append_output(tab, line);
        }

        last_status = 0;
        goto clear_input;
    }

    if (strcmp(args[0], "clear") == 0) {
        tab->output[0] = '\0';
        last_status = 0;
        goto clear_input;
    }

    if (strcmp(args[0], "echo") == 0) {
        for (int i = 1; i < argc; i++) {
            if (strcmp(args[i], "$?") == 0) {
                char status_text[32];
                snprintf(status_text, sizeof(status_text), "%d",
                         last_status);
                append_output(tab, status_text);
            } else {
                append_output(tab, args[i]);
            }

            if (i + 1 < argc) {
                append_output(tab, " ");
            }
        }

        append_output(tab, "\n");
        last_status = 0;
        goto clear_input;
    }

    int pipefd[2];

    if (pipe(pipefd) == -1) {
        append_output(tab, "terminator: pipe failed\n");
        last_status = 1;
        goto clear_input;
    }

    pid_t pid = fork();

    if (pid == -1) {
        append_output(tab, "terminator: fork failed\n");
        close(pipefd[0]);
        close(pipefd[1]);
        last_status = 1;
        goto clear_input;
    }

    if (pid == 0) {
        close(pipefd[0]);

        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);

        close(pipefd[1]);

        execvp(args[0], args);

        perror("terminator");
        _exit(127);
    }

    close(pipefd[1]);

    char buffer[512];
    ssize_t bytes;

    while ((bytes = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
        size_t used = strlen(tab->output);

        if (used < BUFFER_SIZE - 1) {
            size_t available = BUFFER_SIZE - used - 1;
            size_t amount = (size_t)bytes;

            if (amount > available) {
                amount = available;
            }

            strncat(tab->output, buffer, amount);
        }
    }

    close(pipefd[0]);

    int status;

    if (waitpid(pid, &status, 0) == -1) {
        last_status = 1;
    } else if (WIFEXITED(status)) {
        last_status = WEXITSTATUS(status);
    } else {
        last_status = 1;
    }

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
        display,
        RootWindow(display, screen),
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
                  "Task 1 - Initial X11 GUI\n"
                  "Type commands. Ctrl+T opens a tab.\n\n");

    while (running) {
        XEvent event;
        XNextEvent(display, &event);

        if (event.type == Expose) {
            draw_window();
        }

        if (event.type == DestroyNotify) {
            running = 0;
        }

        if (event.type != KeyPress) {
            continue;
        }

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

            if (selected < tab_count) {
                active_tab = selected;
            }

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

    if (font != NULL) {
        XFreeFont(display, font);
    }

    XCloseDisplay(display);

    return 0;
}

