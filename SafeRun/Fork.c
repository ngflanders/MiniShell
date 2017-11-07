#include <stdio.h>
#include <unistd.h>

int main() {
   int pid;

   if (pid = fork())
      printf("I am the parent with child %d\n", pid);
   else
      printf("I am the child, with id %d\n", getpid());

   return 0;
}
