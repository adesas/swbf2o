#pragma once
#include "swbf2_prefix.h"
typedef struct {
  DWORD cb;
  DWORD PageFaultCount;
  SIZE_T PeakWorkingSetSize;
  SIZE_T WorkingSetSize;
  SIZE_T QuotaPeakPagedPoolUsage;
  SIZE_T QuotaPagedPoolUsage;
  SIZE_T QuotaPeakNonPagedPoolUsage;
  SIZE_T QuotaNonPagedPoolUsage;
  SIZE_T PagefileUsage;
  SIZE_T PeakPagefileUsage;
} PROCESS_MEMORY_COUNTERS;
static inline int GetProcessMemoryInfo(HANDLE h, PROCESS_MEMORY_COUNTERS* p, DWORD cb) {
  (void)h; if (p && cb >= sizeof(*p)) memset(p, 0, sizeof(*p)); return 0;
}
#ifndef K32GetProcessMemoryInfo
#define K32GetProcessMemoryInfo GetProcessMemoryInfo
#endif
