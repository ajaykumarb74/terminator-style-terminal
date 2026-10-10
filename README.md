
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
- X11 development library (`libx11-dev`)

On Ubuntu, install the dependencies using:

```bash
sudo apt update
sudo apt install -y build-essential git make libx11-dev
```

## Build

```bash
make
```

## Run

```bash
./terminator
```

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
