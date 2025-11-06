
#include <kern/c2_syscall.h>

int sys_getpid(pid_t *retval) {

  //check that there is a current process running  
    if(curproc != NULL){

       //store the current process PID in the return value pointer
       *retval = curproc->p_pid; 

    }

    return 0;   
}