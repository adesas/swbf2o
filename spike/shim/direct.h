#pragma once
#include <unistd.h>
static inline int _chdir(const char* p) { return chdir(p); }
static inline int _mkdir(const char* p) { return mkdir(p, 0777); }
