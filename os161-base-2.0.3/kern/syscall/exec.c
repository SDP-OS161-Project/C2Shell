#include <types.h>
#include <proc.h>
#include <vfs.h>
#include <synch.h>
#include <copyinout.h>
#include <kern/errno.h>
#include <lib.h>
#include <addrspace.h>
#include <current.h>
#include "exec.h"
#include <kern/fcntl.h>

void exec_create_sem(void) {

    exec_sem = sem_create("execsem", EXEC_MAX_PROC);
	if (exec_sem == NULL) {
		panic("Cannot create exec throttle semaphore\n");
	}
}

int argbuf_copyin(struct exec_args *buf, userptr_t uargv) {

    userptr_t user_arg_ptr;
	size_t copied_len;
	int err;

	for (;;) {
		//retrieve the next pointer from argv (then move uargv forward later)
		err = copyin(uargv, &user_arg_ptr, sizeof(userptr_t));
		if (err) {
			return err;
		}

		//stop when we hit the NULL terminator in argv[]
		if (user_arg_ptr == NULL) {
			break;
		}

		//copy the actual argument string from user space into our kernel buffer
		err = copyinstr(user_arg_ptr, buf->data + buf->len, buf->max - buf->len, &copied_len);
		if (err == ENAMETOOLONG) {
			return E2BIG;
		} else if (err) {
			return err;
		}

		//advance buffer length and argument count
		buf->len += copied_len;
		buf->nargs++;

		//advance to the next argv pointer
		uargv += sizeof(userptr_t);
	}

	return 0;
}

int argbuf_copyout(struct exec_args *args, vaddr_t *stack_top, int *argc_out, userptr_t *argv_out) {

	vaddr_t sp;
	userptr_t user_str_base, user_argv_base, argv_slot;
	userptr_t user_str;
	size_t copied_len, offset;
	int err;

	//initialize stack pointer
	sp = *stack_top;

	/*
	 * Layout plan:
	 * [ strings region ][ alignment padding ][ argv pointers ][ NULL ]
	 */

	//reserve space for argument strings
	sp -= args->len;
	sp &= ~(sizeof(void *) - 1); //align stack
	user_str_base = (userptr_t) sp;

	//reserve space for argv[] pointers (+1 for NULL terminator)
	sp -= (args->nargs + 1) * sizeof(userptr_t);
	user_argv_base = (userptr_t) sp;

	offset = 0;
	argv_slot = user_argv_base;

	for (offset = 0; offset < args->len; ) {
		//compute destination address for the current string
		user_str = user_str_base + offset;

		//store the string pointer into argv[]
		err = copyout(&user_str, argv_slot, sizeof(userptr_t));
		if (err) {
			return err;
		}

		//copy the actual string into user memory
		err = copyoutstr(args->data + offset, user_str, args->len - offset, &copied_len);
		if (err) {
			return err;
		}

		//move forward in both buffers
		offset += copied_len;
		argv_slot += sizeof(userptr_t);
	}

	KASSERT(offset == args->len);

	//append NULL to argv[]
	user_str = NULL;
	err = copyout(&user_str, argv_slot, sizeof(userptr_t));
	if (err) {
		return err;
	}

	//return final stack pointer and argv info
	*stack_top = sp;
	*argc_out = args->nargs;
	*argv_out = user_argv_base;

	return 0;

}

int argbuf_allocate(struct exec_args *args, size_t capacity) {

    args->data = kmalloc(capacity);
	if (args->data == NULL) {
		return ENOMEM;
	}
	args->max = capacity;
	return 0;
}

void argbuf_cleanup(struct exec_args *args) {
    
    if (args->data != NULL) {
		kfree(args->data);
		args->data = NULL;
	}

	args->len = 0;
	args->max = 0;
	args->nargs = 0;

	//release semaphore if taken
	if (args->tooksem) {
		V(exec_sem);
		args->tooksem = false;
	}
}

int argbuf_fromuser(struct exec_args *args, userptr_t user_argv) {

    int err;

	//first attempt: use a single-page buffer
	err = argbuf_allocate(args, PAGE_SIZE);
	if (err) {
		return err;
	}

	err = argbuf_copyin(args, user_argv);
	if (err == E2BIG) {
		//the arguments didn't fit; retry with a larger buffer

		argbuf_cleanup(args);
		memset(args, 0, sizeof(*args));

		//throttle large allocations using semaphore
		P(exec_sem);
		args->tooksem = true;

		err = argbuf_allocate(args, ARG_MAX);
		if (err) {
			return err;
		}

		err = argbuf_copyin(args, user_argv);
	}

	return err;
}

int loadexec(char *progpath, vaddr_t *entry_out, vaddr_t *stack_out) {

	struct addrspace *new_as = NULL;
	struct addrspace *prev_as = NULL;
	struct vnode *vnode = NULL;
	char *proc_name = NULL;
	int result;

	//duplicate program name for the new thread context
	proc_name = kstrdup(progpath);
	if (proc_name == NULL) {
		return ENOMEM;
	}

	//try to open the executable file
	result = vfs_open(progpath, O_RDONLY, 0, &vnode);
	if (result) {
		kfree(proc_name);
		return result;
	}

	//allocate a fresh address space for the process
	new_as = as_create();
	if (new_as == NULL) {
		vfs_close(vnode);
		kfree(proc_name);
		return ENOMEM;
	}

	//install and activate the new address space
	prev_as = proc_setas(new_as);
	as_activate();

 	//load the ELF binary into memory
	result = load_elf(vnode, entry_out);
	if (result) {
		//on failure, roll back to the previous address space
		vfs_close(vnode);
		proc_setas(prev_as);
		as_activate();
		as_destroy(new_as);
		kfree(proc_name);
		return result;
	}

	//file no longer needed once ELF is loaded
	vfs_close(vnode);

	//create a new user stack region
	result = as_define_stack(new_as, stack_out);
	if (result) {
		proc_setas(prev_as);
		as_activate();
		as_destroy(new_as);
		kfree(proc_name);
		return result;
	}

	//discard the previous address space now that we’re successful
	if (prev_as != NULL) {
		as_destroy(prev_as);
	}

	//update the thread name to reflect the new executable
	kfree(curthread->t_name);
	curthread->t_name = proc_name;

	return 0;
}