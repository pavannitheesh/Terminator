# Terminator — Design and Implementation

## 1. System overview

Terminator is divided into two cooperating parts:

1. **X11 GUI:** owns the display connection, window, drawing context, keyboard/mouse event handling, tab presentation and user-visible output buffers.
2. **Per-tab execution session:** owns the shell state for one tab, parses submitted commands, starts child processes, monitors jobs and sends output/state updates back to the GUI.

The central design rule is that **only the GUI process/thread touches Xlib**. Execution sessions do not draw. They send framed messages to the GUI, which updates the corresponding tab and redraws the window.

```text
                    X11 window
                        |
                 X11 event loop
                        |
               GUI / tab management
             (input, drawing, buffers)
                        |
             command and signal frames
                        |
             +----------+----------+
             |                     |
          Tab 1 session          Tab 2 session
             |                     |
       parser / built-ins    parser / built-ins
             |                     |
       jobs / multiWatch      jobs / multiWatch
             |                     |
       fork + execvp          fork + execvp
             |                     |
        child processes        child processes

          Shared command history across tabs
             (mutex or shared-memory semaphore)
```

The implementation supports at most **5 tabs** (`MAX_TABS`).

## 2. X11 GUI and event loop

### 2.1 Window setup

At startup, `main()`:

1. Parses the command-line option `useProcess=...`.
2. Initialises shared history.
3. Opens the X server connection with `XOpenDisplay(NULL)`.
4. Creates an 800 × 500 X11 window using `XCreateSimpleWindow`.
5. Registers the window title and `WM_DELETE_WINDOW` protocol.
6. Selects exposure, structure, keyboard, button and pointer-motion events.
7. Maps the window and creates a graphics context.
8. Loads key bindings, loads persistent history and creates the first tab/session.

If `XOpenDisplay()` fails, the program prints `Unable to connect to X11` and exits.

### 2.2 Event loop without busy-waiting

The GUI obtains the X11 connection descriptor using `ConnectionNumber(display)`. It then uses `select()` to wait for activity on:

- the X11 connection; and
- each tab session's output pipe.

A timeout is used to blink the input caret. When the X11 descriptor is ready, pending events are consumed using `XPending()` and `XNextEvent()`.

Important event handling includes:

- `KeyPress`: converts the key event using `XLookupString()`, then performs editing, history, completion, tab or signal actions.
- Mouse events: support tab interaction and text selection.
- Expose/resize events: redraw the window.
- Selection events: implement X11 clipboard copy/paste.
- Window-manager close: initiates cleanup.

The GUI event loop does not monitor every command's output pipe directly. It monitors one session-output pipe per tab. The session itself monitors its child-job and `multiWatch` pipes.

### 2.3 Drawing and per-tab display

Each `Tab` stores its own input line, cursor, output buffer, current directory, scroll position and interaction state. `redraw()` draws the tab bar, visible output, prompt and input line.

Output is retained in a bounded buffer (`MAX_OUTPUT`). When the buffer fills, older bytes are shifted out to make room for new output. Long output is wrapped to the available display width.

## 3. Tab state and execution sessions

The GUI keeps tab presentation state in `tabs[MAX_TABS]` and execution-session pointers in `sessions[MAX_TABS]`.

A `TabSession` contains:

- execution mode (`EXEC_THREAD` or `EXEC_PROCESS`);
- a private shell-state `Tab`;
- the associated GUI tab;
- command and output pipe descriptors;
- input/output frame buffers;
- thread/process identifiers;
- lifecycle flags such as `alive`, `busy`, `closing` and `dead`.

The session has its own shell-state copy. This is important because the shell's job table, current directory, input-independent execution state and `multiWatch` state belong to that tab.

### 3.1 Thread mode

Run with:

```bash
./terminator
```

In thread mode:

- The GUI and all tab sessions live in one process.
- A new tab session is started with `pthread_create()`.
- Each session runs `session_run_loop()` in its own thread.
- The session and GUI exchange messages through pipes.
- Shared history is allocated in process memory and protected by a `pthread_mutex_t`.

Threads share the process address space, but the implementation avoids using the process-wide current working directory for tab navigation. Each tab keeps its own `cwd` string, and commands are launched after the child calls `chdir(tab->cwd)`.

### 3.2 Process mode

Run with:

```bash
./terminator useProcess=True
```

In process mode:

- The GUI remains in the parent.
- Creating a tab calls `fork()` to create a child process for that tab's session.
- The child runs `session_run_loop()` and keeps only its own communication pipe ends.
- The parent retains the GUI-side pipe ends and monitors session output.
- Shared history is allocated using `shm_open()` and `mmap(..., MAP_SHARED, ...)`.
- A process-shared POSIX semaphore (`sem_init(..., pshared=1, ...)`) protects history access.

The shared-memory object is unlinked after mapping, so the mapping is intended for the lifetime of the current run.

### 3.3 Why the modes behave similarly

Both modes use the same session loop and the same command execution functions. Only the mechanism that hosts a session differs: a pthread or a forked child. The GUI/session protocol remains pipe-based in both modes.

