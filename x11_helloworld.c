#include <X11/Xlib.h>
#include <X11/Xutil.h> 
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/types.h>
#include <signal.h>
#include <glob.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <poll.h>

#define MAX_PIPELINE 16          /* max commands in one pipeline */
#define MAX_ARGS     64          /* max argv per command */
/* Room for one command's argv strings (kept small so the per-pipeline
 * array stays comfortably off the session thread's stack). */
#define MAX_STORAGE  16384
#define MAX_JOBS 32
#define MAX_WATCH 16
#define WATCH_INTERVAL 2
#define WATCH_OUT 8192
#define WATCH_SEP "----------------------------------------------------"

typedef struct {
    char  in_file[1024];         /* "" if no < redirection */
    char  out_file[1024];        /* "" if no > redirection */
    char *argv[MAX_ARGS];
     int   argc;     
} Command;

static int cursor_visible = 1;


#define MAX_TABS 5
#define MAX_LINE 4096
#define MAX_INPUT MAX_LINE       /* longest line the user can type */
#define MAX_OUTPUT (4096 * 16)
#define HISTORY_FILE_MAX 10000  /* commands kept in the shared history file */
#define HISTORY_SHOW_MAX 1000   /* how many `history` prints */
#define MAX_MATCHES 64          /* max Tab-completion candidates */
#define LINE_H 18
#define CHAR_W 6
#define PAD_X 10
#define TEXT_TOP 55
#define TABBAR_BOTTOM 35

typedef struct {
    char cmd[MAX_LINE];
    int pipe_fd;
    pid_t pid;
    int running;
    time_t next_run;
    char out_buf[WATCH_OUT];
    int out_len;
} WatchCmd;

/* One managed command, i.e. a pipeline running in its own process group.
 * `pgid` is the group we signal (Ctrl+C / Ctrl+Z / fg / bg) and `pid` is
 * the last pipeline stage, whose exit ends the job. */
typedef struct {
    int   used;
    pid_t pid;              /* last stage pid */
    pid_t pgid;             /* process group id */
    int   pipe_fd;          /* capture pipe read end (-1 when closed) */
    char  cmd[MAX_LINE];    /* command line, for jobs/fg/bg messages */
    int   nlive;            /* pipeline stages not yet exited */
    int   stopped;          /* 1 = suspended (SIGTSTP/SIGSTOP) */
    int   background;       /* 1 = started with '&' */
    int   status;           /* exit status (or 128+signal) once done */
} Job;

typedef struct {
    char title[64];
    char cwd[1024];
    char input_line[MAX_LINE];
    int input_len;
    int cursor;                     /* caret position within input_line */
    int hist_pos;                   /* browse index, -1 when not browsing */
    char hist_save[MAX_LINE];       /* line saved when history browse starts */
    int  search_mode;               /* 1 = Ctrl+R term entry in progress */

    /* Tab completion */
    int  comp_mode;                     /* 1 = waiting for a digit to pick */
    char comp_prefix[MAX_LINE];         /* directory part incl. trailing '/' */
    char comp_names[MAX_MATCHES][MAX_LINE]; /* candidate basenames */
    int  comp_isdir[MAX_MATCHES];
    int  comp_count;
    int  comp_start, comp_end;          /* span of the word in input_line */
    char comp_list[MAX_OUTPUT];         /* numbered candidates, drawn below */
    int  comp_list_len;
    char output_buffer[MAX_OUTPUT];
    int output_len;
    int last_exit_status;

    /* job control */
    Job jobs[MAX_JOBS];
    int cur_job;                    /* slot of the '+' job, -1 if none */
    int fg_job;                     /* slot of the foreground job, -1 if none */


    int  multiline;
    char multiline_buf[MAX_OUTPUT];
    int  multiline_len;

    /* mouse text selection, in absolute wrapped-display-line coordinates */
    int sel_active;
    int sel_dragging;
    int sel_anchor_line, sel_anchor_col;
    int sel_line, sel_col;

    /* keyboard (Shift+arrows) selection over the input line, in character
     * offsets: anchor is where it started, end is the moving edge. */
    int in_sel_active;
    int in_sel_anchor;
    int in_sel_end;

    int watching;
    int nwatch;
    WatchCmd watches[MAX_WATCH];
    int scroll; /* display lines above the bottom (0 = follow latest) */
} Tab;

static Tab tabs[MAX_TABS];
static int num_tabs = 0;
static int current_tab = 0;
static int win_w = 800, win_h = 500;

/* ===================== shared command history =====================
 * One history shared by every tab.  In thread mode it lives in the
 * process's memory and is guarded by a pthread mutex; in process mode
 * it lives in an shm_open()+mmap() segment guarded by a POSIX
 * process-shared semaphore.  Either way every tab locks it, so a
 * command typed in one tab shows up in the others.                  */
typedef struct {
    sem_t           sem;          /* process mode lock (shared) */
    pthread_mutex_t mutex;        /* thread mode lock */
    int             count;        /* entries currently stored */
    long            base;         /* entries dropped off the front */
    char            entries[HISTORY_FILE_MAX][MAX_LINE];
} History;

static History *H = NULL;             /* the one shared history */
static int      g_use_process = 0;    /* 0 = thread mode, 1 = process mode */
static char     g_history_path[1024];

#define g_history       (H->entries)
#define g_history_count (H->count)
#define g_history_base  (H->base)

static void history_lock(void) {
    if (g_use_process) sem_wait(&H->sem);
    else               pthread_mutex_lock(&H->mutex);
}
static void history_unlock(void) {
    if (g_use_process) sem_post(&H->sem);
    else               pthread_mutex_unlock(&H->mutex);
}

/* Create the one shared history.  Thread mode: plain process memory with
 * a pthread mutex.  Process mode: shm_open()+mmap(MAP_SHARED) so parent
 * and every forked tab see the same records, with a process-shared
 * semaphore (sem_init pshared=1) as the lock. */
#define HISTORY_SHM_NAME "/terminator_history"

static void history_init(void) {
    if (g_use_process) {
        int fd = shm_open(HISTORY_SHM_NAME, O_CREAT | O_RDWR, 0600);
        if (fd < 0) { perror("shm_open"); exit(1); }
        if (ftruncate(fd, (off_t)sizeof(History)) < 0) {
            perror("ftruncate"); close(fd); exit(1);
        }
        void *p = mmap(NULL, sizeof(History), PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
        close(fd);
        if (p == MAP_FAILED) { perror("mmap"); exit(1); }
        /* keep the segment anonymous for this run only */
        shm_unlink(HISTORY_SHM_NAME);
        H = (History *)p;
        if (sem_init(&H->sem, 1, 1) < 0) { perror("sem_init"); exit(1); }
        H->count = 0;
        H->base  = 0;
    } else {
        H = calloc(1, sizeof(History));
        if (!H) { perror("calloc"); exit(1); }
        pthread_mutex_init(&H->mutex, NULL);
    }
}

/* ===================== per-tab execution sessions =====================
 * Every tab runs its shell in its own execution context, so that
 * closing or crashing one tab cannot take the others down:
 *
 *   thread mode  (./terminator)
 *       one pthread per tab; the tabs share the process's memory and
 *       commands travel through a mutex-protected buffer.
 *   process mode (./terminator useProcess=True)
 *       the GUI forks one child per tab and talks to it over two pipes;
 *       history stays in shared memory guarded by a process-shared
 *       semaphore, and the GUI stays in the parent.
 *
 * Only the GUI touches X11.  A session never renders: it streams small
 * frames back to the GUI, which feeds them into the tab's display.
 */
enum { EXEC_THREAD = 0, EXEC_PROCESS = 1 };

/* session -> GUI frames */
enum { F_OUT = 1, F_CWD, F_BUSY, F_QUIT, F_DELEGATE };
/* GUI -> session frames */
enum { C_RUN = 1, C_SIG, C_CLOSE };

#define SES_CHUNK 2048
#define SES_IOBUF (64 * 1024)

typedef struct TabSession {
    int   mode;
    Tab  *shell;                 /* this session's private shell state */
    Tab  *gui_tab;               /* the tab it draws into (GUI side only) */

    /* Transport.  to_sess/from_sess are the GUI's ends of the two pipes;
     * sfds[] are the session's ends, used by the thread or forked child.
     *   GUI --write->to_sess==sfds[0]--read-->  session
     *   GUI <-read--from_sess==sfds[1]<-write-- session            */
    int   to_sess;
    int   from_sess;
    int   sfds[2];

    char  rbuf[SES_IOBUF];       /* GUI side: partial-frame accumulator */
    int   rlen;
    char  cbuf[SES_IOBUF];       /* session side: partial-frame accumulator */
    int   clen;

    /* thread mode */
    pthread_t tid;

    /* process mode */
    pid_t pid;

    int   quitting;              /* session: stop the loop */
    int   user_exit;             /* session: the user typed `exit` */
    int   busy;                  /* GUI: a foreground job / watch is running */
    int   alive;                 /* GUI: session still usable */
    int   closing;               /* GUI: asked the session to stop */
    int   gui_quit;              /* GUI: the session asked this tab to quit */
    int   dead;                  /* GUI: session vanished unexpectedly */
    int   notified;              /* GUI: already reported a dead session */
} TabSession;

static TabSession *sessions[MAX_TABS];

/* Set inside a session thread / forked child; NULL while the GUI runs.
 * It is how append_output() knows whether it belongs to the GUI or to a
 * tab session that must stream its output back instead. */
static __thread TabSession *g_sess = NULL;

static TabSession *session_create(Tab *gui_tab, const char *cwd);
static void session_close(TabSession *s);
static void session_destroy_all(void);
static int  session_gui_fd(TabSession *s);
static int  session_gui_drain(TabSession *s);
static void session_send_run(TabSession *s, const char *line);
static void session_send_signal(TabSession *s, int sig);
static void session_emit(TabSession *s, const char *data, int len);
static void session_emit_int(TabSession *s, int type, int value);
static void session_emit_cwd(TabSession *s, const char *cwd);
static void session_emit_line(TabSession *s, int type, const char *line);

/* Is the GUI tab `gui_tab` currently running a foreground command? */
static int session_busy(Tab *gui_tab) {
    int idx = (int)(gui_tab - tabs);
    if (idx < 0 || idx >= MAX_TABS) return 0;
    TabSession *s = sessions[idx];
    return (s && s->alive && s->busy) ? 1 : 0;
}

/* ===================== configurable key bindings =====================
 * A table of (modifiers, KeySym) -> action. Loaded from ~/.terminatorrc
 * at startup and editable at runtime with the `bind` built-in.          */

enum {
    ACT_NONE = 0,
    ACT_LEFT, ACT_RIGHT,
    ACT_LINE_START, ACT_LINE_END,
    ACT_HIST_UP, ACT_HIST_DOWN,
    ACT_KILL_TO_START, ACT_KILL_TO_END, ACT_KILL_WORD,
    ACT_CLEAR_SCREEN, ACT_EXIT, ACT_COPY, ACT_PASTE
};
typedef int Action;

typedef struct {
    KeySym       keysym;   /* normalised (letters stored lower-case) */
    unsigned int mods;     /* ControlMask | ShiftMask | Mod1Mask */
    Action       action;
    int          user;     /* 1 = set from config file / bind command */
} Binding;

#define MAX_BINDINGS 64
#define BIND_MODS    (ControlMask | ShiftMask | Mod1Mask)

static Binding bindings[MAX_BINDINGS];
static int     num_bindings = 0;

static Atom atom_clipboard, atom_utf8, atom_targets, atom_sel_data;

/* messages gathered while parsing the config, shown in the first tab */
static char config_msgs[MAX_OUTPUT];
static int  config_msgs_len = 0;

static const struct { const char *name; Action act; } action_table[] = {
    { "left",               ACT_LEFT },
    { "right",              ACT_RIGHT },
    { "line-start",         ACT_LINE_START },
    { "line-end",           ACT_LINE_END },
    { "history-up",         ACT_HIST_UP },
    { "prev-history",       ACT_HIST_UP },
    { "history-down",       ACT_HIST_DOWN },
    { "next-history",       ACT_HIST_DOWN },
    { "kill-to-start",      ACT_KILL_TO_START },
    { "kill-to-end",        ACT_KILL_TO_END },
    { "kill-line",          ACT_KILL_TO_END },
    { "kill-word",          ACT_KILL_WORD },
    { "backward-kill-word", ACT_KILL_WORD },
    { "clear-screen",       ACT_CLEAR_SCREEN },
    { "exit",               ACT_EXIT },
    { "copy",               ACT_COPY },
    { "paste",              ACT_PASTE },
    { NULL, ACT_NONE }
};

static KeySym norm_keysym(KeySym ks) {
    if (ks >= XK_A && ks <= XK_Z) return ks - XK_A + XK_a;
    return ks;
}

static Action action_from_name(const char *name) {
    for (int i = 0; action_table[i].name; i++)
        if (strcasecmp(name, action_table[i].name) == 0)
            return action_table[i].act;
    return ACT_NONE;
}

static const char *action_name(Action a) {
    for (int i = 0; action_table[i].name; i++)
        if (action_table[i].act == a) return action_table[i].name;
    return "?";
}

static void config_report(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stderr);
    int l = (int)strlen(buf);
    if (config_msgs_len + l < (int)sizeof(config_msgs) - 1) {
        memcpy(config_msgs + config_msgs_len, buf, l);
        config_msgs_len += l;
        config_msgs[config_msgs_len] = '\0';
    }
}

/* "Ctrl+Shift+C" / "Left" -> KeySym + modifier bits. NoSymbol on failure. */
static KeySym parse_key_spec(const char *spec, unsigned int *mods_out) {
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s", spec);
    unsigned int mods = 0;
    KeySym ks = NoSymbol;

    for (char *tok = strtok(tmp, "+"); tok; tok = strtok(NULL, "+")) {
        if      (!strcasecmp(tok, "ctrl") || !strcasecmp(tok, "control")) mods |= ControlMask;
        else if (!strcasecmp(tok, "shift"))                               mods |= ShiftMask;
        else if (!strcasecmp(tok, "alt") || !strcasecmp(tok, "meta") ||
                 !strcasecmp(tok, "mod1"))                                mods |= Mod1Mask;
        else {
            KeySym k = XStringToKeysym(tok);
            if (k == NoSymbol && strlen(tok) == 1) {
                char u[2] = { tok[0], '\0' };
                k = XStringToKeysym(u);
            }
            if (k != NoSymbol) ks = k;
        }
    }
    *mods_out = mods;
    return norm_keysym(ks);
}

/* (KeySym, mods) -> "Ctrl+Shift+C" */
static void format_key(KeySym ks, unsigned int mods, char *out, int outsz) {
    char base[64];
    if (ks >= XK_a && ks <= XK_z) {
        base[0] = (char)(ks - XK_a + 'A');
        base[1] = '\0';
    } else {
        const char *s = XKeysymToString(ks);
        snprintf(base, sizeof(base), "%s", s ? s : "?");
    }
    out[0] = '\0';
    if (mods & ControlMask) strncat(out, "Ctrl+",  outsz - 1 - strlen(out));
    if (mods & ShiftMask)   strncat(out, "Shift+", outsz - 1 - strlen(out));
    if (mods & Mod1Mask)    strncat(out, "Alt+",   outsz - 1 - strlen(out));
    strncat(out, base, outsz - 1 - strlen(out));
}

static void add_binding(KeySym ks, unsigned int mods, Action a, int user) {
    if (num_bindings >= MAX_BINDINGS) return;
    bindings[num_bindings].keysym = norm_keysym(ks);
    bindings[num_bindings].mods   = mods & BIND_MODS;
    bindings[num_bindings].action = a;
    bindings[num_bindings].user   = user;
    num_bindings++;
}

static int find_binding(KeySym ks, unsigned int mods) {
    ks    = norm_keysym(ks);
    mods &= BIND_MODS;
    for (int i = 0; i < num_bindings; i++)
        if (bindings[i].keysym == ks && bindings[i].mods == mods) return i;
    return -1;
}

static int set_binding(KeySym ks, unsigned int mods, Action a, int user) {
    int i = find_binding(ks, mods);
    if (i >= 0) {
        bindings[i].action = a;
        if (user) bindings[i].user = 1;
        return i;
    }
    add_binding(ks, mods, a, user);
    return num_bindings - 1;
}

static void init_default_bindings(void) {
    num_bindings = 0;
    add_binding(XK_Left,  0,                       ACT_LEFT,          0);
    add_binding(XK_Right, 0,                       ACT_RIGHT,         0);
    add_binding(XK_a,     ControlMask,             ACT_LINE_START,    0);
    add_binding(XK_e,     ControlMask,             ACT_LINE_END,      0);
    add_binding(XK_Up,    0,                       ACT_HIST_UP,       0);
    add_binding(XK_Down,  0,                       ACT_HIST_DOWN,     0);
    add_binding(XK_u,     ControlMask,             ACT_KILL_TO_START, 0);
    add_binding(XK_k,     ControlMask,             ACT_KILL_TO_END,   0);
    add_binding(XK_w,     ControlMask,             ACT_KILL_WORD,     0);
    add_binding(XK_l,     ControlMask,             ACT_CLEAR_SCREEN,  0);
    add_binding(XK_d,     ControlMask,             ACT_EXIT,          0);
    add_binding(XK_c,     ControlMask | ShiftMask, ACT_COPY,          0);
    add_binding(XK_v,     ControlMask | ShiftMask, ACT_PASTE,         0);
}

static int config_path(char *buf, int sz) {
    const char *home = getenv("HOME");
    if (!home || !*home) return -1;
    snprintf(buf, sz, "%s/.terminatorrc", home);
    return 0;
}

/* Parse ~/.terminatorrc into the binding table.
 * Invalid or self-conflicting entries are reported and the default kept. */
