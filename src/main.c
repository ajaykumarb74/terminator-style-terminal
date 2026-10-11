/* Terminator - minimized. Build: gcc -Wall -o terminator terminator_min.c -lX11 -lXft -lXpm -lXrender
 * Run:   ./terminator            (single process)
 *        ./terminator useProcess=True   (one shell process per tab) */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/xpm.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/Xrender.h>
#include <poll.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAX_TABS 4
#define BUFSZ (1024 * 1024)
#define INSZ 1024
#define CMDSZ 4096
#define HISTSZ 1000
#define MAXTOK 256
#define MAXARG 64
#define MAXPIPE 32
#define CHUNK 2048

enum { T_WORD, T_PIPE, T_IN, T_OUT };
typedef struct { char v[INSZ]; int t; } Token;

typedef struct {
    char output[BUFSZ], input[INSZ], command[CMDSZ], cwd[PATH_MAX], history[HISTSZ][INSZ];
    int in_len, cmd_len, cont, hist_n, cmd_fd, out_fd, scroll; /* scroll = lines above the bottom */
    pid_t pid;
    unsigned char ipc[8192];
    size_t ipc_len;
} Tab;
typedef struct { int cont; char cwd[PATH_MAX]; } TabStatus;

static Display *dpy;
static Window win;
static XftFont *font;
static XftDraw *xd;
static XftColor c_head, c_out, c_prompt, c_in;
static Pixmap wp;
static Picture wpic, winpic;
static int wp_w, wp_h, lh = 30, font_px, page_rows = 20;
static const char *bgs[] = {"assets/bg.xpm", "assets/bg2.xpm", "assets/bg3.xpm", "assets/bg4.xpm"};
#define NBG (sizeof bgs / sizeof *bgs)
static size_t cur_bg;
static Tab tabs[MAX_TABS];
static int tab_count = 1, active, running = 1, last_status, proc_mode, proc_child;

/* ---------- small helpers ---------- */
static void out(Tab *t, const char *s) {
    size_t u = strlen(t->output);
    if (u < BUFSZ - 1) strncat(t->output, s, BUFSZ - u - 1);
}
static void cl(int fd) { if (fd >= 0) close(fd); }
static void init_tab(Tab *t) {
    memset(t, 0, sizeof *t);
    t->cmd_fd = t->out_fd = t->pid = -1;
    if (!getcwd(t->cwd, sizeof t->cwd)) strcpy(t->cwd, "/");
}
static void save_history(Tab *t, const char *c) {
    if (!*c) return;
    if (t->hist_n == HISTSZ) { memmove(t->history, t->history + 1, (HISTSZ - 1) * INSZ); t->hist_n--; }
    snprintf(t->history[t->hist_n++], INSZ, "%s", c);
}

/* ---------- wallpaper ---------- */
static int load_wallpaper(const char *path) {
    Pixmap np = None, shape = None;
    XpmAttributes at;
    memset(&at, 0, sizeof at);
    at.valuemask = XpmSize;
    if (XpmReadFileToPixmap(dpy, win, (char *)path, &np, &shape, &at) != XpmSuccess) {
        XpmFreeAttributes(&at);
        if (np) XFreePixmap(dpy, np);
        return -1;
    }
    if (shape) XFreePixmap(dpy, shape);
    XRenderPictFormat *f = XRenderFindVisualFormat(dpy, DefaultVisual(dpy, DefaultScreen(dpy)));
    Picture p = f ? XRenderCreatePicture(dpy, np, f, 0, NULL) : None;
    if (!p) { XpmFreeAttributes(&at); XFreePixmap(dpy, np); return -1; }
    if (!winpic) winpic = XRenderCreatePicture(dpy, win, f, 0, NULL);
    if (wpic) XRenderFreePicture(dpy, wpic);
    if (wp) XFreePixmap(dpy, wp);
    wp = np; wpic = p; wp_w = (int)at.width; wp_h = (int)at.height;
    XpmFreeAttributes(&at);
    return 0;
}
static int background_image_change(void) {
    for (size_t s = 1; s <= NBG; s++) {
        size_t c = (cur_bg + s) % NBG;
        if (load_wallpaper(bgs[c]) == 0) { cur_bg = c; return 0; }
    }
    return -1;
}

