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

---
---

# Implementation

## *C2_syscalls.h/c*

This two files are used to implement the syscalls required for the purpose of this project. This was done for making it easier to keep track of the progress of this project.

The system calls are implemented following the documentation provided by OS161 under the directory *man* of this project. Notice that a few adjustments were made since the documentation is reporting the user level explanation while the system call are implemented at kernel level.

Before starting the explanation of the system calls implemented in kernel side there are a few key points that we are going to discuss; the first regarding the signature of the system calls, in the *OS161* documentation the parameter *returnVal* does not appear since the user-level system call only needs the result while the kernel side needs both the return value and the error code (0 on success and the actual error code when an error occurs); the second is the usage of *userptr_t* that we introduced to separate what is in kernel and user space. The user-level system calls pass pointers to user meory, for example *const char \**. When these calls reach the kernel, the same address cannot be treated as a valid kernel pointer, that is why we use *userptr_t* type to mark user-space addresses, ensuring that the kernel accesses them safely.

### *sys_open*
Opens the file, device, or other kernel object named by the *pathName*. The *openFlags* parameter, specifies how to open the file, while *modeFile* is the optional argument that specifies in Unix which mode will the file be opened.

* **userptr_t pathName** 
* **int openFlags** 
  - *O_RDONLY*: open for reading only
  - *O_WRONLY*: open for writing only
  - *O_RDWR*: open for reading and writing
* **mode_t modeFile**
  - *O_CREAT*: create the file if it does not exist
  - *O_EXCL*: fail if the file already exists
  - *O_TRUNC*: truncate the file to length 0 upon open
  - *O_APPEND*: open the file in append mode
* **int32_t \*returnVal**

> The mode and the flags can be combined to achieve specific behaviors. For example, using O_EXCL together with O_CREAT allows the system to atomically check whether a file exists and create it if it does not. This guarantees that the creation operation is atomic with respect to other processes executing open with the same filename and both O_EXCL and O_CREAT set. If O_CREAT is not specified, the effect of O_EXCL is undefined.

The implementation starts with the validation of input arguments, ensuring that *pathName* pointer is not *NULL*, if it is *EFAULT* error is returned.
After this check a kernel buffer *kernB* is allocated to store a copy of the filename using the function *copyinstr*, while also ensuring that no errors are returned.
Then the function *vfs_open* is called to open the file and return a pointer to its corresponding *vnode*. Once the check for errors is done, the kernel buffer used to copy the name of the file gets freed, as it is no longer needed.
If the file is correctly opened a cycle into the *systemFileTable* makes sure that the new open file *openF* struct is stored in a free slot of this table. In case of a full table the *ENFILE* error is returned.
To allocate a slot in the process file table a search starts with the id 3 since the first three, 0, 1, and 2 are reserved for standard I/O. If the process table is fulle the *EMFILE* error is returned. In case of finding a free slot, the vnode is associated with the process, the offset is set, the reference count *countRefs* is initialized to 1, and the access mode is determined based on a mask betwee *openFlags* & *O_ACCMODE*. A pre-lock is also created for synchronization across concurrent accesses, if it fails all allocated resources are released, and *ENOMEM* error is returned.
Finally the new file descriptor is copied into *returnVal* and the function returns 0 for the success.

### *sys_close*
The file handle fd is closed. Other file handles are not affected in any way, even if they are attatched to the same file.

* **int fd**

The first step is the validation of the file descriptor, it cannot be a negative value or greater than the max valid *fd* for a file. If it does not correspond to this range the *EBADF* error is returned. The same happens if the *fd* corresponds to a file that is not open.
Once the *fd* is checked the corresponding entry in the process's file table is cleared, ensuring that the process can no longer access the file using that *fd*. However, the underlying file structure may still be in unse by other processes or file descriptors that it (for example, after a *fork*).
The function acquires the file's internal lock to safely update shared fields, such as the reference count that gets decremented to check whether some one is still using it or not. If the file is still in use it returns 0, otherwise the *vnode* associated with the file is closed using *vfs_close*. Once called, *vfs_close*, decrements the vnode's reference count and may trigger cleanup at the file system level.
After that, the kernel frees the memory allocated for the *struct openfile*, and returns 0 if everything was successful.

### *sys_read*
Reads up to *bufLen bytes* from the file specified by *fd*, at the location in the file specified by the current seek position of the file, and stores them in the space pointed to by *buffer*. The file must be open for reading otherwise an error occurs.

* **int fd**
* **userptr_t buffer**
* **size_t bufLen**
* **ssize_t \*returnVal**

