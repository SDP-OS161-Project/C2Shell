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

#define SYSTEM_OPEN_MAX (10*OPEN_MAX)
#define CHUNK_SIZE 4096

struct openfile systemFileTable[SYSTEM_OPEN_MAX]

#if OPT_C2OS

int sys_open(userptr_t pathName, int openFlags, mode_t modeFile, int32_t *returnVal) 
{
    size_t len;
    struct vnode *vn;
    int err;
    char *kernB;
    struct openfile *openF = NULL;
    struct stat fileStat;
    
    if (pathName == NULL)
    {
        return EFAULT;
    }

    kernB = (char *) kmalloc(PATH_MAX * sizeof(char) + 1);
    if(kernB == NULL)
    {
        return ENOMEM;
    }

    err = copyinstr((const_userptr_t) pathName, kernB, PATH_MAX, &len);
    if (err)
    {
        kfree(kernB);
        return err;
    }

    err = vfs_open(kernB, openFlags, modeFile, &vn);
    if (err)
    {
        kfree(kernB);
        return err;
    }
    kfree(kernB);

    for (int i = 0; i < systemFileTable.size(); i++)
    {
        if (systemFileTable[i].vn == NULL)
        {
            openF->vn = &systemFileTable[i].vn;
            openF->vn = vn;
            openF->openFlags = openFlags;
            break;
        }
    }

    if (openF == NULL)
    {
        return ENFILE;
    }
    else
    {
        for (int i = 3; i < OPEN_MAX; i++)
        {
            if(i == (OPEN_MAX - 1))
            {
                return EMFILE;
            }
            else if (curproc->fileTable[i] == NULL)
            {
                curproc->fileTable[i] = openF;
                if (openFlags & O_APPEND)
                {
                    err = VOP_STAT(curproc->fileTable[i]->vn, &fileStat);
                    if (err)
                    {
                        kfree(curproc->fileTable[i]);
                        curproc->fileTable[i] = NULL;
                        return err;
                    }
                    curproc->fileTable[i]->offset = fileStat.st_size;
                }
                else
                {
                    curproc->fileTable[i]->offset = 0;
                }
                //TODO: find out if an atomic action should take place instead of this one
                curproc->fileTable[i]->countRefs = 1;

                switch (openFlags & O_ACCMODE)
                {
                    case O_RDONLY:
                        curproc->fileTable[i]->modeFile = O_RDONLY;
                        break;
                    case O_WRONLY:
                        curproc->fileTable[i]->modeFile = O_WRONLY;
                        break;
                    case O_RDWR:
                        curproc->fileTable[i]->modeFile = O_RDWR;
                        break;
                    default:
                        vfs_close(curproc->fileTable[i]->vn);
                        kfree(curproc->fileTablep[i]);
                        curproc->fileTable[i] = NULL;
                        return EINVAL;
                }

                curproc->fileTable[i]->lockFile = lock_create("LOCK_FILE");
                if (curproc->fileTable[i]->lockFile == NULL)
                {
                    vfs_close(curproc->fileTable[i]->vn);
                        kfree(curproc->fileTablep[i]);
                        curproc->fileTable[i] = NULL;
                        return ENOMEM;
                }

                *returnVal = i;

                break;
            }
        }
    }

    return 0;
}

int sys_close(int fd)
{
    //reference: ops class documentation
    //returns 0 on success, -1 on error, and errno is set to indicate the error
    struct openfile *f = NULL;
    struct vnode *vn;

    if(fd < 0 || fd > OPEN_MAX)
    {
        return EBADF; //invalid id for file
    }

    f = curproc->fileTable[fd];
    if(f == NULL)
    {
        return EBADF; //there isnt an open file with this fd
    }
    curproc->fileTable[fd] = NULL;

    lock_acquire(f->lockFile);
    f->countRef--;
    if(f->countRef > 0)
    {
        return 0; //still in use
    }

    vn = f->vn;
    f->vn = NULL;
    if(vn != NULL)
    {
        vfs_close(vn); //closing the open file
    }

    kfree(f);
    return 0;
}

