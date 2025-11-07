
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

    *status = child->p_status;
    *retvalpid = child->p_pid;

    lock_release(child->p_locklock);

    proc_destroy(child);

    return 0;
}

void sys_exit(int exitcode) {

    KASSERT(curproc != NULL);
    KASSERT(curthread != NULL);

    curproc->p_status = _MKWAIT_EXIT(exitcode);    // exitcode & 0xff

    proc_remthread(curthread);     

    lock_acquire(curproc->p_locklock);
    cv_signal(curproc->p_cv, curproc->p_locklock);
    lock_release(curproc->p_locklock);

    thread_exit();   //exit thread

}

int sys_fork(struct trapframe *ctf, pid_t *retval) {

    struct proc *parent = curproc;
    struct proc *child = NULL;
    struct trapframe *child_tf = NULL;

    KASSERT(curproc != NULL);
    KASSERT(ctf != NULL);
    KASSERT(retval != NULL);

    // find a free pid
    int pid = find_valid_pid();
    if (pid <= 0) {
        return ENPROC;  // no available PIDs
    }

    child = proc_create_runprogram(curproc->p_name);
    if (child == NULL) {
        return ENOMEM;  //out of memory
    }

    //copy adress space
    auto err = as_copy(parent->p_addrspace, &child->p_addrspace);
    if (err) {
        proc_destroy(child);
        return err;
    }

    ///copy trapframe
    child_tf = kmalloc(sizeof(struct trapframe));
    if(child_tf == NULL){
        proc_destroy(child);
        return ENOMEM; 
    }
    memmove(child_tf, ctf, sizeof(struct trapframe));

    //add child to parent
    if(add_new_child(parent, pid) == -1){
        kfree(child_tf);
        proc_destroy(child);
        return ENOMEM; 
    }


    //link child to parent
    child->parent_pid=parent->p_pid;

    // add the new child to the process table
    err = proc_add(pid, child);
    if (err == -1) {
        kfree(child_tf);
        proc_destroy(child);
        return ENOMEM;
    }

    //create a thread for the child process
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

    *retval = child->p_pid;      // return pid of child
    return 0;
}

int sys_execv(const char *progname, char *argv[]) {

	KASSERT(curproc != NULL);

    if(progname == NULL || argv == NULL){
        return EFAULT;
    }

    userptr_t prog = (userptr_t) progname;
    userptr_t uargv = (userptr_t) argv;
	
	vaddr_t entrypoint, stackptr;
	int argc, err;

	//copy program name to kernal space
	char *kpath = kmalloc(PATH_MAX);
	if (kpath == NULL) {
		return ENOMEM;
	}

	err = copyinstr(prog, kpath, PATH_MAX, NULL);
	if (err) {
		kfree(kpath);
		return err;
	}

	if(kpath[0] == '\0'){ //empty path not allowed
        kfree(kpath);
        return EINVAL;
    }

    //copy arguments from user space
	argbuf_t kargv; 
	argbuf_init(&kargv);

	err = argbuf_fromuser(&kargv, uargv);
	if (err) {
		argbuf_cleanup(&kargv);
		kfree(kpath);
		return err;
	}

	/**
	 * LOAD THE EXECUTABLE
	 * NB: must not fail from here on, the old address space has been destroyed
	 * 	   and, therefore, there is nothing to restore in case of failure.
	 */
	err = loadexec(kpath, &entrypoint, &stackptr);
	if (err) {
		argbuf_cleanup(&kargv);
		kfree(kpath);
		return err;
	}

	kfree(kpath); //no longer needed

	//copy arguments back to user stack
	err = argbuf_copyout(&kargv, &stackptr, &argc, &uargv);
	if (err) {
		//at this stage failure is unrecoverable 
		panic("execv: copyout_args failed: %s\n", strerror(err));
	}

	argbuf_cleanup(&kargv);

	//enter the new process
	enter_new_process(argc, uargv, NULL /*envp*/, stackptr, entrypoint);

	//should never return
	panic("enter_new_process returned\n");
	return EINVAL;
}