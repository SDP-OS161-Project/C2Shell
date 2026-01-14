#include <types.h>
#include <proc.h>
#include <vnode.h>
#include <kern/unistd.h>
#include <kern/errno.h>
#include <clock.h>
#include <syscall.h>
#include <current.h>
#include <lib.h>
#include <copyinout.h>
#include <vfs.h>
#include <limits.h>
#include <uio.h>
#include <kern/c2_syscall.h>
#include <kern/stat.h>
#include <kern/fcntl.h>
#include <kern/seek.h>
#include <synch.h>
#include <stat.h>

#define SYSTEM_OPEN_MAX (10*OPEN_MAX)
#define CHUNK_SIZE 4096

struct openfile systemFileTable[SYSTEM_OPEN_MAX];

/* * GLOBAL FILESYSTEM LOCK
 * Defined globally here. Initialized in proc_bootstrap (proc.c).
 * Used to serialize access to the emufs hardware simulator.
 */
struct lock *fs_global_lock = NULL;

#if OPT_C2OS

// static void cleanup_openfile(struct openfile *of) {
//     if (of->vn) {
//         lock_acquire(fs_global_lock);
//         vfs_close(of->vn);
//         lock_release(fs_global_lock);
//     }
//     if (of->lockFile) {
//         lock_destroy(of->lockFile);
//     }
//     kfree(of);
// }

int sys_open(userptr_t pathName, int openFlags, mode_t modeFile, int32_t *returnVal) 
{
    size_t len;
    struct vnode *vn;
    int err;
    char *kernB;
    struct openfile *openF;
    struct stat fileStat;
    int fd = -1;

    if (pathName == NULL) return EFAULT;

    /* 1. Allocate kernel buffer */
    kernB = (char *) kmalloc(PATH_MAX);
    if (kernB == NULL) return ENOMEM;

    /* 2. Copy path from user space */
    err = copyinstr((const_userptr_t) pathName, kernB, PATH_MAX, &len);
    if (err) {
        kfree(kernB);
        return err;
    }

    /* 3. Open the vnode */
    lock_acquire(fs_global_lock);
    err = vfs_open(kernB, openFlags, modeFile, &vn);
    lock_release(fs_global_lock);
    
    kfree(kernB); 
    if (err) return err;

    /* 4. Allocate Openfile Structure EARLY to avoid race cleanup mess later */
    openF = (struct openfile *) kmalloc(sizeof(struct openfile));
    if (openF == NULL) {
        lock_acquire(fs_global_lock);
        vfs_close(vn);
        lock_release(fs_global_lock);
        return ENOMEM;
    }

    /* Initialize struct */
    openF->vn = vn;
    openF->offset = 0;
    openF->countRef = 1;
    openF->openFlags = openFlags;
    openF->lockFile = lock_create("LOCK_FILE");
    
    if (openF->lockFile == NULL) {
        lock_acquire(fs_global_lock);
        vfs_close(vn);
        lock_release(fs_global_lock);
        kfree(openF);
        return ENOMEM;
    }

    /* Set Access Mode (Crucial for later read/write checks) */
    switch (openFlags & O_ACCMODE) {
        case O_RDONLY: openF->modeFile = O_RDONLY; break;
        case O_WRONLY: openF->modeFile = O_WRONLY; break;
        case O_RDWR:   openF->modeFile = O_RDWR;   break;
        default:
            lock_acquire(fs_global_lock);
            vfs_close(vn);
            lock_release(fs_global_lock);
            lock_destroy(openF->lockFile);
            kfree(openF);
            return EINVAL;
    }

    /* Handle O_APPEND */
    if (openFlags & O_APPEND) {
        lock_acquire(fs_global_lock);
        err = VOP_STAT(vn, &fileStat);
        lock_release(fs_global_lock);
        if (err) {
            lock_acquire(fs_global_lock);
            vfs_close(vn);
            lock_release(fs_global_lock);
            lock_destroy(openF->lockFile);
            kfree(openF);
            return err;
        }
        openF->offset = fileStat.st_size;
    }

    /* 5. Find free slot & Assign ATOMICALLY */
    /* Assumes curproc has a lock called p_lock initialized */
    lock_acquire(curproc->p_locklock); 
    
    /* Start from 0 to fill holes (e.g. if stdout was closed) */
    for (int i = 3; i < OPEN_MAX; i++) {
        if (curproc->fileTable[i] == NULL) 
        {
            curproc->fileTable[i] = openF; // Claim it immediately
            fd = i;
            break;
        }
    }
    lock_release(curproc->p_locklock);

    if (fd <= -1) {
        lock_acquire(fs_global_lock);
        vfs_close(vn);
        lock_release(fs_global_lock);
        lock_destroy(openF->lockFile);
        kfree(openF);
        return EMFILE;
    }
    
    *returnVal = fd;
    return 0;
}

