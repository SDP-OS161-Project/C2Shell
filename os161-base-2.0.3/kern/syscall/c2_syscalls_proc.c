#include <types.h>
#include <limits.h>
#include <endian.h>
#include <thread.h>
#include <mips/trapframe.h>
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
#include <syscall.h>
#include <current.h>
#include <copyinout.h>
#include <addrspace.h>
#include "exec.h"

extern struct lock *fs_global_lock;

int sys_getpid(pid_t *retvalpid) 
{
  //check that there is a current process running  
    if(curproc != NULL)
    {
      //store the current process PID in the return value pointer
      *retvalpid = curproc->p_pid; 
    }

    return 0;   
}

/* Helper to remove child from list */
void remove_child_node(struct proc *parent, pid_t child_pid) {
    struct child_list *curr = parent->children_list;
    struct child_list *prev = NULL;

    while (curr != NULL) {
        if (curr->child_pid == child_pid) {
            if (prev == NULL) {
                parent->children_list = curr->next_child;
            } else {
                prev->next_child = curr->next_child;
            }
            kfree(curr);
            return;
        }
        prev = curr;
        curr = curr->next_child;
    }
}

#if OPT_C2OS
int sys_waitpid(pid_t pid, int *status, int options, int32_t *retval) {
    int result;

    KASSERT(curproc != NULL);

    /* 1. Filter Invalid PIDs */
    if (pid == curproc->p_pid) return ECHILD;
    
    /* 2. Check if Child Exists in our list */
    if (is_child(curproc, pid) == -1) return ECHILD;

    /* 3. Handle Options (WNOHANG) */
    if (options != 0 && options != WNOHANG) return EINVAL;

    /* 4. Find the Process Structure */
    struct proc *proc = proc_search(pid);
    if (proc == NULL) return ESRCH;

    /* 5. Logic: Has the process already exited? */
    lock_acquire(proc->p_locklock);
    
    if (proc->p_numthreads > 0) {
        /* Process is still running */
        if (options == WNOHANG) {
            lock_release(proc->p_locklock);
            *retval = 0; /* PID 0 indicates running */
            return 0;
        }

        /* Wait for it to die */
        while (proc->p_numthreads > 0) {
            cv_wait(proc->p_cv, proc->p_locklock);
        }
    }
    lock_release(proc->p_locklock);

    /* 6. Process is dead. Extract status safely. */
    int exit_code = proc->p_status;

    /* IF user provided a status pointer, copy it out safely */
    if (status != NULL) {
        /* copyout(src, dest, len) */
        result = copyout(&exit_code, (userptr_t)status, sizeof(int));
        if (result) {
            /* If copyout fails (EFAULT), we still successfully waited for the process.
             * But we can't return the status. Standard behavior is to return EFAULT.
             * CRITICAL: We must arguably still destroy the zombie to prevent leaks,
             * or leave it depending on strict POSIX interp. 
             * For OS161, returning EFAULT is usually enough.
             */
            return result; 
        }
    }

    /* 7. Cleanup and Return */
    *retval = pid;
    
    /* Remove from parent's child list */
    remove_child_node(curproc, pid);
    
    /* Finally, destroy the process structure (The Reaping) */
    proc_destroy(proc);

    return 0;
}
#endif

void sys_exit(int exitcode) {
    struct proc *p = curproc;
    
    /* 1. Set exit status */
    p->p_status = _MKWAIT_EXIT(exitcode);

    /* 2. Signal the parent that we are "done" (logically) */
    lock_acquire(p->p_locklock);
    cv_signal(p->p_cv, p->p_locklock);
    lock_release(p->p_locklock);

    /* 3. Die. (The detachment will happen in thread_exit) */
    thread_exit();
    
    panic("sys_exit: Should not return");
}

