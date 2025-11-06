
#include <kern/c2_syscall.h>
#include <kern/errno.h>
#include <proc.h>
#include <kern/wait.h>
#include <vfs.h>
#include <limits.h>
#include <uio.h>
#include <synch.h>
#include <lib.h>
#include <stat.h>
#include <kern/fcntl.h>
#include <vnode.h>
#include <kern/unistd.h>

int sys_getpid(pid_t *retvalpid) {

  //check that there is a current process running  
    if(curproc != NULL){
      //store the current process PID in the return value pointer
      *retvalpid = curproc->p_pid; 
    }

    return 0;   
}

int sys_waitpid(pid_t pid, int *status, int options, int32_t *retvalpid) {

    struct proc *child;
    
    /* CHECKING ARGUMENTS */
    if(curproc == NULL){
        return EFAULT;
    }

    if (pid == curproc->p_pid) {
        return ECHILD;
    }

    if(retvalpid == NULL){
        return EFAULT;
    }
    
    if (status == NULL) {
        *retvalpid = pid;
        return 0;
    }

    //Invalid option bits (only 0 and WNOHANG allowed)
    if(options != 0 || options != WNOHANG){
        return EINVAL;
    }

    //check if the target process is a child
    if(is_child(curproc, pid)==-1){
        return ECHILD;
    }

    child = proc_search(pid);

    if(child == NULL){
        return ESRCH;
    }

    if(options == WNOHANG && child->p_numthreads > 0){
        *status = 0;
        *retvalpid = 0;
        return 0;
    }

    lock_acquire(child->p_locklock);

    while(child->p_numthreads > 0) {
        cv_wait(child->p_cv,child->p_locklock);
    }

    *status = proc->p_status;
    *retvalpid = proc->p_pid;

    lock_release(child->p_locklock);

    proc_destroy(child);

    return 0;
}