int sys_close(int fd)
{
    struct openfile *f = NULL;

    if(fd < 0 || fd >= OPEN_MAX) return EBADF; 

    /* * CRITICAL SECTION: Process Table Modification
     * We must detach the file from the table ATOMICALLY before 
     * inspecting the file object to prevent race conditions.
     */
    lock_acquire(curproc->p_locklock);
    
    f = curproc->fileTable[fd];
    if(f == NULL) {
        lock_release(curproc->p_locklock);
        return EBADF; 
    }
    
    curproc->fileTable[fd] = NULL; /* Detach */
    
    lock_release(curproc->p_locklock);

    /* * Now we have the pointer 'f' safely detached from this process.
     * We handle the reference counting.
     */
    lock_acquire(f->lockFile);
    KASSERT(f->countRef > 0);
    f->countRef--;
    
    if (f->countRef > 0) {
        /* Still in use by other processes (fork/dup2) */
        lock_release(f->lockFile);
        return 0;
    }

    /* If RefCount == 0, we own the object completely. */
    
    /* Clean up VFS and Memory */
    if (f->vn != NULL) {
        lock_acquire(fs_global_lock);
        vfs_close(f->vn);
        lock_release(fs_global_lock);
    }

    lock_release(f->lockFile);
    lock_destroy(f->lockFile);
    kfree(f);
    
    return 0;
}

int sys_read(int fd, userptr_t buffer, size_t bufLen, ssize_t *returnVal)
{
    struct openfile *fl;
    struct iovec iov;
    struct uio ku;
    int res;
    size_t nRead = 0;
    char *kBuffer = NULL;

    /* 1. Basic Argument Validation */
    if (fd < 0 || fd >= OPEN_MAX) return EBADF;
    if (buffer == NULL) return EFAULT;
    
    /* 2. Retrieve Openfile (Thread-Safe fetch not strictly needed here as table is per-proc, 
       but standard to check NULL) */
    fl = curproc->fileTable[fd];
    if (fl == NULL) return EBADF;
    if (fl->vn == NULL) return EBADF;

    /* 3. Check Access Mode */
    if ((fl->openFlags & O_ACCMODE) == O_WRONLY) return EBADF;

    if (bufLen == 0) {
        *returnVal = 0;
        return 0;
    }

    /* 4. Allocate Kernel Buffer */
    kBuffer = kmalloc(CHUNK_SIZE);
    if (kBuffer == NULL) return ENOMEM;

    /* 5. Acquire File Lock (Protects 'offset' from other threads sharing this fd) */
    lock_acquire(fl->lockFile);

    while (nRead < bufLen) 
    {
        size_t remaining = bufLen - nRead;
        size_t nLen = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;

        /* Prepare UIO for Kernel Buffer */
        uio_kinit(&iov, &ku, kBuffer, nLen, fl->offset, UIO_READ);

        /* --- CRITICAL SECTION: HARDWARE ACCESS --- */
        //lock_acquire(fs_global_lock);
        res = VOP_READ(fl->vn, &ku);
        //lock_release(fs_global_lock);
        /* ----------------------------------------- */

        if (res) {
            lock_release(fl->lockFile);
            kfree(kBuffer);
            return res;
        }

        /* Calculate bytes actually read from disk */
        size_t readThisChunk = nLen - ku.uio_resid;
        
        /* Update Offset (Protected by fl->lockFile) */
        fl->offset += readThisChunk;

        /* If we hit EOF (read 0 bytes), stop */
        if (readThisChunk == 0) break;

        /* Copy data from Kernel Buffer to User Buffer */
        res = copyout(kBuffer, (userptr_t)((uintptr_t)buffer + nRead), readThisChunk);
        if (res) {
            lock_release(fl->lockFile);
            kfree(kBuffer);
            return res; /* likely EFAULT */
        }

        nRead += readThisChunk;

        /* If we read less than requested (EOF or short read), stop */
        if (readThisChunk < nLen) break;
    }

    lock_release(fl->lockFile);
    kfree(kBuffer);

    *returnVal = (ssize_t)nRead;
    return 0;
}

