#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <pwd.h>

#define MAX_TABS 32
#define INPUT_SIZE 4096
#define MAX_LINES 5000
#define LINE_CAP 4096
#define TABBAR_H 34
#define LINE_H 19
#define PAD_X 14
#define PAD_Y 16

typedef struct {
    char **lines;
    size_t count, cap;
    char input[INPUT_SIZE];
    size_t input_len;
    char cwd[PATH_MAX];
    int out_fd;
    pid_t pid;
    int running;
    int scroll;
    char title[64];
    char partial[LINE_CAP];
    size_t partial_len;
    int live_line;
} Tab;

static Display *dpy;
static Window win;
static GC gc;
static XFontStruct *font;
static int screen_no, win_w = 1000, win_h = 650;
static Tab tabs[MAX_TABS];
static int tab_count = 0, active_tab = 0;
static const char *user_name = "user";
static unsigned long col_bg, col_fg, col_dim, col_green, col_tab, col_sel, col_border;

static unsigned long color(const char *name, unsigned long fallback) {
    XColor c, exact;
    Colormap map = DefaultColormap(dpy, screen_no);
    if (XAllocNamedColor(dpy, map, name, &c, &exact)) return c.pixel;
    return fallback;
}

static void ensure_lines(Tab *t) {
    if (t->count < t->cap) return;
    size_t nc = t->cap ? t->cap * 2 : 64;
    if (nc > MAX_LINES) {
        free(t->lines[0]);
        memmove(t->lines, t->lines + 1, (t->count - 1) * sizeof(*t->lines));
        t->count--;
        return;
    }
    char **p = realloc(t->lines, nc * sizeof(*p));
    if (!p) return;
    t->lines = p;
    t->cap = nc;
}

static void append_line(Tab *t, const char *s) {
    ensure_lines(t);
    if (t->count >= t->cap) return;
    t->lines[t->count] = strdup(s ? s : "");
    if (t->lines[t->count]) t->count++;
    t->scroll = 0;
}

static void append_output(Tab *t, const char *buf, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\r') continue;
        if (c == '\n') {
            t->partial[t->partial_len] = '\0';
            if (t->live_line >= 0 && (size_t)t->live_line < t->count) {
                free(t->lines[t->live_line]);
                t->lines[t->live_line] = strdup(t->partial);
            }
            t->partial_len = 0;
            t->partial[0] = '\0';
            t->live_line = -1;
            continue;
        }
        if (c == '\b' || c == 127) {
            if (t->partial_len) t->partial[--t->partial_len] = '\0';
        } else if (c >= 32 || c == '\t') {
            if (t->partial_len + 1 < sizeof(t->partial))
                t->partial[t->partial_len++] = (char)c;
            t->partial[t->partial_len] = '\0';
        } else continue;
        if (t->live_line < 0) {
            append_line(t, "");
            t->live_line = (int)t->count - 1;
        }
        if (t->live_line >= 0 && (size_t)t->live_line < t->count) {
            char *copy = strdup(t->partial);
            if (copy) { free(t->lines[t->live_line]); t->lines[t->live_line] = copy; }
        }
    }
}

static void set_title(Tab *t) {
    const char *home = getenv("HOME");
    if (home && strncmp(t->cwd, home, strlen(home)) == 0 &&
        (t->cwd[strlen(home)] == '/' || t->cwd[strlen(home)] == '\0')) {
        snprintf(t->title, sizeof(t->title), "~%s", t->cwd + strlen(home));
        if (!t->title[1]) strcpy(t->title, "~");
    } else snprintf(t->title, sizeof(t->title), "%.63s", t->cwd);
}

static void init_tab(Tab *t, const char *cwd) {
    memset(t, 0, sizeof(*t));
    t->out_fd = -1;
    t->live_line = -1;
    if (!cwd || !*cwd || strlen(cwd) >= sizeof(t->cwd)) cwd = "/";
    snprintf(t->cwd, sizeof(t->cwd), "%s", cwd);
    set_title(t);
    append_line(t, "Terminator X11 terminal — type commands; Ctrl+Shift+T: new tab, Ctrl+Shift+W: close tab");
    append_line(t, "");
}

static void draw_text(int x, int y, const char *s, unsigned long c) {
    if (!s) return;
    XSetForeground(dpy, gc, c);
    XDrawString(dpy, win, gc, x, y, s, (int)strlen(s));
}

