#include <X11/extensions/Xrender.h>
#include <X11/xpm.h>
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <glob.h>
#include <limits.h>
#define MAX_TABS 4
#define BUFFER_SIZE (1024 * 1024)
#define INPUT_SIZE 1024
#define COMMAND_SIZE 4096
#define HISTORY_SIZE 1000
#define MAX_TOKENS 256
#define MAX_ARGS 64
#define MAX_PIPELINE 32

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
#define MAX_WATCH_COMMANDS 8

typedef struct {
    char command[INPUT_SIZE];
    pid_t pid;
    int fd;
    int active;
    time_t last_run;
} WatchCommand;

typedef struct {
    char output[BUFFER_SIZE];
    char input[INPUT_SIZE];
    int input_length;

    char command[COMMAND_SIZE];
    int command_length;
    int continuation;

    char cwd[PATH_MAX];
    int scroll_offset;
    char history[HISTORY_SIZE][INPUT_SIZE];
    int history_count;
} Tab;

typedef struct {
    WatchCommand commands[MAX_WATCH_COMMANDS];
    int count;
    int running;
    Tab *tab;
} MultiWatch;
Display *display;
Window window;

GC gc;

XftFont *xft_font = NULL;
XftDraw *xft_draw = NULL;
XftColor col_heading, col_output, col_prompt, col_input;
Pixmap wallpaper;
Picture wallpaper_picture;
Picture window_picture;

int wallpaper_width = 0;
int wallpaper_height = 0;
MultiWatch watcher = {0};
Tab tabs[MAX_TABS];
int tab_count = 1;
int active_tab = 0;
int running = 1;
int last_status = 0;

/* Font state: line_height follows the currently loaded font. */
int line_height = 30;
int font_pixel_size = 0;

void append_output(Tab *tab, const char *text) {
    size_t used = strlen(tab->output);

    if (used >= BUFFER_SIZE - 1) return;

    size_t available = BUFFER_SIZE - used - 1;
    strncat(tab->output, text, available);
}

