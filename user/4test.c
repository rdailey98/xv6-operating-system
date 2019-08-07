#include <cdefs.h>
#include <fcntl.h>
#include <fs.h>
#include <memlayout.h>
#include <stat.h>
#include <sysinfo.h>
#include <user.h>

int stdout = 1;

#define error(msg, ...)                                                        \
  do {                                                                         \
    printf(stdout, "ERROR (line %d): ", __LINE__);                             \
    printf(stdout, msg, ##__VA_ARGS__);                                        \
    printf(stdout, "\n");                                                      \
    while (1) {                                                                \
    }                                                                          \
  } while (0)

#define START_PAGES (600)
#define SWAP_TEST_PAGES (START_PAGES * 2)

void swaptest(void) {
  char *start = sbrk(0);
  char *a;
  int i;
  int b = 4096;
  int num_pages_to_alloc = SWAP_TEST_PAGES;
  struct sys_info info1, info2, info3;

  if (!fork()) {
    for (i = 0; i < num_pages_to_alloc; i++) {
      a = sbrk(b);
      if (a == (char *)-1) {
        printf(stdout, "no more memory\n");
        break;
      }
      memset(a, 0, b);
      *(int *)a = i;
        printf(stdout, "%d pages allocated\n", i);
    }

    sysinfo(&info1);

    // check whether memory data is consistent
    for (i = 0; i < num_pages_to_alloc; i++) {
      if (i % 100 == 0)
        printf(stdout, "checking i %d\n", i);
      if (*(int *)(start + i * b) != i) {
        error("data is incorrect, should be %d, but %d\n", i,
              *(int *)(start + i * b));
      }
    }

    sysinfo(&info2);

    printf(stdout, "number of disk reads = %d\n",
           info2.num_disk_reads - info1.num_disk_reads);

    sysinfo(&info3);
    printf(stdout, "number of pages in swap = %d\n", info3.pages_in_swap);

    printf(stdout, "swaptest OK\n");
    exit();
  } else {
    wait();
  }
}


int main(int argc, char *argv[]) {
  swaptest();
  printf(stdout, "lab4 tests passed!!\n");
  exit();
}
