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


/**
 * @brief sys_getpid() retrieves the process ID of the current process.
 * 
 * It checks if a current process exists and stores its PID in the return value pointer.
 * 
 * @param retvalpid pointer to store the current process PID
 * @return zero on success
 */
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


#if OPT_C2OS
/**
 * @brief sys_waitpid() waits for a specific child process to change state.
 * 
 * It verifies that the specified PID is a valid child of the current process.
 * It waits for the child to exit by sleeping on the process condition variable.
 * If WNOHANG is specified, it returns immediately if the child is still running.
 * Once the child exits, it retrieves the exit status, copies it to user space,
 * removes the child from the parent's list, and destroys the child process structure.
 * 
 * @param pid the process ID of the child to wait for
 * @param status pointer to an integer where the exit status will be stored
 * @param options flags to control the wait behavior (e.g., WNOHANG)
 * @param retval pointer to store the PID of the waited-for child
 * @return zero on success, or an error code (ECHILD, EINVAL, ESRCH, EFAULT)
 */
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
            *status = 0;
            *retval = pid;
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
    remove_child_from_list(curproc, pid);
    
    /* Finally, destroy the process structure (The Reaping) */
    proc_destroy(proc);

    return 0;
}
#endif


/**
 * @brief sys_exit() terminates the current process.
 * 
 * It sets the process exit status using _MKWAIT_EXIT, signals the parent process
 * (via the condition variable) that it has finished, and calls thread_exit() 
 * to destroy the thread context. This function does not return.
 * 
 * @param exitcode the exit code to return to the parent process
 */
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


/**
 * @brief sys_fork() creates a new process by duplicating the current one.
 * 
 * It creates a new process structure and copies the address space and trapframe
 * from the parent. It performs a shallow copy of the file table, incrementing 
 * reference counts to share open files. It links the new process as a child 
 * of the current process and creates a new thread to run the child.
 * 
 * @param ctf the trapframe of the current thread (to be copied to the child)
 * @param retval pointer to store the PID of the new child process
 * @return zero on success, or an error code (ENOMEM)
 */
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

    err = as_copy(parent->p_addrspace, &child->p_addrspace);
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
    spinlock_acquire(&parent->p_lock);
    if (parent->p_cwd != NULL) {
        VOP_INCREF(parent->p_cwd);
        child->p_cwd = parent->p_cwd;
    }
    spinlock_release(&parent->p_lock);


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


/**
 * @brief sys_execv() replaces the current process image with a new program.
 * 
 * It copies the program name and arguments from user space into kernel buffers.
 * It destroys the old address space, loads the new executable from disk using loadexec(),
 * and sets up the new user stack with the provided arguments. Finally, it enters 
 * the new process context.
 * 
 * @param progname path to the executable program
 * @param argv array of arguments to pass to the program
 * @return does not return on success; returns an error code on failure
 */
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

    result = loadexec(kprog, &entrypoint, &stackptr);

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