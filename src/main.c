#include <X11/Xutil.h>
#include <X11/Xlib.h>
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
#define LINE_HEIGHT 20

typedef struct {
    char output[BUFFER_SIZE];
    char input[INPUT_SIZE];
    int input_length;
} Tab;

Display *display;
Window window;
GC gc;
XFontStruct *font;

Tab tabs[MAX_TABS];
int tab_count = 1;
int active_tab = 0;
int running = 1;

void append_output(Tab *tab, const char *text) {
    size_t used = strlen(tab->output);
    size_t available = BUFFER_SIZE - used - 1;

    if (available > 0) {
        strncat(tab->output, text, available);
    }
}

void draw_window(void) {
    XClearWindow(display, window);

    char heading[256];
    int heading_length = 0;

    for (int i = 0; i < tab_count; i++) {
        char part[64];

        snprintf(part, sizeof(part), "  [%d]%s  ",
                 i + 1, i == active_tab ? "*" : "");

        int remaining = (int)sizeof(heading) - heading_length - 1;
        if (remaining > 0) {
            int written = snprintf(heading + heading_length,
                                   (size_t)remaining + 1, "%s", part);
            if (written > 0 && written <= remaining) {
                heading_length += written;
            }
        }
    }

    XDrawString(display, window, gc, 10, 22,
                heading, (int)strlen(heading));

    Tab *tab = &tabs[active_tab];

    int width, height;
    Window child;
    XGetGeometry(display, window, &child, &(int){0}, &(int){0},
                 (unsigned int *)&width, (unsigned int *)&height,
                 &(unsigned int){0}, &(unsigned int){0});

    (void)width;

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

        int max_lines = (height - 75) / LINE_HEIGHT;
        if (max_lines < 1) max_lines = 1;

        int start = count > max_lines ? count - max_lines : 0;

        for (int i = start; i < count; i++) {
            XDrawString(display, window, gc, 10,
                        48 + (i - start) * LINE_HEIGHT,
                        lines[i], (int)strlen(lines[i]));
        }

        free(copy);
    }

    int prompt_y = height - 20;
    XDrawString(display, window, gc, 10, prompt_y,
                "user@terminator> ", 17);

    XDrawString(display, window, gc, 180, prompt_y,
                tab->input, tab->input_length);

    XFlush(display);
}

void execute_command(Tab *tab) {
    if (tab->input_length == 0) return;

    char command[INPUT_SIZE];
    snprintf(command, sizeof(command), "%s", tab->input);

    append_output(tab, "user@terminator> ");
    append_output(tab, command);
    append_output(tab, "\n");

    tab->input[tab->input_length] = '\0';

    if (strcmp(command, "exit") == 0) {
        running = 0;
        return;
    }

    char *args[128];
    int argc = 0;

    char *token = strtok(command, " \t");
    while (token != NULL && argc < 127) {
        args[argc++] = token;
        token = strtok(NULL, " \t");
    }
    args[argc] = NULL;

    if (argc == 0) {
        tab->input_length = 0;
        tab->input[0] = '\0';
        return;
    }

    int pipefd[2];

    if (pipe(pipefd) == -1) {
        append_output(tab, "terminator: pipe failed\n");
    } else {
        pid_t pid = fork();

        if (pid == -1) {
            append_output(tab, "terminator: fork failed\n");
            close(pipefd[0]);
            close(pipefd[1]);
        } else if (pid == 0) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);

            execvp(args[0], args);

            perror("terminator");
            _exit(127);
        } else {
            close(pipefd[1]);

            char buffer[512];
            ssize_t bytes;

            while ((bytes = read(pipefd[0], buffer,
                                 sizeof(buffer))) > 0) {
                size_t used = strlen(tab->output);
                size_t available = BUFFER_SIZE - used - 1;

                if (available > 0) {
                    size_t amount = (size_t)bytes;
                    if (amount > available) amount = available;

                    strncat(tab->output, buffer, amount);
                }
            }

            close(pipefd[0]);
            waitpid(pid, NULL, 0);
        }
    }

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
                  "Task 1 - Initial X11 GUI\n"
                  "Type a command. Use F1-F4 to switch tabs.\n\n");

    while (running) {
        XEvent event;
        XNextEvent(display, &event);

        if (event.type == Expose) {
            draw_window();
        }

        if (event.type == KeyPress) {
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
                    append_output(&tabs[active_tab],
                                  "New tab opened.\n\n");
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
                    tab->input[--tab->input_length] = '\0';
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

        if (event.type == DestroyNotify) {
            running = 0;
        }
    }

    XFreeGC(display, gc);
    if (font != NULL) XFreeFont(display, font);
    XCloseDisplay(display);

    return 0;
}
