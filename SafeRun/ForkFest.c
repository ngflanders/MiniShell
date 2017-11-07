#include <stdio.h>

int main() {
   while (!fork())
      ;

   return 0;
}
