/* Host functions with awkward signatures for the bridge test (tests/bridge_test.c). */
#pragma once
#include <stdint.h>
typedef struct { uint64_t v; } small_t;          /* <= 16 bytes: passed and returned in integer registers */
typedef struct { uint64_t a[4]; } big_t;         /* > 16 bytes: returned through the caller's buffer (x8) */
typedef enum { MODE_A = 1, MODE_B = 7 } mode_t_;