int sys_write(int fd, userptr_t buffer, size_t bufLen, ssize_t *returnVal)
{
    struct openfile *fl;
    struct iovec iov;
    struct uio ku;
    int res;
    size_t nWrite = 0;
    char *kBuffer = NULL;

    // Allocate a fixed-size kernel buffer
    kBuffer = kmalloc(CHUNK_SIZE);
    if (kBuffer == NULL) return ENOMEM;

    if (fd < 0 || fd >= OPEN_MAX) { kfree(kBuffer); return EBADF; }
    if (buffer == NULL) return EFAULT;
    if (bufLen == 0) { *returnVal = 0; return 0; }

    fl = curproc->fileTable[fd];
    if (fl == NULL) { kfree(kBuffer); return EBADF; }
    if (fl->modeFile == O_RDONLY) { kfree(kBuffer); return EBADF; }

    lock_acquire(fl->lockFile);
    while (nWrite < bufLen) 
    {
        size_t remaining = bufLen - nWrite;
        size_t nLen = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;

        res = copyin((const_userptr_t)((uintptr_t)buffer + nWrite), kBuffer, nLen);
        if (res) {
            kfree(kBuffer);
            lock_release(fl->lockFile);
            return res;
        }

        uio_kinit(&iov, &ku, kBuffer, nLen, fl->offset, UIO_WRITE);

        /* LOCK: Critical section for hardware write */

        if (fl->openFlags & O_APPEND) 
        {
            struct stat st;
            lock_acquire(fs_global_lock);
            res = VOP_STAT(fl->vn, &st);
            lock_release(fs_global_lock);
            
            if (res) {
                kfree(kBuffer);
                lock_release(fl->lockFile);
                return res;
            }
            fl->offset = st.st_size;
            /* Re-init uio with new offset */
            uio_kinit(&iov, &ku, kBuffer, nLen, fl->offset, UIO_WRITE);
        }

        lock_acquire(fs_global_lock);
        res = VOP_WRITE(fl->vn, &ku);
        lock_release(fs_global_lock);

        if (res) 
        {
            kfree(kBuffer);
            lock_release(fl->lockFile);
            return res;
        }

        size_t wrote = nLen - ku.uio_resid;
        nWrite += wrote;
        fl->offset += wrote;  

        if (wrote == 0 || wrote < nLen) break;
    }

    lock_release(fl->lockFile);

    kfree(kBuffer);
    *returnVal = (ssize_t)nWrite;
    return 0;
}

int sys_lseek(int fd, off_t pos, int whence, off_t *returnVal)
{
    struct openfile *fl;
    off_t newOff;
    struct stat st;
    int res;

    if(fd < 0 || fd >= OPEN_MAX) return EBADF; 
    fl = curproc->fileTable[fd];
    if(fl == NULL) return EBADF;
    if (!VOP_ISSEEKABLE(fl->vn)) return ESPIPE;

    switch(whence)
    {
        case SEEK_SET:
            newOff = pos;
            break;
        case SEEK_CUR:
            newOff = fl->offset + pos;
            break;
        case SEEK_END:
        {
            /* LOCK: VOP_STAT touches disk */
            lock_acquire(fs_global_lock);
            res = VOP_STAT(fl->vn,  &st);
            lock_release(fs_global_lock);

            if(res) return res;

            newOff = st.st_size + pos;
            break;
        }
        default: return EINVAL;
    }

    if(newOff < 0) return EINVAL;

    lock_acquire(fl->lockFile);
    fl->offset = newOff;
    lock_release(fl->lockFile);
    
    *returnVal = newOff;
    return 0;
}

int sys_dup2(int oldfd, int newfd, int *retval) {
    struct proc *p = curproc;
    struct openfile *old_of, *new_of_to_close = NULL;

    if (oldfd < 0 || oldfd >= OPEN_MAX || newfd < 0 || newfd >= OPEN_MAX) return EBADF;
    if (oldfd == newfd) { *retval = newfd; return 0; }

    /* CRITICAL REGION START */
    lock_acquire(p->p_locklock);
    
    old_of = p->fileTable[oldfd];
    if (old_of == NULL) {
        lock_release(p->p_locklock);
        return EBADF;
    }

    /* Grab the file lock to safely increment refcount */
    lock_acquire(old_of->lockFile);
    old_of->countRef++;
    lock_release(old_of->lockFile);

    /* Check if newfd needs closing */
    if (p->fileTable[newfd] != NULL) {
        new_of_to_close = p->fileTable[newfd];
    }

    /* The actual dup */
    p->fileTable[newfd] = old_of;

    lock_release(p->p_locklock);
    /* CRITICAL REGION END */

    /* Cleanup displaced file if it existed */
    if (new_of_to_close != NULL) {
        /* Use the same logic as sys_close here */
        bool is_last = false;
        
        lock_acquire(new_of_to_close->lockFile);
        new_of_to_close->countRef--;
        if (new_of_to_close->countRef == 0) is_last = true;
        lock_release(new_of_to_close->lockFile);

        if (is_last) {
            lock_acquire(fs_global_lock);
            vfs_close(new_of_to_close->vn);
            lock_release(fs_global_lock);
            lock_destroy(new_of_to_close->lockFile);
            kfree(new_of_to_close);
        }
    }

    *retval = newfd;
    return 0;
}

