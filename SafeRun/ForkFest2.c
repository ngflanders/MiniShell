#include <stdio.h>
#include <sys/time.h>
#include <sys/resource.h>

int main() {
   int child;
   int childCount = 0;
   struct rlimit procLim = {40, 40};
   struct rlimit cpuLim = {5, 5};

   setrlimit(RLIMIT_CPU, &cpuLim);
   setrlimit(RLIMIT_NPROC, &procLim);
   do {
      while ((child = fork()) < 0)
         ;
      if (child) {
         printf("Proc %d spawns %d\n", getpid(), child);
         childCount++;
         setsid();
      }
      else
         childCount = 0;
   } while (childCount < 3);

   return 0;
}