/* ---------- font + drawing ---------- */
static void update_font(int w, int h) {
    int size = h / 33, bw = w / 50;
    if (bw < size) size = bw;
    if (size < 8) size = 8;
    if (size > 96) size = 96;
    if (size == font_px) return;
    char name[64];
    snprintf(name, sizeof name, "monospace:pixelsize=%d", size);
    XftFont *f = XftFontOpenName(dpy, DefaultScreen(dpy), name);
    if (!f) return;
    if (font) XftFontClose(dpy, font);
    font = f; font_px = size;
    lh = font->ascent + font->descent + 8;
}
static void text(XftColor *c, int x, int y, const char *s, int n) {
    if (font && xd && n > 0) XftDrawStringUtf8(xd, c, font, x, y, (const FcChar8 *)s, n);
}
static int twidth(const char *s, int n) {
    XGlyphInfo e;
    if (!font || n <= 0) return 0;
    XftTextExtentsUtf8(dpy, font, (const FcChar8 *)s, n, &e);
    return e.xOff;
}
static void draw_window(void) {
    XWindowAttributes a;
    XGetWindowAttributes(dpy, win, &a);
    if (wpic && winpic && wp_w > 0 && wp_h > 0 && a.width > 0 && a.height > 0) {
        XTransform m = {{{XDoubleToFixed((double)wp_w / a.width), 0, 0},
                         {0, XDoubleToFixed((double)wp_h / a.height), 0},
                         {0, 0, XDoubleToFixed(1.0)}}};
        XRenderSetPictureTransform(dpy, wpic, &m);
        XRenderSetPictureFilter(dpy, wpic, FilterBilinear, NULL, 0);
        XRenderComposite(dpy, PictOpSrc, wpic, None, winpic, 0, 0, 0, 0, 0, 0, a.width, a.height);
    } else XClearWindow(dpy, win);

    char head[256] = "", part[64];
    for (int i = 0; i < tab_count; i++) {
        snprintf(part, sizeof part, "  [%d]%s  ", i + 1, i == active ? "*" : "");
        if (strlen(head) + strlen(part) < sizeof head - 1) strcat(head, part);
    }
    text(&c_head, 15, lh, head, (int)strlen(head));

    Tab *t = &tabs[active];
    int maxrows = (a.height - 4 * lh) / lh, rows = 0, y = 2 * lh;
    if (maxrows < 1) maxrows = 1;

    /* show the last maxrows lines of the transcript (a trailing '\n' is not an extra line) */
    const char *o = t->output;
    size_t e = strlen(o), s;
    if (e && o[e - 1] == '\n') e--;
    int total = e ? 1 : 0;
    for (size_t i = 0; i < e; i++) if (o[i] == '\n') total++;
    page_rows = maxrows;
    if (t->scroll > total - maxrows) t->scroll = total - maxrows;
    if (t->scroll < 0) t->scroll = 0;
    for (int k = 0; k < t->scroll; k++) {            /* drop k lines from the bottom */
        while (e > 0 && o[e - 1] != '\n') e--;
        if (e > 0) e--;
    }
    s = e;
    for (int nl = 0; s > 0; s--)
        if (o[s - 1] == '\n') { if (nl + 1 >= maxrows) break; nl++; }
    if (e) {
        const char *p = o + s, *end = o + e;
        for (;;) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            text(&c_out, 15, y, p, (int)((nl ? nl : end) - p));
            rows++; y += lh;
            if (!nl) break;
            p = nl + 1;
        }
    }

    if (!t->scroll) {                                /* prompt only when at the bottom */
        char pr[PATH_MAX + 64];
        if (t->cont) snprintf(pr, sizeof pr, "> ");
        else snprintf(pr, sizeof pr, "user@terminator:%s> ", t->cwd);
        int py = 2 * lh + rows * lh, lim = a.height - lh / 2;
        if (py > lim) py = lim;
        text(&c_prompt, 15, py, pr, (int)strlen(pr));
        text(&c_in, 15 + twidth(pr, (int)strlen(pr)), py, t->input, t->in_len);
    }
    XFlush(dpy);
}