int sys_chdir(userptr_t pathName)
{
    size_t len;
    struct vnode *vn;
    int err;
    char *kernB;

    if (pathName == NULL) return EFAULT;

    kernB = (char *) kmalloc(PATH_MAX * sizeof(char) + 1);
    if(kernB == NULL) return ENOMEM;

    err = copyinstr((const_userptr_t) pathName, kernB, PATH_MAX, &len);
    if (err) {
        kfree(kernB);
        return err;
    }

    /* LOCK: vfs_lookup touches disk */
    lock_acquire(fs_global_lock);
    err = vfs_lookup(kernB, &vn);
    lock_release(fs_global_lock);

    kfree(kernB);
    if (err) return err;

    struct stat fileStat;

    lock_acquire(fs_global_lock);
    err = VOP_STAT(vn, &fileStat);
    lock_release(fs_global_lock);

    if (err)
    {
        lock_acquire(fs_global_lock);
        vfs_close(vn);
        lock_release(fs_global_lock);
        return err;
    }
    
    if ((fileStat.st_mode & S_IFDIR) == 0)
    {
        lock_acquire(fs_global_lock);
        vfs_close(vn);
        lock_release(fs_global_lock);
        return ENOTDIR;
    }

    //it is a directory
    struct vnode *old_cwd = curproc->p_cwd;
    curproc->p_cwd = vn;
    
    if (old_cwd != NULL) {
        lock_acquire(fs_global_lock);
        vfs_close(old_cwd);
        lock_release(fs_global_lock);
    }

    return 0;
}

int sys_getcwd(userptr_t buffer, size_t bufLen, int *returnVal)
{
    int result;
    struct iovec iov;
    struct uio ku;
    char *kBuffer = NULL;

    if (buffer == NULL) return EFAULT;

    kBuffer = kmalloc(CHUNK_SIZE);
    if (kBuffer == NULL) return ENOMEM;

    uio_kinit(&iov, &ku, kBuffer, bufLen < CHUNK_SIZE ? bufLen : CHUNK_SIZE, 0, UIO_READ);

    lock_acquire(fs_global_lock);
    result = vfs_getcwd(&ku);
    lock_release(fs_global_lock);

    if (result) {
        kfree(kBuffer);
        return result;
    }

    /* Calculate how much data was written to kBuffer */
    size_t actualLen = (bufLen < CHUNK_SIZE ? bufLen : CHUNK_SIZE) - ku.uio_resid;

    /* FIX: Now copy the path out to the user */
    result = copyout(kBuffer, buffer, actualLen);
    
    kfree(kBuffer);

    if (result) {
        return result;
    }

    *returnVal = actualLen;
    return 0;
}

/**
 * sys_remove - Remove a file from the filesystem.
 * @pathname: The path of the file to be removed.
 *
 * This function is a placeholder for the system call to remove a file.
 * Currently, it is not implemented and simply returns success.
 *
 * Return: Always returns 0 indicating success.
 */
int sys_remove(const char *pathname)
{

    /* NOT IMPLEMENTED (YET?) */
    (void)pathname;

    /* TASK COMPLETED SUCCESSFULLY */
    return 0;
}

/**
 * sys_fstat - Retrieves the status of an open file.
 *
 * @param fildes: The file descriptor of the file.
 * @param buf: Pointer to a stat structure to store the file status.
 *
 * @return: 0 on success, or an error code on failure.
 *
 * This function is a stub and currently does nothing. It is intended to retrieve
 * the status of the file associated with the file descriptor fildes and store it
 * in the stat structure pointed to by buf.
 */
int sys_fstat(int fildes, struct stat *buf)
{
    (void)fildes;
    (void)buf;

    return 0;
}

#endif