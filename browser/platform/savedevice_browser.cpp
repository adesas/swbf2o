// Browser OPFS-backed replacement for Battlefront2/Platform/Pc/pcSaveDevice.cpp.
//
// Persists SWBF2 profiles (*.profile), Galactic Conquest saves (*.gc), and Campaign saves (*.rote)
// into OPFS `/saves/savegames/` via our Phase 1 OpfsVfs bridge (browser/vfs_opfs.mjs), with a
// POSIX `./SaveGames/` fallback for headless Node / native oracle runs.

#include "Battlefront2/Platform/Pc/pcSaveDevice.h"
#include "PebbleFL/Common/PblUnicode.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// Match retail pcSaveDevice.cpp globals referenced by SaveGameManager / Lua callbacks.
// Note: SWBF2 uses 16-bit UTF-16 strings (unsigned short*), whereas Linux/wasm32 wchar_t is 32-bit.
unsigned short* SAVE_DIRECTORY = (unsigned short*)u"SaveGames";

unsigned short* SFTPrefix[4] = {
    (unsigned short*)u"",
    (unsigned short*)u"profile",
    (unsigned short*)u"gc",
    (unsigned short*)u"rote",
};

namespace {

const char* kExtAscii[4] = {"", "profile", "gc", "rote"};

#ifdef __EMSCRIPTEN__
EM_JS(int, js_save_vfs_active, (), {
  return (typeof Module !== 'undefined' && typeof Module.vfsOpen === 'function') ? 1 : 0;
});
EM_JS(int, js_save_vfs_open, (const char* path, int mode), {
  return Module.vfsOpen(UTF8ToString(path), mode);
});
EM_JS(void, js_save_vfs_close, (int fd), {
  Module.vfsClose(fd);
});
EM_JS(int, js_save_vfs_read, (int fd, uint32_t off, void* dst, int len), {
  return Module.vfsRead(fd, off >>> 0, dst >>> 0, len);
});
EM_JS(int, js_save_vfs_write, (int fd, uint32_t off, const void* src, int len), {
  return Module.vfsWrite(fd, off >>> 0, src >>> 0, len);
});
EM_JS(void, js_save_vfs_flush, (int fd), {
  Module.vfsFlush(fd);
});
EM_JS(void, js_save_vfs_delete, (const char* path), {
  Module.vfsDelete(UTF8ToString(path));
});
EM_JS(int, js_save_vfs_list, (const char* prefix, char* dst, int maxLen), {
  if (typeof Module.vfsListSaves !== 'function') {
    if (maxLen > 0) HEAPU8[dst] = 0;
    return 0;
  }
  return Module.vfsListSaves(UTF8ToString(prefix), dst >>> 0, maxLen);
});
#else
static inline int js_save_vfs_active() { return 0; }
static inline int js_save_vfs_open(const char*, int) { return -1; }
static inline void js_save_vfs_close(int) {}
static inline int js_save_vfs_read(int, uint32_t, void*, int) { return 0; }
static inline int js_save_vfs_write(int, uint32_t, const void*, int) { return 0; }
static inline void js_save_vfs_flush(int) {}
static inline void js_save_vfs_delete(const char*) {}
static inline int js_save_vfs_list(const char*, char* dst, int maxLen) {
  if (maxLen > 0) dst[0] = 0;
  return 0;
}
#endif

struct OpfsSaveHandle {
  int fd;
  uint32_t pos;
};

void utf16ToAscii(const unsigned short* src, char* dst, size_t dstCap) {
  size_t i = 0;
  if (src) {
    while (i + 1 < dstCap && src[i]) {
      unsigned short ch = src[i];
      dst[i] = (ch < 128) ? (char)ch : '_';
      ++i;
    }
  }
  dst[i] = '\0';
}

void asciiToUtf16(const char* src, unsigned short* dst, size_t dstCap) {
  size_t i = 0;
  if (src) {
    while (i + 1 < dstCap && src[i]) {
      dst[i] = (unsigned char)src[i];
      ++i;
    }
  }
  dst[i] = 0;
}

void buildSavePath(const unsigned short* pName, int iType, bool forOpfs, char* out, size_t outCap) {
  char name[256];
  utf16ToAscii(pName, name, sizeof(name));
  const char* ext = (iType >= 1 && iType < 4) ? kExtAscii[iType] : "";
  if (forOpfs) {
    if (ext[0]) {
      snprintf(out, outCap, "/saves/savegames/%s.%s", name, ext);
    } else {
      snprintf(out, outCap, "/saves/savegames/%s", name);
    }
  } else {
    if (ext[0]) {
      snprintf(out, outCap, "SaveGames/%s.%s", name, ext);
    } else {
      snprintf(out, outCap, "SaveGames/%s", name);
    }
  }
}

int extToSaveType(const char* ext) {
  if (!ext || !ext[0]) return 0;
  for (int i = 1; i < 4; ++i) {
    if (strcasecmp(ext, kExtAscii[i]) == 0) return i;
  }
  return 0;
}

}  // namespace

