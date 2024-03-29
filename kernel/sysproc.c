#include <cdefs.h>
#include <date.h>
#include <defs.h>
#include <memlayout.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <x86_64.h>

int sys_crashn(void) {
  int n;
  if (argint(0, &n) < 0)
    return -1;

  crashn_enable = 1;
  crashn = n;

  return 0;
}

int sys_fork(void){
  return fork();
}

void halt(void) {
  while (1)
    ;
}

int sys_exit(void) {
  exit();
  return 0; // not reached
}

int sys_wait(void) { return wait(); }

int sys_kill(void) {
  int pid;

  if (argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int sys_getpid(void) { return myproc()->pid; }

int sys_sbrk(void) {
  // LAB3
  int size;
  int old_limit;
  struct vregion* heap;

  if (argint(0, &size) < 0)
    return -1;

  heap = &myproc()->vspace.regions[VR_HEAP];
  old_limit = heap->va_base + heap->size;

  if (size <= 0) return old_limit;
  
  // allocate and map new pages
  if (vregionaddmap(heap, old_limit, size, 1, 1) != size)
    return -1;
  
  // update heap size
  heap->size += size;

  vspaceinvalidate(&myproc()->vspace);
  vspaceinstall(myproc());

  return old_limit;
}

int sys_sleep(void) {
  int n;
  uint ticks0;

  if (argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (myproc()->killed) {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int sys_uptime(void) {
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
