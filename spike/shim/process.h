#pragma once
#include "swbf2_prefix.h"
static inline unsigned long _beginthread(void (*fn)(void*), unsigned stack, void* arg) { fn(arg); return 0; }
static inline void _endthread(void) {}
