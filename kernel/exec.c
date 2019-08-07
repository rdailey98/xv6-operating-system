#include <cdefs.h>
#include <defs.h>
#include <elf.h>
#include <memlayout.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <trap.h>
#include <x86_64.h>

int exec(char *path, char **argv) {
  // your code here
  struct vspace vs; // new vspace
  struct vspace vs_old = myproc()->vspace; // current vspace
  int size = 0;
  char** argv_new; // argv array on new stack
  char* str;
  struct trap_frame tf = *myproc()->tf; // new trap frame

  // verify argv address and find its length
  while(true) {
    int len = fetchstr((uint64_t)argv[size], &str);
    size++;
    
    if (len == -1)
      return -1;

    // if it is the end \0
    if (strncmp(str, 0, 1) == 0)
      break;
  }

  // Initialize new vspace
  if (vspaceinit(&vs) == -1)
    return -1;

  if (vspaceinitstack(&vs, SZ_2G) == -1)
    return -1;

  if (vspaceloadcode(&vs, path, &tf.rip) == 0)
    return -1;

  tf.rsp = SZ_2G; // set stack pointer
  tf.rdi = size - 1; // set argc argument
  
  // allocate space for string array
  tf.rsp -= sizeof(char*) * size;
  argv_new = (char**) tf.rsp;
  tf.rsi = tf.rsp; // set argv argument

  for (int i = 0; i < size; i++) {
    int len = fetchstr((uint64_t)argv[i], &str);

    // allocate space for the string
    tf.rsp -= len + 1;

    // copy the string over on new stack
    if (vspacewritetova(&vs, tf.rsp, str, len + 1) == -1)
      return -1;

    // copy the pointer to string to the new argv array
    if (vspacewritetova(&vs, (uint64_t)(argv_new + i), (char*)&tf.rsp, sizeof(tf.rsp)) == -1)
      return -1;
  }
  tf.rsp -= 8; // leave room for return address
  
  // clear olg page table map
  if (vspaceinit(&myproc()->vspace) == -1)
    return -1;
  // copy the new space to current process
  if (vspacecopy_cow(&myproc()->vspace, &vs) == -1)
    return -1;

  vspaceinstall(myproc());

  // free old vspace
  vspacefree(&vs_old);
  vspacefree(&vs);

  // set trap frame and return
  tf.rax = 0;
  *myproc()->tf = tf;

  return 0;
}
