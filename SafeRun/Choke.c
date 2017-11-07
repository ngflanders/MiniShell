#include <stdio.h>

int main() {
   int i, *p = (int *) -1;

   for (i = 0; i < 1000; i++)
      printf("%d\n", i);
   *p = 42;
}
