/* Force-included (-include) when compiling upstream games/totk/runtime files unchanged with MinGW: the few POSIX
   names their shared header (runtime.h) uses that Windows lacks. */
#pragma once
#include <setjmp.h>
typedef jmp_buf sigjmp_buf;
