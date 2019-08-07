#include <cdefs.h>
#include <defs.h>
#include <memlayout.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <spinlock.h>
#include <trap.h>
#include <x86_64.h>

// Interrupt descriptor table (shared by all CPUs).
struct gate_desc idt[256];
extern void *vectors[]; // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

int num_page_faults = 0;

void tvinit(void) {
  int i;

  for (i = 0; i < 256; i++)
    set_gate_desc(&idt[i], 0, SEG_KCODE << 3, vectors[i], KERNEL_PL);
  set_gate_desc(&idt[TRAP_SYSCALL], 1, SEG_KCODE << 3, vectors[TRAP_SYSCALL],
                USER_PL);

  initlock(&tickslock, "time");
}

void idtinit(void) { lidt((void *)idt, sizeof(idt)); }

void trap(struct trap_frame *tf) {
  uint64_t addr;

  if (tf->trapno == TRAP_SYSCALL) {
    if (myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if (myproc()->killed)
      exit();
    return;
  }

  switch (tf->trapno) {
  case TRAP_IRQ0 + IRQ_TIMER:
    if (cpunum() == 0) {
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case TRAP_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case TRAP_IRQ0 + IRQ_IDE + 1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case TRAP_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case TRAP_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case TRAP_IRQ0 + 7:
  case TRAP_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n", cpunum(), tf->cs, tf->rip);
    lapiceoi();
    break;

  default:
    addr = rcr2();

    if (tf->trapno == TRAP_PF) {
      num_page_faults += 1;

      if ((tf->err & 5) == 4) {
        struct vregion* vregion;
        struct vpage_info* vpi;

        // Get vpi info
        vregion = va2vregion(&myproc()->vspace, addr);
        if (vregion) {
          vpi = va2vpage_info(vregion, addr);
          if (vpi && vpi->swapped) {
            // Check if the page is a swap page
            if (swappage_copy(vpi->swap_index) == -1) {
              panic ("cannot allocate new page for swap memory");
            }
            vspaceinstall(myproc());
            return;
          }
        }
      }

      if ((tf->err & 1) == 0
        && addr < myproc()->vspace.regions[VR_USTACK].va_base
        && addr > myproc()->vspace.regions[VR_USTACK].va_base - 10 * PGSIZE) {
        // if the page fault is on stack, then grow the stack
        struct vregion* stack = &myproc()->vspace.regions[VR_USTACK];
        uint64_t base = PGROUNDDOWN(addr);
        uint64_t size = stack->va_base - stack->size - base;

        if (vregionaddmap(stack, base, size, 1, 1) != size)
          panic("cannot allocate space in stack");

        stack->size += size;

        vspaceinvalidate(&myproc()->vspace);
        vspaceinstall(myproc());

        return;
      }

      if ((tf->err & 3) == 3) {
        // if it is a read only protecttion issue
        struct vregion* vregion;
        struct vpage_info* vpi;

        vregion = va2vregion(&myproc()->vspace, addr);
        if (vregion == NULL)
          panic("cannot get vregion for attempted address");
        vpi = va2vpage_info(vregion, addr);
        if (vpi == NULL)
          panic("cannot get vpage_info for attempted address");

        if (vpi->is_cow == 1) {
          // if the address is on a copy-on-write page

          // allocate a new page copy the page data
          if (ppage_copy(&vpi->ppn) == -1)
            panic("cannot allocate new page for copy-on-write memory");

          vpi->writable = 1;
          vpi->is_cow = 0;

          vspaceinvalidate(&myproc()->vspace);
          vspaceinstall(myproc());

          return;
        }
      }

      if (myproc() == 0 || (tf->cs & 3) == 0) {
        // In kernel, it must be our mistake.
        cprintf("unexpected trap %d from cpu %d rip %lx (cr2=0x%x)\n",
                tf->trapno, cpunum(), tf->rip, addr);
        panic("trap");
      }
    }

    // Assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "rip 0x%lx addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno, tf->err, cpunum(),
            tf->rip, addr);
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if (myproc() && myproc()->state == RUNNING &&
      tf->trapno == TRAP_IRQ0 + IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER)
    exit();
}