/* ---------- parsing ---------- */
/* Split into words honoring quotes/backslashes; |, < and > (at a token start) are their own tokens. */
static int tokenize(const char *s, Token *tk, int *n) {
    int c = 0, i = 0;
    while (s[i]) {
        while (isspace((unsigned char)s[i])) i++;
        if (!s[i]) break;
        if (c >= MAXTOK) return -1;
        if (strchr("|<>", s[i])) {
            tk[c].v[0] = s[i]; tk[c].v[1] = 0;
            tk[c].t = s[i] == '|' ? T_PIPE : s[i] == '<' ? T_IN : T_OUT;
            c++; i++;
            continue;
        }
        int j = 0;
        char q = 0;
        while (s[i]) {
            char ch = s[i];
            if (!q && isspace((unsigned char)ch)) break;
            if (!q && (ch == '\'' || ch == '"')) { q = ch; i++; continue; }
            if (q && ch == q) { q = 0; i++; continue; }
            if (ch == '\\' && q != '\'') { if (!s[i + 1]) return -1; ch = s[++i]; }
            if (j >= INSZ - 1) return -1;
            tk[c].v[j++] = ch; i++;
        }
        if (q) return -1;
        tk[c].v[j] = 0; tk[c].t = T_WORD; c++;
    }
    *n = c;
    return 0;
}
/* Open quote or trailing backslash => the command continues on the next line. */
static int needs_continuation(const char *s) {
    char q = 0;
    int esc = 0;
    for (; *s; s++) {
        if (esc) { esc = 0; continue; }
        if (*s == '\\' && q != '\'') { esc = 1; continue; }
        if (!q && (*s == '\'' || *s == '"')) q = *s;
        else if (q && *s == q) q = 0;
    }
    return q || esc;
}
/* Expand *, ?, [ via glob(); unmatched patterns stay literal. Runs in a child that execs, so no frees. */
static int expand_wildcards(char **a, char **o) {
    int n = 0;
    for (; *a; a++) {
        glob_t g;
        memset(&g, 0, sizeof g);
        int r = strpbrk(*a, "*?[") ? glob(*a, 0, NULL, &g) : GLOB_NOMATCH;
        if (r && r != GLOB_NOMATCH) { globfree(&g); return -1; }
        if (r) { if (n >= MAXARG - 1) return -1; o[n++] = *a; }
        else for (size_t j = 0; j < g.gl_pathc; j++) {
            if (n >= MAXARG - 1 || !(o[n] = strdup(g.gl_pathv[j]))) { globfree(&g); return -1; }
            n++;
        }
        globfree(&g);
    }
    o[n] = NULL;
    return 0;
}

