#include <types.h>
#include <vm.h>

#include "opt-c2os.h"

#define EXEC_MAX_PROC	1

struct semaphore *exec_sem;

struct exec_args {
    char *data;	
    size_t len;
	size_t max;
	int nargs;
	bool tooksem;
};

void exec_create_sem(void);

int argbuf_fromuser(struct exec_args *buf, userptr_t uargv);

int argbuf_copyin(struct exec_args *buf, userptr_t uargv);

int argbuf_copyout(struct exec_args *buf, vaddr_t *ustackp, int *argc_ret, userptr_t *uargv_ret);

int argbuf_allocate(struct exec_args *buf, size_t size);

void argbuf_cleanup(struct exec_args *buf);

int loadexec(char *path, vaddr_t *entrypoint, vaddr_t *stackptr);