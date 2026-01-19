/*
 * Copyright (c) 2013
 * The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Process support.
 *
 * There is (intentionally) not much here; you will need to add stuff
 * and maybe change around what's already present.
 *
 * p_lock is intended to be held when manipulating the pointers in the
 * proc structure, not while doing any significant work with the
 * things they point to. Rearrange this (and/or change it to be a
 * regular lock) as needed.
 *
 * Unless you're implementing multithreaded user processes, the only
 * process that will have more than one thread is the kernel process.
 */

#include <types.h>
#include <spl.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>
#include <syscall.h>

/* INCLUDES FOR CONSOLE INITIALIZATION */
#include <synch.h>
#include <kern/fcntl.h>
#include <vfs.h>


/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc *kproc;

/**
 * @brief The process table stuct stores an array of user processes, each identified by
 * a specific PID
 */
#if OPT_SHELL
#define PROC_MAX 100                /* maximum number of allowed running process    */
static struct _processTable {
    bool is_active;                 /* table is active and ready to use             */
    struct proc *proc[PROC_MAX+1];  /* [0] not used, PID >= 1                       */
    pid_t last_pid;                 /* last PID used in the table                   */
    struct spinlock lk;             /* lock for this table                          */
} processTable;
#endif


#if OPT_SHELL
/**
 * @brief Starts the new generated thread
 * * @param tfv trapframe of the new thread.
 * @param dummy not used
 */
void call_enter_forked_process(void *tfv, unsigned long dummy) {
    (void) dummy;
    struct trapframe *tf = (struct trapframe *) tfv;
    enter_forked_process(tf); 
    panic("[!] enter_forked_process() returned unexpectedly\n");
}

/**
 * @brief Return the process associated to the given PID
 * * @param pid pid of the process to retrieve
 * @return struct proc* process associated to the pid
 */
struct proc *proc_search(pid_t pid) {
    if (pid <= 0 || pid > PROC_MAX) return NULL;
    struct proc *proc = processTable.proc[pid];
    if (proc && proc->p_pid != pid) return NULL;
    return proc;
}

/**
 * @brief For any given process, the first file descriptors (0, 1, and 2) are 
 * considered to be standard input (stdin), standard output (stdout), and
 * standard error (stderr). These file descriptors should start out attached 
 * to the console device ("con:").
 * * @param name name that will be assigned to the lock
 * @return int 
 */
static int console_init(const char *lock_name, struct proc *proc, int fd, int flag) {
    char *con = kstrdup("con:");
    if (con == NULL) return -1;

    proc->fileTable[fd] = (struct openfile *) kmalloc(sizeof(struct openfile));
    if (proc->fileTable[fd] == NULL) {
        kfree(con);
        return -1;
    }

    /* LOCK: Protected Open */
    int err = vfs_open(con, flag, 0644, &proc->fileTable[fd]->vn);

    kfree(con);
    if (err) {
        kfree(proc->fileTable[fd]);
        proc->fileTable[fd] = NULL; 
        return -1;
    }

    proc->fileTable[fd]->offset = 0;
    proc->fileTable[fd]->lockFile = lock_create(lock_name);
    
    if (proc->fileTable[fd]->lockFile == NULL) {
        /* LOCK: Protected Close */
        vfs_close(proc->fileTable[fd]->vn);

        kfree(proc->fileTable[fd]);
        proc->fileTable[fd] = NULL; 
        return -1;
    }
    proc->fileTable[fd]->countRef = 1;
    proc->fileTable[fd]->modeFile = flag;

    return 0;
}

/**
 * @brief Add the given process to the process table and manage the PID initialization.
 * * @param proc newly created process
 * @param name name of the process
 * @return the pid of the process created, -1 on failure
 */
static int proc_init(struct proc *proc, const char *name) {
    (void)name; 
    spinlock_acquire(&processTable.lk);
    proc->p_pid = -1;

    int index = processTable.last_pid + 1;
    index = (index > PROC_MAX) ? 1 : index;     
    while (index != processTable.last_pid) {
        if (processTable.proc[index] == NULL) {
            processTable.proc[index] = proc;
            processTable.last_pid = index;
            proc->p_pid = index;
            break;
        }
        index++;
        index = (index > PROC_MAX) ? 1 : index;
    }
    spinlock_release(&processTable.lk);
    if (proc->p_pid <= 0) return -1; 
    return proc->p_pid;
}
/**
 * @brief manage the process table when a process is destroyed.
 * * @param proc the process that will be destroyed.
 * @return 0 on sucess, any other value on failure.
 */
static int proc_deinit(struct proc *proc) {
    struct proc* parent_proc;
    
    spinlock_acquire(&processTable.lk);
    int index = proc->p_pid;
    if (index <= 0 || index > PROC_MAX) {
        spinlock_release(&processTable.lk);
        return -1;
    }
    processTable.proc[index] = NULL;
    spinlock_release(&processTable.lk);

    if (proc->p_cv) cv_destroy(proc->p_cv);
    if (proc->p_locklock) lock_destroy(proc->p_locklock);
    destroy_child_list(proc);
    
    if (proc->parent_pid != -1) {
        parent_proc = proc_search(proc->parent_pid);
        if (parent_proc != NULL) {
             remove_child_from_list(parent_proc, proc->p_pid);
        }
    }
    return 0;
}
#endif

