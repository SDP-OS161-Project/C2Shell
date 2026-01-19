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

#if OPT_SHELL

/**
 * @brief sys_open() opens the file, device, or other kernel object named by the pathname 
 * provided. The flags argument specifies how to open the file. 
 * 
 * It copies the path from user space, opens the vnode, and creates a thread-safe 
 * openfile structure. It sets access modes (RD/WR), handles O_APPEND by seeking 
 * to the end, and atomically installs the file into the current process's file table.
 * 
 * @param pathName relative or absolute path of the file to open
 * @param openFlags how to open the file
 * @param modeFile permissions to use if creating a file
 * @param returnVal pointer to store the new file descriptor
 * @return zero on success, an error value in case of failure
 */
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
    err = vfs_open(kernB, openFlags, modeFile, &vn);
    
    kfree(kernB); 
    if (err) return err;

    /* 4. Allocate Openfile Structure EARLY to avoid race cleanup mess later */
    openF = (struct openfile *) kmalloc(sizeof(struct openfile));
    if (openF == NULL) {
        vfs_close(vn);
        return ENOMEM;
    }

    /* Initialize struct */
    openF->vn = vn;
    openF->offset = 0;
    openF->countRef = 1;
    openF->openFlags = openFlags;
    openF->lockFile = lock_create("LOCK_FILE");
    
    if (openF->lockFile == NULL) {
        vfs_close(vn);
        kfree(openF);
        return ENOMEM;
    }

    /* Set Access Mode (Crucial for later read/write checks) */
    switch (openFlags & O_ACCMODE) {
        case O_RDONLY: openF->modeFile = O_RDONLY; break;
        case O_WRONLY: openF->modeFile = O_WRONLY; break;
        case O_RDWR:   openF->modeFile = O_RDWR;   break;
        default:
            vfs_close(vn);
            lock_destroy(openF->lockFile);
            kfree(openF);
            return EINVAL;
    }

    /* Handle O_APPEND */
    if (openFlags & O_APPEND) {
        err = VOP_STAT(vn, &fileStat);
        if (err) {
            vfs_close(vn);
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
        vfs_close(vn);
        lock_destroy(openF->lockFile);
        kfree(openF);
        return EMFILE;
    }
    
    *returnVal = fd;
    return 0;
}


/**
 * @brief sys_close() closes the file handle fd.
 *
 *  It atomically removes the file entry from the process's file table and 
 * decrements the reference count. If the reference count drops to zero 
 * (meaning no other process or descriptor points to this open file), 
 * it closes the underlying vnode and frees the memory.
 * 
 * @param fd file descriptor to close
 * @return zero on success, an error value in case of failure 
 */
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
        vfs_close(f->vn);
    }

    lock_release(f->lockFile);
    lock_destroy(f->lockFile);
    kfree(f);
    
    return 0;
}


