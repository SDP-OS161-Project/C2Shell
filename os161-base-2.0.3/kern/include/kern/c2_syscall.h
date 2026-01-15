#ifndef _C2_SYSCALL_H_
#define _C2_SYSCALL_H_

#include <types.h>
#include <lib.h>
#include <stat.h>
#include <mips/trapframe.h>
#include "opt-c2os.h"

struct openfile {
    struct vnode *vn;
    off_t offset;
    unsigned int countRef;
    mode_t modeFile;
    int openFlags;
    struct lock *lockFile;
};

struct proc;

#if OPT_C2OS

int sys_open(userptr_t pathName, int openFlags, mode_t modeFile, int32_t *returnVal);

int sys_close(int fd);

int sys_read(int fd, userptr_t buffer, size_t bufLen, ssize_t *returnVal);

int sys_write(int fd, userptr_t buffer, size_t bufLen, ssize_t *returnVal);

int sys_lseek(int fd, off_t pos, int whence, int32_t *returnVal_low32, int32_t *returnVal_upp32);

int sys_dup2(int oldFd, int newFd, int *returnVal);

int sys_chdir(userptr_t pathName);

int sys_getcwd(userptr_t buffer, size_t bufLen, int *returnVal);

int sys_getpid(pid_t *retvalpid);

int sys_waitpid(pid_t pid, int *status, int options, int32_t *retvalpid);

void sys_exit(int exitcode);

int sys_fork(struct trapframe *ctf, pid_t *retval);

int sys_execv(const char *progname, char *argv[]);

void remove_child_node(struct proc *parent, pid_t child_pid);

int sys_remove(const char *pathname);

int sys_fstat(int fildes, struct stat *buf);
#endif

#endif