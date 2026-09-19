/* */
#include <stdio.h>

int main(int argc, char** argv)
{
  (void)argv;
#ifndef fwrite_unlocked
  return ((int*)(&fwrite_unlocked))[argc];
#else
  (void)argc;
  return 0;
#endif
}