/*
 * Create a proc structure.
 */
struct proc *proc_create(const char *name)
{
    struct proc *proc;
    proc = kmalloc(sizeof(*proc));
    if (proc == NULL) return NULL;

    proc->p_name = kstrdup(name);
    if (proc->p_name == NULL) { kfree(proc); return NULL; }

    proc->p_numthreads = 0;
    spinlock_init(&proc->p_lock);
    proc->p_addrspace = NULL;
    proc->p_cwd = NULL;

#if OPT_SHELL
    proc->children_list = NULL;
    proc->p_status = 0;
    proc->parent_pid = -1;
    bzero(proc->fileTable, OPEN_MAX * sizeof(struct openfile*));

    proc->p_cv = cv_create(name);
    proc->p_locklock = lock_create(name);
    
    if (proc->p_cv == NULL || proc->p_locklock == NULL) {
        if (proc->p_cv) cv_destroy(proc->p_cv);
        if (proc->p_locklock) lock_destroy(proc->p_locklock);
        kfree(proc->p_name);
        kfree(proc);
        return NULL;
    }

    if (strcmp(name, "[kernel]") != 0) {
        if (proc_init(proc, name) <= 0) {
            cv_destroy(proc->p_cv);
            lock_destroy(proc->p_locklock);
            kfree(proc->p_name);
            kfree(proc);
            return NULL;
        }
    }
#endif

    return proc;
}

/*
 * Destroy a proc structure.
 *
 * Note: nothing currently calls this. Your wait/exit code will
 * probably want to do so.
 */
void proc_destroy(struct proc *proc)
{
    KASSERT(proc != NULL);
    KASSERT(proc != kproc);

    /* VFS fields */
    if (proc->p_cwd) {
        /* LOCK: Protected DECREF */
        VOP_DECREF(proc->p_cwd);
        proc->p_cwd = NULL;
    }

#if OPT_SHELL
    /* Clean up the File Table with thread safety */
    /* In proc_destroy */
    for (int i = 0; i < OPEN_MAX; i++) {
        struct openfile *of = proc->fileTable[i];
        if (of != NULL) {
            bool is_last_ref = false;

            /* 1. Check refcount safely */
            lock_acquire(of->lockFile);
            if (of->countRef > 0) {
                of->countRef--;
            }
            if (of->countRef == 0) {
                is_last_ref = true;
            }
            lock_release(of->lockFile); /* <--- RELEASE HERE */

            /* 2. If we are the last one, destroy it.
            * No one else has a pointer to 'of' now (or they would have incremented ref).
            */
            if (is_last_ref) {
                if (of->vn != NULL) {
                    vfs_close(of->vn);
                }
                lock_destroy(of->lockFile);
                kfree(of);
            }

            proc->fileTable[i] = NULL;
        }
    }
#endif

    /* VM fields */
    if (proc->p_addrspace) {
        struct addrspace *as;
        if (proc == curproc) {
            as = proc_setas(NULL);
            as_deactivate();
        } else {
            as = proc->p_addrspace;
            proc->p_addrspace = NULL;
        }
        as_destroy(as);
    }

    KASSERT(proc->p_numthreads == 0);
    spinlock_cleanup(&proc->p_lock);

#if OPT_SHELL
    if (proc_deinit(proc) != 0) {
        panic("[ERROR] some errors occurred in the management of the process table\n");
    }
#endif

    kfree(proc->p_name);
    kfree(proc);
}

/*
 * Create the process structure for the kernel.
 */
void proc_bootstrap(void)
{
#if OPT_SHELL
    spinlock_init(&processTable.lk);
    processTable.is_active = true;
    processTable.last_pid = 0;
    for (int i = 0; i <= PROC_MAX; i++) processTable.proc[i] = NULL;
#endif

    kproc = proc_create("[kernel]");
    if (kproc == NULL) panic("proc_create for kproc failed\n");

#if OPT_SHELL
    processTable.proc[0] = kproc;
    kproc->p_pid = 0; 
#endif
}
/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
struct proc *proc_create_runprogram(const char *name)
{
    struct proc *newproc;

    newproc = proc_create(name);
    if (newproc == NULL) return NULL;

    newproc->p_addrspace = NULL;

#if OPT_SHELL
    if (console_init("STDIN", newproc, 0, O_RDONLY) == -1) return NULL;
    if (console_init("STDOUT", newproc, 1, O_WRONLY) == -1) return NULL;
    if (console_init("STDERR", newproc, 2, O_WRONLY) == -1) return NULL;
#endif