static void load_config(void) {
    typedef struct {
        KeySym       keysym;
        unsigned int mods;
        Action       action;
        int          line;
        char         keystr[64];
    } PendingBind;

    init_default_bindings();
    config_msgs[0] = '\0';
    config_msgs_len = 0;

    char path[1200];
    if (config_path(path, sizeof(path)) < 0) return;

    FILE *f = fopen(path, "r");
    if (!f) return;                       /* no config file: pure defaults */

    PendingBind pend[MAX_BINDINGS];
    int np = 0;
    char line[512];
    int lineno = 0;

    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        char *e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' ||
                         e[-1] == ' '  || e[-1] == '\t')) *--e = '\0';
        if (!*s) continue;

        if (strncmp(s, "bind", 4) != 0 || (s[4] && !isspace((unsigned char)s[4]))) {
            config_report("terminator: %s:%d: unknown directive "
                          "(expected 'bind <key> <action>')\n", path, lineno);
            continue;
        }
        char *p = s + 4;
        while (*p == ' ' || *p == '\t') p++;

        char key[64] = ""; int ki = 0;
        while (*p && !isspace((unsigned char)*p) && ki < 63) key[ki++] = *p++;
        key[ki] = '\0';
        while (*p == ' ' || *p == '\t') p++;

        char act[64] = ""; int ai = 0;
        while (*p && !isspace((unsigned char)*p) && ai < 63) act[ai++] = *p++;
        act[ai] = '\0';

        if (!key[0] || !act[0]) {
            config_report("terminator: %s:%d: malformed binding\n", path, lineno);
            continue;
        }

        unsigned int mods = 0;
        KeySym ks = parse_key_spec(key, &mods);
        if (ks == NoSymbol) {
            config_report("terminator: %s:%d: unknown key '%s', keeping default\n",
                          path, lineno, key);
            continue;
        }
        Action a = action_from_name(act);
        if (a == ACT_NONE) {
            config_report("terminator: %s:%d: unknown action '%s', keeping default\n",
                          path, lineno, act);
            continue;
        }
        if (np >= MAX_BINDINGS) continue;
        pend[np].keysym = ks;
        pend[np].mods   = mods & BIND_MODS;
        pend[np].action = a;
        pend[np].line   = lineno;
        snprintf(pend[np].keystr, sizeof(pend[np].keystr), "%s", key);
        np++;
    }
    fclose(f);

    /* flag any key bound to two different actions by the file itself */
    int bad[MAX_BINDINGS] = { 0 };
    for (int i = 0; i < np; i++)
        for (int j = i + 1; j < np; j++)
            if (pend[i].keysym == pend[j].keysym && pend[i].mods == pend[j].mods &&
                pend[i].action != pend[j].action)
                bad[i] = bad[j] = 1;

    for (int i = 0; i < np; i++) {
        if (bad[i]) {
            config_report("terminator: %s:%d: conflicting binding for '%s', "
                          "keeping default\n", path, pend[i].line, pend[i].keystr);
            continue;
        }
        set_binding(pend[i].keysym, pend[i].mods, pend[i].action, 1);
    }
}

/* Persist the user's custom bindings back to ~/.terminatorrc. */
static void rewrite_config(void) {
    char path[1200];
    if (config_path(path, sizeof(path)) < 0) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# ~/.terminatorrc - Terminator key bindings\n");
    fprintf(f, "# syntax: bind <key> <action>\n");
    fprintf(f, "# keys: Ctrl+/Shift+/Alt+ prefixes, e.g. Ctrl+Shift+C, Left, Up\n");
    for (int i = 0; i < num_bindings; i++) {
        if (!bindings[i].user) continue;
        char kb[64];
        format_key(bindings[i].keysym, bindings[i].mods, kb, sizeof(kb));
        fprintf(f, "bind %s %s\n", kb, action_name(bindings[i].action));
    }
    fclose(f);
}

/* ===================================================================== */

void append_output(Tab *tab, const char *str) {
    /* Inside a tab session this buffer belongs to the GUI: stream the
     * text back over the session pipe instead of writing it locally. */
    if (g_sess != NULL && tab == g_sess->shell) {
        session_emit(g_sess, str, (int)strlen(str));
        return;
    }
    int len = strlen(str);
    if (tab->output_len + len < MAX_OUTPUT - 1) {
        strcpy(tab->output_buffer + tab->output_len, str);
        tab->output_len += len;
    } else {
        // Shift buffer if full
        int overflow = tab->output_len + len - (MAX_OUTPUT - 1);
        memmove(tab->output_buffer, tab->output_buffer + overflow, tab->output_len - overflow);
        tab->output_len -= overflow;
        strcpy(tab->output_buffer + tab->output_len, str);
        tab->output_len += len;
    }
}
/* Split a complete command line into argv with shell quoting rules.
 * Caller supplies args[] and storage. Returns argc. */
static int shell_split(const char *line, char **args, int maxargs,
                       char *storage, int storage_sz) {
    int argc = 0, si = 0, ci = 0, have_word = 0;
    char cur[MAX_LINE];

    for (int i = 0; line[i] && argc < maxargs - 1; ) {
        char c = line[i];

        if (c == ' ' || c == '\t' || c == '\n') {
            if (have_word) {
                cur[ci] = '\0';
                int need = ci + 1;
                if (si + need > storage_sz) break;
                memcpy(storage + si, cur, need);
                args[argc++] = storage + si;
                si += need;
                ci = 0;
                have_word = 0;
            }
            i++;
            continue;
        }

        if (c == '\'') {
            have_word = 1; i++;
            while (line[i] && line[i] != '\'') {
                if (ci < (int)sizeof(cur) - 1) cur[ci++] = line[i];
                i++;
            }
            if (line[i] == '\'') i++;
            continue;
        }

        if (c == '"') {
            have_word = 1; i++;
            while (line[i] && line[i] != '"') {
                if (line[i] == '\\' && line[i+1]) {
                    char n = line[i+1];
                    char e = 0;
                    /* Inside double quotes the shell resolves these
                     * escapes, so `echo "l1\nl2"` prints two lines and a
                     * backslash-newline keeps the newline (it ends the
                     * argument's visual line but not the word). */
                    switch (n) {
                        case 'n':  e = '\n'; break;
                        case 't':  e = '\t'; break;
                        case 'r':  e = '\r'; break;
                        case 'a':  e = '\a'; break;
                        case 'b':  e = '\b'; break;
                        case 'f':  e = '\f'; break;
                        case 'v':  e = '\v'; break;
                        case 'e':  e = 27;   break;
                        case '\\': e = '\\'; break;
                        case '"':  e = '"';  break;
                        case '$':  e = '$';  break;
                        case '`':  e = '`';  break;
                        case '\n': e = '\n'; break;  /* \<newline> keeps it */
                        default:   e = 0;    break;
                    }
                    if (e) {
                        if (ci < (int)sizeof(cur) - 1) cur[ci++] = e;
                        i += 2;
                        continue;
                    }
                    /* unknown escape: keep the backslash literally */
                    if (ci < (int)sizeof(cur) - 1) cur[ci++] = line[i];
                    i++;
                    continue;
                }
                if (ci < (int)sizeof(cur) - 1) cur[ci++] = line[i];
                i++;
            }
            if (line[i] == '"') i++;
            continue;
        }

        if (c == '\\' && line[i+1]) {
            have_word = 1;
            char n = line[i+1];
            if (n == '\n') { i += 2; continue; }   /* continuation */
            if (ci < (int)sizeof(cur) - 1) cur[ci++] = n;
            i += 2;
            continue;
        }

        have_word = 1;
        if (ci < (int)sizeof(cur) - 1) cur[ci++] = c;
        i++;
    }

    if (have_word && argc < maxargs - 1) {
        cur[ci] = '\0';
        int need = ci + 1;
        if (si + need <= storage_sz) {
            memcpy(storage + si, cur, need);
            args[argc++] = storage + si;
        }
    }

    args[argc] = NULL;
    return argc;
}

/* Expand wildcard arguments in argv[] in place.
 * Relative patterns are matched against `cwd` (the tab's directory), never
 * the process cwd, so one tab's `cd` can't affect another tab's globs.
 * Non-matching args are kept literally (POSIX shell behaviour).
 * Returns the new argc. */
static int expand_globs(char **argv, int argc,
                        char **new_argv, int new_max, const char *cwd) {
    int out = 0;

    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];

        /* Only bother if the arg has a glob metachar and isn't quoted...
         * (Your shell_split already stripped quotes, so we can't tell.
         *  For simplicity: glob only args that contain a metachar. */
        if (!strpbrk(arg, "*?[")) {
            if (out < new_max - 1) new_argv[out++] = argv[i];
            continue;
        }

        /* Rewrite a relative pattern to an absolute one rooted at the tab
         * cwd, then strip that prefix from the results so the argv entries
         * keep their original (relative) shape. */
        char prefix[PATH_MAX];
        char pattern[PATH_MAX];
        const char *pat = arg;
        prefix[0] = '\0';
        if (arg[0] != '/' && cwd && cwd[0]) {
            snprintf(prefix, sizeof(prefix), "%s/", cwd);
            snprintf(pattern, sizeof(pattern), "%s%s", prefix, arg);
            pat = pattern;
        }

        glob_t g;
        int rc = glob(pat, GLOB_NOCHECK, NULL, &g);
        if (rc == 0) {
            size_t plen = strlen(prefix);
            for (size_t k = 0; k < g.gl_pathc && out < new_max - 1; k++) {
                const char *r = g.gl_pathv[k];
                if (plen > 0 && strncmp(r, prefix, plen) == 0) r += plen;
                new_argv[out++] = strdup(r);
            }
            globfree(&g);
        } else {
            if (out < new_max - 1) new_argv[out++] = argv[i];
        }
    }

    new_argv[out] = NULL;
    return out;
}


/* Extract unquoted < file and > file from a raw command line.
 * Everything else (including quotes and $() content) is copied to `cleaned`.
 * Returns 0 on success, -1 on missing filename. */
 int parse_redirections(const char *line,
                              char *cleaned, int cleaned_sz,
                              char *in_file,  int in_sz,
                              char *out_file, int out_sz) {
    int ci = 0;
    int in_sq = 0, in_dq = 0;

    in_file[0]  = '\0';
    out_file[0] = '\0';

    for (int i = 0; line[i]; ) {
        char c = line[i];

        if (in_sq) {
            if (ci < cleaned_sz - 1) cleaned[ci++] = c;
            if (c == '\'') in_sq = 0;
            i++;
            continue;
        }

        if (c == '\\' && line[i+1]) {
            if (ci < cleaned_sz - 1) cleaned[ci++] = c;
            if (ci < cleaned_sz - 1) cleaned[ci++] = line[i+1];
            i += 2;
            continue;
        }

        if (in_dq) {
            if (ci < cleaned_sz - 1) cleaned[ci++] = c;
            if (c == '"') in_dq = 0;
            i++;
            continue;
        }

        if (c == '\'') { in_sq = 1; if (ci < cleaned_sz-1) cleaned[ci++] = c; i++; continue; }
        if (c == '"')  { in_dq = 1; if (ci < cleaned_sz-1) cleaned[ci++] = c; i++; continue; }

        if (c == '<' || c == '>') {
            int j = i + 1;
            while (line[j] == ' ' || line[j] == '\t') j++;

            char fname[1024]; int fi = 0;
            char q = 0;
            if (line[j] == '\'' || line[j] == '"') { q = line[j]; j++; }

            while (line[j] && fi < (int)sizeof(fname) - 1) {
                if (q) {
                    if (line[j] == q) { j++; break; }
                    fname[fi++] = line[j++];
                } else {
                    if (line[j] == ' ' || line[j] == '\t' ||
                        line[j] == '<' || line[j] == '>') break;
                    if (line[j] == '\\' && line[j+1]) { fname[fi++] = line[j+1]; j += 2; continue; }
                    fname[fi++] = line[j++];
                }
            }
            fname[fi] = '\0';
            if (fi == 0) return -1;            /* missing filename */

            if (c == '<') { strncpy(in_file,  fname, in_sz  - 1); in_file [in_sz  - 1] = '\0'; }
            else          { strncpy(out_file, fname, out_sz - 1); out_file[out_sz - 1] = '\0'; }

            i = j;
            continue;
        }

        if (ci < cleaned_sz - 1) cleaned[ci++] = c;
        i++;
    }

    cleaned[ci] = '\0';
    return 0;
}



/* Parse a single pipeline segment into a Command.
 * Returns 0 on success, -1 on error (missing redirection target). */
 int parse_command(const char *segment, Command *cmd,
                         char *storage, int storage_sz) {
    char cleaned[MAX_OUTPUT];

    if (parse_redirections(segment, cleaned, sizeof(cleaned),
                           cmd->in_file,  sizeof(cmd->in_file),
                           cmd->out_file, sizeof(cmd->out_file)) < 0)
        return -1;

    int argc = shell_split(cleaned, cmd->argv, MAX_ARGS, storage, storage_sz);
        cmd->argv[argc] = NULL;
        cmd->argc = argc;
        return 0;
}

/* Split `line` on top-level '|' chars (not inside quotes / after backslash).
 * Writes pointers into `out` and NUL-terminates each segment in-place.
 * Returns count, or -1 if too many segments. */
 int split_pipeline(char *line, char **out, int max_out) {
    int count = 0;
    int in_sq = 0, in_dq = 0;

    out[count++] = line;

    for (int i = 0; line[i]; i++) {
        char c = line[i];

        if (in_sq) { if (c == '\'') in_sq = 0; continue; }
        if (c == '\\') { i++; continue; }        /* skip escaped char */
        if (in_dq) { if (c == '"') in_dq = 0; continue; }
        if (c == '\'') { in_sq = 1; continue; }
        if (c == '"')  { in_dq = 1; continue; }

        if (c == '|') {
            if (count >= max_out) return -1;
            line[i] = '\0';
            out[count++] = &line[i + 1];
        }
    }
    return count;
}


/* Resolve a filename against the TAB's cwd instead of the process cwd.
 * In thread mode every tab shares one process cwd, so relying on it would
 * let a `cd` in one tab silently redirect another tab's relative paths.
 * Returns `buf` when a rewrite happened, otherwise the original pointer. */
static const char *tab_path(Tab *tab, const char *name, char *buf, size_t sz) {
    if (name[0] == '/' || !tab || tab->cwd[0] == '\0') return name;
    snprintf(buf, sz, "%s/%s", tab->cwd, name);
    return buf;
}

/* Open redirection fds for `cmd`. Fills *in_fd / *out_fd (-1 if unused).
 * Returns 0 on success, -1 on error (message already appended). */
 int open_redirections(Tab *tab, Command *cmd, int *in_fd, int *out_fd) {
    *in_fd = -1;
    *out_fd = -1;

    if (cmd->in_file[0]) {
        char p[PATH_MAX];
        *in_fd = open(tab_path(tab, cmd->in_file, p, sizeof(p)), O_RDONLY);
        if (*in_fd < 0) {
            char err[1200];
            snprintf(err, sizeof(err),
                     "terminator: %s: No such file or directory\n", cmd->in_file);
            append_output(tab, err);
            return -1;
        }
    }
    if (cmd->out_file[0]) {
        char p[PATH_MAX];
        *out_fd = open(tab_path(tab, cmd->out_file, p, sizeof(p)),
                       O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (*out_fd < 0) {
            char err[1200];
            snprintf(err, sizeof(err),
                     "terminator: %s: cannot open for writing\n", cmd->out_file);
            append_output(tab, err);
            if (*in_fd >= 0) close(*in_fd);
            *in_fd = -1;
            return -1;
        }
    }
    return 0;
}

/* ===================== job control =====================
 * Every command runs in its own process group.  Keys arrive as X11
 * events, so Terminator itself delivers Ctrl+C / Ctrl+Z with kill(). */

static void job_free(Tab *t, int idx) {
    t->jobs[idx].used = 0;
    t->jobs[idx].pipe_fd = -1;
    t->jobs[idx].cmd[0] = '\0';
    if (t->fg_job == idx) t->fg_job = -1;
    if (t->cur_job == idx) {
        t->cur_job = -1;
        for (int i = MAX_JOBS - 1; i >= 0; i--)
            if (t->jobs[i].used) { t->cur_job = i; break; }
    }
}

/* Allocate the lowest free job slot, or -1 when the table is full. */
static int job_alloc(Tab *t) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!t->jobs[i].used) {
            memset(&t->jobs[i], 0, sizeof(Job));
            t->jobs[i].used = 1;
            t->jobs[i].pipe_fd = -1;
            t->cur_job = i;
            return i;
        }
    }
    return -1;
}

static Job *foreground_job(Tab *t) {
    if (t->fg_job < 0 || !t->jobs[t->fg_job].used) return NULL;
    return &t->jobs[t->fg_job];
}

/* Drain everything currently buffered on a job's capture pipe.
 * The pipe is non-blocking, so this never stalls the GUI. */