int sys_read(int fd, userptr_t buffer, size_t bufLen, ssize_t *returnVal)
{
    struct openfile *fl;
    struct vnode *vn;
    struct iovec iov;
    struct uio ku;
    int res;

    char *kBuffer = NULL;
    size_t nRead;

    if(fd < 0 || fd > OPEN_MAX)
    {
        return EBADF; //invalid id for file
    }

    fl = curproc->fileTable[fd];
    if(fl == NULL)
    {
        return EBADF; //there isnt an open file with this fd
    }
    curproc->fileTable[fd] = NULL;

    vn = fl->vn;
    if(vn == NULL)
    {
        return EBADF; 
    }

    if (buffer == NULL) 
    {
        return EFAULT;
    }

    if (bufLen > SSIZE_MAX)
    {
        return EINVAL;
    }

    if (bufLen == 0) 
    {
        *returnVal = 0;
        return 0;
    }

    kBuffer = kmalloc(CHUNK_SIZE);
    if (kBuffer == NULL)
    {
        return ENOMEM;
    }

    while (nRead < bufLen) 
    {
        size_t remaining = bufLen - nRead;
        size_t nLen = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;

        lock_acquire(fl->lockFile);

        uio_kinit(&iov, &ku, kBuffer, nLen, fl->offset, UIO_READ);

        res = VOP_READ(vn, &ku);
        if (res) 
        {
            kfree(kBuffer);
            lock_release(fl->lockFile);
            return res;
        }

        size_t readThisChunk = nLen - ku.uio_resid;
        fl->offset += readThisChunk;
        lock_release(fl->lockFile);

        if (readThisChunk == 0) 
        {
            break;
        }

        // copyout to user buffer at offset nRead
        res = copyout(kBuffer, (userptr_t)((uintptr_t)buffer + nRead), readThisChunk);
        if (res) 
        {
            kfree(kBuffer);
            return res;
        }

        nRead += readThisChunk;

        // If the VOP read less or none break
        if (readThisChunk < nLen || readThisChunk == 0) 
        {
            break;
        }
    }

    fl->offset += nRead;
    *returnVal = nRead;
    return 0;
}

// TODO: check whether it would be better to check the bufLen in order to create a kernel buffer
// PageSize = 4096 bytes, so if bufLen is too large it may cause memory issues
// it would be better to cycle and write in chunks of CHUNK_SIZE
int sys_write(int fd, userptr_t buffer, size_t bufLen, ssize_t *returnVal)
{
    struct openfile *fl;
    struct iovec iov;
    struct uio ku;
    int res;
    size_t nWrite = 0;
    char *kBuffer = NULL;

    // Validate arguments
    if (buffer == NULL) 
    {
        return EFAULT;
    }
    if (bufLen > SSIZE_MAX) 
    {
        return EINVAL;
    }
    if (bufLen == 0) 
    {
        *returnVal = 0;
        return 0;
    }

    // Allocate a fixed-size kernel buffer
    kBuffer = kmalloc(CHUNK_SIZE);
    if (kBuffer == NULL) 
    {
        return ENOMEM;
    }

    if (fd < 0 || fd >= OPEN_MAX) 
    {
        kfree(kBuffer);
        return EBADF;
    }

    fl = curproc->fileTable[fd];
    if (fl == NULL) 
    {
        kfree(kBuffer);
        return EBADF;
    }

    // Check that file is opened for writing
    // TODO: add handling for different modes for example O_APPEND
    if (fl->openFlags == O_RDONLY) 
    {
        kfree(kBuffer);
        return EBADF;
    }

    while (nWrite < bufLen) 
    {
        size_t remaining = bufLen - nWrite;
        size_t nLen = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;

        // copyin from user buffer at offset nWrite
        res = copyin((const_userptr_t)((uintptr_t)buffer + nWrite), kBuffer, nLen);
        if (res) 
        {
            kfree(kBuffer);
            lock_release(fl->lockFile);
            return res;
        }

        // Acquire file lock for the duration of the write/update of offset
        lock_acquire(fl->lockFile);

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
        }

        res = VOP_WRITE(fl->vn, &ku);
        if (res) 
        {
            kfree(kBuffer);
            return res;
        }

        // Count actual bytes written in this chunk
        size_t wrote = nLen - ku.uio_resid;
        nWrite += wrote;
        fl->offset += wrote;

        lock_release(fl->lockFile);  

        // If the VOP wrote less or none break
        if (wrote < nLen || wrote == 0) 
        {
            break;
        }
    }

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

    if(fd < 0 || fd > OPEN_MAX)
    {
        return EBADF; //invalid id for file
    }

    fl = curproc->fileTable[fd];
    if(fl == NULL)
    {
        return EBADF; //there isnt an open file with this fd
    }

    if (!VOP_ISSEEKABLE(fl->vn))
    {
        return ESPIPE;
    }

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
            res = VOP_STAT(fl->vn,  &st);
            if(res)
            {
                return res;
            }

            newOff = st.st_size + pos;
            break;
        }
        default:
            return EINVAL;
    }

    if(newOff < 0)
    {
        return EINVAL;
    }

    lock_acquire(fl->lockFile);
    fl->offset = newOff;
    lock_release(fl->lockFile);
    
    *returnVal = newOff;
    return 0;
}