/**
 * @brief sys_read() reads up to bufLen bytes from the file specified by fd, at the 
 * location in the file specified by the current seek position of the file, and 
 * stores them in the space pointed to by buffer. The file must be open for reading.
 * 
 * It acquires the file lock to ensure atomic reads, reads data from disk 
 * into a kernel buffer, and copies it to the user buffer. 
 * The current seek position of the file is advanced by the number of bytes read.
 *
 * @param fd source file descriptor
 * @param buffer destination buffer in user space
 * @param bufLen number of bytes to be read
 * @param returnVal pointer to store the actual number of bytes read
 * @return zero on success, an error value in case of failure
 */
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
    
    /* 2. Retrieve Openfile */
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

    /* 5. Acquire File Lock */
    lock_acquire(fl->lockFile);

    while (nRead < bufLen) 
    {
        size_t remaining = bufLen - nRead;
        size_t nLen = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;

        /* Prepare UIO for Kernel Buffer */
        uio_kinit(&iov, &ku, kBuffer, nLen, fl->offset, UIO_READ);

        res = VOP_READ(fl->vn, &ku);

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


/**
 * @brief sys_write() writes up to bufLen bytes to the file specified by fd, 
 * at the location in the file specified by the current seek position of the 
 * file, taking the data from the space pointed to by buffer.
 *
 *  It acquires the file lock, copies data from the user buffer to a kernel buffer,
 * and writes it to disk. If O_APPEND is set, it updates the offset to the 
 * end of the file before every write.
 * 
 * @param fd destination file descriptor
 * @param buffer source buffer in user space
 * @param bufLen number of bytes to be written
 * @param returnVal pointer to store the actual number of bytes written
 * @return zero on success, an error value in case of failure
 */
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


        if (fl->openFlags & O_APPEND) 
        {
            struct stat st;
            res = VOP_STAT(fl->vn, &st);
            
            if (res) {
                kfree(kBuffer);
                lock_release(fl->lockFile);
                return res;
            }
            fl->offset = st.st_size;
            /* Re-init uio with new offset */
            uio_kinit(&iov, &ku, kBuffer, nLen, fl->offset, UIO_WRITE);
        }

        res = VOP_WRITE(fl->vn, &ku);

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


/**
 * @brief sys_lseek() alters the current seek position of the file handle fd, seeking 
 * to a new position based on pos and whence.
 * 
 * It acquires the file lock to safely calculate and update the file offset.
 * It splits the resulting 64-bit offset into two 32-bit values (low and high)
 * to support the MIPS ABI return convention.
 * 
 * @param fd file handle
 * @param pos signed quantity indicating the offset to add
 * @param whence flag indicating the operation (SEEK_SET, SEEK_CUR, SEEK_END)
 * @param retval_low32 pointer to store the new seek position (lower 32 bits)
 * @param retval_upp32 pointer to store the new seek position (upper 32 bits)
 * @return zero on success, an error value in case of failure
 */
int sys_lseek(int fd, off_t pos, int whence, int32_t *retval_low32, int32_t *retval_upp32)
{
    struct openfile *fl;
    off_t retvalJoined = -1;
    struct stat st;
    int res;

    if(fd < 0 || fd >= OPEN_MAX) return EBADF; 
    fl = curproc->fileTable[fd];
    if(fl == NULL) return EBADF;
    if (!VOP_ISSEEKABLE(fl->vn)) return ESPIPE;

    lock_acquire(fl->lockFile);
    switch(whence)
    {
        case SEEK_SET:
            if (pos < 0) {
                lock_release(fl->lockFile);
                return EINVAL;
            }
            retvalJoined = pos;
            break;
        case SEEK_CUR:
            if (pos < 0 && -pos > fl->offset) {
                lock_release(fl->lockFile);
                return EINVAL;
            }
            retvalJoined = fl->offset + pos;
            break;
        case SEEK_END:
        {
            res = VOP_STAT(fl->vn,  &st);

            if(res) {
                lock_release(fl->lockFile);
                return res;
            }

            retvalJoined = st.st_size + pos;
            break;
        }
        default: 
        {
            lock_release(fl->lockFile);
            return EINVAL;
        }
    }

    if(retvalJoined < 0) {
        lock_release(fl->lockFile); // Unlock on error
        return EINVAL;
    }
    
    fl->offset = retvalJoined;
    lock_release(fl->lockFile);

    /* SET RETURN VALUES */
    *retval_upp32 = (int32_t)(retvalJoined >> 32);
    *retval_low32 = (int32_t)(retvalJoined & 0x00000000ffffffff);    
    return 0;
}


/**
 * @brief sys_dup2() clones the file handle oldfd onto the file handle newfd. 
 * If newfd names an open file, that file is closed first.
 * 
 * It increments the reference count of the file object to ensure it 
 * persists until both descriptors are closed. The operation is thread-safe 
 * using process locks.
 * 
 * @param oldfd existing file descriptor
 * @param newfd new file descriptor to be created/overwritten
 * @param retval pointer to store the new file descriptor
 * @return zero on success, an error value in case of failure 
 */
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
            vfs_close(new_of_to_close->vn);
            lock_destroy(new_of_to_close->lockFile);
            kfree(new_of_to_close);
        }
    }

    *retval = newfd;
    return 0;
}


/**
 * @brief sys_chdir() changes the current working directory of the current process 
 * to the directory named by pathName.
 * 
 * It performs a VFS lookup to find the corresponding vnode, verifies that 
 * the vnode is a directory, and updates the process structure.
 * 
 * @param pathName directory to be set as current
 * @return zero on success, an error value in case of failure
 */
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

    err = vfs_lookup(kernB, &vn);

    kfree(kernB);
    if (err) return err;

    struct stat fileStat;

    err = VOP_STAT(vn, &fileStat);

    if (err)
    {
        vfs_close(vn);
        return err;
    }
    
    if ((fileStat.st_mode & S_IFDIR) == 0)
    {
        vfs_close(vn);
        return ENOTDIR;
    }

    //it is a directory
    struct vnode *old_cwd = curproc->p_cwd;
    curproc->p_cwd = vn;
    
    if (old_cwd != NULL) {
        vfs_close(old_cwd);
    }

    return 0;
}


/**
 * @brief sys_getcwd() computes the name of the current directory and stores it in buffer.
 * The length of data actually stored is returned in returnVal.
 * 
 * It allocates a kernel buffer, calls vfs_getcwd to retrieve the absolute path,
 * and copies the result safely to the user-provided buffer.
 * 
 * @param buffer user buffer to store the result
 * @param bufLen length of the buffer
 * @param returnVal pointer to store the length of data actually written
 * @return zero on success, an error value in case of failure 
 */
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

    result = vfs_getcwd(&ku);

    if (result) {
        kfree(kBuffer);
        return result;
    }

    /* Calculate how much data was written to kBuffer */
    size_t actualLen = (bufLen < CHUNK_SIZE ? bufLen : CHUNK_SIZE) - ku.uio_resid;

    result = copyout(kBuffer, buffer, actualLen);
    
    kfree(kBuffer);

    if (result) {
        return result;
    }

    *returnVal = actualLen;
    return 0;
}


/**
 * @brief sys_remove() removes the file referred to by pathname from the filesystem.
 * 
 * (Stub implementation)
 * This function is currently a placeholder and returns success immediately.
 *
 *  @param pathname path of the file to remove
 * @return zero on success, an error value in case of failure
 */
int sys_remove(const char *pathname)
{

    /* NOT IMPLEMENTED  */
    (void)pathname;

    return 0;
}


/**
 * @brief sys_fstat() retrieves the status of the file associated with the file descriptor.
 * 
 * (Stub implementation)
 * This function is currently a placeholder and does not fill the stat structure.
 * 
 * @param fildes file descriptor
 * @param buf pointer to a stat structure to store the info
 * @return zero on success, an error value in case of failure
 */
int sys_fstat(int fildes, struct stat *buf)
{
    (void)fildes;
    (void)buf;

    return 0;
}

#endif