static void draw_window(void) {
    XSetForeground(dpy, gc, col_bg);
    XFillRectangle(dpy, win, gc, 0, 0, (unsigned)win_w, (unsigned)win_h);
    XSetForeground(dpy, gc, col_border);
    XFillRectangle(dpy, win, gc, 0, TABBAR_H - 1, (unsigned)win_w, 1);

    int x = 8;
    for (int i = 0; i < tab_count; ++i) {
        int tw = 150;
        if (i == active_tab) {
            XSetForeground(dpy, gc, col_sel);
            XFillRectangle(dpy, win, gc, x, 4, (unsigned)tw, TABBAR_H - 7);
        }
        char label[128];
        snprintf(label, sizeof(label), "%d: %.18s%s", i + 1, tabs[i].title,
                 tabs[i].running ? " *" : "");
        draw_text(x + 10, 23, label, i == active_tab ? col_fg : col_dim);
        x += tw + 4;
    }
    XSetForeground(dpy, gc, col_tab);
    XFillRectangle(dpy, win, gc, x, 4, 36, TABBAR_H - 7);
    draw_text(x + 11, 23, "+", col_green);

    Tab *t = &tabs[active_tab];
    int usable = (win_h - TABBAR_H - 55) / LINE_H;
    if (usable < 1) usable = 1;
    int end = (int)t->count - t->scroll;
    if (end < 0) end = 0;
    int start = end - usable;
    if (start < 0) start = 0;
    int y = TABBAR_H + PAD_Y + LINE_H;
    for (int i = start; i < end; ++i, y += LINE_H) {
        draw_text(PAD_X, y, t->lines[i], col_fg);
    }

    char prompt[PATH_MAX + 128];
    snprintf(prompt, sizeof(prompt), "%s@terminator:%s> ", user_name, t->title);
    int py = win_h - 27;
    draw_text(PAD_X, py, prompt, col_green);
    int prompt_width = XTextWidth(font, prompt, (int)strlen(prompt));
    draw_text(PAD_X + prompt_width, py, t->input, col_fg);
    int cursor_x = PAD_X + prompt_width + XTextWidth(font, t->input, (int)t->input_len);
    XSetForeground(dpy, gc, col_fg);
    XFillRectangle(dpy, win, gc, cursor_x, py - font->ascent, 2, font->ascent + font->descent);
    XFlush(dpy);
}

static int is_cd_command(const char *s, char *arg, size_t argsz) {
    while (*s == ' ' || *s == '\t') s++;
    if (strncmp(s, "cd", 2) != 0 || (s[2] && s[2] != ' ' && s[2] != '\t')) return 0;
    s += 2;
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t')) n--;
    if (n >= argsz) n = argsz - 1;
    memcpy(arg, s, n); arg[n] = '\0';
    return 1;
}

static void execute_command(Tab *t) {
    char command[INPUT_SIZE];
    memcpy(command, t->input, t->input_len + 1);
    t->input_len = 0; t->input[0] = '\0';
    append_line(t, "");
    char prompt[PATH_MAX + 128];
    snprintf(prompt, sizeof(prompt), "%s@terminator:%s> %s", user_name, t->title, command);
    append_line(t, prompt);
    if (!command[0]) return;

    char arg[PATH_MAX];
    if (is_cd_command(command, arg, sizeof(arg))) {
        if (!arg[0]) snprintf(arg, sizeof(arg), "%s", getenv("HOME") ? getenv("HOME") : "/");
        else if (arg[0] == '~' && (arg[1] == '/' || arg[1] == '\0')) {
            char tmp[PATH_MAX];
            snprintf(tmp, sizeof(tmp), "%s%s", getenv("HOME") ? getenv("HOME") : "/", arg + 1);
            snprintf(arg, sizeof(arg), "%s", tmp);
        }
        char target[PATH_MAX];
        if (arg[0] == '/') snprintf(target, sizeof(target), "%s", arg);
        else snprintf(target, sizeof(target), "%s/%s", t->cwd, arg);
        char resolved[PATH_MAX];
        if (chdir(target) == 0) {
            if (getcwd(resolved, sizeof(resolved))) snprintf(t->cwd, sizeof(t->cwd), "%s", resolved);
            set_title(t);
            /* Restore the GUI process cwd: each tab owns its own cwd string. */
            chdir("/");
        } else {
            char msg[PATH_MAX + 64]; snprintf(msg, sizeof(msg), "cd: %s: %s", arg, strerror(errno)); append_line(t, msg);
        }
        return;
    }

    if (t->running) { append_line(t, "A command is already running in this tab."); return; }
    int p[2];
    if (pipe(p) < 0) { append_line(t, "pipe: failed to create output pipe"); return; }
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); append_line(t, "fork: failed to create process"); return; }
    if (pid == 0) {
        close(p[0]);
        dup2(p[1], STDOUT_FILENO); dup2(p[1], STDERR_FILENO); close(p[1]);
        int xfd = ConnectionNumber(dpy); if (xfd >= 0) close(xfd);
        if (chdir(t->cwd) < 0) _exit(126);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        dprintf(STDERR_FILENO, "exec: %s\n", strerror(errno)); _exit(127);
    }
    close(p[1]);
    int flags = fcntl(p[0], F_GETFL, 0);
    if (flags >= 0) fcntl(p[0], F_SETFL, flags | O_NONBLOCK);
    t->out_fd = p[0]; t->pid = pid; t->running = 1;
}