SaveDevice::SaveDevice()
    : mNumFiles(0), mCurFile(0), mLastAsyncResult(0) {
}

int SaveDevice::GetStatus() { return 0; }
unsigned int SaveDevice::GetFreeFiles() { return 100; }
unsigned int SaveDevice::GetFreeSpace() { return 64u * 1024u * 1024u; }
unsigned int SaveDevice::BytesToBlocks(unsigned int uiBytes) { return uiBytes; }
void SaveDevice::MarkMemoryCard() {}
bool SaveDevice::CheckMemoryCardMark() { return true; }

int SaveDevice::StartCreateFile(const unsigned short* pName, int iType) {
  char path[512];
  if (js_save_vfs_active()) {
    buildSavePath(pName, iType, true, path, sizeof(path));
    int fd = js_save_vfs_open(path, 1);  // create/truncate rw
    if (fd < 0) return -1;
    js_save_vfs_flush(fd);
    js_save_vfs_close(fd);
  } else {
    mkdir("SaveGames", 0755);
    buildSavePath(pName, iType, false, path, sizeof(path));
    FILE* f = fopen(path, "wb+");
    if (!f) return -1;
    fclose(f);
  }

  int iResult = StartRefreshFileList();
  if (iResult == 0) {
    do {
      iResult = CycleRefreshFileList();
    } while (iResult == 1);
  }
  return iResult;
}

int SaveDevice::CycleCreateFile(const unsigned short*, int) { return 0; }

int SaveDevice::StartOpenFile(SaveFile& file, bool) {
  char path[512];
  if (js_save_vfs_active()) {
    buildSavePath(file.mFileName, file.GetType(), true, path, sizeof(path));
    int fd = js_save_vfs_open(path, 2);  // read-write
    if (fd < 0) return -1;
    OpfsSaveHandle* h = (OpfsSaveHandle*)malloc(sizeof(OpfsSaveHandle));
    if (!h) { js_save_vfs_close(fd); return -1; }
    h->fd = fd;
    h->pos = 0;
    file.mFileHandle = reinterpret_cast<FILE*>(h);
  } else {
    buildSavePath(file.mFileName, file.GetType(), false, path, sizeof(path));
    FILE* f = fopen(path, "rb+");
    if (!f) return -1;
    file.mFileHandle = f;
  }
  mLastAsyncResult = 0;
  return 0;
}

int SaveDevice::CycleOpenFile(SaveFile&, bool) { return 0; }

int SaveDevice::StartWriteFile(SaveFile& file, void* pBuf, unsigned int uiSize) {
  if (!file.mFileHandle) return -1;
  if (js_save_vfs_active()) {
    OpfsSaveHandle* h = reinterpret_cast<OpfsSaveHandle*>(file.mFileHandle);
    int written = js_save_vfs_write(h->fd, h->pos, pBuf, (int)uiSize);
    if (written != (int)uiSize) return -1;
    h->pos += uiSize;
    mLastAsyncResult = (int)uiSize;
    return 0;
  }
  size_t written = fwrite(pBuf, 1, uiSize, file.mFileHandle);
  if (written != uiSize) return -1;
  mLastAsyncResult = (int)written;
  return 0;
}

int SaveDevice::CycleWriteFile(SaveFile&, void*, unsigned int) { return 0; }

int SaveDevice::StartReadFile(SaveFile& file, void* pBuf, unsigned int uiSize) {
  if (!file.mFileHandle) return -5;
  if (js_save_vfs_active()) {
    OpfsSaveHandle* h = reinterpret_cast<OpfsSaveHandle*>(file.mFileHandle);
    int n = js_save_vfs_read(h->fd, h->pos, pBuf, (int)uiSize);
    if (n != (int)uiSize) return -5;
    h->pos += uiSize;
    mLastAsyncResult = (int)uiSize;
    return 0;
  }
  size_t n = fread(pBuf, 1, uiSize, file.mFileHandle);
  if (n != uiSize) return -5;
  mLastAsyncResult = (int)n;
  return 0;
}