The first step is the validation of the file descriptor. The file descriptor *fd* cannot be a negative value or greater than the maximum valid fd defined by the system. If the value is outside this range, the function returns the *EBADF* error code. The same happens if the *fd* corresponds to a file that is not currently open in the process’s file table.
Once the *fd* is validated, the corresponding openfile structure is retrieved from the current process’s file table. This structure contains important information about the file, such as the current offset, access mode, and vnode pointer. If the *vnode* is *NULL*, the file cannot be accessed and the function again returns *EBADF*.
After obtaining a valid file reference, the function prepares the necessary kernel data structures for the read operation, it initializes a *uio* and an *iovec* structure using the helper function *uio_kinit*.
These structures define the kernel buffer *kBuffer*, the length of the data to read, and the current offset within the file.
In this case, a temporary kernel buffer *kBuffer* of size 128 is used to store the data read from the file before copying it to user space.
The actual reading is performed by calling the file system operation *VOP_READ*, passing it the file’s *vnode* and the *uio* structure.
This function reads up to *bufLen* bytes from the file, starting at the current offset, and stores the result into the kernel buffer.
Since *VOP_READ* may fail , for example, due to I/O errors or permission issues, the return value is checked and any error is immediately propagated back to the caller.
Once the read operation completes successfully, the number of bytes actually read is calculated by subtracting the remaining bytes *uio_resid* from the requested length.
The kernel then uses *copyout* to safely transfer this data from the kernel buffer into the user-provided buffer in user space. If an invalid user memory address is encountered during this step, the function returns an appropriate error, such as *EFAULT*.
After the data is successfully copied, the file offset is updated to reflect the new position in the file, ensuring that subsequent reads continue from where the last one ended. The number of bytes read is stored in returnVal, which is then passed back to the user program. If all steps succeed, the function returns 0, indicating a successful operation.

### *sys_write*
Writes up to *bufLen bytes* to the file specified by *fd*, at the location in the file specified by the current seek poistion of the filem taking the data from the space pointed to by *buffer*. The file must be open for writing, ensuring no error is triggered during the process. The current seek position of the file is advanced by the number of bytes written.
Each write/read operation is atomic relative to other I/O to the same file. 

* **int fd**
* **userptr_t buffer**
* **size_t bufLen**
* **ssize_t \*returnVal**

The first step is the validation of the file descriptor. The file descriptor *fd* cannot be a negative value or greater than the maximum valid descriptor *OPEN_MAX*. If it is outside this range, the function immediately returns the *EBADF* error code, indicating an invalid or non-existent file descriptor. The same error is also returned if the *fd* does not correspond to an open file in the process’s file table.
Once the *fd* has been validated, the corresponding entry is retrieved from the current process’s file table. This entry, represented by a *struct openfile*, provides access to the file’s vnode, offset, access mode, and internal lock. If this entry is *NULL*, it means the file has not been opened, and the function again returns *EBADF*.
Next, the function safely copies the data from user space into the previously allocated kernel buffer *kBuffer* using the *copyin* function.
This prevents the kernel from directly accessing user memory and avoids invalid memory accesses that could lead to kernel crashes. If the copy fails, for exmaple, if the user buffer points to invalid memory, the function frees the allocated kernel buffer and returns the corresponding error code.
Once the data is securely copied into the kernel, the function initializes two structures, *iovec* and *uio*, using *uio_kinit*.
These structures describe the I/O operation for the VFS layer, including the buffer address, data length, file offset, and operation type, UIO_WRITE in this case.
The actual write operation is then performed through the VFS interface by calling *VOP_WRITE*.
This function writes up to *bufLen* bytes from the kernel buffer into the file represented by the *vnode*.
If an error occurs during this stage, for example, due to permission issues or device errors, the function immediately returns the error code from *VOP_WRITE*.
If the operation completes successfully, the number of bytes written is calculated as the difference between the requested size and the remaining bytes *uio_resid*.
The file offset is then advanced by this number, ensuring that subsequent writes continue from the correct position within the file.
Finally, the number of bytes successfully written is stored in *returnVal*, which is returned to the user process. If all steps succeed, the function returns 0 to indicate success.

### *sys_lseek*

* int fd
* off_t pos
* int whence
* off_t *returnVal

### *sys_dup2*

* int olfFd
* int newFd
* int *returnVal

### *sys_chdir*

* userptr_t pathName

### *sys_getcwd*

* userptr_t buffer
* size_t bufLen
* int *returnVal