/*
Error codes:
EBADF: The file descriptor is not valid or is not open.
EMFILE: The file descriptor table for the process is full or limit
    was reached.
ENFILE: The system-wide limit on the total number of open files has
    been reached.
*/
int sys_dup2(int oldFd, int newFd, int *returnVal)
{
    struct file *oldfile;
    struct file *newfile;
    struct proc *p = curproc;
    struct openfile *toClose = NULL;

    /* Validate file descriptor range */
    if (oldFd < 0 || oldFd >= OPEN_MAX || 
        newFd < 0 || newFd >= OPEN_MAX)
    {
        return EBADF;
    }

    /* If oldFd == newFd, do nothing per POSIX */
    if (oldFd == newFd)
    {
        *returnVal = newFd;
        return 0;
    }

    /* Check that oldFd is actually open */
    /* TODO: Add check for global file count to be less or equal
         than system max open files (SYSTEM_OPEN_MAX)
    */
    lock_acquire(p->ft_lock);
    oldfile = p->filetable[oldFd];
    if (oldfile == NULL)
    {
        lock_release(p->ft_lock);
        return EBADF;
    }

    /* If newFd already open, prepare to close it first */
    newfile = p->filetable[newFd];
    if (newfile != NULL)
    {
        /* Drop the old reference */
        toClose = newfile;
        p->filetable[newFd] = NULL;
    }

    /* Increment reference count on the existing file */
    file_incref(oldfile);
    p->filetable[newFd] = oldfile;

    lock_release(p->ft_lock);

    if (toClose != NULL)
    {
        /* Close the old file outside the lock */
        file_decref(toClose);
    }

    *returnVal = newFd;
    return 0;
}

/*
Error codes:
- ENODEV: device prefix of pathname did not exist
- ENOTDIR: a non-final component of the path prefix is not a directory
- ENOTDIR: the final component of the path prefix is not a directory
- ENOENT: did not exist
- EIO: a hard io error occurred
- EFAULT: pathName points to an invalid address
*/
int sys_chdir(userptr_t pathName)
{
    size_t len;
    struct vnode *vn;
    int err;
    char *kernB;

    if (pathName == NULL)
    {
        return EFAULT;
    }

    kernB = (char *) kmalloc(PATH_MAX * sizeof(char) + 1);
    if(kernB == NULL)
    {
        return ENOMEM;
    }

    err = copyinstr((const_userptr_t) pathName, kernB, PATH_MAX, &len);
    if (err)
    {
        kfree(kernB);
        return err;
    }

    err = vfs_lookup(kernB, &vn);
    kfree(kernB);
    if (err)
    {
        return err;
    }

    //check if it is a directory
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
    vfs_close(curproc->p_cwd);
    curproc->p_cwd = vn;

    return 0;
}

/*
Error codes:
- ENOENT: a component of the pathname no longer exists
- EIO: a hard io error occurred
- EFAULT: buf points to an invalid address space
*/
int sys_getcwd(userptr_t buffer, size_t bufLen, int *returnVal)
{
    int result;
    struct iovec iov;
    struct uio ku;

    /* Validate arguments */
    if (buffer == NULL) {
        return EFAULT;
    }

    /* Initialize kernel-side UIO for writing the path into user buffer */
    uio_uinit(&iov, &ku, buffer, bufLen, 0, UIO_READ);

    /* Ask the VFS for the current working directory */
    result = vfs_getcwd(&ku);
    if (result) {
        /*
         * vfs_getcwd() already returns the correct codes:
         *   ENOENT if cwd vanished
         *   EIO on disk error
         *   EFAULT if copyout failed (invalid user pointer)
         */
        return result;
    }

    /* Success: uio_resid is bytes left, so subtract from buflen */
    *returnVal = bufLen - ku.uio_resid;
    return 0;
}

#endif