static int add_tab(const char *cwd) {
    if (tab_count >= MAX_TABS) return -1;
    init_tab(&tabs[tab_count], cwd);
    active_tab = tab_count++;
    return active_tab;
}

static void close_tab(int idx) {
    if (tab_count <= 1 || idx < 0 || idx >= tab_count) return;
    Tab *t = &tabs[idx];
    if (t->out_fd >= 0) close(t->out_fd);
    if (t->running && t->pid > 0) kill(t->pid, SIGHUP);
    for (size_t i = 0; i < t->count; ++i) free(t->lines[i]);
    free(t->lines);
    for (int i = idx; i < tab_count - 1; ++i) tabs[i] = tabs[i+1];
    tab_count--;
    if (active_tab >= tab_count) active_tab = tab_count - 1;
    else if (idx < active_tab) active_tab--;
}

static void handle_key(XKeyEvent *kev) {
    char bytes[64]; KeySym ks = NoSymbol;
    int n = XLookupString(kev, bytes, sizeof(bytes), &ks, NULL);
    Tab *t = &tabs[active_tab];
    int ctrl = (kev->state & ControlMask) != 0;
    int shift = (kev->state & ShiftMask) != 0;
    if (ctrl && shift && (ks == XK_T || ks == XK_t)) { add_tab(t->cwd); draw_window(); return; }
    if (ctrl && shift && (ks == XK_W || ks == XK_w)) { close_tab(active_tab); draw_window(); return; }
    if (ctrl && ks == XK_Tab) { active_tab = (active_tab + 1) % tab_count; draw_window(); return; }
    if (ctrl && shift && ks == XK_ISO_Left_Tab) { active_tab = (active_tab + tab_count - 1) % tab_count; draw_window(); return; }
    if (ks == XK_Left || ks == XK_Right || ks == XK_Home || ks == XK_End) return;
    if (ks == XK_Page_Up) { t->scroll += 15; if (t->scroll > (int)t->count) t->scroll = (int)t->count; draw_window(); return; }
    if (ks == XK_Page_Down) { t->scroll -= 15; if (t->scroll < 0) t->scroll = 0; draw_window(); return; }
    if (ks == XK_Up) { if (t->scroll < (int)t->count) t->scroll += 1; draw_window(); return; }
    if (ks == XK_Down) { if (t->scroll > 0) t->scroll -= 1; draw_window(); return; }
    if (ks == XK_Return || ks == XK_KP_Enter) { execute_command(t); draw_window(); return; }
    if (ks == XK_BackSpace) { if (t->input_len) t->input[--t->input_len] = '\0'; draw_window(); return; }
    if (ks == XK_Escape) { t->input_len = 0; t->input[0] = '\0'; draw_window(); return; }
    if (ctrl && (ks == XK_c || ks == XK_C)) {
        if (t->running && t->pid > 0) kill(t->pid, SIGINT);
        else { t->input_len = 0; t->input[0] = '\0'; }
        draw_window(); return;
    }
    if (n > 0 && !ctrl) {
        for (int i = 0; i < n && t->input_len + 1 < sizeof(t->input); ++i) {
            unsigned char c = (unsigned char)bytes[i];
            if (c >= 32 && c != 127) t->input[t->input_len++] = (char)c;
        }
        t->input[t->input_len] = '\0'; draw_window();
    }
}

