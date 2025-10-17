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

## ***C2_syscalls.h/c***

This two files are used to implement the syscalls required for the purpose of this project. This was done for making it easier to keep track of the progress of this project.

The system calls are implemented following the documentation provided by OS161 under the directory *man* of this project. Notice that a few adjustments were made since the documentation is reporting the user level explanation while the system call are implemented at kernel level.

Before starting the explanation of the system calls implemented in kernel side there are a few key points that we are going to discuss; the first regarding the signature of the system calls, in the *OS161* documentation the parameter *returnVal* does not appear since the user-level system call only needs the result while the kernel side needs both the return value and the error code (0 on success and the actual error code when an error occurs); the second is the usage of *userptr_t* that we introduced to separate what is in kernel and user space. The user-level system calls pass pointers to user meory, for example *const char \**. When these calls reach the kernel, the same address cannot be treated as a valid kernel pointer, that is why we use *userptr_t* type to mark user-space addresses, ensuring that the kernel accesses them safely.

### ***sys_open***
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

### ***sys_close***
The file handle fd is closed. Other file handles are not affected in any way, even if they are attatched to the same file.

* **int fd**

The first step is the validation of the file descriptor, it cannot be a negative value or greater than the max valid *fd* for a file. If it does not correspond to this range the *EBADF* error is returned. The same happens if the *fd* corresponds to a file that is not open.
Once the *fd* is checked the corresponding entry in the process's file table is cleared, ensuring that the process can no longer access the file using that *fd*. However, the underlying file structure may still be in unse by other processes or file descriptors that it (for example, after a *fork*).
The function acquires the file's internal lock to safely update shared fields, such as the reference count that gets decremented to check whether some one is still using it or not. If the file is still in use it returns 0, otherwise the *vnode* associated with the file is closed using *vfs_close*. Once called, *vfs_close*, decrements the vnode's reference count and may trigger cleanup at the file system level.
After that, the kernel frees the memory allocated for the *struct openfile*, and returns 0 if everything was successful.

### ***sys_read***
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

### ***sys_write***
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

### ***sys_lseek***
Alters the current seek position of the file handle *fd* seeking to a new position based on *pos* and *whence*. Note that *pos* is a signed quantity, meaning that it is possible to move forward or backward the seek position.
It is not meaningful to seek on certain objects, such as the console device that will eventually fail.
Although, positions less than zero are invalid, positions beyond EOF are legal.
Note that each distinct open of a file should have an independent seek pointer.

* **int fd**
* **off_t pos**
* **int whence**
  - *SEEK_SET*: the new position is *pos*
  - *SEEK_CUR*: the new position is the current position plus *pos*
  - *SEEK_END*: the new position is the position of end-of-file plus *pos*
  - *others*: lseek fails
* **off_t \*returnVal**

The first step in the implementation is the validation of the file descriptor.
The *fd* must be within the valid range, it cannot be negative or exceed the system limit *OPEN_MAX*.
If this condition fails, the function returns *EBADF*, indicating an invalid file descriptor, the same error is also returned if the *fd* does not correspond to an open file in the current process’s *fileTable*.
Once the *fd* has been validated, the corresponding openfile structure is retrieved from the process’s file table.
This structure contains the file’s current offset, access mode, and vnode pointer, which are necessary for determining and updating the new offset.
The *whence* parameter specifies how the new file position should be calculated, *SEEK_SET*, *SEEK_CUR*, or *SEEK_END*.
If *whence* does not correspond to any of these valid constants, the function returns *EINVAL*, indicating an invalid argument.
After computing the new offset, an additional check ensures that the resulting value is not negative.
If *newOff* is less than zero, *EINVAL* is returned again, since negative offsets are not valid for seekable files.
If all validations pass, the file’s offset is updated to the newly calculated position.
The new offset is also stored in *returnVal*, allowing the user process to know the updated file position.
Finally, the function returns 0 to indicate success.

### ***sys_dup2***
Clones the file handle *oldFd* ont the file handle *newFd*. If *newFd* names an open file, that file is closed.
The two handles refer to the same "open" of the file, meaning that they are references to the same object and share the same *seek* pointer. But is not the same thing of opening the same file twice.
*dup2* is most commonly used to relocate opened files onto *STDIN_FILENO*, *STDOUT_FILENO*, and/or *STDERR_FILENO*.

* **int olfFd**
* **int newFd**
* **int \*returnVal**

