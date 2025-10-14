# C2 Shell Project for System Device Programming 2025

## Summary

The purpose of this project is to support running multiple processes at once from actual compiled programs sotred on disk. These programs will be loaded into OS161 and executed in user mode, under the control of your kernel and the command shell in *bin/sh*.
The project is highly based on the availability of the *execv* and *dup2* system calls, and also limited to the *EMUFS* emulated file system.

## Environment

The basic implementation of OS161 does not provide full support for running executables, and the key files that are responisble for the loading and running of user-level programs are *loadelf.c*, *runprogram.c*, and *uio.c*. Understanding these files is the key to getting started with the implementation of multiprogramming.

### *kern/syscall/loadelf.c*

This file contains the functions responsible for loading and ELF executable from the filesystem and into virtual memory space. ELF is the format of executable files produced by *os161-gcc*. In addition, the minimal implementation does not provide the actual *virtual memory* although there is a translation between the adresses that executables "believe" they are using and physical adresses, there is no mechanism for providing more memory than exists physically.

### *kern/syscall/runprogram.c*

Contains only the function *runprogram*, which is responsible for running a program from the kernel menu. From here, the *execv* system call is implemented together with customization of the *runprogram* function and also additional needed logics to start a program and give it the correct standard file descriptors available while running.

### *kern/lib/uio.c*

File containing the logic for moving data between kernel and user space, which is critical to properly implement user level program.

## Traps and Syscalls
Exceptions play a crucial role in operating systems — they provide the mechanism that allows the OS to regain control of execution and perform its core functions. In essence, exceptions act as the interface between the processor and the operating system.

When the OS boots, it installs an exception handler, a carefully designed piece of assembly code located at a specific address in memory. Whenever the processor raises an exception, it triggers this handler. The handler then sets up a trap frame and transfers control to the operating system.

Because the term exception can mean many things in computer science, OS terminology often refers to exceptions as traps. Both interrupts and system calls fall under this category of traps. In particular, system calls are handled in the *syscall/syscall.c* file, which manages traps that occur due to system call instructions.

To deepen your understanding of how operating systems work, it’s highly recommended to explore and read through the C code in this directory — it’s an essential step toward becoming a true operating systems enthusiast.

### *locore/trap.c*

*mips_trap* is the key function for returning control to the OS, which is called by the assembly exception handler. *enter_new_process* is the key function for returning control to user programs. *kill_curthread* handles broken user programs, for example, when the processor hits something it cannot handle in usermode, it raises an exception. Since there is no way to recover from this, the OS needs to kill the process.

### *syscall/syscall.c*

*syscall* function is used to delegate the actual work of a system call implemented by the kernel. Here all the system call for the purpose of this project will be placed.

### *user/lib/crt0/mips*

This is the user program startup code. The only file here is the *crt0.S*, which contains the MIPS assembly code that receives control first when a user-level program is started. It calls the user program's *main*.

### *user/lib/libc*

This is a user-level C library, which implements the user-level side of system calls.

### *user/lib/libc/unix/errno.c*

This is where the global variable *errno* is defined.

### *user/lib/libc/arch/mips/syscalls-mips.S*

Contains the machine-dependent code necessary for implementing the user-level side of MIPS system calls.

### *syscalls.S*

This file is created ad compile time from *syscalls-mips.S*, where the actual names of the system calls are placed within the file using a script called *syscalls/gensyscalls.sh*. OS161 puts all the syscalls together to simplify the makefiles instead of having each of them placed as a stub in source files.

## Problem Definition

Implement system calls for file, process mangement and exception handling. The list of all defined system calls can be found in the file *kern/include/kern/syscall.h*.

* open, read, write, lseek, close, dup2, chdir, getcwd
* getpid
* fork, execv, waitpid, _exit

All the system call handle errors as described in the manual pages from the OS161 distribution. This is done to be compliant with the grading scripts that rely on the appropriate return value and error codes, which is as important as the correctness of the implementation.

For later developed system calls the *user/include/unistd.h* file contains the user-level interfaces, which is different from the ones for the kernel functions. The defined interface is present in the *kern/include/syscall.h* and the paired integer value inside the *kern/include/kern/syscall.h*.

"Can two different user-level processes find themselves running a system call at the same time?"
TODO: add the choice here and documentation

The file descriptors *0, 1, and 2* are always linked to *standard input (stdin)*, *standard output (stdout)*, and *standard error (stderr)*, respectively. These file descriptors start out attatched to the console device but by using *dup2* it is possible to allow programs to change them to point somewhere else.

A large part of this assignment is designing and implementing a system to track the state of file descriptors that are manipulated by the system calls.

### *getpid*

Each process is identified by a unique number, also called Process ID (PID). The implementation of this function allows the system to allocate and reclaim PIDs. This ensures that with a long execution PIDs do not expire by using all the possible space for them.

### *fork, execv, waitpid, _exit*

These system calls enable multiprogramming and make OS161 a much more useful entity.

*fork* creates a new process by creating a copy of the invoking process and makes sure that the parent and child processes observe the correct return value, which is 0 for the new child process and the newely created pid for the parent. 

*execv* is a fundamental system call that allows newly created process to execute something different from what the parent is doing. Essentially, replaces the existing address space with a brand new one for the new executable and then run it.

### *kill_curthread*

A simple implementation of this system call that enables to clear the space used by the current thread, keeping in mind that the userspace state can not be trusted if it has suffered a fata exception.

## Testing

Inside *user/bin/sh* there is a shell that allows the user to test the system calls. When executed, it prints a prompt and allows the developer to type simple commands to run other programs. 