static int drain_job(Tab *t, Job *j) {
    if (j->pipe_fd < 0) return 0;
    char buf[256];
    int got = 0;
    for (;;) {
        ssize_t n = read(j->pipe_fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            append_output(t, buf);
            got = 1;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;                          /* 0 = EOF, -1 = EAGAIN */
    }
    return got;
}

static void job_print_stopped(Tab *t, int idx, int current) {
    char buf[MAX_LINE + 32];
    snprintf(buf, sizeof(buf), "[%d]%c Stopped %s\n",
             idx + 1, current ? '+' : ' ', t->jobs[idx].cmd);
    append_output(t, buf);
}

static void job_print_done(Tab *t, int idx, int current) {
    char buf[MAX_LINE + 32];
    snprintf(buf, sizeof(buf), "[%d]%c Done %s\n",
             idx + 1, current ? '+' : ' ', t->jobs[idx].cmd);
    append_output(t, buf);
}

static void job_print_bg(Tab *t, int idx, const char *cmd) {
    char buf[MAX_LINE + 32];
    snprintf(buf, sizeof(buf), "[%d]%c %s &\n", idx + 1, '+', cmd);
    append_output(t, buf);
}

/* Take every notification the kernel has for a job: stops, exits of
 * bookkeeping stages, and the exit of the last stage (which ends it).
 * With `wait_event` set, keep polling briefly for a state change — used
 * right after SIGTSTP so the suspend is reported synchronously. */
static void job_reap(Tab *t, int idx, int wait_event) {
    Job *j = &t->jobs[idx];
    int spins = 0;
    for (;;) {
        int status = 0;
        pid_t r = waitpid(-j->pgid, &status, WNOHANG | WUNTRACED);
        if (r == 0) {                   /* nothing to report (yet) */
            if (!wait_event || spins++ >= 500) return;
            usleep(2000);
            continue;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (WIFSTOPPED(status)) {
            if (!j->stopped) {
                j->stopped = 1;
                if (t->fg_job == idx) t->fg_job = -1;
                job_print_stopped(t, idx, t->cur_job == idx);
            }
            return;                     /* a stop is not the end of the job */
        } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (r == j->pid)
                j->status = WIFEXITED(status) ? WEXITSTATUS(status)
                                              : 128 + WTERMSIG(status);
            if (--j->nlive <= 0) break; /* every stage has exited */
        } else {
            return;
        }
    }

    drain_job(t, j);                    /* flush whatever is still buffered */
    int was_fg = (t->fg_job == idx);
    if (j->pipe_fd >= 0) { close(j->pipe_fd); j->pipe_fd = -1; }
    t->last_exit_status = j->status;
    if (!was_fg) job_print_done(t, idx, t->cur_job == idx);
    job_free(t, idx);
}

/* Non-blocking sweep of every job in the tab. */
static void tab_reap_jobs(Tab *t) {
    for (int i = 0; i < MAX_JOBS; i++)
        if (t->jobs[i].used) job_reap(t, i, 0);
}

/* Run a command line that may contain '|' pipelines and redirections.
 * `background` routes the job to the background (see `cmd &`).
 * Returns exit status of the LAST command (or 1 on setup error). */
static int execute_pipeline(Tab *tab, char *line, int background) {
    /* Keep the command text for jobs/fg/bg messages before split_pipeline
     * NUL-terminates the segments in place. */
    char cmd_name[MAX_LINE];
    snprintf(cmd_name, sizeof(cmd_name), "%s", line);

    /* 1) Split on '|' */
    char *segments[MAX_PIPELINE];
    int n = split_pipeline(line, segments, MAX_PIPELINE);
    if (n <= 0) {
        append_output(tab, "terminator: syntax error: too many pipes\n");
        return 1;
    }

    /* 2) Parse each segment (redirections + argv).  These are automatic,
     * not static: in thread mode several tab shells parse concurrently. */
    char storage[MAX_PIPELINE][MAX_STORAGE];
    Command cmds[MAX_PIPELINE];

    for (int i = 0; i < n; i++) {
        if (parse_command(segments[i], &cmds[i],
                          storage[i], sizeof(storage[i])) < 0) {
            append_output(tab, "terminator: syntax error: missing redirection target\n");
            return 1;
        }
        if (cmds[i].argv[0] == NULL) {
            append_output(tab, "terminator: syntax error: empty command in pipeline\n");
            return 1;
        }
    }
    char *glob_storage[MAX_PIPELINE][MAX_ARGS];

    for (int i = 0; i < n; i++) {
        int newc = expand_globs(cmds[i].argv, cmds[i].argc,
                                glob_storage[i], MAX_ARGS, tab->cwd);
        /* Repoint argv at the expanded array */
        memcpy(cmds[i].argv, glob_storage[i], sizeof(char*) * (newc + 1));
        cmds[i].argc = newc;
    }

    /* 3) Create n-1 pipes */
    int pipes[MAX_PIPELINE - 1][2];
    for (int i = 0; i < n - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            for (int j = 0; j < i; j++) {
                close(pipes[j][0]); close(pipes[j][1]);
            }
            return 1;
        }
    }

    /* 4) Open all redirections up-front (so failures abort cleanly) */
    int  in_fds[MAX_PIPELINE];
    int out_fds[MAX_PIPELINE];
    for (int i = 0; i < n; i++) {
        if (open_redirections(tab, &cmds[i], &in_fds[i], &out_fds[i]) < 0) {
            for (int j = 0; j < n - 1; j++) {
                close(pipes[j][0]); close(pipes[j][1]);
            }
            for (int j = 0; j < i; j++) {
                if (in_fds[j]  >= 0) close(in_fds[j]);
                if (out_fds[j] >= 0) close(out_fds[j]);
            }
            return 1;
        }
    }

    /* 5) One pipe for the tab to capture the FINAL command's stdout+stderr */
    int capture[2];
    if (pipe(capture) < 0) { perror("pipe"); /* cleanup… */ return 1; }

    /* 6) Fork every stage, all sharing one process group so Ctrl+C,
     * Ctrl+Z, fg and bg can signal the whole pipeline at once. */
    pid_t pids[MAX_PIPELINE];
    pid_t pgid = 0;                     /* first stage's pid */
    for (int i = 0; i < n; i++) {
        int prev_read  = (i > 0)        ? pipes[i - 1][0] : -1;
        int next_write = (i < n - 1)    ? pipes[i][1]     : -1;

        pid_t pid = fork();
        if (pid < 0) { perror("fork"); /* cleanup… */ return 1; }

        if (pid == 0) {
            /* ---- CHILD ---- */
            setpgid(0, i == 0 ? 0 : pgid);
            if (in_fds[i] >= 0)         dup2(in_fds[i], STDIN_FILENO);
            else if (prev_read >= 0)    dup2(prev_read, STDIN_FILENO);

            /* stdout: file redirect > next pipe > capture pipe (last stage) */
            if (out_fds[i] >= 0) {
                dup2(out_fds[i], STDOUT_FILENO);
            } else if (next_write >= 0) {
                dup2(next_write, STDOUT_FILENO);       /* into the next stage */
            } else {
                dup2(capture[1], STDOUT_FILENO);       /* last stage → tab display */
            }

            /* stderr always goes to the tab's capture pipe — never into a stage pipe */
            dup2(capture[1], STDERR_FILENO);

            /* close ALL inherited pipe + redirect fds */
            for (int k = 0; k < n - 1; k++) {
                close(pipes[k][0]); close(pipes[k][1]);
            }
            close(capture[0]); close(capture[1]);
            for (int k = 0; k < n; k++) {
                if (in_fds[k]  >= 0) close(in_fds[k]);
                if (out_fds[k] >= 0) close(out_fds[k]);
            }

            signal(SIGPIPE, SIG_DFL);   /* undoes the GUI's SIG_IGN */
            /* Reset dispositions the launcher ignored, so job control works
               identically in thread mode (the GUI's SIG_IGN would otherwise
               be inherited and make the job unkillable by Ctrl+C/Ctrl+Z). */
            signal(SIGINT,  SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);
            chdir(tab->cwd);
            execvp(cmds[i].argv[0], cmds[i].argv);
            fprintf(stderr, "terminator: %s: command not found\n", cmds[i].argv[0]);
            _exit(127);
        }
        if (i == 0) pgid = pid;
        setpgid(pid, pgid);             /* also done here to close the race */
        pids[i] = pid;
    }

    /* 7) Parent: close everything it doesn't need */
    for (int i = 0; i < n - 1; i++) {
        close(pipes[i][0]); close(pipes[i][1]);
    }
    close(capture[1]);
    for (int i = 0; i < n; i++) {
        if (in_fds[i]  >= 0) close(in_fds[i]);
        if (out_fds[i] >= 0) close(out_fds[i]);
    }

    int flags = fcntl(capture[0], F_GETFL, 0);
    if (flags >= 0) fcntl(capture[0], F_SETFL, flags | O_NONBLOCK);

    /* 8) Register the job; the event loop drains it and reaps the group. */
    int slot = job_alloc(tab);
    if (slot < 0) {
        append_output(tab, "terminator: too many jobs\n");
        close(capture[0]);
        return 1;
    }
    Job *j = &tab->jobs[slot];
    j->pid        = pids[n - 1];
    j->pgid       = pgid;
    j->pipe_fd    = capture[0];
    j->nlive      = n;
    j->background = background;
    snprintf(j->cmd, sizeof(j->cmd), "%s", cmd_name);

    if (background) {
        job_print_bg(tab, slot, j->cmd);
    } else {
        tab->fg_job = slot;
    }

    return 0;
}



void update_tab_title(Tab *tab) {
    char *home = getenv("HOME");
    if (home && strncmp(tab->cwd, home, strlen(home)) == 0) {
        snprintf(tab->title, sizeof(tab->title), "~%s", tab->cwd + strlen(home));
       
    } else {
        snprintf(tab->title, sizeof(tab->title), "%s", tab->cwd);
    }
}

void init_tab(Tab *tab, int id) {
    getcwd(tab->cwd, sizeof(tab->cwd));
    update_tab_title(tab);
    tab->input_len = 0;
    tab->input_line[0] = '\0';
    tab->cursor = 0;
    tab->hist_pos = -1;
    tab->hist_save[0] = '\0';
    tab->sel_active = 0;
    tab->sel_dragging = 0;
    tab->sel_anchor_line = tab->sel_line = 0;
    tab->sel_anchor_col  = tab->sel_col  = 0;
    tab->in_sel_active = 0;
    tab->in_sel_anchor = 0;
    tab->in_sel_end    = 0;
    tab->output_len = 0;
    tab->output_buffer[0] = '\0';
    tab->last_exit_status = 0;
    tab->search_mode = 0;
    tab->comp_mode = 0;
    tab->comp_count = 0;
    tab->comp_list_len = 0;
    tab->cur_job = -1;
    tab->fg_job  = -1;
    for (int i = 0; i < MAX_JOBS; i++) {
        memset(&tab->jobs[i], 0, sizeof(Job));
        tab->jobs[i].pipe_fd = -1;
    }

    tab->multiline = 0;
    tab->multiline_len = 0;
    tab->multiline_buf[0] = '\0';
    tab->watching = 0;
    tab->nwatch = 0;
    tab->scroll = 0;
    for (int i = 0; i < MAX_WATCH; i++) {
        tab->watches[i].pipe_fd = -1;
        tab->watches[i].pid = 0;
        tab->watches[i].running = 0;
        tab->watches[i].next_run = 0;
        tab->watches[i].out_len = 0;
        tab->watches[i].cmd[0] = '\0';
    }


    // char welcome[128];
    // snprintf(welcome, sizeof(welcome), "Terminator Tab %d initialized.\n", id);
    // append_output(tab, welcome);

    /* Every tab gets its own execution session: a pthread in thread mode,
     * a fork()ed child in process mode.  The GUI only renders. */
    int idx = (int)(tab - tabs);
    if (idx >= 0 && idx < MAX_TABS) {
        if (sessions[idx]) { session_close(sessions[idx]); sessions[idx] = NULL; }
        sessions[idx] = session_create(tab, tab->cwd);
    }
}

// Compute layout of tab i in window coordinates.
// label_x  = left edge of the "[ n: title ]" text
// close_x  = left edge of the [X] box
// tab_w    = total width of this tab (label + gap + close box)
static void tab_layout(int i, int *label_x, int *label_w,
                       int *close_x, int *close_w, int *tab_w) {
    char label[128];
    snprintf(label, sizeof(label), "[ %d: %s ]", i + 1, tabs[i].title);

    *label_x = 0;                          // caller adds running offset
    *label_w = strlen(label) * 7;
    *close_w = 14;
    *close_x = *label_w + 4;
    *tab_w   = *label_w + *close_w + 10;
}



void close_tab(int index) {
    if (num_tabs <= 1) return;          // keep at least one tab

    /* Stop this tab's execution session.  The GUI tab owns no jobs any
     * more: the session does, and it tears its own children down. */
    if (sessions[index]) {
        session_close(sessions[index]);
        sessions[index] = NULL;
    }

    Tab *t = &tabs[index];

    /* Kill and collect every job owned by this tab. */
    for (int i = 0; i < MAX_JOBS; i++) {
        Job *j = &t->jobs[i];
        if (!j->used) continue;
        if (j->pgid > 0) {
            kill(-j->pgid, SIGCONT);        /* wake a stopped job so it can die */
            kill(-j->pgid, SIGTERM);
        }
        if (j->pipe_fd >= 0) { close(j->pipe_fd); j->pipe_fd = -1; }
        if (j->pid > 0) waitpid(j->pid, NULL, 0);
        j->used = 0;
    }
    t->cur_job = -1;
    t->fg_job  = -1;

    if (t->watching) {
        for (int i = 0; i < t->nwatch; i++) {
            WatchCmd *w = &t->watches[i];
            if (w->pipe_fd >= 0) { close(w->pipe_fd); w->pipe_fd = -1; }
            if (w->pid > 0) {
                kill(-w->pid, SIGKILL);
                waitpid(w->pid, NULL, WNOHANG);
                w->pid = 0;
            }
        }
        t->watching = 0;
        t->nwatch = 0;
    }

   int moved = num_tabs - index - 1;
    if (moved > 0) {
        memmove(&tabs[index], &tabs[index + 1], moved * sizeof(Tab));
        memmove(&sessions[index], &sessions[index + 1],
                moved * sizeof(TabSession *));
    }

    // 3) Wipe the now-vacated tail slot so no stale fd/pid/data remains.
    memset(&tabs[num_tabs - 1], 0, sizeof(Tab));
    sessions[num_tabs - 1] = NULL;
    for (int i = 0; i < MAX_JOBS; i++)
        tabs[num_tabs - 1].jobs[i].pipe_fd = -1;
    tabs[num_tabs - 1].cur_job = -1;
    tabs[num_tabs - 1].fg_job  = -1;

    num_tabs--;

    // 4) Fix current_tab for every case:
    //    - closed tab was before current  -> shift left
    //    - closed tab was current or after -> clamp
    if (index < current_tab) {
        current_tab--;
    } else if (current_tab >= num_tabs) {
        current_tab = num_tabs - 1;
    }

    /* tabs[] shifted, so re-point every session at its tab. */
    for (int i = 0; i < num_tabs; i++)
        if (sessions[i]) sessions[i]->gui_tab = &tabs[i];
}

/* ===================== line-editing actions ===================== */

static int g_exit_requested = 0;

/* --------------------- shared history storage --------------------- */

static void history_path_init(void) {
    const char *home = getenv("HOME");
    if (!home || !*home) home = ".";
    snprintf(g_history_path, sizeof(g_history_path), "%s/.terminator_history", home);
}

/* append one command to the in-memory history, dropping the oldest
 * entry once HISTORY_FILE_MAX is reached.  The caller holds the lock. */
static void history_store(const char *cmd) {
    if (!cmd || !*cmd) return;
    if (g_history_count < HISTORY_FILE_MAX) {
        snprintf(g_history[g_history_count], MAX_LINE, "%s", cmd);
        g_history_count++;
    } else {
        memmove(g_history, g_history + 1,
                sizeof(g_history[0]) * (HISTORY_FILE_MAX - 1));
        snprintf(g_history[HISTORY_FILE_MAX - 1], MAX_LINE, "%s", cmd);
        g_history_base++;
    }
}

static void history_append_file(const char *cmd) {
    if (!g_history_path[0]) history_path_init();
    FILE *f = fopen(g_history_path, "a");
    if (!f) return;
    fprintf(f, "%s\n", cmd);
    fclose(f);
}

/* rewrite the file so it holds exactly the in-memory history */
static void history_save_file(void) {
    history_lock();
    if (!g_history_path[0]) history_path_init();
    FILE *f = fopen(g_history_path, "w");
    if (f) {
        for (int i = 0; i < g_history_count; i++)
            fprintf(f, "%s\n", g_history[i]);
        fclose(f);
    }
    history_unlock();
}

/* read ~/.terminator_history at startup; trim it back to the cap */
static void history_load(void) {
    history_path_init();
    FILE *f = fopen(g_history_path, "r");
    if (!f) return;
    char line[MAX_LINE];
    int  read = 0;
    history_lock();
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (n == 0) continue;
        history_store(line);
        read++;
    }
    history_unlock();
    fclose(f);
    if (read > HISTORY_FILE_MAX) history_save_file();   /* drop the oldest */
}

/* longest contiguous substring shared by a and b */
static int longest_common_substr(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la == 0 || lb == 0) return 0;
    int *prev = calloc((size_t)lb + 1, sizeof(int));
    int *cur  = calloc((size_t)lb + 1, sizeof(int));
    if (!prev || !cur) { free(prev); free(cur); return 0; }
    int best = 0;
    for (int i = 1; i <= la; i++) {
        for (int j = 1; j <= lb; j++) {
            if (a[i - 1] == b[j - 1]) {
                cur[j] = prev[j - 1] + 1;
                if (cur[j] > best) best = cur[j];
            } else {
                cur[j] = 0;
            }
        }
        int *tmp = prev; prev = cur; cur = tmp;
    }
    free(prev); free(cur);
    return best;
}

/* Ctrl+R search: exact match, else longest-common-substring (>2), else
 * "No match".  Results are appended to the tab's output. */
static void run_history_search(Tab *t) {
    const char *term = t->input_line;
    if (!*term) {
        append_output(t, "No match for search term in history\n");
        return;
    }

    history_lock();
    /* (a) most recent command that exactly matches the term */
    for (int i = g_history_count - 1; i >= 0; i--) {
        if (strcmp(g_history[i], term) == 0) {
            char buf[MAX_LINE + 2];
            snprintf(buf, sizeof(buf), "%s\n", g_history[i]);
            history_unlock();
            append_output(t, buf);
            return;
        }
    }

    /* (b) commands sharing the longest common substring with the term */
    int best = 0;
    for (int i = 0; i < g_history_count; i++) {
        int l = longest_common_substr(term, g_history[i]);
        if (l > best) best = l;
    }
    if (best > 2) {
        for (int i = 0; i < g_history_count; i++) {
            if (longest_common_substr(term, g_history[i]) == best) {
                char buf[MAX_LINE + 2];
                snprintf(buf, sizeof(buf), "%s\n", g_history[i]);
                append_output(t, buf);
            }
        }
        history_unlock();
        return;
    }
    history_unlock();

    append_output(t, "No match for search term in history\n");
}

/* ===================== Tab completion ===================== */

/* replace input_line[start,end) with `text` and put the caret after it.
 * The text after `end` is preserved, so completing with the caret in the
 * middle of the line keeps the rest of the line intact. */
static void splice_word(Tab *t, int start, int end, const char *text) {
    int tlen = (int)strlen(text);
    /* trust the string, not the cached length, so a mid-line cursor can
     * never make us drop the tail of the line */
    int ilen = (int)strlen(t->input_line);
    if (start < 0)      start = 0;
    if (start > ilen)   start = ilen;
    if (end < start)    end = start;
    if (end > ilen)     end = ilen;
    int rest = ilen - end;
    int room = MAX_LINE - 1 - start - rest;
    if (room < 0) room = 0;
    if (tlen > room) tlen = room;
    memmove(t->input_line + start + tlen, t->input_line + end, (size_t)rest + 1);
    memcpy(t->input_line + start, text, (size_t)tlen);
    t->input_len = start + tlen + rest;
    t->cursor = start + tlen;
    t->input_line[t->input_len] = '\0';
    t->hist_pos = -1;
}

