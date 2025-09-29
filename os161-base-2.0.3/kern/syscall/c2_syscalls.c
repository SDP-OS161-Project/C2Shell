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

#define SYSTEM_OPEN_MAX (10*OPEN_MAX)

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
            openF = &systemFileTable[i].vn;
            openF->vn = vn;
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
    //reference: man7 documentation
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

    char kBuffer[128];
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

    vn = f->vn;
    if(vn == NULL)
    {
        return EBADF; 
    }

    uio_kinit(&iov, &ku, kBuffer, bufLen, fl->offset, UIO_READ);

    res = VOP_READ(fl->vn, &ku);
    if(res)
    {   // VOP_READ may return errors so we need this check in case 
        return res;
    }

    nRead = bufLen - ku.uio_resid;
    res = copyout(kBuffer, buffer, nRead);
    if(res)
    {   //check for user space memory errors while copying the data inside buffer
        return res;
    }

    fl->offset += nRead;
    *returnVal = nRead;
    return 0;
}

#endif