int sys_fork(struct trapframe *ctf, pid_t *retval) 
{
    struct proc *parent = curproc;
    struct proc *child = NULL;
    struct trapframe *child_tf = NULL;
    int err;

    KASSERT(curproc != NULL);
    KASSERT(ctf != NULL);
    KASSERT(retval != NULL);

    /* 1. Create Child Process */
    /* CHANGED: Use proc_create directly to avoid creating new console FDs */
    child = proc_create(parent->p_name);
    if (child == NULL) {
        return ENOMEM;
    }

    /* 2. Copy Address Space */
    lock_acquire(fs_global_lock);
    err = as_copy(parent->p_addrspace, &child->p_addrspace);
    lock_release(fs_global_lock);
    if (err) {
        proc_destroy(child);
        return err;
    }

    /* 3. Copy Trapframe */
    child_tf = kmalloc(sizeof(struct trapframe));
    if(child_tf == NULL) {
        proc_destroy(child);
        return ENOMEM; 
    }
    memmove(child_tf, ctf, sizeof(struct trapframe));

    /* 4. Copy Current Working Directory (CWD) */
    /* We must do this manually since we stopped using proc_create_runprogram */
    lock_acquire(fs_global_lock);
    spinlock_acquire(&parent->p_lock);
    if (parent->p_cwd != NULL) {
        VOP_INCREF(parent->p_cwd);
        child->p_cwd = parent->p_cwd;
    }
    spinlock_release(&parent->p_lock);
    lock_release(fs_global_lock);


    /* 5. SHARE THE FILE TABLE (The Fix for the Crash) */
    for (int i = 0; i < OPEN_MAX; i++) {
        struct openfile *file = parent->fileTable[i];
        
        if (file != NULL) {
            /* Shallow Copy: Point to the SAME openfile struct */
            lock_acquire(file->lockFile); // Safety while modifying refcount
            
            child->fileTable[i] = file;
            file->countRef++;  // Increment reference count
            
            lock_release(file->lockFile);
        }
    }

    /* 6. Link Parent */
    child->parent_pid = parent->p_pid;

    /* 7. ADD TO PARENT'S LIST */
    if (add_new_child(parent, child->p_pid) == -1) {
        kfree(child_tf);
        proc_destroy(child);
        return ENOMEM; 
    }

    /* 8. Create Thread */
    err = thread_fork(
        parent->p_name,
        child,   
        call_enter_forked_process,
        (void *) child_tf,
        (unsigned long) 0
    );

    if (err) {
        kfree(child_tf);
        proc_destroy(child);
        return err;
    }

    *retval = child->p_pid; 
    return 0;
}

int sys_execv(const char *progname, char *argv[]) 
{
    KASSERT(curproc != NULL);

    int result, argc;
    vaddr_t entrypoint, stackptr;
    userptr_t user_prog = (userptr_t) progname;
    userptr_t user_argv = (userptr_t) argv;

    char *kprog = kmalloc(PATH_MAX);
    if (kprog == NULL) return ENOMEM;

    result = copyinstr(user_prog, kprog, PATH_MAX, NULL);
    if (result) {
        kfree(kprog);
        return result;
    }

    struct exec_args args = {0}; 
    /* Assuming argbuf_init is not needed or handled by struct init */

    result = argbuf_fromuser(&args, user_argv);
    if (result) {
        argbuf_cleanup(&args);
        kfree(kprog);
        return result;
    }

    /* --- ADDED LOCK HERE: loadexec reads from disk --- */
    lock_acquire(fs_global_lock);
    result = loadexec(kprog, &entrypoint, &stackptr);
    lock_release(fs_global_lock);
    /* ------------------------------------------------ */

    kfree(kprog); 
    if (result) {
        argbuf_cleanup(&args);
        return result;
    }

    result = argbuf_copyout(&args, &stackptr, &argc, &user_argv);
    if (result) {
        panic("sys_execv: argbuf_copyout failed: %s\n", strerror(result));
    }

    argbuf_cleanup(&args);

    enter_new_process(argc, user_argv, NULL, stackptr, entrypoint);
    panic("sys_execv: enter_new_process returned unexpectedly\n");

    return EINVAL;
}