Closing one tab closes its session communication and joins the thread or waits for the child process. Other tabs have separate sessions and separate job tables.

## 4. GUI/session IPC protocol

Each session is connected to the GUI using **two unidirectional pipes**:

```text
GUI -- command/signal frames --> session
GUI <-- output/state frames --- session
```

The GUI writes to `to_sess`; the session reads from `sfds[0]`. The session writes to `sfds[1]`; the GUI reads from `from_sess`.

Messages use a small frame format with a header containing two integers (message type and payload length), followed by the payload. The implementation accumulates partial reads in buffers because a pipe read is not guaranteed to return a complete frame.

The message types are:

**GUI to session**
- `C_RUN`: execute a submitted command line.
- `C_SIG`: deliver a signal such as `SIGINT` or `SIGTSTP`.
- `C_CLOSE`: ask the session to close.

**Session to GUI**
- `F_OUT`: append output text to the tab.
- `F_CWD`: update the GUI's displayed working directory/title.
- `F_BUSY`: indicate whether the session currently has a foreground job or active `multiWatch`.
- `F_QUIT`: request tab closure after the user runs `exit`.
- `F_DELEGATE`: ask the GUI to handle a GUI-owned command such as `bind`.

This separation keeps Xlib operations in the GUI while allowing sessions to run independently.

## 5. Command parsing and execution

### 5.1 Command-line flow

When the user presses Enter on a complete input:

1. The GUI sends the line to the selected tab's session using `C_RUN`.
2. The session echoes the prompt and command into its output stream.
3. `execute_command()` checks built-ins.
4. If the command is external, it is passed to `execute_pipeline()`.
5. The session monitors the resulting job, drains its output pipe and reaps exited children.
6. Output and state updates are sent back to the GUI.

### 5.2 Parsing

The implementation uses small parsing helpers:

- `split_pipeline()` separates top-level `|` characters while respecting single quotes, double quotes and escaped characters.
- `parse_redirections()` extracts unquoted `<` and `>` targets.
- `shell_split()` converts a command segment into an `argv` array and handles basic single-quote, double-quote and backslash grouping.
- `expand_globs()` expands wildcard arguments relative to the tab's own working directory.

This is a purpose-built subset parser, not a full Bash parser.

### 5.3 Pipeline creation

For a pipeline of `N` commands, `execute_pipeline()` creates `N - 1` pipes and forks one child for each stage.

For example:

```bash
cat input.txt | sort | head -5
```

has three child processes and two inter-stage pipes:

```text
cat input.txt --pipe 1--> sort --pipe 2--> head -5
```

All stages are placed in one process group. The first child's PID is used as the process-group ID. This lets the shell signal the entire pipeline together.

The last stage's standard output is connected to a capture pipe. Standard error from each stage is also sent to the capture pipe, so the session can display errors in the tab.

### 5.4 Redirection and `dup2()`

Before forking, the parent opens redirection files:

- `< file` opens the file read-only.
- `> file` opens/creates the file for writing and truncates it.

In each child:

- `dup2(input_fd, STDIN_FILENO)` connects redirected input.
- Otherwise, a previous pipeline pipe may become standard input.
- `dup2(output_fd, STDOUT_FILENO)` connects redirected output.
- Otherwise, a next pipeline pipe or the final capture pipe becomes standard output.
- `dup2(capture_fd, STDERR_FILENO)` routes standard error to the tab.

The child closes inherited descriptors it no longer needs, restores default signal dispositions and calls `chdir(tab->cwd)`. It then calls `execvp()` to replace itself with the requested program. If `execvp()` fails, an error is printed and the child exits with status 127.

### 5.5 Job table and reaping

Each tab has a fixed-size `Job` array. A job records:

- whether the slot is in use;
- the last pipeline-stage PID and the process-group ID;
- the captured-output descriptor;
- the original command text;
- the number of live pipeline stages;
- stopped/background state and exit status.

The session uses non-blocking reads to drain job output. `waitpid()` with `WNOHANG` and `WUNTRACED` detects exits and stops without blocking the session loop. When all pipeline stages have exited, the job is completed and its capture descriptor is closed.

## 6. Built-ins and tab-local state

The command dispatcher implements these built-ins:

| Built-in | Purpose |
|---|---|
| `cd` | Update only the selected tab's working-directory string |
| `exit` | Ask the session to quit and the GUI to close that tab |
| `history` | Print recent entries from shared history |
| `multiWatch` | Start concurrent periodic commands |
| `bind` | Configure GUI key bindings |
| `jobs` | List the tab's managed jobs |
| `fg` | Continue a job in the foreground |
| `bg` | Continue a stopped job in the background |
| `source` / `.` | Read and execute non-empty, non-comment lines from a file |

`cd` deliberately does not call process-wide `chdir()` in the session. This avoids one thread-mode tab changing another tab's directory. Instead, relative paths for command execution, redirection and wildcard expansion are resolved against the tab's `cwd`.

## 7. Job control and signal handling

The GUI receives keyboard events, so it handles `Ctrl+C` and `Ctrl+Z` itself rather than relying on a terminal driver's line discipline.