/* ---------- pipelines ---------- */
static int execute_pipeline(Tab *t, Token *tk, int n) {
    char *args[MAXPIPE][MAXARG], *inf[MAXPIPE] = {0}, *outf[MAXPIPE] = {0};
    int ac[MAXPIPE] = {0}, stages = 1, s = 0;

    for (int i = 0; i < n; i++)
        if (tk[i].t == T_PIPE && ++stages > MAXPIPE) { out(t, "terminator: too many pipeline stages\n"); return 2; }
    for (int i = 0; i < n; i++) {
        if (tk[i].t == T_PIPE) {
            if (!ac[s]) { out(t, "terminator: empty pipeline stage\n"); return 2; }
            s++;
        } else if (tk[i].t == T_IN || tk[i].t == T_OUT) {
            if (i + 1 >= n || tk[i + 1].t != T_WORD) { out(t, "terminator: missing redirection filename\n"); return 2; }
            (tk[i].t == T_IN ? inf : outf)[s] = tk[i + 1].v;
            i++;
        } else {
            if (ac[s] >= MAXARG - 1) { out(t, "terminator: too many arguments\n"); return 2; }
            args[s][ac[s]++] = tk[i].v;
        }
    }
    if (!ac[s]) { out(t, "terminator: empty command\n"); return 2; }
    for (int i = 0; i < stages; i++) args[i][ac[i]] = NULL;

    int op[2], prev = -1;
    pid_t pids[MAXPIPE];
    if (pipe(op) == -1) { out(t, "terminator: pipe failed\n"); return 1; }

    for (s = 0; s < stages; s++) {
        int np[2] = {-1, -1};
        pid_t pid = -1;
        const char *why = NULL;
        if (s < stages - 1 && pipe(np) == -1) why = "pipe";
        else if ((pid = fork()) == -1) why = "fork";
        if (why) {
            out(t, "terminator: "); out(t, why); out(t, " failed\n");
            cl(prev); cl(np[0]); cl(np[1]); cl(op[0]); cl(op[1]);
            return 1;
        }
        if (pid == 0) {
            if (chdir(t->cwd) == -1) { dprintf(STDERR_FILENO, "terminator: cannot enter tab directory\n"); _exit(1); }
            if (prev != -1) dup2(prev, STDIN_FILENO);
            dup2(s < stages - 1 ? np[1] : op[1], STDOUT_FILENO);
            dup2(op[1], STDERR_FILENO);
            cl(prev); cl(np[0]); cl(np[1]); cl(op[0]); cl(op[1]);
            if (inf[s]) {
                int fd = open(inf[s], O_RDONLY);
                if (fd == -1) { perror(inf[s]); _exit(1); }
                dup2(fd, STDIN_FILENO); close(fd);
            }
            if (outf[s]) {
                int fd = open(outf[s], O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd == -1) { perror(outf[s]); _exit(1); }
                dup2(fd, STDOUT_FILENO); close(fd);
            }
            char *ex[MAXARG];
            if (expand_wildcards(args[s], ex) == -1) { dprintf(STDERR_FILENO, "terminator: wildcard expansion failed\n"); _exit(1); }
            execvp(ex[0], ex);
            perror(ex[0]);
            _exit(127);
        }
        pids[s] = pid;
        cl(prev); cl(np[1]);
        prev = np[0];
    }
    cl(prev); close(op[1]);

    char b[CHUNK / 4 + 1];
    ssize_t r;
    while ((r = read(op[0], b, sizeof b - 1)) > 0) { b[r] = 0; out(t, b); }
    close(op[0]);

    int res = 0;
    for (s = 0; s < stages; s++) {
        int st;
        if (waitpid(pids[s], &st, 0) == -1) res = 1;
        else if (s == stages - 1) res = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    }
    return res;
}

/* ---------- command execution ---------- */
static void run_command(Tab *t) {
    Token tk[MAXTOK];
    int n = 0, simple = 1;
    if (tokenize(t->command, tk, &n) == -1) { out(t, "terminator: invalid or incomplete command\n"); last_status = 2; return; }
    if (!n) return;
    for (int i = 0; i < n; i++) if (tk[i].t != T_WORD) simple = 0;
    const char *cmd = tk[0].v;

    if (!simple) { last_status = execute_pipeline(t, tk, n); return; }

    if (!strcmp(cmd, "exit")) { running = 0; return; }
    if (!strcmp(cmd, "clear")) { t->output[0] = 0; last_status = 0; return; }

    if (!strcmp(cmd, "cd")) {
        const char *p = n > 1 ? tk[1].v : getenv("HOME");
        if (!p || !*p) { out(t, "terminator: cd: missing directory\n"); last_status = 1; return; }
        char res[PATH_MAX];
        int w = p[0] == '/' ? snprintf(res, sizeof res, "%s", p) : snprintf(res, sizeof res, "%s/%s", t->cwd, p);
        char *c = w < (int)sizeof res ? realpath(res, NULL) : NULL;
        int ok = 0;
        if (w >= (int)sizeof res || (c && strlen(c) >= sizeof t->cwd)) out(t, "terminator: cd: path too long\n");
        else if (!c) out(t, "terminator: cd: cannot change directory\n");
        else { snprintf(t->cwd, sizeof t->cwd, "%s", c); ok = 1; }
        free(c);
        last_status = !ok;
        return;
    }
    if (!strcmp(cmd, "history")) {
        char s[INSZ + 32];
        for (int i = 0; i < t->hist_n; i++) { snprintf(s, sizeof s, "%4d  %s\n", i + 1, t->history[i]); out(t, s); }
        last_status = 0;
        return;
    }
    if (!strcmp(cmd, "background_image_change")) {
        if (proc_child) { out(t, "terminator: background changes are handled by the GUI (use F5)\n"); last_status = 1; }
        else if (background_image_change() == 0) { draw_window(); out(t, "Background image changed.\n"); }
        else { out(t, "terminator: no other loadable XPM background found\n"); last_status = 1; }
        return;
    }
    if (!strcmp(cmd, "echo")) {
        for (int i = 1; i < n; i++) {
            char s[32];
            snprintf(s, sizeof s, "%d", last_status);
            out(t, strcmp(tk[i].v, "$?") ? tk[i].v : s);
            if (i + 1 < n) out(t, " ");
        }
        out(t, "\n");
        last_status = 0;
        return;
    }
    last_status = execute_pipeline(t, tk, n);
}