int SaveDevice::CycleReadFile(SaveFile&, void*, unsigned int) { return 0; }

int SaveDevice::StartCloseFile(SaveFile& file, bool) {
  if (!file.mFileHandle) return 0;
  if (js_save_vfs_active()) {
    OpfsSaveHandle* h = reinterpret_cast<OpfsSaveHandle*>(file.mFileHandle);
    js_save_vfs_flush(h->fd);
    js_save_vfs_close(h->fd);
    free(h);
    file.mFileHandle = 0;
    mLastAsyncResult = 0;
    return 0;
  }
  int r = fclose(file.mFileHandle);
  file.mFileHandle = 0;
  if (r != 0) return -1;
  mLastAsyncResult = 0;
  return 0;
}

int SaveDevice::CycleCloseFile(SaveFile&, bool) { return 0; }

int SaveDevice::StartDeleteFile(const unsigned short* pName, int iType, int) {
  char path[512];
  if (js_save_vfs_active()) {
    buildSavePath(pName, iType, true, path, sizeof(path));
    js_save_vfs_delete(path);
  } else {
    buildSavePath(pName, iType, false, path, sizeof(path));
    remove(path);
  }

  int iResult = StartRefreshFileList();
  if (iResult == 0) {
    do {
      iResult = CycleRefreshFileList();
    } while (iResult == 1);
  }
  return iResult;
}

int SaveDevice::CycleDeleteFile(const unsigned short*, int) { return 0; }
int SaveDevice::StartFormat() { return 0; }
int SaveDevice::CycleFormat() { return 0; }

SaveFile* SaveDevice::FindFileInList(const unsigned short* pName, int iType) {
  auto utf16CaseEq = [](const unsigned short* a, const unsigned short* b) {
    if (!a || !b) return false;
    while (*a && *b) {
      unsigned short ca = (*a >= 'A' && *a <= 'Z') ? (unsigned short)(*a + 32) : *a;
      unsigned short cb = (*b >= 'A' && *b <= 'Z') ? (unsigned short)(*b + 32) : *b;
      if (ca != cb) return false;
      ++a; ++b;
    }
    return *a == 0 && *b == 0;
  };
  ResetFileIterator();
  for (SaveFile* pFile = GetNextFile(); pFile; pFile = GetNextFile()) {
    if (pFile->GetType() == iType && utf16CaseEq(pName, pFile->GetGameName())) {
      return pFile;
    }
  }
  return 0;
}

int SaveDevice::StartRefreshFileList() {
  EmptyFileList();
  ResetFileIterator();

  auto addFilename = [&](const char* fname) {
    if (!fname || !fname[0] || strcmp(fname, ".") == 0 || strcmp(fname, "..") == 0) return;
    if (mNumFiles >= 200) return;
    const char* dot = strrchr(fname, '.');
    char base[256];
    const char* ext = "";
    if (dot) {
      size_t blen = (size_t)(dot - fname);
      if (blen >= sizeof(base)) blen = sizeof(base) - 1;
      memcpy(base, fname, blen);
      base[blen] = '\0';
      ext = dot + 1;
    } else {
      snprintf(base, sizeof(base), "%s", fname);
    }
    int iType = extToSaveType(ext);
    unsigned short uName[256];
    asciiToUtf16(base, uName, 256);
    mFileList[mNumFiles++].SetFileInfo(uName, iType);
  };

  if (js_save_vfs_active()) {
    char listing[16384];
    int n = js_save_vfs_list("/saves/savegames/", listing, (int)sizeof(listing));
    if (n > 0) {
      char* cur = listing;
      while (cur && *cur) {
        char* nl = strchr(cur, '\n');
        if (nl) *nl = '\0';
        const char* slash = strrchr(cur, '/');
        addFilename(slash ? slash + 1 : cur);
        cur = nl ? nl + 1 : nullptr;
      }
    }
  } else {
    if (DIR* d = opendir("SaveGames")) {
      while (struct dirent* e = readdir(d)) {
        addFilename(e->d_name);
      }
      closedir(d);
    }
  }

  mLastAsyncResult = 0;
  return 0;
}

int SaveDevice::CycleRefreshFileList() { return 0; }
void SaveDevice::ResetFileIterator() { mCurFile = 0; }

SaveFile* SaveDevice::GetNextFile() {
  if (mCurFile < mNumFiles) {
    return &mFileList[mCurFile++];
  }
  return 0;
}

void SaveDevice::EmptyFileList() { mNumFiles = 0; }
