#pragma once
#include <stdio.h>
#include <unistd.h>
static inline int access_msvc(const char* p) { return access(p, 0); }
