
# Terminator: A Custom Terminal

A custom terminal emulator and shell written in C for Linux using X11.

## Project status

Task 1: Initial X11 graphical interface and basic command execution.

This is a work in progress. Advanced shell features will be implemented
incrementally.

## Dependencies

- GCC
- GNU Make
- Git
- X11 development libraries (`libx11-dev`, `libxpm-dev`, `libxrender-dev`)
- Xft/fontconfig development files (`libxft-dev`, `libfontconfig1-dev`, `pkg-config`)

On Ubuntu, install the dependencies using:

```bash
sudo apt update
sudo apt install -y build-essential git make libx11-dev libxpm-dev libxrender-dev libxft-dev libfontconfig1-dev pkg-config
```

## Build

```bash
make
```

## Run

Default mode (existing behavior):

```bash
./terminator
```

Optional process mode (one shell-engine child process per tab):

```bash
./terminator useProcess=True
```

In process mode, the GUI polls the X11 connection and tab-output pipes so waiting for a shell command does not block X event handling. Each tab keeps its own working directory and command history. Wallpaper changes remain a GUI action (F5) in this mode.

## Current controls

- Enter: execute a basic command
- Backspace: delete a character
- Escape: clear the input
- Ctrl+T: open a tab
- Ctrl+W: close a tab
- F1-F4: switch tabs

## Planned features

- Built-in shell commands
- Multiline input and quoting
- Input/output redirection
- Pipelines
- multiWatch
- Configurable keyboard shortcuts
- Signal handling and job control
- Shared searchable command history
- Filename auto-completion
- Bash-script interpretation
- Thread mode and process mode

## Limitations

The current implementation is a prototype. It does not yet implement
all assignment requirements.
# terminator-style-terminal
A custom terminal emulator and shell in C using X11 and POSIX system calls