static void initialize_tab_cwd(Tab *tab) {
    if (getcwd(tab->cwd, sizeof(tab->cwd)) == NULL) {
        snprintf(tab->cwd, sizeof(tab->cwd), "%s", "/");
    }
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

/* Open a scalable monospace font sized from the window dimensions. */
void update_font(int win_w, int win_h) {
    int size = win_h / 33;        /* 600px tall -> ~18px font */
    int by_width = win_w / 50;    /* 900px wide -> ~18px font */

    if (by_width < size) size = by_width;
    if (size < 8) size = 8;
    if (size > 96) size = 96;

    if (size == font_pixel_size) return;

    char name[64];
    snprintf(name, sizeof(name), "monospace:pixelsize=%d", size);

    XftFont *new_font = XftFontOpenName(display, DefaultScreen(display), name);

    if (new_font == NULL) return;

    if (xft_font != NULL) XftFontClose(display, xft_font);

    xft_font = new_font;
    font_pixel_size = size;
    line_height = xft_font->ascent + xft_font->descent + 8;
}

static void draw_text(XftColor *color, int x, int y,
                      const char *text, int length) {
    if (xft_font == NULL || xft_draw == NULL || length <= 0) return;

    XftDrawStringUtf8(xft_draw, color, xft_font, x, y,
                      (const FcChar8 *)text, length);
}

static int text_width(const char *text, int length) {
    XGlyphInfo extents;

    if (xft_font == NULL || length <= 0) return 0;

    XftTextExtentsUtf8(display, xft_font,
                       (const FcChar8 *)text, length, &extents);
    return extents.xOff;
}

void draw_window(void) {
    XWindowAttributes attributes;
    XGetWindowAttributes(display, window, &attributes);
    /* Draw the wallpaper scaled to the current window size. */
    if (wallpaper_picture != None &&
        window_picture != None &&
        wallpaper_width > 0 &&
        wallpaper_height > 0 &&
        attributes.width > 0 &&
        attributes.height > 0) {

        XTransform transform = {{
            { XDoubleToFixed((double) wallpaper_width /
                             attributes.width), 0, 0 },
            { 0, XDoubleToFixed((double) wallpaper_height /
                                attributes.height), 0 },
            { 0, 0, XDoubleToFixed(1.0) }
        }};

        XRenderSetPictureTransform(
            display, wallpaper_picture, &transform);

        XRenderSetPictureFilter(
            display, wallpaper_picture, FilterBilinear,
            NULL, 0);

        XRenderComposite(
            display, PictOpSrc,
            wallpaper_picture, None, window_picture,
            0, 0, 0, 0, 0, 0,
            (unsigned int) attributes.width,
            (unsigned int) attributes.height);
    } else {
        XClearWindow(display, window);
    }
    /* Tab heading */
    char heading[256] = "";
    char part[64];

    for (int i = 0; i < tab_count; i++) {
        snprintf(part, sizeof(part), "  [%d]%s  ",
                 i + 1, i == active_tab ? "*" : "");

        if (strlen(heading) + strlen(part) < sizeof(heading) - 1) {
            strcat(heading, part);
        }
    }

    draw_text(&col_heading, 15, line_height,
                heading, (int)strlen(heading));

    Tab *tab = &tabs[active_tab];

    int max_lines = (attributes.height - 3 * line_height) / line_height;
    if (max_lines < 1) max_lines = 1;

    char *copy = malloc(strlen(tab->output) + 1);

    if (copy != NULL) {
        strcpy(copy, tab->output);

        /* Split on '\n' manually so blank lines are preserved. */
        char *lines[512];
        int count = 0;
        char *p = copy;

        while (count < 512) {
            lines[count++] = p;

            char *newline = strchr(p, '\n');
            if (newline == NULL) break;

            *newline = '\0';
            p = newline + 1;
        }

        /* Drop the empty segment after a trailing newline. */
        if (count > 0 && lines[count - 1][0] == '\0') count--;

        int start = count > max_lines ? count - max_lines : 0;

        for (int i = start; i < count; i++) {
            draw_text(&col_output, 15,
                        2 * line_height + (i - start) * line_height,
                        lines[i], (int)strlen(lines[i]));
        }

        free(copy);
    }

    /* Command prompt */
    int prompt_y = attributes.height - line_height / 2;

    char prompt_buffer[PATH_MAX + 64];

if (tab->continuation) {
    snprintf(prompt_buffer, sizeof(prompt_buffer), "> ");
} else {
    snprintf(prompt_buffer, sizeof(prompt_buffer),
             "user@terminator:%s> ", tab->cwd);
}

const char *prompt = prompt_buffer;

    draw_text(&col_prompt, 15, prompt_y,
                prompt, (int)strlen(prompt));

    int input_x = 15 + text_width(prompt, (int)strlen(prompt));

    draw_text(&col_input, input_x, prompt_y,
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

/*
 * Expand wildcard arguments such as *.txt.
 *
 * Quoting information must be preserved by the tokenizer
 * if quoted wildcards are to remain literal.
 */
static int expand_wildcards(
    char *args[],
    int max_args,
    char *expanded[],
    int *expanded_count
) {
    int out = 0;

    for (int i = 0; args[i] != NULL; i++) {
        glob_t matches;
        memset(&matches, 0, sizeof(matches));

        int has_wildcard =
            strpbrk(args[i], "*?[") != NULL;

        if (!has_wildcard) {
            if (out >= max_args - 1) {
                return -1;
            }

            expanded[out++] = args[i];
            continue;
        }

        int result = glob(args[i], 0, NULL, &matches);

        if (result == GLOB_NOMATCH) {
            /* Shell convention: keep unmatched patterns literal. */
            globfree(&matches);

            if (out >= max_args - 1) {
                return -1;
            }

            expanded[out++] = args[i];
            continue;
        }

        if (result != 0) {
            globfree(&matches);
            return -1;
        }

        for (size_t j = 0; j < matches.gl_pathc; j++) {
            if (out >= max_args - 1) {
                globfree(&matches);
                return -1;
            }

            expanded[out++] = strdup(matches.gl_pathv[j]);

            if (expanded[out - 1] == NULL) {
                globfree(&matches);

                for (int k = 0; k < out; k++) {
                    /* Only duplicated entries should be freed. */
                    /* Use the ownership-aware version below. */
                }

                return -1;
            }
        }

        globfree(&matches);
    }

    expanded[out] = NULL;
    *expanded_count = out;
    return 0;
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
            if (chdir(tab->cwd) == -1) {
    dprintf(STDERR_FILENO,
            "terminator: cannot enter tab directory\n");
    _exit(1);
}
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

          
            char *expanded[MAX_ARGS];
            int expanded_count = 0;

            if (expand_wildcards(
                    args[s],
                    MAX_ARGS,
                    expanded,
                    &expanded_count
                ) == -1) {
                dprintf(STDERR_FILENO,
                        "terminator: wildcard expansion failed\n");
                _exit(1);
            }

            execvp(expanded[0], expanded);

            /* execvp returns only when execution fails. */
            perror(expanded[0]);

            /* Free duplicated wildcard arguments on failure. */
            for (int i = 0; i < expanded_count; i++) {
                int original = 0;

                for (int j = 0; args[s][j] != NULL; j++) {
                    if (expanded[i] == args[s][j]) {
                        original = 1;
                        break;
                    }
                }

                if (!original) {
                    free(expanded[i]);
                }
            }

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

    if (path == NULL || path[0] == '\0') {
        append_output(tab, "terminator: cd: missing directory\n");
        last_status = 1;
    } else {
        char resolved[PATH_MAX];

        if (path[0] == '/') {
    if (snprintf(resolved, sizeof(resolved), "%s", path)
        >= (int)sizeof(resolved)) {
        append_output(tab, "terminator: cd: path too long\n");
        last_status = 1;
        goto reset_command;
    }
} else {
    if (snprintf(resolved, sizeof(resolved), "%s/%s",
                 tab->cwd, path)
        >= (int)sizeof(resolved)) {
        append_output(tab, "terminator: cd: path too long\n");
        last_status = 1;
        goto reset_command;
    }
}

        char *canonical = realpath(resolved, NULL);

        if (canonical == NULL) {
            append_output(tab,
                          "terminator: cd: cannot change directory\n");
            last_status = 1;
        } else {
            if (strlen(canonical) >= sizeof(tab->cwd)) {
                append_output(tab, "terminator: cd: path too long\n");
                last_status = 1;
            } else {
                snprintf(tab->cwd, sizeof(tab->cwd), "%s", canonical);
                last_status = 0;
            }

            free(canonical);
        }
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

    /* Xft drawing surface and colors. */
    Visual *xft_visual = DefaultVisual(display, screen);
    Colormap colormap = DefaultColormap(display, screen);

    xft_draw = XftDrawCreate(display, window, xft_visual, colormap);

    XftColorAllocName(display, xft_visual, colormap, "#FF4D6D", &col_heading);
    XftColorAllocName(display, xft_visual, colormap, "#EAF2FF", &col_output);
    XftColorAllocName(display, xft_visual, colormap, "#7CFF6B", &col_prompt);
    XftColorAllocName(display, xft_visual, colormap, "#63E6FF", &col_input);

    /* Pick a font that matches the initial window size. */
    update_font(900, 600);

    if (xft_font == NULL) {
        fprintf(stderr, "Cannot load a monospace Xft font.\n");
        return 1;
    }

    /* Load the anime wallpaper. */
    wallpaper = None;
    wallpaper_picture = None;
    window_picture = None;

    Pixmap shape = None;
    XpmAttributes xpm_attributes;
    memset(&xpm_attributes, 0, sizeof(xpm_attributes));

    int xpm_result = XpmReadFileToPixmap(
        display, window, "assets/bg.xpm",
        &wallpaper, &shape, &xpm_attributes);

    if (xpm_result == XpmSuccess) {
        wallpaper_width = (int)xpm_attributes.width;
        wallpaper_height = (int)xpm_attributes.height;

        Visual *visual = DefaultVisual(display, screen);
        XRenderPictFormat *format =
            XRenderFindVisualFormat(display, visual);

        if (format != NULL) {
            wallpaper_picture = XRenderCreatePicture(
                display, wallpaper, format, 0, NULL);

            window_picture = XRenderCreatePicture(
                display, window, format, 0, NULL);
        } else {
            fprintf(stderr, "Warning: could not find XRender visual format\n");
        }
    } else {
        fprintf(stderr, "Warning: could not load assets/bg.xpm\n");
    }

    XpmFreeAttributes(&xpm_attributes);
    initialize_tab_cwd(&tabs[0]);
    XMapWindow(display, window);

    append_output(&tabs[0],
                  "Terminator: Custom Terminal\n"
                  "Basic quoting, redirection and pipelines\n"
                  "Ctrl+T: new tab; Ctrl+W: close tab; F1-F4: switch\n\n");

    while (running) {
        XEvent event;
        XNextEvent(display, &event);

        if (event.type == Expose) {
            if (event.xexpose.count == 0) {
                draw_window();
            }
            continue;
        }

        if (event.type == DestroyNotify) {
            running = 0;
            continue;
        }

        if (event.type == ConfigureNotify) {
            update_font(event.xconfigure.width,
                        event.xconfigure.height);
            draw_window();
            continue;
        }

        if (event.type != KeyPress) {
            continue;
        }

        char text[32];
        KeySym key;
        XKeyEvent *key_event = &event.xkey;

        int length = XLookupString(
            key_event, text, (int)sizeof(text), &key, NULL);

        Tab *tab = &tabs[active_tab];

        if ((key_event->state & ControlMask) &&
            (key == XK_t || key == XK_T)) {
            if (tab_count < MAX_TABS) {
                active_tab = tab_count++;
memset(&tabs[active_tab], 0, sizeof(Tab));
initialize_tab_cwd(&tabs[active_tab]);
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

    if (wallpaper_picture != None) {
        XRenderFreePicture(display, wallpaper_picture);
    }

    if (window_picture != None) {
        XRenderFreePicture(display, window_picture);
    }

    if (wallpaper != None) {
        XFreePixmap(display, wallpaper);
    }

    XFreeGC(display, gc);

    if (xft_font != NULL) XftFontClose(display, xft_font);
    if (xft_draw != NULL) XftDrawDestroy(xft_draw);

    XCloseDisplay(display);
    return 0;
}