void execute_command(Tab *t) {
    if (!t->in_len) return;
    char line[INSZ];
    snprintf(line, sizeof line, "%s", t->input);
    out(t, t->cont ? "> " : "user@terminator> ");
    out(t, line); out(t, "\n");

    if (t->cmd_len > 0 && t->cmd_len < CMDSZ - 1) t->command[t->cmd_len++] = '\n';
    size_t ll = strlen(line);
    if (t->cmd_len + (int)ll >= CMDSZ) {
        out(t, "terminator: command too long\n");
        t->cont = 0;
    } else {
        memcpy(t->command + t->cmd_len, line, ll + 1);
        t->cmd_len += (int)ll;
        if (needs_continuation(t->command)) t->cont = 1;
        else { t->cont = 0; save_history(t, t->command); run_command(t); }
    }
    if (!t->cont) { t->cmd_len = 0; t->command[0] = 0; }
    t->in_len = 0; t->input[0] = 0;
}

/* ---------- process mode: one shell process per tab; IPC = [type][uint32 len][payload] ---------- */
static int wr(int fd, const void *d, size_t n) {
    const char *p = d;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        p += k; n -= (size_t)k;
    }
    return 0;
}
static int send_ipc(int fd, char type, const void *d, uint32_t n) {
    return (wr(fd, &type, 1) || wr(fd, &n, sizeof n) || (n && wr(fd, d, n))) ? -1 : 0;
}
static void shell_child(int in, int fo, const char *cwd) {
    static Tab sh;
    char line[INSZ], ch;
    size_t u = 0;
    proc_child = 1; running = 1;
    snprintf(sh.cwd, sizeof sh.cwd, "%s", cwd);
    for (;;) {
        ssize_t n = read(in, &ch, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        if (ch != '\n') { if (u < sizeof line - 1) line[u++] = ch; continue; }
        line[u] = 0; u = 0;
        snprintf(sh.input, sizeof sh.input, "%s", line);
        sh.in_len = (int)strlen(sh.input);
        size_t old = strlen(sh.output);
        execute_command(&sh);
        size_t nw = strlen(sh.output);
        if (nw < old) { if (send_ipc(fo, 'C', NULL, 0)) return; old = 0; }
        for (size_t pos = old; pos < nw;) {
            uint32_t amt = (uint32_t)(nw - pos > CHUNK ? CHUNK : nw - pos);
            if (send_ipc(fo, 'O', sh.output + pos, amt)) return;
            pos += amt;
        }
        TabStatus st;
        memset(&st, 0, sizeof st);
        st.cont = sh.cont;
        snprintf(st.cwd, sizeof st.cwd, "%s", sh.cwd);
        if (send_ipc(fo, 'S', &st, (uint32_t)sizeof st) || !running) return;
    }
}
static int start_process_tab(Tab *t) {
    int cm[2] = {-1, -1}, ou[2] = {-1, -1};
    pid_t pid = -1;
    if (pipe(cm) == -1 || pipe(ou) == -1 || (pid = fork()) == -1) {
        cl(cm[0]); cl(cm[1]); cl(ou[0]); cl(ou[1]);
        return -1;
    }
    if (pid == 0) {
        close(cm[1]); close(ou[0]);
        for (int i = 0; i < tab_count; i++) { cl(tabs[i].cmd_fd); cl(tabs[i].out_fd); }
        shell_child(cm[0], ou[1], t->cwd);
        _exit(0);
    }
    close(cm[0]); close(ou[1]);
    int fl = fcntl(ou[0], F_GETFL, 0);
    if (fl != -1) fcntl(ou[0], F_SETFL, fl | O_NONBLOCK);
    t->pid = pid; t->cmd_fd = cm[1]; t->out_fd = ou[0]; t->ipc_len = 0;
    return 0;
}
static void stop_process_tab(Tab *t) {
    cl(t->cmd_fd); cl(t->out_fd);
    if (t->pid > 0) { kill(t->pid, SIGTERM); while (waitpid(t->pid, NULL, 0) == -1 && errno == EINTR) {} }
    t->cmd_fd = t->out_fd = -1; t->pid = -1;
}
static void parse_ipc(Tab *t) {
    size_t u = 0;
    while (t->ipc_len - u >= 5) {
        unsigned char *p = t->ipc + u;
        uint32_t len;
        memcpy(&len, p + 1, sizeof len);
        if (len > sizeof t->ipc - 5) { t->ipc_len = 0; return; }
        if (t->ipc_len - u < 5 + (size_t)len) break;
        if (p[0] == 'O') {
            char c[CHUNK + 1];
            size_t a = len > CHUNK ? CHUNK : len;
            memcpy(c, p + 5, a); c[a] = 0;
            out(t, c);
        } else if (p[0] == 'C') t->output[0] = 0;
        else if (p[0] == 'S' && len == sizeof(TabStatus)) {
            TabStatus st;
            memcpy(&st, p + 5, sizeof st);
            t->cont = st.cont;
            snprintf(t->cwd, sizeof t->cwd, "%s", st.cwd);
        }
        u += 5 + (size_t)len;
    }
    if (u) { memmove(t->ipc, t->ipc + u, t->ipc_len - u); t->ipc_len -= u; }
}
/* Drain the pipe; parse after every chunk so the 8 KB frame buffer never overflows. */
static void process_tab_ipc(Tab *t) {
    unsigned char in[CHUNK];
    for (;;) {
        ssize_t n = read(t->out_fd, in, sizeof in);
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) { close(t->out_fd); t->out_fd = -1; break; }
        if (n < 0) break;
        for (size_t off = 0; off < (size_t)n;) {
            size_t room = sizeof t->ipc - t->ipc_len, a = (size_t)n - off < room ? (size_t)n - off : room;
            memcpy(t->ipc + t->ipc_len, in + off, a);
            t->ipc_len += a; off += a;
            parse_ipc(t);
        }
    }
}