    spinlock_acquire(&curproc->p_lock);
    if (curproc->p_cwd != NULL) {
        VOP_INCREF(curproc->p_cwd);
        newproc->p_cwd = curproc->p_cwd;
    }
    spinlock_release(&curproc->p_lock);

    return newproc;
}

/*
 * Add a thread to a process. Either the thread or the process might
 * or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
int proc_addthread(struct proc *proc, struct thread *t)
{
    int spl;
    KASSERT(t->t_proc == NULL);
    spinlock_acquire(&proc->p_lock);
    proc->p_numthreads++;
    spinlock_release(&proc->p_lock);
    spl = splhigh();
    t->t_proc = proc;
    splx(spl);
    return 0;
}

/*
 * Remove a thread from its process. Either the thread or the process
 * might or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
void proc_remthread(struct thread *t)
{
    struct proc *proc;
    int spl;

    proc = t->t_proc;
    KASSERT(proc != NULL);

    spinlock_acquire(&proc->p_lock);
    KASSERT(proc->p_numthreads > 0);
    proc->p_numthreads--;
    spinlock_release(&proc->p_lock);

#if OPT_SHELL
    if (proc->p_cv != NULL && proc->p_locklock != NULL) {
        lock_acquire(proc->p_locklock);
        cv_broadcast(proc->p_cv, proc->p_locklock);
        lock_release(proc->p_locklock);
    }
#endif

    spl = splhigh();
    t->t_proc = NULL;
    splx(spl);
}



/*
 * Fetch the address space of (the current) process.
 *
 * Caution: address spaces aren't refcounted. If you implement
 * multithreaded processes, make sure to set up a refcount scheme or
 * some other method to make this safe. Otherwise the returned address
 * space might disappear under you.
 */
struct addrspace *proc_getas(void)
{
    struct addrspace *as;
    struct proc *proc = curproc;
    if (proc == NULL) return NULL;
    spinlock_acquire(&proc->p_lock);
    as = proc->p_addrspace;
    spinlock_release(&proc->p_lock);
    return as;
}

/*
 * Change the address space of (the current) process. Return the old
 * one for later restoration or disposal.
 */
struct addrspace *proc_setas(struct addrspace *newas)
{
    struct addrspace *oldas;
    struct proc *proc = curproc;
    KASSERT(proc != NULL);
    spinlock_acquire(&proc->p_lock);
    oldas = proc->p_addrspace;
    proc->p_addrspace = newas;
    spinlock_release(&proc->p_lock);
    return oldas;
}


/*
 * Adds a new child to the children list. If It is not possible, it returns -1, otherwise 0.
 */
#if OPT_SHELL
int add_new_child(struct proc* proc, pid_t child_pid){
    struct child_list* app=proc->children_list;

    if(proc->children_list==NULL){
        proc->children_list=(struct child_list *) kmalloc(sizeof(struct child_list));
        if(proc->children_list==NULL) return -1;
        proc->children_list->next_child=NULL;
        proc->children_list->child_pid=child_pid;
        return 0;
    }
    while(app->next_child!=NULL){
        app=app->next_child;
    }
    app->next_child=(struct child_list *) kmalloc(sizeof(struct child_list));
    if(app->next_child==NULL) return -1;
    app->next_child->next_child=NULL;
    app->next_child->child_pid=child_pid;
    return 0;
}
#endif

/*
 * Destroys the child_list of a parent process which is being destroyed.
 * Sets the childrens's parent pid to -1, the "root" process.
 * If It is not possible, it returns -1, otherwise 0.
 */
#if OPT_SHELL
int destroy_child_list(struct proc* proc){
    struct child_list* app=proc->children_list;
    struct proc* child_proc;
    while(app!=NULL){
        proc->children_list=app->next_child;
        child_proc=proc_search(app->child_pid);
        if(child_proc==NULL) return -1;
        child_proc->parent_pid=-1;
        app->next_child=NULL;
        kfree(app);
        app=proc->children_list;
    }
    return 0;
}
#endif

/*
 * Removes the child (which is being destroyed) from the child list of its parent process.
 * If It is not possible, it returns -1, otherwise 0.
 */
#if OPT_SHELL
int remove_child_from_list(struct proc* proc, pid_t child_pid){
    struct child_list* app=proc->children_list;
    struct child_list* prev_child=NULL;
    while(app!=NULL){
        if(app->child_pid==child_pid){
            if(prev_child==NULL)
                proc->children_list=app->next_child;
            else
                prev_child->next_child=app->next_child;
            kfree(app);
            return 0;
        }
        prev_child=app;
        app=app->next_child;
    }
    return -1;
}
#endif


/*
 * Checks if the process with pid child_pid is a son of the parent process
 * If It is not a child, it returns -1, otherwise 0.
 */
#if OPT_SHELL
int is_child(struct proc* proc, pid_t child_pid){
    struct child_list* app=proc->children_list;
    while(app!=NULL){
        if(app->child_pid==child_pid){
            return 0;
        }
        app=app->next_child;
    }
    return -1;
}
#endif