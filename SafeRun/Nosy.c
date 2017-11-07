#include <stdio.h>

int main() {
   FILE *fp = fopen("../Sample.txt", "r");
   int ch;

   if (fp == NULL)
      printf("No info for you!\n");
   else
      while (EOF != (ch = getc(fp)))
         putchar(ch);

   return 0;
}