/* ---------- main ---------- */
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "useProcess=True")) proc_mode = 1;
    if (!(dpy = XOpenDisplay(NULL))) { fprintf(stderr, "Cannot open X11 display.\n"); return 1; }
    int scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 100, 100, 900, 600, 1, BlackPixel(dpy, scr), WhitePixel(dpy, scr));
    XStoreName(dpy, win, "Terminator - Custom Terminal");
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask);

    Visual *vis = DefaultVisual(dpy, scr);
    Colormap cm = DefaultColormap(dpy, scr);
    xd = XftDrawCreate(dpy, win, vis, cm);
    XftColorAllocName(dpy, vis, cm, "#FF4D6D", &c_head);
    XftColorAllocName(dpy, vis, cm, "#EAF2FF", &c_out);
    XftColorAllocName(dpy, vis, cm, "#7CFF6B", &c_prompt);
    XftColorAllocName(dpy, vis, cm, "#63E6FF", &c_in);
    update_font(900, 600);
    if (!font) { fprintf(stderr, "Cannot load a monospace Xft font.\n"); return 1; }
    if (load_wallpaper(bgs[cur_bg]) == -1) fprintf(stderr, "Warning: could not load initial wallpaper %s\n", bgs[cur_bg]);

    for (int i = 0; i < MAX_TABS; i++) init_tab(&tabs[i]);
    if (proc_mode && start_process_tab(&tabs[0]) == -1) {
        fprintf(stderr, "Cannot start tab shell process.\n");
        XCloseDisplay(dpy);
        return 1;
    }
    XMapWindow(dpy, win);
    out(&tabs[0], "Terminator: Custom Terminal\nBasic quoting, redirection and pipelines\n"
                  "Ctrl+T: new tab; Ctrl+W: close tab; F1-F4: switch\n\n");

    while (running) {
        if (proc_mode) {
            struct pollfd fds[MAX_TABS + 1];
            int map[MAX_TABS + 1], nf = 1;
            fds[0] = (struct pollfd){ConnectionNumber(dpy), POLLIN, 0};
            for (int i = 0; i < tab_count; i++)
                if (tabs[i].out_fd >= 0) { fds[nf] = (struct pollfd){tabs[i].out_fd, POLLIN | POLLHUP, 0}; map[nf++] = i; }
            if (XPending(dpy) == 0) {
                if (poll(fds, (nfds_t)nf, 50) > 0)
                    for (int j = 1; j < nf; j++)
                        if (fds[j].revents & (POLLIN | POLLHUP | POLLERR)) process_tab_ipc(&tabs[map[j]]);
                if (XPending(dpy) == 0) { draw_window(); continue; }
            }
        }
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == Expose) { if (ev.xexpose.count == 0) draw_window(); continue; }
        if (ev.type == DestroyNotify) { running = 0; continue; }
        if (ev.type == ConfigureNotify) { update_font(ev.xconfigure.width, ev.xconfigure.height); draw_window(); continue; }
        if (ev.type == ButtonPress) {                /* mouse wheel */
            if (ev.xbutton.button == Button4) tabs[active].scroll += 3;
            else if (ev.xbutton.button == Button5) tabs[active].scroll -= 3;
            else continue;
            draw_window();
            continue;
        }
        if (ev.type != KeyPress) continue;

        char txt[32];
        KeySym key;
        int len = XLookupString(&ev.xkey, txt, (int)sizeof txt, &key, NULL);
        int ctrl = ev.xkey.state & ControlMask;
        Tab *t = &tabs[active];

        if (ctrl && (key == XK_t || key == XK_T)) {
            if (tab_count < MAX_TABS) {
                active = tab_count++;
                init_tab(&tabs[active]);
                out(&tabs[active], "New tab opened.\n\n");
                if (proc_mode && start_process_tab(&tabs[active]) == -1) {
                    tab_count--; active = tab_count - 1;
                    out(&tabs[active], "terminator: could not start tab process\n");
                }
            }
        } else if (ctrl && (key == XK_w || key == XK_W)) {
            if (tab_count > 1) {
                if (proc_mode) stop_process_tab(t);
                for (int i = active; i < tab_count - 1; i++) tabs[i] = tabs[i + 1];
                if (active >= --tab_count) active = tab_count - 1;
            } else running = 0;
        } else if (key == XK_F5) {
            if (background_image_change() == -1) out(t, "terminator: no other loadable XPM background found\n");
            else draw_window();
        } else if (key >= XK_F1 && key <= XK_F4) {
            if ((int)(key - XK_F1) < tab_count) active = (int)(key - XK_F1);
        } else if (key == XK_Prior || key == XK_Next || key == XK_Home || key == XK_End ||
                   ((ev.xkey.state & ShiftMask) && (key == XK_Up || key == XK_Down))) {
            int step = (key == XK_Prior || key == XK_Next) ? page_rows - 1 : 1;
            if (key == XK_Prior || key == XK_Up) t->scroll += step;
            else if (key == XK_Next || key == XK_Down) t->scroll -= step;
            else t->scroll = key == XK_Home ? INT_MAX / 2 : 0;
        } else if (key == XK_Return || key == XK_KP_Enter) {
            t->scroll = 0;
            if (!proc_mode) execute_command(t);
            else {
                if (t->cmd_fd >= 0) {
                    char l[INSZ + 2];
                    int n = snprintf(l, sizeof l, "%s\n", t->input);
                    if (n > 0 && wr(t->cmd_fd, l, (size_t)n) == -1) out(t, "terminator: failed to send command to tab process\n");
                }
                t->in_len = 0; t->input[0] = 0;
            }
        } else if (key == XK_BackSpace) {
            t->scroll = 0;
            if (t->in_len > 0) t->input[--t->in_len] = 0;
        } else if (key == XK_Escape) {
            t->scroll = 0; t->in_len = 0; t->input[0] = 0;
        } else if (len > 0 && txt[0] >= 32 && txt[0] <= 126 && t->in_len < INSZ - 1) {
            t->scroll = 0;
            t->input[t->in_len++] = txt[0];
            t->input[t->in_len] = 0;
        }
        draw_window();
    }
    if (proc_mode) for (int i = 0; i < tab_count; i++) stop_process_tab(&tabs[i]);
    XCloseDisplay(dpy);
    return 0;
}