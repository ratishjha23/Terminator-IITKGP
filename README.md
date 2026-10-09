# Terminator - A Custom Terminal

Terminator is a custom Linux terminal and shell implemented in C as part of the IIT Kharagpur M.Tech CSE Computing Lab project.

The project is designed to explore Linux systems programming and operating-system concepts by building a functional terminal from the ground up.

## Project Goals

The main goal is to implement a terminal and shell with features such as:

- Command execution
- Process creation and management
- Pipes and I/O redirection
- Signals and job control
- Inter-process communication (IPC)
- Threads and synchronization
- Command history
- Auto-completion
- Basic shell scripting
- X11-based graphical interface

## Technologies

- C
- Linux
- X11
- GCC
- Make
- GDB

## Current Status

### Completed
- Basic project structure and development environment
- Build system using Make
- Git/GitHub version control setup
- Basic X11 window creation and event handling
- Shell prompt and command parsing
- External command execution using fork() and execvp()
- Child-process waiting and exit-status handling
- Basic built-in commands: cd and exit

### In Progress
- Integration of shell core with X11 terminal GUI
- Pipes and I/O redirection
- Signals and job control
- Command history and auto-completion
- Threads, synchronization, and IPC
- Basic shell scripting
## Project Structure

```text
Terminator/
├── docs/
├── include/
├── src/
├── tests/
├── .gitignore
├── Makefile
└── README.md