- If the selected session is busy, the GUI sends a `C_SIG` frame.
- The session identifies its foreground job and signals its process group with `kill(-pgid, signal)`.
- `SIGINT` interrupts the foreground job.
- `SIGTSTP` suspends the foreground job.
- `jobs`, `fg` and `bg` operate on the selected tab's own job table.

The GUI ignores `SIGPIPE` so a session writing after the GUI has closed its read end does not terminate the GUI process. Child processes restore default signal dispositions before `execvp()` so they respond normally to job-control signals.

## 8. Shared command history

History is shared across all tabs and persisted in:

```text
~/.terminator_history
```

The `History` structure contains:

- `entries[HISTORY_FILE_MAX][MAX_LINE]`: command strings;
- `count`: number of stored commands;
- `base`: count of entries dropped from the front;
- a mutex for thread mode;
- a process-shared semaphore for process mode.

The implementation retains up to **10,000** entries and the `history` built-in prints up to the most recent **1,000**.

Every history read/write is protected by `history_lock()` and `history_unlock()`. These select a pthread mutex or POSIX semaphore according to the execution mode.

### Ctrl+R search

The search order is:

1. Search backwards for the most recent exact match.
2. If no exact match exists, calculate the longest common contiguous substring against each stored command.
3. Print commands whose score equals the best score, provided that score is greater than 2.
4. Otherwise print `No match for search term in history`.

The longest-common-substring calculation uses dynamic programming with two rows.

## 9. Filename auto-completion

The `Tab` structure stores the completion candidates, directory prefix, candidate types and the word range being replaced.

When Tab is pressed:

1. Find the word immediately before the input cursor.
2. Separate any directory prefix from the basename.
3. Open the target directory, resolved relative to the tab's `cwd` when necessary.
4. Collect entries whose names begin with the basename prefix.
5. Hide dotfiles unless the typed basename begins with `.`.
6. Sort candidates.
7. If one candidate exists, complete it.
8. If several exist, extend the input to their longest common prefix.
9. If ambiguity remains, display a numbered list and wait for a digit selection.

The implementation stores at most `MAX_MATCHES` candidates and displays at most nine numbered choices.

## 10. `multiWatch`

`multiWatch` starts the requested commands concurrently. Each command has its own `WatchCmd` state and output pipe. The session waits on the command pipes with `select()` rather than spinning continuously.

When a command finishes, it is scheduled to run again after `WATCH_INTERVAL` seconds (2 seconds by default). Output is printed in a labelled block containing the command, a Unix timestamp and separator lines.

When `Ctrl+C` is received, the session stops the watched child processes, closes their pipes and returns to the prompt.

## 11. Multiline input and quoting

The GUI's `input_incomplete()` check scans the accumulated input and tracks whether it is inside single or double quotes. It also recognises a trailing backslash as a continuation marker.

If the input is incomplete after Enter, the GUI keeps the accumulated command and shows a continuation prompt. When the input is considered complete, it is sent to the tab session.

The command tokenizer handles basic single quotes, double quotes and backslash escapes. This is not equivalent to complete Bash quoting or expansion semantics.

## 12. Key bindings and clipboard

Default bindings are installed at startup. The program then reads `~/.terminatorrc`, parses `bind <key> <action>` directives and applies valid bindings. Conflicting or invalid directives are reported; defaults are retained for those entries.

The `bind` built-in updates the GUI-owned binding table and writes user bindings back to the configuration file. In a session, `bind` is delegated to the GUI over the session protocol because the GUI owns keyboard handling.

Clipboard integration uses X11 selection atoms for `CLIPBOARD`, `UTF8_STRING` and `TARGETS`. Copy publishes the selected text; paste requests clipboard data and inserts it into the input line.

## 13. Cleanup and failure handling

On application shutdown, the GUI:

1. asks every session to close;
2. joins threads or waits for session child processes;
3. closes communication descriptors and frees session state;
4. saves command history;
5. destroys the X11 window and closes the display.

When a session exits, it also terminates its managed jobs and `multiWatch` children before returning. A user-entered `exit` sends a quit notification so the GUI can close only that tab (or the application if it was the last tab).

## 14. Fixed limits and known limitations

The implementation uses fixed-size arrays and buffers. Important limits include:

| Resource | Limit in source |
|---|---:|
| Tabs | 5 |
| Pipeline stages | 16 |
| Arguments per command | 64 |
| Jobs per tab | 32 |
| `multiWatch` commands | 16 |
| History entries | 10,000 |
| History display | 1,000 |
| Completion candidates | 64 |
| Input line | 256 characters |

Other limitations to document honestly:

- This is not a complete Bash or POSIX shell implementation.
- `source` currently executes ordinary lines sequentially; it does not implement the full conditional/script grammar in the project specification.
- General variable expansion, positional script arguments and command substitution are not implemented by the shown command parser.
- The multiline check handles quote/backslash continuation only; it is not a complete parser for nested substitutions or shell syntax.
- The command parser supports a limited set of quoting and redirection forms.
- X11 is required; the application is not a native Wayland terminal.

These limitations should be revisited if the implementation is extended to meet every feature in the original project specification.