The first step in the implementation is the validation of both file descriptors *oldFd* and *newFd*.
The *oldFd* and *newFd* must be within the valid range, they cannot be negative or exceed the system limit *OPEN_MAX*.
If this condition fails, the function returns *EBADF*, indicating an invalid file descriptor.
Once both file descriptors have been validated, a check is performed to determine if *oldFd* and *newFd* are the same.
According to the POSIX specification, if they are equal, the function does not perform any duplication and simply returns *newFd* as the result, since both descriptors already refer to the same open file.
After this check, the process’s file table lock is acquired to ensure mutual exclusion while accessing or modifying shared data.
The function retrieves the *oldfile* entry from the current process’s *fileTable*.
If *oldFd* does not correspond to an open file, the entry is *NULL*, the lock is released and the function returns *EBADF*.
If *newFd* already refers to an open file, it must be closed first to comply with the *dup2* behavior.
This is done by calling *file_decref(newfile)*, which decrements the reference count of the file associated with *newFd*.
If the count reaches zero, the file is closed and its resources are released.
The entry in the *fileTable* for *newFd* is then cleared.
Next, the reference count of the *oldfile* is incremented using *file_incref(oldfile)* to account for the new reference created by *newFd*.
The *fileTable* entry for *newFd* is updated to point to the same struct file as *oldFd*, meaning both descriptors now share the same file offset, access mode, and underlying file object.
Finally, the new descriptor value is stored in *returnVal* as *newFd*, the process lock is released, and the function returns 0 to indicate success.

### ***sys_chdir***
The current directory of the current process is set to the directory named by *pathName*.

* **userptr_t pathName**

The first step in the implementation is the validation of the *pathName* parameter.
If pathName is *NULL*, the function immediately returns *EFAULT*, indicating that the user has provided an invalid memory address.
This check ensures that the kernel does not attempt to access memory that belongs to user space without proper validation.
Next, a buffer is allocated in kernel space to hold the path string passed from user space.
The allocation uses *kmalloc* and reserves enough space to accommodate the maximum possible path length *PATH_MAX*.
If the memory allocation fails, the function returns *ENOMEM* to indicate that there is not enough memory available in the kernel.
Once the buffer is successfully allocated, the user-provided path is copied safely from user space into the kernel buffer using *copyinstr*.
This function also ensures that the string is null-terminated and within the specified maximum length.
If *copyinstr* fails, for example, if the user address is invalid, the kernel buffer is freed and the corresponding error code is returned.
After the path has been copied, the function attempts to locate the *vnode* corresponding to the provided directory path using *vfs_lookup*.
If this lookup fails—meaning the directory does not exist or cannot be accessed—the function frees the kernel buffer and returns the appropriate error code from *vfs_lookup*.
Once the *vnode* is successfully retrieved, the buffer is freed since it is no longer needed.
Before changing the current working directory, the function verifies that the *vnode* actually represents a directory and not a regular file or another object type.
This is done by calling *VOP_STAT* to retrieve information about the *vnode* and checking whether the file’s mode includes the *S_IFDIR* flag.
If *VOP_STAT* fails, the *vnode* is closed and the error is returned.
If the *vnode* is not a directory, the function also closes it and returns *ENOTDIR*, signaling that the provided path is not a valid directory.
If all checks pass, the function proceeds to update the current process’s working directory.
The old working directory *vnode* (*curproc->p_cwd*) is closed using *vfs_close*, and the new vnode retrieved from *vfs_lookup* is assigned as the process’s current working directory.
Finally, the function returns 0 to indicate success.

### ***sys_getcwd***
The name of the current directory is computed and stored in *buffer*, an area of size *bufLen*. The length of data actually stored, which must be non-negative, is returned.

* **userptr_t buffer**
* **size_t bufLen**
* **int \*returnVal**

The first step in the implementation is the validation of the *buffer* parameter.
If *buffer* is *NULL*, the function immediately returns *EFAULT*, indicating that the provided user-space pointer is invalid.
This ensures that the kernel does not attempt to write data to an invalid or restricted memory address in user space.
After validating the parameters, a kernel-side uio structure is initialized to manage the transfer of data from the kernel to user space.
This structure is set up with the user-provided buffer, its length *bufLen*, and a direction flag indicating a read from the kernel’s perspective *UIO_READ*.
The initialization is done using *uio_uinit*, which prepares the *iovec* and *uio* structures to handle the *copyout* process safely.
Once the *uio* is ready, the function calls *vfs_getcwd*, which queries the virtual file system layer to obtain the absolute path of the current working directory for the calling process.
The resulting path is written directly into the user buffer through the *uio* structure.
If *vfs_getcwd* fails, it returns an appropriate error code depending on the situation:
*ENOENT* if the current working directory no longer exists, for example, it was deleted;
*EIO* in case of a low-level disk I/O error;
*EFAULT* if the provided user buffer could not be written to due to invalid memory access.
The function simply returns this error code to the caller without further processing.
If the call succeeds, the function calculates how many bytes were actually written to the user buffer.
This is done by subtracting the remaining bytes in *ku.uio_resid* from the total buffer length *bufLen*.
The resulting value is stored in *returnVal*, which represents the number of bytes successfully copied.
Finally, the function returns 0 to indicate that the operation completed successfully and the current working directory path has been correctly retrieved.