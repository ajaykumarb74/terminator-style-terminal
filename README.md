# Terminator: A Custom Terminal

A Linux terminal-emulator prototype written in C using X11, Xft, POSIX pipes,
and child processes.

## Build dependencies

On Ubuntu/Mint:

```bash
sudo apt update
sudo apt install -y build-essential git make libx11-dev libxpm-dev \
  libxrender-dev libxft-dev libfontconfig1-dev pkg-config
```

## Build

```bash
make clean
make
```

## Run

```bash
./terminator
```

The default launch selects thread-backed tab shells. To select process-backed
tab shells explicitly:

```bash
./terminator useProcess=True
```

The GUI owns the X11 connection. In tab-worker modes it polls the X11
connection and worker-output pipes so command execution in a worker does not
intentionally block tab switching.

## Current controls

- Enter: execute a command
- Backspace: delete the last input character
- Escape: clear the input line
- Ctrl+T: open a tab
- Ctrl+W: close the active tab
- F1–F4: switch tabs
- F5: cycle the wallpaper

## Scope and known gaps

This branch is an implementation baseline, not a claim of full compliance
with the CS69201 term-project specification. The following requirements still
need implementation and testing before the project can be called complete:

- Process-shared command history with process-shared synchronization
- Full shell quoting, variable expansion, nested command substitution, and
  multiline semantics
- `multiWatch` with concurrent command output and Ctrl+C cancellation
- Configurable persistent key bindings, cursor editing, history navigation,
  and X11 selection copy/paste
- Complete job control (`&`, `jobs`, `fg`, `bg`, Ctrl+C, Ctrl+Z)
- `source`/script execution and reliable per-tab exit status semantics
- End-to-end testing of tab isolation, cancellation, pipe/redirection edge
  cases, and clean shutdown

Do not treat a successful compile as proof that all project requirements pass.
