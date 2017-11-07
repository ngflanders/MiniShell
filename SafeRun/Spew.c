#include <stdio.h>
#include <string.h>

int main() {
   char *spew = "Spew\n";
   int len = strlen(spew);

   for (;;)
      write(1, spew, len);
}
