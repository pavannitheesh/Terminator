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
    int i = 0;
    int in_sq = 0, in_dq = 0;

    while (i < len) {
        char c = s[i];

        if (in_sq) {
            if (c == '\'') in_sq = 0;
            i++;
            continue;
        }

        if (c == '\\') {
            if (i + 1 >= len) return 1;   /* trailing backslash -> continue */
            i += 2;
            continue;
        }

        if (in_dq) {
            if (c == '"') in_dq = 0;
            i++;
            continue;
        }

        if (c == '\'') { in_sq = 1; i++; continue; }
        if (c == '"')  { in_dq = 1; i++; continue; }

        i++;
    }
    return (in_sq || in_dq) ? 1 : 0;
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
    char prompt_str[MAX_LINE + 128];
    snprintf(prompt_str, sizeof(prompt_str), "user@terminator> %s\n", tab->input_line);
    append_output(tab, prompt_str);

    // Parse arguments
    char *args[64];
    int argc = 0;
    char cmd_copy[MAX_LINE];
    strcpy(cmd_copy, tab->input_line);

    char *token = strtok(cmd_copy, " \t\n");
    while (token && argc < 63) {
        args[argc++] = token;
        token = strtok(NULL, " \t\n");
    }
    args[argc] = NULL;

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
        // External command via fork + execvp + pipe
        int pipefds[2];
        if (pipe(pipefds) < 0) {
            perror("pipe");
            tab->last_exit_status = 1;
        } else {
            pid_t pid = fork();
            if (pid < 0) {
                perror("fork");
                close(pipefds[0]);
                close(pipefds[1]);
                tab->last_exit_status = 1;
            } else if (pid == 0) {
                // Child process
                dup2(pipefds[1], STDOUT_FILENO);
                dup2(pipefds[1], STDERR_FILENO);
                close(pipefds[0]);
                close(pipefds[1]);

                chdir(tab->cwd);
                execvp(args[0], args);

                // If execvp fails
                fprintf(stderr, "terminator: %s: command not found\n", args[0]);
                _exit(127);
            } else {
                // Parent process
                close(pipefds[1]);
                tab->pipe_fd = pipefds[0];
                tab->pid = pid;
            }
        }
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

    // Draw active prompt and current input
    char current_prompt[MAX_LINE + 64];
    snprintf(current_prompt, sizeof(current_prompt), "user@terminator> %s", t->input_line);
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
                        execute_command(t);
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