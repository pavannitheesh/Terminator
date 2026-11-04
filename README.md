# Terminator — A Custom X11 Terminal

**Computing Lab (CS69201) · Term Project**

Terminator is a terminal-like application written in C for Linux. It creates its own graphical terminal window using **X11/Xlib**, accepts commands through that window, launches external programs, and displays their output in independent tabs.

Unlike a conventional terminal emulator that delegates shell behaviour to Bash, Terminator contains its own command parsing and execution code for the features implemented in this project.

## Features

### GUI and tabs
- X11 window created with Xlib.
- Up to **5 tabs**, each with its own input buffer, output buffer, working directory, command state and execution session.
- Open, switch and close tabs; a tab's session continues while another tab is selected.
- Output wrapping, scrolling, mouse text selection and clipboard copy/paste.
- GUI input and output are handled in the X11 window.

### Command execution
- External commands are launched using `fork()` and `execvp()`.
- Pipelines are supported, with a maximum of **16 stages**.
- Input (`<`) and output (`>`) redirection are supported.
- Wildcard expansion is performed for `*`, `?` and `[` patterns.
- Each tab maintains its own working directory. A `cd` in one tab does not change another tab's directory.
- The last command status is stored per tab.

### Job and signal management
- Background execution using `&`.
- `jobs`, `fg` and `bg` built-ins.
- Pipeline stages are placed in a process group so signals can be delivered to the complete pipeline.
- `Ctrl+C` interrupts the active foreground job in the selected tab.
- `Ctrl+Z` suspends the active foreground job in the selected tab.

### History and editing
- Shared command history across tabs.
- History is persisted in `~/.terminator_history`.
- Up/Down browse history; `history` displays recent entries.
- `Ctrl+R` searches for an exact command, or falls back to the longest common substring match.
- Tab completion scans the current tab's working directory, extends a common prefix and displays numbered choices when ambiguity remains.
- Default editing shortcuts include Left/Right, `Ctrl+A`, `Ctrl+E`, `Ctrl+U`, `Ctrl+K`, `Ctrl+W` and `Ctrl+L`.
- Key bindings can be customised with `~/.terminatorrc` and the `bind` built-in.

### Other features
- Multiline input for open quotes and trailing backslashes.
- `multiWatch` runs several commands concurrently, collects their output and restarts completed commands periodically until interrupted.
- `source` / `.` reads a file and executes its non-empty, non-comment lines in the current tab.

## Requirements

- Linux with an X11 display server (or an X server available through your environment).
- GCC or another C compiler.
- GNU Make.
- Xlib development headers and library.
- POSIX threads and POSIX process/shared-memory interfaces.

On Debian/Ubuntu:

```bash
sudo apt update
sudo apt install build-essential libx11-dev
```

If your system requires a separate realtime library for POSIX shared-memory functions, add `-lrt` to the link flags in the Makefile.

## Build

Place the source file in the repository root. The example below assumes it is named `terminator.c`; change `SRC` in the Makefile if your filename differs.

```bash
make
```

A minimal Makefile:

```make
CC       := gcc
CFLAGS   := -Wall -Wextra -O2 -pthread
LDFLAGS  := -lX11 -pthread
# On older Linux toolchains, append -lrt to LDFLAGS if required.

SRC      := terminator.c
TARGET   := terminator

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)
```

## Run

**Thread mode (default):**

```bash
./terminator
```

Each tab gets a separate POSIX thread running its shell session.

**Process mode:**

```bash
./terminator useProcess=True
```

The GUI remains in the parent process and each tab gets a separate child process.

Both modes use the same user-facing command interface.

## Usage examples

```bash
pwd
ls -la
cd ~/Downloads
cat file.txt | wc -l
sort < input.txt
ls > listing.txt
sleep 30 &
jobs
fg %1
bg %1
history
multiWatch ["date +%T" "ls | wc -l"]
source commands.txt
```

## Key bindings

| Key | Action |
|---|---|
| Left / Right | Move cursor in the input line |
| `Ctrl+A` / `Ctrl+E` | Move to beginning / end |
| Up / Down | Browse command history |
| `Ctrl+U` / `Ctrl+K` | Delete to beginning / end |
| `Ctrl+W` | Delete previous word |
| `Ctrl+L` | Clear the screen |
| `Ctrl+D` | Exit the shell/tab |
| `Ctrl+Shift+C` / `Ctrl+Shift+V` | Copy / paste |
| `Ctrl+C` | Interrupt the selected tab's foreground job |
| `Ctrl+Z` | Suspend the selected tab's foreground job |
| `Ctrl+R` | Search command history |
| `Ctrl+T` | Open a tab |
| `Shift+Tab` | Switch tabs |
| `Tab` | Complete a filename |

Custom bindings are read from `~/.terminatorrc`. Example:

```text
bind Ctrl+Shift+C copy
bind Ctrl+Shift+V paste
bind Ctrl+H history-up
```

The action names accepted by the implementation include `left`, `right`, `line-start`, `line-end`, `history-up`, `history-down`, `kill-to-start`, `kill-to-end`, `kill-word`, `clear-screen`, `exit`, `copy` and `paste`. Invalid or conflicting entries are reported and defaults are retained for those entries.

## Important implementation boundaries

This is a course project shell, not a complete POSIX shell or Bash replacement. In particular:

- Pipeline capacity, tab count, argument count and other buffers have fixed limits in the source.
- This application uses X11 and is not a native Wayland terminal.

See [DESIGN.md](DESIGN.md) for the architecture, execution flow, IPC, synchronisation and implementation details.

## Repository layout

A suggested layout:

```text
.
├── terminator.c
├── Makefile
├── README.md
└── DESIGN.md
```

## Project context

Developed as the **Computing Lab (CS69201) term project**. The implementation demonstrates process creation, pipes, file-descriptor redirection, process groups, signals, `select()`, POSIX threads, shared memory, semaphores and X11 event handling.