static int comp_name_cmp(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

/* insert candidate `i` in place of the word being completed */
static void comp_apply(Tab *t, int i) {
    char text[MAX_LINE];
    snprintf(text, sizeof(text), "%s%s%s", t->comp_prefix, t->comp_names[i],
             t->comp_isdir[i] ? "/" : "");
    splice_word(t, t->comp_start, t->comp_end, text);
    t->comp_mode  = 0;
    t->comp_count = 0;
    t->in_sel_active = 0;
}

/* Tab pressed: complete the word before the cursor from the tab's cwd */
static void do_completion(Tab *t) {
    const char *s = t->input_line;
    /* recompute from the string so the cached length can never be stale */
    int ilen = (int)strlen(s);
    t->input_len = ilen;
    int cur = t->cursor;
    if (cur < 0) cur = 0;
    if (cur > ilen) cur = ilen;

    /* the word ends at the cursor and starts after the previous blank */
    int ws = cur;
    while (ws > 0 && !isspace((unsigned char)s[ws - 1])) ws--;
    int wlen = cur - ws;
    if (wlen <= 0 || wlen >= MAX_LINE) return;   /* nothing to complete */

    char word[MAX_LINE];
    memcpy(word, s + ws, (size_t)wlen);
    word[wlen] = '\0';

    /* split "dir/part" into a typed directory prefix and the base name */
    char prefix[MAX_LINE];
    const char *base;
    char *slash = strrchr(word, '/');
    if (slash) {
        int plen = (int)(slash - word) + 1;
        memcpy(prefix, word, (size_t)plen);
        prefix[plen] = '\0';
        base = slash + 1;
    } else {
        prefix[0] = '\0';
        base = word;
    }

    /* directory to scan: the tab's cwd, an absolute path or ~/... */
    char dir[PATH_MAX];
    if (prefix[0] == '\0') {
        snprintf(dir, sizeof(dir), "%s", t->cwd);
    } else if (prefix[0] == '~' && (prefix[1] == '\0' || prefix[1] == '/')) {
        const char *home = getenv("HOME");
        if (!home || !*home) home = ".";
        snprintf(dir, sizeof(dir), "%s%s", home, prefix + 1);
    } else if (prefix[0] == '/') {
        snprintf(dir, sizeof(dir), "%s", prefix);
    } else {
        snprintf(dir, sizeof(dir), "%s/%s", t->cwd, prefix);
    }
    size_t dl = strlen(dir);
    while (dl > 1 && dir[dl - 1] == '/') dir[--dl] = '\0';

    DIR *dp = opendir(dir);
    if (!dp) return;                     /* unreadable dir: do nothing */

    int baselen     = (int)strlen(base);
    int want_hidden = (baselen > 0 && base[0] == '.');
    int count       = 0;

    struct dirent *de;
    while ((de = readdir(dp)) != NULL && count < MAX_MATCHES) {
        const char *nm = de->d_name;
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
        if (!want_hidden && nm[0] == '.') continue;
        if (strncmp(nm, base, (size_t)baselen) != 0) continue;
        char full[PATH_MAX];
        struct stat st;
        snprintf(full, sizeof(full), "%s/%s", dir, nm);
        snprintf(t->comp_names[count], MAX_LINE, "%s", nm);
        t->comp_isdir[count] = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));
        count++;
    }
    closedir(dp);

    if (count == 0) return;              /* no match: do nothing */

    qsort(t->comp_names, (size_t)count, MAX_LINE, comp_name_cmp);
    t->comp_count = count;
    t->comp_start = ws;
    t->comp_end   = cur;
    snprintf(t->comp_prefix, sizeof(t->comp_prefix), "%s", prefix);

    /* one match: complete it */
    if (count == 1) {
        comp_apply(t, 0);
        return;
    }

    /* several matches: extend the word to their longest common prefix */
    int lcp = 0;
    while (t->comp_names[0][lcp] != '\0') {
        int ok = 1;
        for (int i = 1; i < count; i++)
            if (t->comp_names[i][lcp] != t->comp_names[0][lcp]) { ok = 0; break; }
        if (!ok) break;
        lcp++;
    }

    if (lcp > baselen) {
        char text[MAX_LINE];
        snprintf(text, sizeof(text), "%s%.*s", t->comp_prefix, lcp, t->comp_names[0]);
        splice_word(t, t->comp_start, t->comp_end, text);
        t->comp_end = t->comp_start + (int)strlen(text);
        /* if the extension now names a candidate exactly, we are done */
        for (int i = 0; i < count; i++)
            if ((int)strlen(t->comp_names[i]) == lcp) {
                t->comp_mode = 0;
                t->comp_count = 0;
                t->in_sel_active = 0;
                return;
            }
    }

    /* still ambiguous: keep a numbered list to draw just below the input
     * line and wait for a digit to pick. */
    int n = 0, shown = count < 9 ? count : 9;
    for (int i = 0; i < shown; i++)
        n += snprintf(t->comp_list + n, sizeof(t->comp_list) - (size_t)n,
                      "%s%d. %s%s", i ? " " : "", i + 1, t->comp_names[i],
                      t->comp_isdir[i] ? "/" : "");
    t->comp_list_len = n;
    t->comp_mode = 1;
    t->in_sel_active = 0;
}

static void history_prev(Tab *t) {
    history_lock();
    if (g_history_count == 0) { history_unlock(); return; }
    if (t->hist_pos == -1) {
        snprintf(t->hist_save, sizeof(t->hist_save), "%s", t->input_line);
        t->hist_pos = g_history_count - 1;
    } else if (t->hist_pos > 0) {
        t->hist_pos--;
    } else {
        history_unlock();
        return;
    }
    snprintf(t->input_line, sizeof(t->input_line), "%s", g_history[t->hist_pos]);
    history_unlock();
    t->input_len = (int)strlen(t->input_line);
    t->cursor = t->input_len;
    t->sel_active = 0;
}

static void history_next(Tab *t) {
    if (t->hist_pos == -1) return;
    history_lock();
    if (t->hist_pos < g_history_count - 1) {
        t->hist_pos++;
        snprintf(t->input_line, sizeof(t->input_line), "%s", g_history[t->hist_pos]);
    } else {
        t->hist_pos = -1;
        snprintf(t->input_line, sizeof(t->input_line), "%s", t->hist_save);
    }
    history_unlock();
    t->input_len = (int)strlen(t->input_line);
    t->cursor = t->input_len;
    t->sel_active = 0;
}

/* delete from the cursor back to the start of the line (Ctrl+U) */
static void kill_to_start(Tab *t) {
    if (t->cursor <= 0) return;
    memmove(t->input_line, t->input_line + t->cursor,
            (size_t)(t->input_len - t->cursor + 1));
    t->input_len -= t->cursor;
    t->cursor = 0;
    t->sel_active = 0;
}

/* delete from the cursor to the end of the line (Ctrl+K) */
static void kill_to_end(Tab *t) {
    t->input_line[t->cursor] = '\0';
    t->input_len = t->cursor;
    t->sel_active = 0;
}

/* delete the word before the cursor (Ctrl+W) */
static void kill_word(Tab *t) {
    int i = t->cursor;
    while (i > 0 && isspace((unsigned char)t->input_line[i - 1])) i--;
    while (i > 0 && !isspace((unsigned char)t->input_line[i - 1])) i--;
    memmove(t->input_line + i, t->input_line + t->cursor,
            (size_t)(t->input_len - t->cursor + 1));
    t->input_len -= (t->cursor - i);
    t->cursor = i;
    t->sel_active = 0;
}

/* insert pasted text at the cursor (single-line: newlines become spaces) */
static void insert_text(Tab *t, const char *data, int len) {
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c == '\n' || c == '\r') c = ' ';
        if (c < 32) continue;
        if (t->input_len >= MAX_LINE - 1) break;
        memmove(t->input_line + t->cursor + 1, t->input_line + t->cursor,
                (size_t)(t->input_len - t->cursor + 1));
        t->input_line[t->cursor++] = (char)c;
        t->input_len++;
    }
    t->sel_active = 0;
}

static void do_copy(Display *dpy, Window win, Tab *t);
static void do_paste(Display *dpy, Window win, Tab *t);

static int perform_action(Tab *t, Action a, Display *dpy, Window win) {
    /* Any action other than copy ends a keyboard selection: only
     * Shift+arrows (which never reach here) keep it alive. */
    if (a != ACT_COPY) t->in_sel_active = 0;
    switch (a) {
    case ACT_LEFT:          if (t->cursor > 0) t->cursor--; break;
    case ACT_RIGHT:         if (t->cursor < t->input_len) t->cursor++; break;
    case ACT_LINE_START:    t->cursor = 0; break;
    case ACT_LINE_END:      t->cursor = t->input_len; break;
    case ACT_HIST_UP:       history_prev(t); break;
    case ACT_HIST_DOWN:     history_next(t); break;
    case ACT_KILL_TO_START: kill_to_start(t); break;
    case ACT_KILL_TO_END:   kill_to_end(t); break;
    case ACT_KILL_WORD:     kill_word(t); break;
    case ACT_CLEAR_SCREEN:
        t->output_len = 0;
        t->output_buffer[0] = '\0';
        t->scroll = 0;
        break;
    case ACT_EXIT:
        if (t->input_len == 0) {
            if (num_tabs > 1) close_tab(current_tab);
            else              g_exit_requested = 1;
        }
        break;
    case ACT_COPY:
        do_copy(dpy, win, t);
        break;
    case ACT_PASTE:
        do_paste(dpy, win, t);
        return 0;                    /* repaint happens on SelectionNotify */
    default:
        break;
    }
    return 1;
}

/* ================================================================= */

/* Returns 1 if the buffer needs more input (unclosed quote or trailing \),
 * 0 if complete. */
static int input_incomplete(const char *s, int len) {
    /* strip trailing newline for the trailing-backslash check */
    int real = len;
    if (real > 0 && s[real-1] == '\n') real--;

    int i = 0, in_sq = 0, in_dq = 0, par = 0, tick = 0;
    while (i < len) {
        char c = s[i];

        if (in_sq) {
            if (c == '\'') in_sq = 0;
            i++; continue;
        }
        if (c == '\\') {
            if (i + 1 >= len) return 1;
            i += 2; continue;
        }
        if (in_dq) {
            if (c == '"') in_dq = 0;
            else if (c == '$' && s[i+1] == '(') { par++; i += 2; continue; }
            else if (c == ')' && par > 0) par--;
            else if (c == '`') tick = !tick;
            i++; continue;
        }
        if (c == '\'') { in_sq = 1; i++; continue; }
        if (c == '"')  { in_dq = 1; i++; continue; }
        if (c == '$' && s[i+1] == '(') { par++; i += 2; continue; }
        if (c == ')' && par > 0) par--;
        else if (c == '`') tick = !tick;
        i++;
    }
    if (in_sq || in_dq || par > 0 || tick) return 1;

    /* trailing backslash (before the added \n) means continue */
    if (real > 0 && s[real-1] == '\\') return 1;

    return 0;
}




static int parse_multiwatch_cmds(const char *line, char cmds[][MAX_LINE], int maxn) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "multiWatch", 10) != 0) return -1;
    p += 10;
    if (*p && *p != ' ' && *p != '\t' && *p != '[') return -1;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '[') return -1;
    p++;

    int n = 0;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',') p++;
        if (*p == ']') break;
        if (*p != '"') return -1;
        p++;
        if (n >= maxn) return -1;
        int i = 0;
        while (*p && *p != '"') {
            if (i < MAX_LINE - 1) cmds[n][i++] = *p;
            p++;
        }
        if (*p != '"') return -1;
        p++;
        cmds[n][i] = '\0';
        if (cmds[n][0]) n++;
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (*p == ',') { p++; continue; }
        if (*p == ']') break;
        if (*p) return -1;
    }
    if (*p != ']') return -1;
    return n;
}

static void print_watch_block(Tab *tab, WatchCmd *w) {
    char hdr[MAX_LINE + 64];
    snprintf(hdr, sizeof(hdr), "\"%s\", %ld:\n", w->cmd, (long)time(NULL));
    append_output(tab, hdr);
    append_output(tab, WATCH_SEP "\n");
    if (w->out_len > 0) {
        append_output(tab, w->out_buf);
        if (w->out_buf[w->out_len - 1] != '\n')
            append_output(tab, "\n");
    }
    append_output(tab, WATCH_SEP "\n");
}

static int start_watch_cmd(Tab *tab, int idx) {
    WatchCmd *w = &tab->watches[idx];
    int pfd[2];
    if (pipe(pfd) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        setpgid(0, 0);
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT,  SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        if (chdir(tab->cwd) < 0) _exit(126);
        // --- replace the execl("/bin/sh",...) with your own parse+exec ---
        char storage[MAX_STORAGE];
        Command cmd;
        if (parse_command(w->cmd, &cmd, storage, sizeof(storage)) < 0 ||
            cmd.argv[0] == NULL)
            _exit(127);
        execvp(cmd.argv[0], cmd.argv);
        _exit(127);
    }
    setpgid(pid, pid);
    close(pfd[1]);
    int flags = fcntl(pfd[0], F_GETFL, 0);
    if (flags >= 0) fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);
    w->pipe_fd = pfd[0];
    w->pid = pid;
    w->running = 1;
    w->next_run = 0;
    w->out_len = 0;
    w->out_buf[0] = '\0';
    return 0;
}

static void stop_multiwatch(Tab *tab, int show_cc) {
    for (int i = 0; i < tab->nwatch; i++) {
        WatchCmd *w = &tab->watches[i];
        if (w->pid > 0) {
            kill(-w->pid, SIGINT);
            kill(-w->pid, SIGKILL);
            waitpid(w->pid, NULL, 0);
            w->pid = 0;
        }
        if (w->pipe_fd >= 0) {
            close(w->pipe_fd);
            w->pipe_fd = -1;
        }
        w->running = 0;
        w->next_run = 0;
        w->out_len = 0;
    }
    tab->watching = 0;
    tab->nwatch = 0;
    if (show_cc) append_output(tab, "^C\n");
}

static void finish_watch_cmd(Tab *tab, WatchCmd *w) {
    if (w->pipe_fd >= 0) {
        close(w->pipe_fd);
        w->pipe_fd = -1;
    }
    if (w->pid > 0) {
        int status;
        waitpid(w->pid, &status, 0);
        w->pid = 0;
    }
    print_watch_block(tab, w);
    w->running = 0;
    w->out_len = 0;
    w->out_buf[0] = '\0';
    w->next_run = time(NULL) + WATCH_INTERVAL;
}