int main(void) {
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_name) user_name = pw->pw_name;
    char cwd[PATH_MAX]; if (!getcwd(cwd, sizeof(cwd))) snprintf(cwd, sizeof(cwd), "%s", getenv("HOME") ? getenv("HOME") : "/");
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "terminator: cannot open X display (is DISPLAY set?)\n"); return 1; }
    screen_no = DefaultScreen(dpy);
    col_bg = color("#111827", BlackPixel(dpy, screen_no));
    col_fg = color("#e5e7eb", WhitePixel(dpy, screen_no));
    col_dim = color("#9ca3af", WhitePixel(dpy, screen_no));
    col_green = color("#34d399", WhitePixel(dpy, screen_no));
    col_tab = color("#1f2937", BlackPixel(dpy, screen_no));
    col_sel = color("#374151", BlackPixel(dpy, screen_no));
    col_border = color("#4b5563", WhitePixel(dpy, screen_no));
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen_no), 80, 60, (unsigned)win_w, (unsigned)win_h, 1,
                              col_border, col_bg);
    XStoreName(dpy, win, "Terminator — X11 Terminal");
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask);
    XMapWindow(dpy, win);
    gc = XCreateGC(dpy, win, 0, NULL);
    font = XLoadQueryFont(dpy, "fixed"); if (font) XSetFont(dpy, gc, font->fid);
    XSetForeground(dpy, gc, col_fg);
    add_tab(cwd);
    int xfd = ConnectionNumber(dpy);
    while (1) {
        struct pollfd pfds[1 + MAX_TABS]; int map[MAX_TABS + 1]; nfds_t nfds = 1;
        pfds[0].fd = xfd; pfds[0].events = POLLIN; pfds[0].revents = 0; map[0] = -1;
        for (int i = 0; i < tab_count; ++i) if (tabs[i].out_fd >= 0) {
            pfds[nfds].fd = tabs[i].out_fd; pfds[nfds].events = POLLIN | POLLHUP | POLLERR; pfds[nfds].revents = 0; map[nfds] = i; nfds++;
        }
        int pr = poll(pfds, nfds, -1);
        if (pr < 0) { if (errno == EINTR) continue; perror("poll"); break; }
        int redraw = 0;
        if (pfds[0].revents & POLLIN) {
            while (XPending(dpy)) {
                XEvent ev; XNextEvent(dpy, &ev);
                if (ev.type == Expose) redraw = 1;
                else if (ev.type == ConfigureNotify) { win_w = ev.xconfigure.width; win_h = ev.xconfigure.height; redraw = 1; }
                else if (ev.type == KeyPress) { handle_key(&ev.xkey); redraw = 0; }
                else if (ev.type == ButtonPress) {
                    int x = ev.xbutton.x, y = ev.xbutton.y;
                    if (y < TABBAR_H) {
                        if (x >= 8) {
                            int idx = (x - 8) / 154;
                            if (idx >= 0 && idx < tab_count) active_tab = idx;
                            else { int plusx = 8 + tab_count * 154; if (x >= plusx && x < plusx + 36) add_tab(tabs[active_tab].cwd); }
                        }
                        redraw = 1;
                    }
                }
            }
        }
        for (nfds_t j = 1; j < nfds; ++j) if (pfds[j].revents) {
            int idx = map[j];
            if (idx < 0 || idx >= tab_count) continue;
            Tab *t = &tabs[idx]; char buf[4096];
            if (t->out_fd < 0 || t->out_fd != pfds[j].fd) continue;
            for (;;) {
                ssize_t nr = read(t->out_fd, buf, sizeof(buf));
                if (nr > 0) { append_output(t, buf, (size_t)nr); redraw = 1; continue; }
                if (nr == 0) {
                    close(t->out_fd); t->out_fd = -1; t->running = 0;
                    int status; while (waitpid(t->pid, &status, WNOHANG) < 0 && errno == EINTR) {}
                    t->pid = 0; t->live_line = -1; t->partial_len = 0; append_line(t, ""); redraw = 1;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    close(t->out_fd); t->out_fd = -1; t->running = 0; t->pid = 0; redraw = 1;
                }
                break;
            }
        }
        for (int i = 0; i < tab_count; ++i) if (tabs[i].running && tabs[i].pid > 0) {
            int status; pid_t r = waitpid(tabs[i].pid, &status, WNOHANG);
            if (r == tabs[i].pid) { tabs[i].pid = 0; /* Pipe EOF closes the running state. */ }
        }
        if (redraw) draw_window();
    }
    XCloseDisplay(dpy);
    return 0;
}
