#include <X11/Xlib.h>
#include <X11/Xutil.h> 
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/types.h>
#include <signal.h>
#include <glob.h>   

#define MAX_PIPELINE 16          /* max commands in one pipeline */
#define MAX_ARGS     64          /* max argv per command */
#define MAX_STORAGE  (MAX_LINE * 64)

typedef struct {
    char  in_file[1024];         /* "" if no < redirection */
    char  out_file[1024];        /* "" if no > redirection */
    char *argv[MAX_ARGS];
     int   argc;     
} Command;


#define MAX_TABS 5
#define MAX_LINE 256
#define MAX_OUTPUT (4096 * 16)
#define HISTORY_MAX 50


typedef struct {
    char title[64];
    char cwd[1024];
    char input_line[MAX_LINE];
    int input_len;
    char output_buffer[MAX_OUTPUT];
    int output_len;
    int last_exit_status;
    char history[HISTORY_MAX][MAX_LINE];
    int history_count;
    int pipe_fd;
    pid_t pid;


    int  multiline;
    char multiline_buf[MAX_OUTPUT];
    int  multiline_len;
} Tab;

static Tab tabs[MAX_TABS];
static int num_tabs = 0;
static int current_tab = 0;

void append_output(Tab *tab, const char *str) {
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
                    if (n == '"' || n == '\\' || n == '$' || n == '`') {
                        if (ci < (int)sizeof(cur) - 1) cur[ci++] = n;
                        i += 2;
                        continue;
                    }
                    if (n == '\n') { i += 2; continue; } /* continuation */
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
 * Non-matching args are kept literally (POSIX shell behaviour).
 * Returns the new argc. */
static int expand_globs(char **argv, int argc,
                        char **new_argv, int new_max) {
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

        glob_t g;
        int rc = glob(arg, GLOB_NOCHECK, NULL, &g);
        if (rc == 0) {
            for (size_t k = 0; k < g.gl_pathc && out < new_max - 1; k++) {
                new_argv[out++] = strdup(g.gl_pathv[k]);
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


/* Open redirection fds for `cmd`. Fills *in_fd / *out_fd (-1 if unused).
 * Returns 0 on success, -1 on error (message already appended). */
 int open_redirections(Tab *tab, Command *cmd, int *in_fd, int *out_fd) {
    *in_fd = -1;
    *out_fd = -1;

    if (cmd->in_file[0]) {
        *in_fd = open(cmd->in_file, O_RDONLY);
        if (*in_fd < 0) {
            char err[1200];
            snprintf(err, sizeof(err),
                     "terminator: %s: No such file or directory\n", cmd->in_file);
            append_output(tab, err);
            return -1;
        }
    }
    if (cmd->out_file[0]) {
        *out_fd = open(cmd->out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
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

/* Fork + exec a single command in the pipeline.
 *   prev_read  : read end of previous pipe (-1 for first stage)
 *   next_write : write end of next pipe (-1 for last stage)
 *   out_fd     : file redirect for stdout (-1 = use next_write / parent pipe)
 * Returns child pid, or -1 on error. */
 pid_t spawn_child(Tab *tab, Command *cmd,
                         int prev_read, int next_write, int out_fd) {
    pid_t pid = fork();
    if (pid != 0) return pid;     /* parent or error */

    /* --- child --- */
    if (prev_read >= 0) {
        dup2(prev_read, STDIN_FILENO);
    } else if (cmd->in_file[0] == '\0' && tab->pipe_fd != -1) {
        /* no pipe before us, but shell may already have an in_fd — handled by caller */
    }

    /* stdout: file redirect wins, else pipe to next stage */
    if (out_fd >= 0) {
        dup2(out_fd, STDOUT_FILENO);
    } else if (next_write >= 0) {
        dup2(next_write, STDOUT_FILENO);
    }

    /* stderr always goes to the tab's pipe (via next_write or the
     * caller-provided stderr_fd). */
    if (next_write >= 0) dup2(next_write, STDERR_FILENO);

    /* close every inherited pipe fd */
    /* (caller passes them via globals, or we close a known range — see below) */

    chdir(tab->cwd);
    execvp(cmd->argv[0], cmd->argv);
    fprintf(stderr, "terminator: %s: command not found\n", cmd->argv[0]);
    _exit(127);
}

/* Run a command line that may contain '|' pipelines and redirections.
 * Returns exit status of the LAST command (or 1 on setup error). */
static int execute_pipeline(Tab *tab, char *line) {
    /* 1) Split on '|' */
    char *segments[MAX_PIPELINE];
    int n = split_pipeline(line, segments, MAX_PIPELINE);
    if (n <= 0) {
        append_output(tab, "terminator: syntax error: too many pipes\n");
        return 1;
    }

    /* 2) Parse each segment (redirections + argv) */
    static char storage[MAX_PIPELINE][MAX_STORAGE];
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
    static char *glob_storage[MAX_PIPELINE][MAX_ARGS];

    for (int i = 0; i < n; i++) {
        int newc = expand_globs(cmds[i].argv, cmds[i].argc,
                                glob_storage[i], MAX_ARGS);
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

    /* 6) Fork every stage */
    pid_t pids[MAX_PIPELINE];
    for (int i = 0; i < n; i++) {
        int prev_read  = (i > 0)        ? pipes[i - 1][0] : -1;
        int next_write = (i < n - 1)    ? pipes[i][1]     : -1;

        pid_t pid = fork();
        if (pid < 0) { perror("fork"); /* cleanup… */ return 1; }

        if (pid == 0) {
            /* ---- CHILD ---- */
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

            chdir(tab->cwd);
            execvp(cmds[i].argv[0], cmds[i].argv);
            fprintf(stderr, "terminator: %s: command not found\n", cmds[i].argv[0]);
            _exit(127);
        }
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

    /* 8) Hand the capture read-end to the tab so the event loop drains it */
    tab->pipe_fd = capture[0];
    tab->pid     = pids[n - 1];   /* reaped by the loop */

    /* Reap all but the last; the last is reaped when capture hits EOF. */
    for (int i = 0; i < n - 1; i++) {
        waitpid(pids[i], NULL, 0);
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
    tab->output_len = 0;
    tab->output_buffer[0] = '\0';
    tab->last_exit_status = 0;
    tab->history_count = 0;
    tab->pipe_fd = -1;
    tab->pid = 0;

    tab->multiline = 0;
    tab->multiline_len = 0;
    tab->multiline_buf[0] = '\0';


    // char welcome[128];
    // snprintf(welcome, sizeof(welcome), "Terminator Tab %d initialized.\n", id);
    // append_output(tab, welcome);
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

    Tab *t = &tabs[index];
    if (t->pipe_fd != -1) {
        close(t->pipe_fd);
        t->pipe_fd = -1;
    }
    if (t->pid > 0) {
        kill(t->pid, SIGTERM);
        waitpid(t->pid, NULL, 0);
        t->pid = 0;
    }

   int moved = num_tabs - index - 1;
    if (moved > 0) {
        memmove(&tabs[index], &tabs[index + 1], moved * sizeof(Tab));
    }

    // 3) Wipe the now-vacated tail slot so no stale fd/pid/data remains.
    memset(&tabs[num_tabs - 1], 0, sizeof(Tab));
    tabs[num_tabs - 1].pipe_fd = -1;
    tabs[num_tabs - 1].pid     = 0;

    num_tabs--;

    // 4) Fix current_tab for every case:
    //    - closed tab was before current  -> shift left
    //    - closed tab was current or after -> clamp
    if (index < current_tab) {
        current_tab--;
    } else if (current_tab >= num_tabs) {
        current_tab = num_tabs - 1;
    }
}


/* Returns 1 if the buffer needs more input (unclosed quote or trailing \),
 * 0 if complete. */
static int input_incomplete(const char *s, int len) {
    /* strip trailing newline for the trailing-backslash check */
    int real = len;
    if (real > 0 && s[real-1] == '\n') real--;

    int i = 0, in_sq = 0, in_dq = 0;
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
            i++; continue;
        }
        if (c == '\'') { in_sq = 1; i++; continue; }
        if (c == '"')  { in_dq = 1; i++; continue; }
        i++;
    }
    if (in_sq || in_dq) return 1;

    /* trailing backslash (before the added \n) means continue */
    if (real > 0 && s[real-1] == '\\') return 1;

    return 0;
}




void execute_command(Tab *tab) {
    if (tab->input_len == 0) return;

    // Add to history
    if (tab->history_count < HISTORY_MAX) {
        strcpy(tab->history[tab->history_count++], tab->input_line);
    } else {
        memmove(tab->history, tab->history + 1, sizeof(tab->history[0]) * (HISTORY_MAX - 1));
        strcpy(tab->history[HISTORY_MAX - 1], tab->input_line);
    }

    // Echo prompt and command
    char tmp[MAX_OUTPUT];
        strncpy(tmp, tab->input_line, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';

        int first = 1;
        char *line = strtok(tmp, "\n");
        while (line) {
            char buf[MAX_LINE + 64];
            snprintf(buf, sizeof(buf), "%s%s\n",
                     first ? "user@terminator> " : "> ", line);
            append_output(tab, buf);
            first = 0;
            line = strtok(NULL, "\n");
        }
    // Parse arguments
   
    char cmd_copy[MAX_LINE];
    strcpy(cmd_copy, tab->input_line);

   char *args[64];
    static char storage[MAX_LINE * 64];
    int argc = shell_split(tab->input_line, args, 64, storage, sizeof(storage));

    if (argc == 0) {
        tab->input_len = 0;
        tab->input_line[0] = '\0';
        return;
    }

    // Handle Built-ins
    if (strcmp(args[0], "exit") == 0) {
        exit(0);
    } else if (strcmp(args[0], "cd") == 0) {
        char *target = args[1];
        // if (!target || strcmp(target, "~") == 0) {
        //     struct passwd *pw = getpwuid(getuid());
        //     target = pw ? pw->dir : getenv("HOME"); 
        // }
        if (chdir(target) == 0) {
            getcwd(tab->cwd, sizeof(tab->cwd));
            update_tab_title(tab);
            tab->last_exit_status = 0;
        } else {
            char err[256];
            snprintf(err, sizeof(err), "terminator: cd: %s: No such file or directory\n", target);
            append_output(tab, err);
            tab->last_exit_status = 1;
        }
    } else if (strcmp(args[0], "history") == 0) {
        for (int i = 0; i < tab->history_count; i++) {
            char hist_buf[MAX_LINE + 16];
            snprintf(hist_buf, sizeof(hist_buf), " %2d  %s\n", i + 1, tab->history[i]);
            append_output(tab, hist_buf);
        }
        tab->last_exit_status = 0;
       } else {
        char cmd_copy[MAX_OUTPUT];
            strncpy(cmd_copy, tab->input_line, sizeof(cmd_copy) - 1);
            cmd_copy[sizeof(cmd_copy) - 1] = '\0';

            int status = execute_pipeline(tab, cmd_copy);
            if (status != 0) tab->last_exit_status = status;
    }

    tab->input_len = 0;
    tab->input_line[0] = '\0';
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
    XDrawLine(display, window, gc, 10, 35, 780, 35);

    // Draw Terminal Output and Current Input Line for Current Tab
    Tab *t = &tabs[current_tab];
    int text_y = 55;
    
    // Print output lines
    char line_buf[MAX_LINE];
    int line_idx = 0;
    for (int i = 0; i < t->output_len; i++) {
        if (t->output_buffer[i] == '\n' || line_idx >= MAX_LINE - 1) {
            line_buf[line_idx] = '\0';
            XDrawString(display, window, gc, 10, text_y, line_buf, strlen(line_buf));
            text_y += 18;
            line_idx = 0;
        } else {
            line_buf[line_idx++] = t->output_buffer[i];
        }
    }
    if (line_idx > 0) {
        line_buf[line_idx] = '\0';
        XDrawString(display, window, gc, 10, text_y, line_buf, strlen(line_buf));
        text_y += 18;
    }
     if (t->multiline && t->multiline_len > 0) {
        char copy[MAX_OUTPUT];
        strncpy(copy, t->multiline_buf, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';

        int first = 1;
        char *line = strtok(copy, "\n");
        while (line) {
            char shown[MAX_LINE + 64];
            snprintf(shown, sizeof(shown), "%s%s",
                     first ? "user@terminator> " : "> ", line);
            XDrawString(display, window, gc, 10, text_y, shown, strlen(shown));
            text_y += 18;
            first = 0;
            line = strtok(NULL, "\n");
        }
    }

    /* Draw active prompt and current input */
    char current_prompt[MAX_LINE + 64];
    const char *prompt = t->multiline ? "> " : "user@terminator> ";
    snprintf(current_prompt, sizeof(current_prompt), "%s%s", prompt, t->input_line);
    XDrawString(display, window, gc, 10, text_y, current_prompt, strlen(current_prompt));

    XFlush(display);
}

int main(void) {
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
    XStoreName(display, window, "Terminator");
    XSetWMProtocols(display, window, &delete_window, 1);
    XSelectInput(display, window, ExposureMask | StructureNotifyMask | KeyPressMask | ButtonPressMask);
    XMapWindow(display, window);
     XFlush(display);  

    GC gc = XCreateGC(display, window, 0, NULL);

    // Initialize first tab
    num_tabs = 1;
    init_tab(&tabs[0], 1);

    int x11_fd = ConnectionNumber(display);

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(x11_fd, &rfds);
        int max_fd = x11_fd;

        // Monitor pipe file descriptors for running processes across tabs
        for (int i = 0; i < num_tabs; i++) {
            if (tabs[i].pipe_fd != -1) {
                FD_SET(tabs[i].pipe_fd, &rfds);
                if (tabs[i].pipe_fd > max_fd) max_fd = tabs[i].pipe_fd;
            }
        }

        // Use select() to avoid busy-waiting
        int retval = select(max_fd + 1, &rfds, NULL, NULL, NULL);
        if (retval < 0) break;

        // Check X11 events
        if (FD_ISSET(x11_fd, &rfds) || XPending(display)) {
            while (XPending(display)) {
                XEvent event;
                XNextEvent(display, &event);

                if (event.type == Expose) {
                    redraw(display, window, gc, scr);
                } else if (event.type == ClientMessage && event.xclient.data.l[0] == delete_window) {
                    goto cleanup;
                } else if (event.type == ButtonPress) {
                    // Simple click detection for tabs (y < 35)
                   if (event.xbutton.y < 35) {
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
                } else if (event.type == KeyPress) {
                    char buf[32];
                    KeySym keysym;
                    int len = XLookupString(&event.xkey, buf, sizeof(buf), &keysym, NULL);

                    Tab *t = &tabs[current_tab];

                    Bool ctrl = (event.xkey.state & ControlMask) != 0;

                    if (ctrl && (keysym == XK_w || keysym == XK_W)) {
                        close_tab(current_tab);
                        redraw(display, window, gc, scr);
                        continue;   // inside while(XPending) — skip normal handling
                    }

                    if (ctrl && (keysym == XK_t || keysym == XK_T)) {
                        if (num_tabs < MAX_TABS) {
                            init_tab(&tabs[num_tabs], num_tabs + 1);
                            current_tab = num_tabs;
                            num_tabs++;
                        }
                        redraw(display, window, gc, scr);
                        continue;
                    }

                    if (keysym == XK_Return || keysym == XK_KP_Enter) {

                            if (t->multiline) {
                                if (t->multiline_len + t->input_len + 2 < (int)sizeof(t->multiline_buf)) {
                                    memcpy(t->multiline_buf + t->multiline_len,
                                        t->input_line, t->input_len);
                                    t->multiline_len += t->input_len;
                                    t->multiline_buf[t->multiline_len++] = '\n';
                                    t->multiline_buf[t->multiline_len]   = '\0';
                                }
                            } else {
                                strncpy(t->multiline_buf, t->input_line, sizeof(t->multiline_buf) - 1);
                                t->multiline_buf[sizeof(t->multiline_buf) - 1] = '\0';
                                t->multiline_len = t->input_len;
                                t->multiline_buf[t->multiline_len++] = '\n';
                                t->multiline_buf[t->multiline_len]   = '\0';
                            }

                            if (input_incomplete(t->multiline_buf, t->multiline_len)) {
                                /* stay in multiline */
                                t->multiline = 1;
                                t->input_len = 0;
                                t->input_line[0] = '\0';
                            } else {
                                /* complete: run the full accumulated command */
                                t->multiline = 0;

                                strncpy(t->input_line, t->multiline_buf, MAX_LINE - 1);
                                t->input_line[MAX_LINE - 1] = '\0';
                                t->input_len = strlen(t->input_line);

                                while (t->input_len > 0 &&
                                    (t->input_line[t->input_len-1] == '\n' ||
                                        t->input_line[t->input_len-1] == '\r')) {
                                    t->input_line[--t->input_len] = '\0';
                                }

                                execute_command(t);

                                t->multiline_len = 0;
                                t->multiline_buf[0] = '\0';
                                t->input_len = 0;
                                t->input_line[0] = '\0';
                            }
                        } else if (keysym == XK_BackSpace) {
                        if (t->input_len > 0) {
                            t->input_len--;
                            t->input_line[t->input_len] = '\0';
                        }
                    } else if (keysym == XK_Tab) {
                        // Switch tab with Tab key
                        current_tab = (current_tab + 1) % num_tabs;
                    } else if (len > 0 && buf[0] >= 32 && buf[0] < 127) {
                        if (t->input_len < MAX_LINE - 1) {
                            t->input_line[t->input_len++] = buf[0];
                            t->input_line[t->input_len] = '\0';
                        }
                    }
                    redraw(display, window, gc, scr);
                }
            }
        }

        // Check background pipes for child output
        for (int i = 0; i < num_tabs; i++) {
            if (tabs[i].pipe_fd != -1 && FD_ISSET(tabs[i].pipe_fd, &rfds)) {
                char read_buf[256];
                int n = read(tabs[i].pipe_fd, read_buf, sizeof(read_buf) - 1);
                if (n > 0) {
                    read_buf[n] = '\0';
                    append_output(&tabs[i], read_buf);
                } else {
                    close(tabs[i].pipe_fd);
                    tabs[i].pipe_fd = -1;
                    int status;
                    waitpid(tabs[i].pid, &status, 0);
                    if (WIFEXITED(status)) {
                        tabs[i].last_exit_status = WEXITSTATUS(status);
                    } else {
                        tabs[i].last_exit_status = 1;
                    }
                    tabs[i].pid = 0;
                }
                redraw(display, window, gc, scr);
            }
        }
    }

cleanup:
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}