static int drain_watch_cmd(Tab *tab, WatchCmd *w) {
    char buf[512];
    int got = 0;
    for (;;) {
        ssize_t n = read(w->pipe_fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            got = 1;
            if (w->out_len + (int)n < WATCH_OUT - 1) {
                memcpy(w->out_buf + w->out_len, buf, (size_t)n);
                w->out_len += (int)n;
                w->out_buf[w->out_len] = '\0';
            } else if (w->out_len < WATCH_OUT - 1) {
                int room = WATCH_OUT - 1 - w->out_len;
                memcpy(w->out_buf + w->out_len, buf, (size_t)room);
                w->out_len += room;
                w->out_buf[w->out_len] = '\0';
            }
            continue;
        }
        if (n == 0) {
            finish_watch_cmd(tab, w);
            return 1;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        finish_watch_cmd(tab, w);
        return 1;
    }
    return got;
}

static void restart_due_watches(Tab *tab) {
    if (!tab->watching) return;
    time_t now = time(NULL);
    for (int i = 0; i < tab->nwatch; i++) {
        WatchCmd *w = &tab->watches[i];
        if (!w->running && w->next_run != 0 && now >= w->next_run)
            start_watch_cmd(tab, i);
    }
}

static int start_multiwatch(Tab *tab, const char *line) {
    char cmds[MAX_WATCH][MAX_LINE];
    int n = parse_multiwatch_cmds(line, cmds, MAX_WATCH);
    if (n < 0) {
        append_output(tab, "terminator: multiWatch: syntax error, expected multiWatch [\"cmd1\", \"cmd2\", ...]\n");
        tab->last_exit_status = 1;
        return -1;
    }
    if (n == 0) {
        append_output(tab, "terminator: multiWatch: no commands\n");
        tab->last_exit_status = 1;
        return -1;
    }
    if (tab->watching) {
        append_output(tab, "terminator: multiWatch: already running (Ctrl+C to stop)\n");
        return -1;
    }
    if (foreground_job(tab) != NULL) {
        append_output(tab, "terminator: a command is already running in this tab\n");
        return -1;
    }

    tab->nwatch = n;
    tab->watching = 1;
    for (int i = 0; i < n; i++) {
        strncpy(tab->watches[i].cmd, cmds[i], MAX_LINE - 1);
        tab->watches[i].cmd[MAX_LINE - 1] = '\0';
        tab->watches[i].pipe_fd = -1;
        tab->watches[i].pid = 0;
        tab->watches[i].running = 0;
        tab->watches[i].next_run = 0;
        tab->watches[i].out_len = 0;
        if (start_watch_cmd(tab, i) < 0) {
            append_output(tab, "terminator: multiWatch: failed to start a command\n");
            stop_multiwatch(tab, 0);
            tab->last_exit_status = 1;
            return -1;
        }
    }
    tab->last_exit_status = 0;
    return 0;
}

/* `bind` built-in: list bindings or set one and persist it. */
static void handle_bind(Tab *tab, char **args, int argc) {
    if (argc == 1) {
        append_output(tab, "Key bindings (* = custom):\n");
        for (int i = 0; i < num_bindings; i++) {
            char kb[64], line[160];
            format_key(bindings[i].keysym, bindings[i].mods, kb, sizeof(kb));
            snprintf(line, sizeof(line), "  %c %-16s %s\n",
                     bindings[i].user ? '*' : ' ', kb,
                     action_name(bindings[i].action));
            append_output(tab, line);
        }
        append_output(tab, "Usage: bind <key> <action>  (e.g. bind Ctrl+Y clear-screen)\n");
        tab->last_exit_status = 0;
        return;
    }
    if (argc < 3) {
        append_output(tab, "terminator: bind: usage: bind <key> <action>\n");
        tab->last_exit_status = 1;
        return;
    }

    unsigned int mods = 0;
    KeySym ks = parse_key_spec(args[1], &mods);
    if (ks == NoSymbol) {
        char err[256];
        snprintf(err, sizeof(err), "terminator: bind: unknown key '%s'\n", args[1]);
        append_output(tab, err);
        tab->last_exit_status = 1;
        return;
    }
    Action a = action_from_name(args[2]);
    if (a == ACT_NONE) {
        char err[256];
        snprintf(err, sizeof(err), "terminator: bind: unknown action '%s'\n", args[2]);
        append_output(tab, err);
        tab->last_exit_status = 1;
        return;
    }

    set_binding(ks, mods, a, 1);
    rewrite_config();

    char kb[64], msg[256];
    format_key(ks, mods, kb, sizeof(kb));
    snprintf(msg, sizeof(msg),
             "terminator: bound %s to %s (saved to ~/.terminatorrc)\n",
             kb, action_name(a));
    append_output(tab, msg);
    tab->last_exit_status = 0;
}

/* ===================== jobs / fg / bg / source ===================== */

void execute_command(Tab *tab);

/* Resolve a job spec: %n, %+, %-, or a command prefix.  An empty spec
 * means "the current job".  Returns a slot index, or -1 if not found. */
static int job_lookup(Tab *t, const char *spec) {
    if (!spec || !*spec || strcmp(spec, "%") == 0 ||
        strcmp(spec, "%+") == 0 || strcmp(spec, "+") == 0)
        return t->cur_job;

    const char *p = spec;
    if (*p == '%') p++;

    if (*p == '-') {
        for (int i = MAX_JOBS - 1; i >= 0; i--)
            if (t->jobs[i].used && i != t->cur_job) return i;
        return -1;
    }
    if (isdigit((unsigned char)*p)) {
        int n = atoi(p);
        if (n >= 1 && n <= MAX_JOBS && t->jobs[n - 1].used) return n - 1;
        return -1;
    }
    for (int i = 0; i < MAX_JOBS; i++)
        if (t->jobs[i].used && strncmp(t->jobs[i].cmd, p, strlen(p)) == 0)
            return i;
    return -1;
}

static void builtin_jobs(Tab *t) {
    for (int i = 0; i < MAX_JOBS; i++) {
        Job *j = &t->jobs[i];
        if (!j->used) continue;
        char buf[MAX_LINE + 48];
        snprintf(buf, sizeof(buf), "[%d]%c  %-8s %s%s\n",
                 i + 1, (i == t->cur_job) ? '+' : ' ',
                 j->stopped ? "Stopped" : "Running",
                 j->cmd, (j->background && !j->stopped) ? " &" : "");
        append_output(t, buf);
    }
    t->last_exit_status = 0;
}

static void builtin_bg(Tab *t, const char *spec) {
    int idx = job_lookup(t, spec);
    if (idx < 0) {
        append_output(t, "bg: no such job\n");
        t->last_exit_status = 1;
        return;
    }
    Job *j = &t->jobs[idx];
    if (j->stopped) {
        j->stopped = 0;
        kill(-j->pgid, SIGCONT);
    }
    j->background = 1;
    if (t->fg_job == idx) t->fg_job = -1;
    t->cur_job = idx;
    job_print_bg(t, idx, j->cmd);
    t->last_exit_status = 0;
}

static void builtin_fg(Tab *t, const char *spec) {
    int idx = job_lookup(t, spec);
    if (idx < 0) {
        append_output(t, "fg: no such job\n");
        t->last_exit_status = 1;
        return;
    }
    Job *j = &t->jobs[idx];
    j->background = 0;
    t->cur_job = idx;
    t->fg_job  = idx;

    char buf[MAX_LINE + 8];
    snprintf(buf, sizeof(buf), "%s\n", j->cmd);
    append_output(t, buf);              /* echo the resumed command */

    if (j->stopped) {
        j->stopped = 0;
        kill(-j->pgid, SIGCONT);
    }
}

static __thread int source_depth = 0;

static void builtin_source(Tab *t, const char *path) {
    if (!path) {
        append_output(t, "source: filename argument required\n");
        t->last_exit_status = 1;
        return;
    }
    /* Copy the name: args[] points into storage that the nested
     * execute_command() calls will overwrite. */
    char name[MAX_LINE];
    snprintf(name, sizeof(name), "%s", path);

    if (source_depth > 8) {
        append_output(t, "source: too many nested calls\n");
        t->last_exit_status = 1;
        return;
    }
    char full[PATH_MAX + MAX_LINE];
    FILE *fp = fopen(tab_path(t, name, full, sizeof(full)), "r");
    if (!fp) {
        char err[MAX_LINE + 64];
        snprintf(err, sizeof(err),
                 "terminator: source: %s: No such file or directory\n", name);
        append_output(t, err);
        t->last_exit_status = 1;
        return;
    }

    source_depth++;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0' || *s == '\n' || *s == '#') continue;
        int len = (int)strlen(s);
        while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
            s[--len] = '\0';
        snprintf(t->input_line, MAX_LINE, "%s", s);
        t->input_len = (int)strlen(t->input_line);
        t->cursor   = t->input_len;
        execute_command(t);
    }
    fclose(fp);
    source_depth--;
    t->last_exit_status = 0;
}

/* ===================== expansion: $VAR, $(...), `...` =====================
 * A light version of the shell's expansion pass.  It runs on the raw
 * command line before it is split into a pipeline, so a substitution may
 * itself contain pipes and newlines.
 *
 *   - single quotes are literal,
 *   - double quotes still expand $VAR and command substitutions,
 *   - $(...) and `...` run through /bin/sh in the tab's own directory and
 *     their stdout replaces the text; because the body is expanded first,
 *     the innermost substitution runs first.
 */

#define EXPAND_DEPTH_MAX 16

static void ex_put(char *out, int *o, int outsz, char c) {
    if (*o < outsz - 1) out[(*o)++] = c;
}
static void ex_puts(char *out, int *o, int outsz, const char *s, int n) {
    for (int i = 0; i < n; i++) ex_put(out, o, outsz, s[i]);
}

/* Index of the ')' matching the '(' at open_idx, or -1. */
static int match_paren(const char *s, int open_idx) {
    int depth = 0, in_sq = 0, in_dq = 0;
    for (int i = open_idx; s[i]; i++) {
        char c = s[i];
        if (in_sq) { if (c == '\'') in_sq = 0; continue; }
        if (c == '\\' && s[i+1]) { i++; continue; }
        if (in_dq) {
            if (c == '"') in_dq = 0;
            else if (c == '\\' && s[i+1]) i++;
            continue;
        }
        if (c == '\'') { in_sq = 1; continue; }
        if (c == '"')  { in_dq = 1; continue; }
        if (c == '(') depth++;
        else if (c == ')') { if (--depth == 0) return i; }
    }
    return -1;
}

/* Index of the backtick matching the one at open_idx, or -1. */
static int match_backtick(const char *s, int open_idx) {
    for (int i = open_idx + 1; s[i]; i++) {
        if (s[i] == '\\' && s[i+1]) { i++; continue; }
        if (s[i] == '`') return i;
    }
    return -1;
}

/* Run `cmd` through /bin/sh with the tab's cwd and capture its stdout.
 * Trailing newlines are stripped, as every shell does. */
