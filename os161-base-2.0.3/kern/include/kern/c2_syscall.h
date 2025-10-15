#ifndef _C2_SYSCALL_H_
#define _C2_SYSCALL_H_

#include <types.h>
#include <lib.h>
#include <mips/trapframe.h>
#include "opt-c2os.h"

struct openfile {
    struct vnode *vn;
    off_t offset;
    unsigned int countRef;
    int modeFile;
    struct lock *lockFile;
}

#if OPT_C2OS

int sys_open(userptr_t pathName, int openFlags, mode_t modeFile, int32_t *returnVal);

int sys_close(int fd);

int sys_read(int fd, userptr_t buffer, size_t bufLen, ssize_t *returnVal);

int sys_write(int fd, userptr_t buffer, size_t bufLen, ssize_t *returnVal);

int sys_lseek(int fd, off_t pos, int whence, off_t *returnVal);

int sys_dup2(int oldFd, int newFd, int *returnVal);

int sys_chdir(userptr_t pathName);

#endif

#endif