static int capture_substitution(Tab *tab, const char *cmd, char *out, int outsz) {
    out[0] = '\0';
    int p[2];
    if (pipe(p) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (pid == 0) {
        close(p[0]);
        if (p[1] != STDOUT_FILENO) { dup2(p[1], STDOUT_FILENO); close(p[1]); }
        if (tab && tab->cwd[0]) chdir(tab->cwd);
        signal(SIGINT,  SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(p[1]);

    int total = 0;
    char buf[4096];
    for (;;) {
        ssize_t r = read(p[0], buf, sizeof(buf));
        if (r > 0) {
            int room = outsz - 1 - total;
            if (room > 0) {
                int cpy = (r < room) ? (int)r : room;
                memcpy(out + total, buf, (size_t)cpy);
                total += cpy;
            }
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        break;
    }
    out[total] = '\0';
    close(p[0]);

    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }

    while (total > 0 && (out[total-1] == '\n' || out[total-1] == '\r'))
        out[--total] = '\0';
    return total;
}

static void expand_scan(Tab *tab, const char *s, char *out, int *o, int outsz, int depth);

/* Substitute one $(...) / `...`: expand the body first (so the innermost
 * substitution runs first), then run it through the shell. */
static void expand_substitution(Tab *tab, const char *body, int blen,
                                char *out, int *o, int outsz, int depth) {
    char *raw      = malloc(MAX_OUTPUT);
    char *expanded = malloc(MAX_OUTPUT);
    char *sub      = malloc(MAX_OUTPUT);
    if (!raw || !expanded || !sub) { free(raw); free(expanded); free(sub); return; }

    int io = 0;
    for (int i = 0; i < blen; i++) ex_put(raw, &io, MAX_OUTPUT, body[i]);
    raw[io] = '\0';

    int eo = 0;
    expand_scan(tab, raw, expanded, &eo, MAX_OUTPUT, depth + 1);
    expanded[eo] = '\0';

    capture_substitution(tab, expanded, sub, MAX_OUTPUT);
    ex_puts(out, o, outsz, sub, (int)strlen(sub));

    free(raw); free(expanded); free(sub);
}

static void expand_scan(Tab *tab, const char *s, char *out, int *o, int outsz, int depth) {
    int in_sq = 0, in_dq = 0;

    for (int i = 0; s[i]; ) {
        char c = s[i];

        if (in_sq) {
            ex_put(out, o, outsz, c);
            if (c == '\'') in_sq = 0;
            i++;
            continue;
        }
        if (c == '\\' && s[i+1]) {
            /* keep escapes for the quoting pass, which resolves them */
            ex_put(out, o, outsz, c);
            ex_put(out, o, outsz, s[i+1]);
            i += 2;
            continue;
        }
        if (c == '\'' && !in_dq) { in_sq = 1; ex_put(out, o, outsz, c); i++; continue; }
        if (c == '"') { in_dq = !in_dq; ex_put(out, o, outsz, c); i++; continue; }

        if (c == '$' && s[i+1] == '(' && depth < EXPAND_DEPTH_MAX) {
            int close = match_paren(s, i + 1);
            if (close < 0) { ex_put(out, o, outsz, c); i++; continue; }
            expand_substitution(tab, s + i + 2, close - (i + 2), out, o, outsz, depth);
            i = close + 1;
            continue;
        }
        if (c == '`' && depth < EXPAND_DEPTH_MAX) {
            int close = match_backtick(s, i);
            if (close < 0) { ex_put(out, o, outsz, c); i++; continue; }
            expand_substitution(tab, s + i + 1, close - (i + 1), out, o, outsz, depth);
            i = close + 1;
            continue;
        }
        if (c == '$') {
            char num[32];
            if (s[i+1] == '?') {
                int n = snprintf(num, sizeof(num), "%d", tab ? tab->last_exit_status : 0);
                ex_puts(out, o, outsz, num, n);
                i += 2; continue;
            }
            if (s[i+1] == '$') {
                int n = snprintf(num, sizeof(num), "%d", (int)getpid());
                ex_puts(out, o, outsz, num, n);
                i += 2; continue;
            }
            if (s[i+1] == '{') {
                int j = i + 2;
                char name[256]; int k = 0;
                while (s[j] && s[j] != '}' && k < (int)sizeof(name) - 1) name[k++] = s[j++];
                name[k] = '\0';
                if (s[j] == '}') j++;
                const char *v = getenv(name);
                if (v) ex_puts(out, o, outsz, v, (int)strlen(v));
                i = j; continue;
            }
            if (isalpha((unsigned char)s[i+1]) || s[i+1] == '_') {
                int j = i + 1;
                char name[256]; int k = 0;
                while ((isalnum((unsigned char)s[j]) || s[j] == '_') &&
                       k < (int)sizeof(name) - 1) name[k++] = s[j++];
                name[k] = '\0';
                const char *v = getenv(name);
                if (v) ex_puts(out, o, outsz, v, (int)strlen(v));
                i = j; continue;
            }
            ex_put(out, o, outsz, c);
            i++;
            continue;
        }
        ex_put(out, o, outsz, c);
        i++;
    }
}

/* Expand `src` into `out`; returns the number of bytes written. */
static int expand_line(Tab *tab, const char *src, char *out, int outsz) {
    int o = 0;
    expand_scan(tab, src, out, &o, outsz, 0);
    out[o] = '\0';
    return o;
}

void execute_command(Tab *tab) {
    if (tab->input_len == 0) return;

    /* Record in the shared, file-backed history so every tab sees it. */
    if (tab->input_line[0]) {
        history_lock();
        history_store(tab->input_line);
        history_append_file(tab->input_line);
        history_unlock();
    }

    /* Echo the prompt and the command exactly as typed (multi-line aware). */
    {
        char *echo = strdup(tab->input_line);
        if (echo) {
            int first = 1;
            for (char *line = strtok(echo, "\n"); line;
                 line = strtok(NULL, "\n")) {
                char buf[MAX_LINE + 64];
                snprintf(buf, sizeof(buf), "%s%s\n",
                         first ? "user@terminator> " : "> ", line);
                append_output(tab, buf);
                first = 0;
            }
            free(echo);
        }
    }

    /* Expand $VAR, ${VAR}, $?, $$, $(...) and `...` into a private copy.
     * The raw line stays untouched for the echo above and the history. */
    char *expanded = malloc(MAX_OUTPUT);
    if (!expanded) {
        append_output(tab, "terminator: out of memory\n");
        tab->input_len = 0;
        tab->input_line[0] = '\0';
        tab->cursor = 0;
        return;
    }
    expand_line(tab, tab->input_line, expanded, MAX_OUTPUT);

    /* A trailing '&' starts the command as a background job. */
    int background = 0;
    {
        int e = (int)strlen(expanded);
        while (e > 0 && (expanded[e-1] == ' ' || expanded[e-1] == '\t')) e--;
        if (e > 0 && expanded[e-1] == '&' &&
            (e == 1 || expanded[e-2] != '&')) {
            background = 1;
            expanded[--e] = '\0';
            while (e > 0 && (expanded[e-1] == ' ' || expanded[e-1] == '\t'))
                expanded[--e] = '\0';
        }
    }

    /* Parse the expanded line into argv.  Both buffers live on the heap so
     * this frame stays small: the session thread's stack must still have
     * room for execute_pipeline(), which forks a whole pipeline. */
    char *args[MAX_ARGS];
    char *storage = malloc(MAX_LINE * 64);
    if (!storage) {
        append_output(tab, "terminator: out of memory\n");
        free(expanded);
        tab->input_len = 0;
        tab->input_line[0] = '\0';
        tab->cursor = 0;
        return;
    }
    int argc = shell_split(expanded, args, MAX_ARGS, storage, MAX_LINE * 64);

    if (argc == 0) {
        free(storage);
        free(expanded);
        tab->input_len = 0;
        tab->input_line[0] = '\0';
        tab->cursor = 0;
        return;
    }

    // Handle Built-ins
    if (strcmp(args[0], "exit") == 0) {
        if (g_sess) {
            /* inside a tab session: close just this tab, don't kill the GUI */
            g_sess->user_exit = 1;
            g_sess->quitting  = 1;
        } else {
            exit(0);
        }
    } else if (strcmp(args[0], "cd") == 0) {
        /* Resolve against THIS tab's directory, never the process's, so a
         * `cd` in one tab cannot move another tab (matters in thread mode,
         * where every tab shares the process cwd). */
        char *target = args[1];
        char  path[PATH_MAX];
        char  resolved[PATH_MAX];

        if (!target || !*target || strcmp(target, "~") == 0) {
            const char *home = getenv("HOME");
            snprintf(path, sizeof(path), "%s", (home && *home) ? home : "/");
        } else if (target[0] == '~' && target[1] == '/') {
            const char *home = getenv("HOME");
            if (!home || !*home) home = "/";
            snprintf(path, sizeof(path), "%s%s", home, target + 1);
        } else if (target[0] == '/') {
            snprintf(path, sizeof(path), "%s", target);
        } else {
            snprintf(path, sizeof(path), "%s/%s", tab->cwd, target);
        }

        /* Update ONLY this tab's directory.  Deliberately no process-wide
         * chdir(): in thread mode that would move every other tab too.
         * Relative paths for commands, globs and redirections are resolved
         * against tab->cwd instead of the process cwd. */
        struct stat st;
        if (realpath(path, resolved) && stat(resolved, &st) == 0 &&
            S_ISDIR(st.st_mode)) {
            snprintf(tab->cwd, sizeof(tab->cwd), "%s", resolved);
            update_tab_title(tab);
            tab->last_exit_status = 0;
        } else {
            char err[256];
            snprintf(err, sizeof(err), "terminator: cd: %s: No such file or directory\n", target);
            append_output(tab, err);
            tab->last_exit_status = 1;
        }
    } else if (strcmp(args[0], "history") == 0) {
        /* most recent HISTORY_SHOW_MAX commands from the shared history */
        history_lock();
        int shown = g_history_count < HISTORY_SHOW_MAX ? g_history_count
                                                       : HISTORY_SHOW_MAX;
        int start = g_history_count - shown;
        for (int i = g_history_count-1; i >=start; i--) {
            char hist_buf[MAX_LINE + 24];
            snprintf(hist_buf, sizeof(hist_buf), "%5ld  %s\n",
                     g_history_base + i + 1, g_history[i]);
            append_output(tab, hist_buf);
        }
        history_unlock();
        tab->last_exit_status = 0;
    } else if (strcmp(args[0], "multiWatch") == 0) {
        start_multiwatch(tab, tab->input_line);
    } else if (strcmp(args[0], "bind") == 0) {
        /* key bindings live in the GUI; a session hands the command back */
        if (g_sess) session_emit_line(g_sess, F_DELEGATE, tab->input_line);
        else        handle_bind(tab, args, argc);
    } else if (strcmp(args[0], "jobs") == 0) {
        builtin_jobs(tab);
    } else if (strcmp(args[0], "fg") == 0) {
        builtin_fg(tab, args[1]);
    } else if (strcmp(args[0], "bg") == 0) {
        builtin_bg(tab, args[1]);
    } else if (strcmp(args[0], "source") == 0 || strcmp(args[0], ".") == 0) {
        builtin_source(tab, args[1]);
    } else {
        int status = execute_pipeline(tab, expanded, background);
        if (status != 0) tab->last_exit_status = status;
    }

    free(storage);
    free(expanded);

    tab->scroll = 0;
    tab->input_len = 0;
    tab->input_line[0] = '\0';
    tab->cursor = 0;
    tab->hist_pos = -1;
}

static int char_cols(void) {
    int cols = (win_w - 2 * PAD_X) / CHAR_W;
    if (cols < 8) cols = 8;
    if (cols > MAX_LINE - 1) cols = MAX_LINE - 1;
    return cols;
}

static int usable_rows(void) {
    int h = win_h - TEXT_TOP;
    int rows = h / LINE_H;
    return rows < 1 ? 1 : rows;
}

/* One visual line: wrap at `cols`, treat '\n' as a hard break. */
static int next_display_line(const char *buf, int len, int *i, int cols, char *out) {
    if (*i >= len) return 0;
    int n = 0;
    while (*i < len && n < cols) {
        char c = buf[*i];
        if (c == '\r') { (*i)++; continue; }
        if (c == '\n') { (*i)++; break; }
        out[n++] = c;
        (*i)++;
    }
    out[n] = '\0';
    if (n == cols && *i < len && buf[*i] == '\n')
        (*i)++;
    return 1;
}

static int count_wrapped(const char *buf, int len, int cols) {
    char line[MAX_LINE];
    int i = 0, n = 0;
    while (next_display_line(buf, len, &i, cols, line)) n++;
    return n;
}

static int tab_display_lines(Tab *t, int cols) {
    int n = count_wrapped(t->output_buffer, t->output_len, cols);
    if (t->multiline && t->multiline_len > 0)
        n += count_wrapped(t->multiline_buf, t->multiline_len, cols);
    return n;
}

static void clamp_scroll(Tab *t) {
    int cols = char_cols();
    int rows = usable_rows();
    int total = tab_display_lines(t, cols);
    int maxs = total - rows;
    if (maxs < 0) maxs = 0;
    if (t->scroll > maxs) t->scroll = maxs;
    if (t->scroll < 0) t->scroll = 0;
}

/* ===================== X11 selection: copy / paste =====================
 * The copying program keeps the text and only hands it over when another
 * program asks (SelectionRequest), driven through the X server.          */

static char *clip_text = NULL;
static int   clip_len  = 0;

/* text of absolute wrapped-display-line `want` (0-based) */
static int display_line_text(Tab *t, int cols, int want, char *out, int outsz) {
    char line[MAX_LINE];
    int i = 0, idx = 0;
    while (next_display_line(t->output_buffer, t->output_len, &i, cols, line)) {
        if (idx == want) { snprintf(out, outsz, "%s", line); return 1; }
        idx++;
    }
    if (t->multiline && t->multiline_len > 0) {
        i = 0;
        while (next_display_line(t->multiline_buf, t->multiline_len, &i, cols, line)) {
            if (idx == want) { snprintf(out, outsz, "%s", line); return 1; }
            idx++;
        }
    }
    return 0;
}

static void selection_ordered(Tab *t, int *l1, int *c1, int *l2, int *c2) {
    int la = t->sel_anchor_line, ca = t->sel_anchor_col;
    int lb = t->sel_line,        cb = t->sel_col;
    if (la > lb || (la == lb && ca > cb)) {
        int tmp;
        tmp = la; la = lb; lb = tmp;
        tmp = ca; ca = cb; cb = tmp;
    }
    *l1 = la; *c1 = ca; *l2 = lb; *c2 = cb;
}

static char *extract_selection(Tab *t, int *out_len) {
    *out_len = 0;
    if (!t->sel_active) return NULL;

    int cols = char_cols();
    int la, ca, lb, cb;
    selection_ordered(t, &la, &ca, &lb, &cb);

    size_t cap = 256, n = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    for (int L = la; L <= lb; L++) {
        char text[MAX_LINE];
        if (!display_line_text(t, cols, L, text, sizeof(text))) break;
        int len  = (int)strlen(text);
        int from = (L == la) ? ca : 0;
        int to   = (L == lb) ? cb : len;
        if (from < 0) from = 0;
        if (to > len) to = len;
        if (to < from) to = from;

        size_t need = (size_t)(to - from) + 2;
        if (n + need > cap) {
            cap = (n + need) * 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        memcpy(buf + n, text + from, (size_t)(to - from));
        n += (size_t)(to - from);
        if (L < lb) buf[n++] = '\n';
    }
    buf[n] = '\0';
    *out_len = (int)n;
    return buf;
}

/* claim CLIPBOARD and remember the text for later SelectionRequests */
static void do_copy(Display *dpy, Window win, Tab *t) {
    int len = 0;
    char *text = NULL;

    /* a Shift+arrow selection over the input line takes priority */
    if (t->in_sel_active) {
        int a = t->in_sel_anchor, b = t->in_sel_end;
        if (a > b) { int tmp = a; a = b; b = tmp; }
        int ilen = (int)strlen(t->input_line);
        if (a < 0)    a = 0;
        if (b > ilen) b = ilen;
        if (b > a) {
            len = b - a;
            text = malloc((size_t)len + 1);
            if (text) { memcpy(text, t->input_line + a, (size_t)len); text[len] = '\0'; }
        }
    }
    /* otherwise fall back to the mouse/output selection */
    if (!text)
        text = extract_selection(t, &len);

    if (!text || len == 0) { free(text); return; }

    if (clip_text) free(clip_text);
    clip_text = text;
    clip_len  = len;

    XSetSelectionOwner(dpy, atom_clipboard, win, CurrentTime);
    if (XGetSelectionOwner(dpy, atom_clipboard) != win)
        append_output(t, "terminator: could not claim the CLIPBOARD selection\n");
}

/* ask the current owner for UTF-8 text (answer arrives as SelectionNotify) */
static void do_paste(Display *dpy, Window win, Tab *t) {
    (void)t;
    if (XGetSelectionOwner(dpy, atom_clipboard) == None) return;
    XConvertSelection(dpy, atom_clipboard, atom_utf8, atom_sel_data, win, CurrentTime);
}

/* map a mouse y / x to selection (display line, column) coordinates */
static int y_to_display_line(Tab *t, int y) {
    int cols = char_cols();
    int rows = usable_rows();
    clamp_scroll(t);
    int total = tab_display_lines(t, cols);
    int end = total - t->scroll;
    if (end < 0) end = 0;
    if (end > total) end = total;
    int start = end - rows;
    if (start < 0) start = 0;
    int line = (y - TEXT_TOP) / LINE_H + start;
    if (line > total - 1) line = total - 1;
    if (line < 0) line = 0;
    return line;
}

static int x_to_col(int x) {
    int c = (x - PAD_X) / CHAR_W;
    if (c < 0) c = 0;
    if (c > MAX_LINE - 1) c = MAX_LINE - 1;
    return c;
}

static void draw_wrapped_range(Tab *t, const char *buf, int len, int cols, int screen,
                               int *index, int start, int end,
                               int *y, Display *display, Window window, GC gc) {
    char line[MAX_LINE];
    int i = 0;
    int la = 0, ca = 0, lb = 0, cb = 0;
    if (t->sel_active) selection_ordered(t, &la, &ca, &lb, &cb);

    while (next_display_line(buf, len, &i, cols, line)) {
        if (*index >= start && *index < end) {
            int n = (int)strlen(line);

            /* Selected span of this display line, if any. */
            int from = -1, to = -1;
            if (t->sel_active && *index >= la && *index <= lb) {
                from = (*index == la) ? ca : 0;
                to   = (*index == lb) ? cb : n;
                if (from < 0) from = 0;
                if (to > n)   to = n;
                if (to <= from) { from = -1; to = -1; }
            }

            if (from >= 0) {
                /* 1. Highlight background: a solid black block behind the text. */
                XSetForeground(display, gc, BlackPixel(display, screen));
                XFillRectangle(display, window, gc,
                               PAD_X + from * CHAR_W, *y - 13,
                               (to - from) * CHAR_W, LINE_H - 2);

                /* 2. Draw text in layers: black, then the selected part in
                 *    white over the block, then black again - inverse video. */
                if (from > 0) {
                    XSetForeground(display, gc, BlackPixel(display, screen));
                    XDrawString(display, window, gc, PAD_X, *y, line, from);
                }
                XSetForeground(display, gc, WhitePixel(display, screen));
                XDrawString(display, window, gc, PAD_X + from * CHAR_W, *y,
                            line + from, to - from);
                if (to < n) {
                    XSetForeground(display, gc, BlackPixel(display, screen));
                    XDrawString(display, window, gc, PAD_X + to * CHAR_W, *y,
                                line + to, n - to);
                }
            } else {
                XSetForeground(display, gc, BlackPixel(display, screen));
                XDrawString(display, window, gc, PAD_X, *y, line, n);
            }
            *y += LINE_H;
        }
        (*index)++;
    }
}

/* Draw the prompt and the input line, wrapping long input onto extra
 * display lines, plus the caret and any keyboard (Shift+arrow) selection. */
static void draw_input(Tab *t, Display *display, Window window, GC gc, int screen,
                       const char *prompt, int plen, int cols, int y0) {
    const char *s = t->input_line;
    int ilen = (int)strlen(s);
    int icur = t->cursor;
    if (icur < 0)    icur = 0;
    if (icur > ilen) icur = ilen;

    /* keyboard selection, clamped to the real input length */
    int hl_from = -1, hl_to = -1;
    if (t->in_sel_active) {
        hl_from = t->in_sel_anchor;
        hl_to   = t->in_sel_end;
        if (hl_from > hl_to) { int tmp = hl_from; hl_from = hl_to; hl_to = tmp; }
        if (hl_from < 0)  hl_from = 0;
        if (hl_to > ilen) hl_to   = ilen;
        if (hl_to <= hl_from) { hl_from = -1; hl_to = -1; }
    }

    /* caret: which display line and column it lands on */
    int caret_line, caret_col;
    {
        int flow = plen + icur;
        if (flow < cols) { caret_line = 0; caret_col = flow; }
        else { int rem = flow - cols; caret_line = 1 + rem / cols; caret_col = rem % cols; }
    }

    int i = 0, first = 1, y = y0;
    for (;;) {
        int avail = cols - (first ? plen : 0);
        if (avail < 1) avail = 1;
        int seg_len = ilen - i;
        if (seg_len > avail) seg_len = avail;
        if (seg_len < 0) seg_len = 0;

        int base_x = PAD_X + (first ? plen : 0) * CHAR_W;

        XSetForeground(display, gc, BlackPixel(display, screen));
        if (first)
            XDrawString(display, window, gc, PAD_X, y, prompt, plen);

        int sf = -1, st = -1;
        if (hl_from >= 0 && seg_len > 0) {
            sf = hl_from - i;
            st = hl_to   - i;
            if (sf < 0)       sf = 0;
            if (st > seg_len) st = seg_len;
            if (st <= sf) { sf = -1; st = -1; }
        }

        if (sf >= 0) {
            /* black block + white selected text (inverse video) */
            XSetForeground(display, gc, BlackPixel(display, screen));
            XFillRectangle(display, window, gc,
                           base_x + sf * CHAR_W, y - 13,
                           (st - sf) * CHAR_W, LINE_H - 2);
            if (sf > 0)
                XDrawString(display, window, gc, base_x, y, s + i, sf);
            XSetForeground(display, gc, WhitePixel(display, screen));
            XDrawString(display, window, gc, base_x + sf * CHAR_W, y,
                        s + i + sf, st - sf);
            if (st < seg_len) {
                XSetForeground(display, gc, BlackPixel(display, screen));
                XDrawString(display, window, gc, base_x + st * CHAR_W, y,
                            s + i + st, seg_len - st);
            }
        } else if (seg_len > 0) {
            XDrawString(display, window, gc, base_x, y, s + i, seg_len);
        }

        i += seg_len;
        first = 0;
        y += LINE_H;
        if (i >= ilen)     break;        /* whole input drawn */
        if (y > win_h - 4) break;        /* no room for more lines */
    }

    /* caret last, so it sits on top of the text */
    int caret_y = y0 + caret_line * LINE_H;
    if (cursor_visible && caret_y <= win_h - 4) {
        XSetForeground(display, gc, BlackPixel(display, screen));
        int cxp = PAD_X + caret_col * CHAR_W;
        XDrawLine(display, window, gc, cxp, caret_y - 12, cxp, caret_y + 2);
    }
}

/* how many display lines draw_input() uses for prompt + input */
static int input_lines_used(int plen, int ilen, int cols) {
    int lines = 0, i = 0, first = 1;
    for (;;) {
        int avail = cols - (first ? plen : 0);
        if (avail < 1) avail = 1;
        int seg = ilen - i;
        if (seg > avail) seg = avail;
        if (seg < 0) seg = 0;
        i += seg;
        lines++;
        first = 0;
        if (i >= ilen) break;
        if (lines > 200) break;      /* safety */
    }
    return lines;
}

void redraw(Display *display, Window window, GC gc, int screen) {
    XClearWindow(display, window);

    // Draw Tab Bar at the top
    int x = 5, y = 20;
    for (int i = 0; i < num_tabs; i++) {
        // Split label around the X so we can place X between title and ']'
    char left[128], right[8];
    snprintf(left,  sizeof(left),  "[ %d: %s ", i + 1, tabs[i].title);
    snprintf(right, sizeof(right), "]");

    int left_w  = strlen(left)  * 7;
    int x_w     = 1 * 7;                 // width of "X"
    int right_w = strlen(right) * 7;
    int label_w = left_w + x_w + right_w;

    int tab_w   = label_w + 10;          // +10 padding after the tab

    // Left part: "[ n: title "
    if (i == current_tab) XSetForeground(display, gc, BlackPixel(display, screen));
    else                  XSetForeground(display, gc, 0x888888);
    XDrawString(display, window, gc, x, y, left, strlen(left));

    // X (clickable close region)
    int cx = x + left_w;
    XSetForeground(display, gc, 0xCC0000);
    XDrawString(display, window, gc, cx, y, "X", 1);

    // Right part: "]"
    if (i == current_tab) XSetForeground(display, gc, BlackPixel(display, screen));
    else                  XSetForeground(display, gc, 0x888888);
    XDrawString(display, window, gc, x + left_w + x_w, y, right, strlen(right));

    x += tab_w;
    }
    // Draw [+] tab button if less than max tabs
    if (num_tabs < MAX_TABS) {
        char *plus = "[ + ]";
        XSetForeground(display, gc, 0x00AA00);
        XDrawString(display, window, gc, x, y, plus, strlen(plus));
    }

    // Draw separator line
    XSetForeground(display, gc, BlackPixel(display, screen));
    XDrawLine(display, window, gc, 10, TABBAR_BOTTOM, win_w - 10, TABBAR_BOTTOM);

    Tab *t = &tabs[current_tab];
    int cols = char_cols();
    int rows = usable_rows();
    clamp_scroll(t);
    int total = tab_display_lines(t, cols);
    int end = total - t->scroll;
    if (end < 0) end = 0;
    if (end > total) end = total;
    int start = end - rows;
    if (start < 0) start = 0;

    int text_y = TEXT_TOP;
    int index = 0;
    draw_wrapped_range(t, t->output_buffer, t->output_len, cols, screen,
                       &index, start, end, &text_y, display, window, gc);
    if (t->multiline && t->multiline_len > 0)
        draw_wrapped_range(t, t->multiline_buf, t->multiline_len, cols, screen,
                           &index, start, end, &text_y, display, window, gc);

    /* Prompt: only shown when following the latest output (scroll == 0)
     * and no foreground job owns the terminal.  While a command runs the
     * prompt stays hidden, exactly like a real shell. */
    if (t->scroll == 0 && !session_busy(t)) {
        const char *prompt = t->search_mode ? "Enter search term: "
                           : t->multiline   ? "> "
                                            : "user@terminator> ";
        int plen = (int)strlen(prompt);
        if (text_y <= win_h - 4) {
            draw_input(t, display, window, gc, screen, prompt, plen, cols, text_y);

            /* Tab-completion menu, drawn just below the input line */
            if (t->comp_mode && t->comp_list_len > 0) {
                int lines = input_lines_used(plen, (int)strlen(t->input_line), cols);
                int ly = text_y + lines * LINE_H;
                int max_lines = (win_h - 4 - ly) / LINE_H;
                if (max_lines > 0) {
                    int li = 0, saved_sel = t->sel_active;
                    t->sel_active = 0;   /* output selection doesn't apply here */
                    draw_wrapped_range(t, t->comp_list, t->comp_list_len, cols,
                                       screen, &li, 0, max_lines, &ly,
                                       display, window, gc);
                    t->sel_active = saved_sel;
                }
            }
        }
    }

    XFlush(display);
}

static void sync_input(Tab *t) {
    int ilen = (int)strlen(t->input_line);
    t->input_len = ilen;
    if (t->cursor < 0)    t->cursor = 0;
    if (t->cursor > ilen) t->cursor = ilen;
}

/* multiline_buf stores every line prefixed with the prompt it was typed
 * at ("user@terminator> " for the first line, "> " for continuations), so
 * the accumulated text can be drawn exactly like a real shell.  Recover
 * the command the user actually typed by dropping one prompt per line. */
static void strip_prompt_prefixes(const char *in, char *out, int outsz) {
    int o = 0;
    const char *p = in;
    while (*p && o < outsz - 1) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        int skip = 0;
        if (len >= 17 && strncmp(p, "user@terminator> ", 17) == 0) skip = 17;
        else if (len >= 2 && p[0] == '>' && p[1] == ' ')            skip = 2;
        for (int i = skip; i < len && o < outsz - 1; i++) out[o++] = p[i];
        if (!nl) break;
        if (o < outsz - 1) out[o++] = '\n';
        p = nl + 1;
    }
    out[o] = '\0';
}

/* Ctrl+C / Ctrl+Z typed at the prompt: show the line just as it was, with
 * the usual ^C / ^Z marker, then leave a fresh prompt (like a shell does). */
static void echo_control_marker(Tab *t, char marker) {
    char buf[2 * MAX_LINE + 32];
    snprintf(buf, sizeof(buf), "user@terminator> %s^%c\n",
             t->input_line, marker);
    append_output(t, buf);
    t->input_len = 0;
    t->input_line[0] = '\0';
    t->cursor = 0;
}

/* ===================== session transport =====================
 * Everything between a tab's GUI half and its execution half travels
 * as length-prefixed frames:   [type:int][len:int][payload:len]
 * The GUI writes C_* frames to the session, the session writes F_*
 * frames back.  A frame never exceeds SES_IOBUF.
 */
static int write_full(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;              /* EPIPE / EAGAIN: abandon this frame */
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static int frame_write(int fd, int type, const void *payload, int len) {
    int hdr[2];
    if (fd < 0) return -1;
    hdr[0] = type;
    hdr[1] = len;
    if (write_full(fd, hdr, sizeof(hdr)) < 0) return -1;
    if (len > 0 && payload && write_full(fd, payload, (size_t)len) < 0) return -1;
    return 0;
}

/* One complete frame in buf?  Fills type/plen and returns 1; returns 0
 * when more bytes are needed; resets the buffer and returns -1 on a
 * corrupt length. */
static int frame_ready(const char *buf, int have, int *type, int *plen) {
    int hdr = (int)(2 * sizeof(int));
    if (have < hdr) return 0;
    memcpy(type, buf, sizeof(int));
    memcpy(plen, buf + sizeof(int), sizeof(int));
    if (*plen < 0 || *plen > SES_IOBUF) return -1;
    if (have < hdr + *plen) return 0;
    return 1;
}

static void session_emit(TabSession *s, const char *data, int len) {
    int off = 0;
    if (!s || len <= 0) return;
    while (off < len) {
        int chunk = len - off;
        if (chunk > SES_CHUNK) chunk = SES_CHUNK;
        if (frame_write(s->sfds[1], F_OUT, data + off, chunk) < 0) return;
        off += chunk;
    }
}

static void session_emit_int(TabSession *s, int type, int value) {
    if (s) frame_write(s->sfds[1], type, &value, (int)sizeof(value));
}

static void session_emit_line(TabSession *s, int type, const char *line) {
    if (s) frame_write(s->sfds[1], type, line, (int)strlen(line));
}

static void session_emit_cwd(TabSession *s, const char *cwd) {
    session_emit_line(s, F_CWD, cwd);
}

static void session_send_run(TabSession *s, const char *line) {
    if (!s || s->to_sess < 0) return;
    frame_write(s->to_sess, C_RUN, line, (int)strlen(line));
}

static void session_send_signal(TabSession *s, int sig) {
    if (!s || s->to_sess < 0) return;
    frame_write(s->to_sess, C_SIG, &sig, (int)sizeof(sig));
}

static int session_gui_fd(TabSession *s) {
    return s ? s->from_sess : -1;
}

/* Apply one frame the session sent, on the GUI side. */
static void session_gui_frame(TabSession *s, int type, char *payload, int len) {
    Tab *gt = s->gui_tab;
    switch (type) {
    case F_OUT:
        if (gt) {
            char tmp[SES_CHUNK + 1];
            int c = len < SES_CHUNK ? len : SES_CHUNK;
            memcpy(tmp, payload, (size_t)c);
            tmp[c] = '\0';
            append_output(gt, tmp);
        }
        break;
    case F_CWD:
        if (gt) {
            int c = len < (int)sizeof(gt->cwd) - 1 ? len : (int)sizeof(gt->cwd) - 1;
            memcpy(gt->cwd, payload, (size_t)c);
            gt->cwd[c] = '\0';
            update_tab_title(gt);
        }
        break;
    case F_BUSY:
        if (len >= (int)sizeof(int)) {
            int v;
            memcpy(&v, payload, sizeof(int));
            s->busy = v ? 1 : 0;
        }
        break;
    case F_QUIT:
        s->gui_quit = 1;             /* session asked this tab to close */
        break;
    case F_DELEGATE:
        /* A built-in that only the GUI can run (key bindings). */
        if (gt && len > 0 && len < MAX_LINE) {
            char tmp[MAX_LINE];
            char *args[MAX_ARGS];
            static __thread char storage[MAX_LINE * 64];
            memcpy(tmp, payload, (size_t)len);
            tmp[len] = '\0';
            int argc = shell_split(tmp, args, MAX_ARGS, storage, sizeof(storage));
            if (argc > 0 && strcmp(args[0], "bind") == 0)
                handle_bind(gt, args, argc);
        }
        break;
    default:
        break;
    }
}

/* Non-blocking: pull everything the session has produced and dispatch it. */
static int session_gui_drain(TabSession *s) {
    int got = 0;
    if (!s || s->from_sess < 0) return 0;

    for (;;) {
        if (s->rlen >= SES_IOBUF) s->rlen = 0;      /* corrupt: drop it */
        ssize_t n = read(s->from_sess, s->rbuf + s->rlen,
                         (size_t)(SES_IOBUF - s->rlen));
        if (n > 0) { s->rlen += (int)n; got = 1; continue; }
        if (n == 0) { s->alive = 0; s->dead = 1; break; }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) { s->alive = 0; s->dead = 1; }
        break;
    }

    int hdr = (int)(2 * sizeof(int));
    while (s->rlen >= hdr) {
        int type = 0, plen = 0, ok = frame_ready(s->rbuf, s->rlen, &type, &plen);
        if (ok < 0) { s->rlen = 0; break; }
        if (ok == 0) break;
        session_gui_frame(s, type, s->rbuf + hdr, plen);
        int used = hdr + plen;
        memmove(s->rbuf, s->rbuf + used, (size_t)(s->rlen - used));
        s->rlen -= used;
    }
    return got;
}

/* ===================== session lifecycle ===================== */

/* A tab's private shell state.  It starts from the GUI tab's directory
 * and from then on is completely independent: its own cwd, jobs and
 * multiWatch timers never leak into another tab. */
static void session_shell_reset(Tab *t, const char *cwd) {
    memset(t, 0, sizeof(*t));
    if (cwd && cwd[0]) snprintf(t->cwd, sizeof(t->cwd), "%s", cwd);
    else if (!getcwd(t->cwd, sizeof(t->cwd))) t->cwd[0] = '\0';
    update_tab_title(t);
    t->hist_pos = -1;
    t->cur_job  = -1;
    t->fg_job   = -1;
    for (int i = 0; i < MAX_JOBS;  i++) t->jobs[i].pipe_fd    = -1;
    for (int i = 0; i < MAX_WATCH; i++) t->watches[i].pipe_fd = -1;
}

/* Ctrl+C / Ctrl+Z arrives from the GUI as a frame: act on this tab's
 * own foreground job only. */
static void session_do_signal(TabSession *s, int sig) {
    Tab *sh = s->shell;
    Job *j = foreground_job(sh);

    if (sig == SIGINT) {
        if (j) {
            kill(-j->pgid, SIGINT);
            kill(-j->pgid, SIGCONT);
            append_output(sh, "^C\n");
        } else if (sh->watching) {
            stop_multiwatch(sh, 1);
        } else {
            append_output(sh, "^C\n");
        }
    } else if (sig == SIGTSTP) {
        if (j) {
            int idx = sh->fg_job;
            kill(-j->pgid, SIGTSTP);
            append_output(sh, "^Z\n");
            job_reap(sh, idx, 1);        /* report the suspend now */
            tab_reap_jobs(sh);
        } else {
            append_output(sh, "^Z\n");
        }
    }
}

/* Consume GUI->session frames sitting in s->cbuf. */
static void session_handle_frames(TabSession *s, int *did_run) {
    int hdr = (int)(2 * sizeof(int));
    char line[MAX_LINE];

    while (s->clen >= hdr) {
        int type = 0, plen = 0, ok = frame_ready(s->cbuf, s->clen, &type, &plen);
        if (ok < 0) { s->clen = 0; break; }
        if (ok == 0) break;

        char *payload = s->cbuf + hdr;
        switch (type) {
        case C_RUN: {
            int c = plen < MAX_LINE - 1 ? plen : MAX_LINE - 1;
            memcpy(line, payload, (size_t)c);
            line[c] = '\0';
            Tab *sh = s->shell;
            snprintf(sh->input_line, MAX_LINE, "%s", line);
            sh->input_len = (int)strlen(sh->input_line);
            sh->cursor    = sh->input_len;
            sh->scroll    = 0;
            execute_command(sh);
            session_emit_cwd(s, sh->cwd);
            if (did_run) *did_run = 1;
            break;
        }
        case C_SIG:
            if (plen >= (int)sizeof(int)) {
                int sig;
                memcpy(&sig, payload, sizeof(sig));
                session_do_signal(s, sig);
            }
            break;
        case C_CLOSE:
            s->quitting = 1;
            break;
        default:
            break;
        }

        int used = hdr + plen;
        memmove(s->cbuf, s->cbuf + used, (size_t)(s->clen - used));
        s->clen -= used;
    }
}

/* The heart of a tab's execution half: run in the session thread (thread
 * mode) or in the forked child (process mode).  It owns the shell state,
 * runs the commands, reaps jobs, ticks multiWatch, and streams whatever
 * the shell prints back to the GUI.  It never touches X11. */
static void session_run_loop(TabSession *s) {
    g_sess = s;
    Tab *sh = s->shell;
    int last_busy = -1;

    session_emit_cwd(s, sh->cwd);

    while (!s->quitting) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s->sfds[0], &rfds);
        int max_fd = s->sfds[0];

        for (int k = 0; k < MAX_JOBS; k++) {
            int fd = sh->jobs[k].pipe_fd;
            if (sh->jobs[k].used && fd >= 0) {
                FD_SET(fd, &rfds);
                if (fd > max_fd) max_fd = fd;
            }
        }
        if (sh->watching) {
            for (int w = 0; w < sh->nwatch; w++) {
                int fd = sh->watches[w].pipe_fd;
                if (fd >= 0) {
                    FD_SET(fd, &rfds);
                    if (fd > max_fd) max_fd = fd;
                }
            }
        }

        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 200000;             /* wake up to tick multiWatch */
        int r = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }

        int did_run = 0;
        if (r > 0 && FD_ISSET(s->sfds[0], &rfds)) {
            char buf[SES_CHUNK];
            ssize_t n = read(s->sfds[0], buf, sizeof(buf));
            if (n == 0) break;           /* GUI went away */
            if (n > 0) {
                if (s->clen + (int)n > SES_IOBUF) s->clen = 0;
                memcpy(s->cbuf + s->clen, buf, (size_t)n);
                s->clen += (int)n;
                session_handle_frames(s, &did_run);
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                break;
            }
            if (s->quitting) break;
        }

        for (int k = 0; k < MAX_JOBS; k++) {
            Job *j = &sh->jobs[k];
            if (j->used && j->pipe_fd >= 0 && FD_ISSET(j->pipe_fd, &rfds))
                drain_job(sh, j);
        }
        tab_reap_jobs(sh);

        restart_due_watches(sh);
        if (sh->watching) {
            for (int w = 0; w < sh->nwatch; w++) {
                WatchCmd *wc = &sh->watches[w];
                if (wc->pipe_fd >= 0 && FD_ISSET(wc->pipe_fd, &rfds))
                    drain_watch_cmd(sh, wc);
            }
        }

        int busy = (foreground_job(sh) != NULL) || sh->watching;
        if (did_run) last_busy = -1;     /* always refresh after a command */
        if (busy != last_busy) {
            last_busy = busy;
            session_emit_int(s, F_BUSY, busy);
        }
    }

    /* Tear down this tab's children before going away. */
    for (int i = 0; i < MAX_JOBS; i++) {
        Job *j = &sh->jobs[i];
        if (!j->used) continue;
        if (j->pgid > 0) { kill(-j->pgid, SIGCONT); kill(-j->pgid, SIGTERM); }
        if (j->pipe_fd >= 0) close(j->pipe_fd);
        j->used = 0;
    }
    if (sh->watching) {
        for (int i = 0; i < sh->nwatch; i++) {
            WatchCmd *w = &sh->watches[i];
            if (w->pid > 0) { kill(-w->pid, SIGKILL); w->pid = 0; }
            if (w->pipe_fd >= 0) { close(w->pipe_fd); w->pipe_fd = -1; }
        }
    }

    if (s->user_exit) session_emit_int(s, F_QUIT, 0);   /* `exit` → close tab */
}

static void *session_thread_main(void *arg) {
    session_run_loop((TabSession *)arg);
    return NULL;
}

static TabSession *session_create(Tab *gui_tab, const char *cwd) {
    TabSession *s = calloc(1, sizeof(TabSession));
    if (!s) return NULL;

    s->mode    = g_use_process ? EXEC_PROCESS : EXEC_THREAD;
    s->gui_tab = gui_tab;
    s->alive   = 1;
    s->shell   = calloc(1, sizeof(Tab));
    if (!s->shell) { free(s); return NULL; }
    session_shell_reset(s->shell, cwd);

    int ab[2], ba[2];
    if (pipe(ab) < 0 || pipe(ba) < 0) {
        perror("pipe");
        free(s->shell); free(s);
        return NULL;
    }
    s->to_sess   = ab[1];        /* GUI writes commands here     */
    s->sfds[0]   = ab[0];        /* session reads them here      */
    s->from_sess = ba[0];        /* GUI reads output here        */
    s->sfds[1]   = ba[1];        /* session writes output here   */

    /* the GUI side must never block on a session that is slow to answer */
    int fl = fcntl(s->from_sess, F_GETFL, 0);
    if (fl >= 0) fcntl(s->from_sess, F_SETFL, fl | O_NONBLOCK);

    if (s->mode == EXEC_THREAD) {
        /* A tab shell parses and forks pipelines, so give it a real stack
         * instead of the tiny (512 KB) default some pthreads use. */
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 8u * 1024u * 1024u);
        int rc = pthread_create(&s->tid, &attr, session_thread_main, s);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            perror("pthread_create");
            close(s->to_sess); close(s->from_sess);
            close(s->sfds[0]); close(s->sfds[1]);
            free(s->shell); free(s);
            return NULL;
        }
    } else {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(s->to_sess); close(s->from_sess);
            close(s->sfds[0]); close(s->sfds[1]);
            free(s->shell); free(s);
            return NULL;
        }
        if (pid == 0) {
            /* ---- the tab's own process: keep only its pipe ends ---- */
            close(s->to_sess);
            close(s->from_sess);
            for (int i = 0; i < MAX_TABS; i++) {
                TabSession *o = sessions[i];
                if (!o || o == s) continue;
                if (o->to_sess   >= 0) close(o->to_sess);
                if (o->from_sess >= 0) close(o->from_sess);
                if (o->sfds[0]   >= 0) close(o->sfds[0]);
                if (o->sfds[1]   >= 0) close(o->sfds[1]);
            }
            signal(SIGINT,  SIG_DFL);        /* a tab is its own session */
            signal(SIGTSTP, SIG_DFL);
            session_run_loop(s);
            _exit(0);
        }
        s->pid = pid;
        close(s->sfds[0]); s->sfds[0] = -1;  /* parent keeps only its ends */
        close(s->sfds[1]); s->sfds[1] = -1;
    }
    return s;
}

static void session_close(TabSession *s) {
    if (!s) return;

    if (!s->closing) {
        s->closing = 1;
        frame_write(s->to_sess, C_CLOSE, NULL, 0);
        if (s->mode == EXEC_THREAD) {
            /* the thread must not block on a full output pipe while we
             * wait for it, so make its writing end non-blocking */
            int fl = fcntl(s->sfds[1], F_GETFL, 0);
            if (fl >= 0) fcntl(s->sfds[1], F_SETFL, fl | O_NONBLOCK);
        }
    }

    if (s->mode == EXEC_THREAD) {
        if (s->tid) { pthread_join(s->tid, NULL); s->tid = 0; }
    } else if (s->pid > 0) {
        int st, done = 0;
        for (int i = 0; i < 100; i++) {          /* up to ~1s to exit politely */
            pid_t r = waitpid(s->pid, &st, WNOHANG);
            if (r == s->pid) { done = 1; break; }
            if (r < 0 && errno != EINTR) { done = 1; break; }
            usleep(10000);
        }
        if (!done) {                             /* rude but it is only a tab */
            kill(s->pid, SIGKILL);
            waitpid(s->pid, &st, 0);
        }
        s->pid = 0;
    }

    if (s->to_sess   >= 0) close(s->to_sess);
    if (s->from_sess >= 0) close(s->from_sess);
    if (s->sfds[0]   >= 0) close(s->sfds[0]);
    if (s->sfds[1]   >= 0) close(s->sfds[1]);
    free(s->shell);
    free(s);
}

static void session_destroy_all(void) {
    for (int i = 0; i < MAX_TABS; i++) {
        if (sessions[i]) {
            session_close(sessions[i]);
            sessions[i] = NULL;
        }
    }
}

int main(int argc, char **argv) {
    /* Execution mode comes from the command line:
     *     ./terminator                  -> thread mode  (one pthread per tab)
     *     ./terminator useProcess=True  -> process mode (one fork() per tab) */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "useProcess=", 11) == 0) {
            const char *v = argv[i] + 11;
            g_use_process = (strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0);
        }
    }

    /* A session may outlive the pipe the GUI reads from; without this a
     * write to a closed pipe would kill the whole GUI in thread mode. */
    signal(SIGPIPE, SIG_IGN);

    /* One shared, locked history: process memory + mutex in thread mode,
     * shm_open()+MAP_SHARED + process-shared semaphore in process mode. */
    history_init();

    Display *display = XOpenDisplay(NULL);
    if (display == NULL) {
        printf("Unable to connect to X11\n");
        return 1;
    }
    int scr = DefaultScreen(display);

    Window window = XCreateSimpleWindow(
        display,
        DefaultRootWindow(display),
        100, 100, 800, 500,
        1,
        BlackPixel(display, scr),
        WhitePixel(display, scr)
    );

    Atom delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    atom_clipboard = XInternAtom(display, "CLIPBOARD",   False);
    atom_utf8      = XInternAtom(display, "UTF8_STRING", False);
    atom_targets   = XInternAtom(display, "TARGETS",     False);
    atom_sel_data  = XInternAtom(display, "TERMINATOR_SELECTION", False);
    XStoreName(display, window, "Terminator");
    XSetWMProtocols(display, window, &delete_window, 1);
    XSelectInput(display, window, ExposureMask | StructureNotifyMask | KeyPressMask |
                                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
    XMapWindow(display, window);
     XFlush(display);  

    GC gc = XCreateGC(display, window, 0, NULL);

    /* Load key bindings from ~/.terminatorrc (falls back to defaults) */
    load_config();

    /* Load the shared command history from ~/.terminator_history */
    history_load();

    // Initialize first tab
    num_tabs = 1;
    init_tab(&tabs[0], 1);
    if (config_msgs_len > 0) append_output(&tabs[0], config_msgs);

    int x11_fd = ConnectionNumber(display);

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(x11_fd, &rfds);
        int max_fd = x11_fd;

        // Monitor the output pipe of every tab's session.  The jobs and
        // multiWatch pipes live in the session, not here, so the GUI only
        // watches the X11 socket plus one fd per tab.
        for (int i = 0; i < num_tabs; i++) {
            int fd = session_gui_fd(sessions[i]);
            if (fd >= 0) {
                FD_SET(fd, &rfds);
                if (fd > max_fd) max_fd = fd;
            }
        }

        /* No scheduled wake-up here: multiWatch timers belong to the
         * sessions.  Poll every 500 ms just to blink the caret. */
        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 500000;

        int retval = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        if (retval < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (retval == 0) {
            /* timeout with no I/O: flip the caret and repaint */
            cursor_visible = !cursor_visible;
            redraw(display, window, gc, scr);
        }

        // Check X11 events
        if (FD_ISSET(x11_fd, &rfds) || XPending(display)) {
            while (XPending(display)) {
                XEvent event;
                XNextEvent(display, &event);

                if (event.type == Expose) {
                    redraw(display, window, gc, scr);
                } else if (event.type == ConfigureNotify) {
                    if (event.xconfigure.width > 0 && event.xconfigure.height > 0) {
                        win_w = event.xconfigure.width;
                        win_h = event.xconfigure.height;
                        clamp_scroll(&tabs[current_tab]);
                        redraw(display, window, gc, scr);
                    }
                } else if (event.type == ClientMessage && event.xclient.data.l[0] == delete_window) {
                    goto cleanup;
                } else if (event.type == ButtonPress) {
                    Tab *t = &tabs[current_tab];
                    /* A click anywhere drops a Shift+arrow keyboard
                     * selection, so the two kinds never stay active at once. */
                    t->in_sel_active = 0;
                    if (event.xbutton.button == Button4) {
                        t->scroll += 3;
                        clamp_scroll(t);
                        redraw(display, window, gc, scr);
                    } else if (event.xbutton.button == Button5) {
                        t->scroll -= 3;
                        clamp_scroll(t);
                        redraw(display, window, gc, scr);
                    } else if (event.xbutton.button == Button1 && event.xbutton.y >= TEXT_TOP) {
                        /* begin a mouse text selection */
                        t->sel_anchor_line = t->sel_line = y_to_display_line(t, event.xbutton.y);
                        t->sel_anchor_col  = t->sel_col  = x_to_col(event.xbutton.x);
                        t->sel_dragging = 1;
                        t->sel_active   = 0;
                        redraw(display, window, gc, scr);
                    } else if (event.xbutton.y < TABBAR_BOTTOM) {
                    int click_x = event.xbutton.x;
                    int x = 5;
                    int hit_tab = -1, hit_close = -1;

                    for (int i = 0; i < num_tabs; i++) {
                        int label_w, close_x, close_w, tab_w;
                        tab_layout(i, &(int){0}, &label_w, &close_x, &close_w, &tab_w);

                        int tab_left  = x;
                        int tab_right = x + tab_w;
                        int cx        = x + close_x;

                        if (click_x >= tab_left && click_x < tab_right) {
                            hit_tab = i;
                            if (click_x >= cx && click_x <= cx + close_w)
                                hit_close = i;
                            break;
                        }
                        x += tab_w;
                    }

                    if (hit_close >= 0) {
                        close_tab(hit_close);
                    } else if (hit_tab >= 0) {
                        current_tab = hit_tab;
                    } else if (num_tabs < MAX_TABS && click_x >= x && click_x <= x + 40) {
                        init_tab(&tabs[num_tabs], num_tabs + 1);
                        current_tab = num_tabs;
                        num_tabs++;
                    }

                    redraw(display, window, gc, scr);
                }
                } else if (event.type == ButtonRelease) {
                    Tab *t = &tabs[current_tab];
                    if (t->sel_dragging) {
                        t->sel_dragging = 0;
                        t->sel_active   = 1;
                        redraw(display, window, gc, scr);
                    }
                } else if (event.type == MotionNotify) {
                    Tab *t = &tabs[current_tab];
                    if (t->sel_dragging) {
                        t->sel_line    = y_to_display_line(t, event.xmotion.y);
                        t->sel_col     = x_to_col(event.xmotion.x);
                        t->sel_active  = 1;
                        redraw(display, window, gc, scr);
                    }
                } else if (event.type == SelectionRequest) {
                    /* another program wants our CLIPBOARD text */
                    XSelectionRequestEvent *r = &event.xselectionrequest;
                    XSelectionEvent ev;
                    memset(&ev, 0, sizeof(ev));
                    ev.type      = SelectionNotify;
                    ev.display   = r->display;
                    ev.requestor = r->requestor;
                    ev.selection = r->selection;
                    ev.target    = r->target;
                    ev.time      = r->time;
                    ev.property  = None;
                    if (r->property == None) r->property = r->target;

                    if (r->target == atom_targets) {
                        Atom avail[2] = { atom_utf8, XA_STRING };
                        XChangeProperty(display, r->requestor, r->property, XA_ATOM, 32,
                                        PropModeReplace, (unsigned char *)avail, 2);
                        ev.property = r->property;
                    } else if ((r->target == atom_utf8 || r->target == XA_STRING) && clip_text) {
                        XChangeProperty(display, r->requestor, r->property, r->target, 8,
                                        PropModeReplace, (unsigned char *)clip_text, clip_len);
                        ev.property = r->property;
                    }
                    XSendEvent(display, r->requestor, False, 0, (XEvent *)&ev);
                    XFlush(display);
                } else if (event.type == SelectionClear) {
                    if (clip_text) { free(clip_text); clip_text = NULL; clip_len = 0; }
                } else if (event.type == SelectionNotify) {
                    cursor_visible = 1;
                    /* answer to our paste request */
                    if (event.xselection.property != None) {
                        Atom type; int fmt; unsigned long nitems, after;
                        unsigned char *data = NULL;
                        if (XGetWindowProperty(display, window, atom_sel_data, 0, 1 << 20, True,
                                               AnyPropertyType, &type, &fmt, &nitems, &after,
                                               &data) == Success && data) {
                            insert_text(&tabs[current_tab], (char *)data, (int)nitems);
                            XFree(data);
                            redraw(display, window, gc, scr);
                        }
                    }
                } else if (event.type == KeyPress) {
                     cursor_visible = 1;
                    char buf[32];
                    KeySym keysym;
                    int len = XLookupString(&event.xkey, buf, sizeof(buf), &keysym, NULL);

                    Tab *t = &tabs[current_tab];
                    unsigned int state = event.xkey.state;

                    /* Ctrl+R term entry: Return runs the search, Escape
                     * cancels; arrows are ignored so they cannot overwrite
                     * the term.  Other keys fall through to line editing. */
                    if (t->search_mode) {
                        if (keysym == XK_Return || keysym == XK_KP_Enter) {
                            run_history_search(t);
                            t->search_mode = 0;
                            t->input_len = 0;
                            t->input_line[0] = '\0';
                            t->cursor = 0;
                            redraw(display, window, gc, scr);
                            continue;
                        }
                        if (keysym == XK_Escape) {
                            t->search_mode = 0;
                            t->input_len = 0;
                            t->input_line[0] = '\0';
                            t->cursor = 0;
                            redraw(display, window, gc, scr);
                            continue;
                        }
                        if (keysym == XK_Up || keysym == XK_Down ||
                            keysym == XK_Prior || keysym == XK_Next)
                            continue;
                    }

                    /* Tab-completion menu: a digit picks a candidate, any
                     * other key cancels the menu and is handled normally. */
                    if (t->comp_mode) {
                        if (keysym >= XK_1 && keysym <= XK_9) {
                            int pick = (int)(keysym - XK_1);
                            if (pick < t->comp_count) {
                                comp_apply(t, pick);
                            } else {
                                t->comp_mode = 0;
                                t->comp_count = 0;
                            }
                            redraw(display, window, gc, scr);
                            continue;
                        }
                        t->comp_mode = 0;
                        t->comp_count = 0;
                    }

                    /* configurable bindings take priority */
                    int bi = find_binding(keysym, state & BIND_MODS);
                    if (bi >= 0) {
                        if (perform_action(t, bindings[bi].action, display, window))
                            redraw(display, window, gc, scr);
                        if (g_exit_requested) goto cleanup;
                        continue;
                    }

                    Bool ctrl = (state & ControlMask) != 0;
                    TabSession *sess = sessions[current_tab];

                    /* Ctrl+C: interrupt this tab's foreground job.  The
                     * session does the killing, so no other tab is touched. */
                    if (ctrl && (keysym == XK_c || keysym == XK_C)) {
                        if (session_busy(t)) {
                            if (sess) session_send_signal(sess, SIGINT);
                        } else {
                            echo_control_marker(t, 'C');
                        }
                        t->in_sel_active = 0;
                        redraw(display, window, gc, scr);
                        continue;
                    }

                    /* Ctrl+Z: suspend this tab's foreground job. */
                    if (ctrl && (keysym == XK_z || keysym == XK_Z)) {
                        if (session_busy(t)) {
                            if (sess) session_send_signal(sess, SIGTSTP);
                        } else {
                            echo_control_marker(t, 'Z');
                        }
                        t->in_sel_active = 0;
                        redraw(display, window, gc, scr);
                        continue;
                    }

                    /* Ctrl+R: start a reverse history search.  The prompt
                     * becomes "Enter search term: " until Enter/Escape. */
                    if (ctrl && (keysym == XK_r || keysym == XK_R)) {
                        if (!t->search_mode && !session_busy(t)) {
                            t->search_mode = 1;
                            t->in_sel_active = 0;
                            t->input_len = 0;
                            t->input_line[0] = '\0';
                            t->cursor = 0;
                            append_output(t, "user@terminator> (Ctrl+R)\n");
                        }
                        redraw(display, window, gc, scr);
                        continue;
                    }

                    /* While a command owns this tab, hide the prompt and
                     * swallow every other keystroke: the shell is "busy"
                     * until the job finishes or is suspended.  Only Ctrl+C /
                     * Ctrl+Z above reach the session. */
                    if (session_busy(t))
                        continue;

                    if (ctrl && (keysym == XK_t || keysym == XK_T)) {
                        if (num_tabs < MAX_TABS) {
                            init_tab(&tabs[num_tabs], num_tabs + 1);
                            current_tab = num_tabs;
                            num_tabs++;
                        }
                        redraw(display, window, gc, scr);
                        continue;
                    }

                    /* Shift + arrows: extend a keyboard selection over the
                     * input line, highlighted like the mouse selection. */
                    if ((state & ShiftMask) &&
                        (keysym == XK_Left || keysym == XK_Right ||
                         keysym == XK_Home || keysym == XK_End)) {
                        /* a keyboard selection replaces any mouse selection */
                        t->sel_active   = 0;
                        t->sel_dragging = 0;
                        int old = t->cursor;
                        if      (keysym == XK_Left  && t->cursor > 0)            t->cursor--;
                        else if (keysym == XK_Right && t->cursor < t->input_len) t->cursor++;
                        else if (keysym == XK_Home)                             t->cursor = 0;
                        else if (keysym == XK_End)                              t->cursor = t->input_len;

                        if (!t->in_sel_active) {
                            t->in_sel_active = 1;
                            t->in_sel_anchor = old;
                        }
                        t->in_sel_end = t->cursor;
                        if (t->in_sel_anchor == t->in_sel_end)
                            t->in_sel_active = 0;
                        redraw(display, window, gc, scr);
                        continue;
                    }

                    if (keysym == XK_Prior || keysym == XK_Page_Up) {
                        t->scroll += usable_rows() - 1;
                        if (t->scroll < 1) t->scroll = 1;
                        clamp_scroll(t);
                    } else if (keysym == XK_Next || keysym == XK_Page_Down) {
                        t->scroll -= usable_rows() - 1;
                        clamp_scroll(t);
                    } else if (keysym == XK_Home) {
                        t->scroll = 1000000;
                        clamp_scroll(t);
                    } else if (keysym == XK_End) {
                        t->scroll = 0;
                    } else if (keysym == XK_Return || keysym == XK_KP_Enter) {
                            if (t->watching) {
                                redraw(display, window, gc, scr);
                                continue;
                            }
                            t->in_sel_active = 0;

                            /* Keep the prompt each line was typed at inside
                             * multiline_buf, so the lines already entered
                             * stay visible with their prompt while the user
                             * keeps typing the rest of the command. */
                            if (t->multiline) {
                                const char *pfx = "> ";
                                int pl = 2;
                                if (t->multiline_len + pl + t->input_len + 2 <
                                        (int)sizeof(t->multiline_buf)) {
                                    memcpy(t->multiline_buf + t->multiline_len, pfx, pl);
                                    t->multiline_len += pl;
                                    memcpy(t->multiline_buf + t->multiline_len,
                                           t->input_line, t->input_len);
                                    t->multiline_len += t->input_len;
                                    t->multiline_buf[t->multiline_len++] = '\n';
                                    t->multiline_buf[t->multiline_len]   = '\0';
                                }
                            } else {
                                const char *pfx = "user@terminator> ";
                                int pl = (int)strlen(pfx);
                                if (pl + t->input_len + 2 <
                                        (int)sizeof(t->multiline_buf)) {
                                    memcpy(t->multiline_buf, pfx, pl);
                                    memcpy(t->multiline_buf + pl,
                                           t->input_line, t->input_len);
                                    t->multiline_len = pl + t->input_len;
                                    t->multiline_buf[t->multiline_len++] = '\n';
                                    t->multiline_buf[t->multiline_len]   = '\0';
                                }
                            }

                            if (input_incomplete(t->multiline_buf, t->multiline_len)) {
                                /* stay in multiline */
                                t->multiline = 1;
                                t->input_len = 0;
                                t->input_line[0] = '\0';
                                t->cursor = 0;
                            } else {
                                /* complete: run the full accumulated command */
                                t->multiline = 0;

                                /* drop the prompts we rendered above */
                                strip_prompt_prefixes(t->multiline_buf,
                                                      t->input_line, MAX_LINE);
                                t->input_len = (int)strlen(t->input_line);

                                while (t->input_len > 0 &&
                                    (t->input_line[t->input_len-1] == '\n' ||
                                        t->input_line[t->input_len-1] == '\r')) {
                                    t->input_line[--t->input_len] = '\0';
                                }

                                /* Hand the finished line to this tab's
                                 * session; it runs it, echoes it and sends
                                 * the output back.  The GUI stays idle. */
                                if (sess && sess->alive) {
                                    session_send_run(sess, t->input_line);
                                    sess->busy = 1;   /* hide the prompt now */
                                }

                                t->multiline_len = 0;
                                t->multiline_buf[0] = '\0';
                                t->input_len = 0;
                                t->input_line[0] = '\0';
                                t->cursor = 0;
                            }
                        } else if (keysym == XK_BackSpace) {
                        if (t->in_sel_active) {
                            int a = t->in_sel_anchor, b = t->in_sel_end;
                            if (a > b) { int tmp = a; a = b; b = tmp; }
                            if (a < 0) a = 0;
                            if (b > t->input_len) b = t->input_len;
                            memmove(t->input_line + a, t->input_line + b,
                                    (size_t)(t->input_len - b + 1));
                            t->input_len -= (b - a);
                            t->cursor = a;
                            t->in_sel_active = 0;
                        } else if (t->cursor > 0) {
                            memmove(t->input_line + t->cursor - 1,
                                    t->input_line + t->cursor,
                                    (size_t)(t->input_len - t->cursor + 1));
                            t->cursor--;
                            t->input_len--;
                            t->sel_active = 0;
                        }
                    } else if (keysym == XK_Tab) {
                        /* Tab completes the word before the cursor */
                        do_completion(t);
                    } else if (keysym == XK_ISO_Left_Tab) {
                        /* Shift+Tab cycles tabs (Tab now completes) */
                        current_tab = (current_tab + 1) % num_tabs;
                    } else if (len > 0 && buf[0] >= 32 && buf[0] < 127) {
                        /* typed text replaces an active selection */
                        if (t->in_sel_active) {
                            int a = t->in_sel_anchor, b = t->in_sel_end;
                            if (a > b) { int tmp = a; a = b; b = tmp; }
                            if (a < 0) a = 0;
                            if (b > t->input_len) b = t->input_len;
                            memmove(t->input_line + a, t->input_line + b,
                                    (size_t)(t->input_len - b + 1));
                            t->input_len -= (b - a);
                            t->cursor = a;
                            t->in_sel_active = 0;
                        }
                        if (t->input_len < MAX_LINE - 1) {
                            int ilen = (int)strlen(t->input_line);
                    memmove(t->input_line + t->cursor + 1, t->input_line + t->cursor,
                            (size_t)(ilen - t->cursor + 1));
                            t->input_line[t->cursor++] = buf[0];
                            t->input_len++;
                            t->sel_active = 0;
                        }
                         sync_input(t);   
                    }
                    redraw(display, window, gc, scr);
                }
            }
        }

        /* Pull whatever each tab's session produced and show it.  The
         * session already ran the command, drained its pipes and reaped
         * its jobs, so the GUI has nothing else to do here. */
        for (int i = 0; i < num_tabs; i++) {
            TabSession *s = sessions[i];
            if (!s) continue;

            if (s->from_sess >= 0 && FD_ISSET(s->from_sess, &rfds)) {
                session_gui_drain(s);
                redraw(display, window, gc, scr);
            }

            if (s->dead && !s->notified && !s->closing) {
                s->notified = 1;
                if (s->gui_tab) {
                    append_output(s->gui_tab, "\nterminator: session ended\n");
                    redraw(display, window, gc, scr);
                }
            }
        }

        /* A session asked for its tab to go away (`exit` built-in). */
        for (int i = 0; i < num_tabs; i++) {
            if (sessions[i] && sessions[i]->gui_quit) {
                if (num_tabs > 1) {
                    close_tab(i);
                    redraw(display, window, gc, scr);
                } else {
                    goto cleanup;
                }
                break;
            }
        }
    }

cleanup:
    session_destroy_all();
    history_save_file();
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}