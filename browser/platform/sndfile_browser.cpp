// Browser & native PblFile-backed implementation of Snd::FilePlatform.
//
// Replaces SoundFL/source/win/sndfileplatform.cpp (which used Win32 CreateFileA/ReadFile/
// SetFilePointer). By routing Snd::FilePlatform through PblFile, both RedMoviePlayer
// (.mvs Bink1 video streams) and SoundFL streaming audio (Phase 5 music/VO banks) read
// transparently from the OPFS on-demand streaming VFS in browser workers and from the
// local filesystem in Node/native checks.

#include "SoundFL/source/win/sndfileplatform.h"
#include "PebbleFL/Common/PblFile.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

extern "C" int swbf2_pblfile_is_range_cached(const PblFile* f, uint32_t offset, uint32_t length);
extern "C" void swbf2_pblfile_prefetch_range(const PblFile* f, uint32_t offset, uint32_t length);

namespace Snd
{

namespace
{
inline PblFile* AsPblFile(void* h)
{
    return (h && h != (void*)-1) ? static_cast<PblFile*>(h) : nullptr;
}
}  // namespace

FilePlatform::FilePlatform()
{
    mHandle = nullptr;
}

FilePlatform::~FilePlatform()
{
    if (IsOpen())
    {
        Close();
    }
}

FilePlatform& FilePlatform::operator=(FilePlatform& rOther)
{
    FilePlatformBase::operator=(rOther);
    mHandle = rOther.mHandle;
    rOther.mHandle = nullptr;
    return *this;
}

void FilePlatform::Close()
{
    FilePlatformBase::Close();
    if (PblFile* f = AsPblFile(mHandle))
    {
        f->Close();
        f->~PblFile();
        free(f);
    }
    mHandle = nullptr;
}

unsigned int FilePlatform::GetSize() const
{
    if (PblFile* f = AsPblFile(mHandle))
    {
        return (unsigned int)f->GetSize();
    }
    return 0;
}

bool FilePlatform::Open(char const* pcFileName, unsigned int uiAccessFlags,
                        unsigned int uiBufferSize, unsigned int uiSectorSize,
                        unsigned int uiAlignment)
{
    // Treat browser/PblFile streams as unbuffered byte-addressable files (mAccess |= 8) so
    // FilePlatformBase::BufferedReadWrite issues direct SeekRead calls without 512-byte
    // Win32 FILE_FLAG_NO_BUFFERING alignment restrictions. Pass uiBufferSize = 0 so
    // FilePlatformBase::Open keeps mSectorSize = 1 instead of rounding seeks to uiBufferSize.
    uiAccessFlags |= 8;
    uiBufferSize = 0;
    uiSectorSize = 1;
    uiAlignment = 1;

    if (!FilePlatformBase::Open(pcFileName, uiAccessFlags, uiBufferSize, uiSectorSize, uiAlignment))
    {
        return false;
    }

    void* mem = malloc(sizeof(PblFile));
    if (!mem)
    {
        FilePlatformBase::Close();
        return false;
    }
    PblFile* f = new (mem) PblFile();
    const bool bWrite = (uiAccessFlags & 2) != 0;
    const bool bCreate = (uiAccessFlags & 4) != 0;
    bool ok = false;
    if (bCreate)
    {
        ok = f->OpenCreate(pcFileName, false, false, true);
    }
    else if (bWrite)
    {
        ok = f->OpenReadWrite(pcFileName, false, false, true);
    }
    else
    {
        ok = f->Open(pcFileName, true, false, true);
    }
    if (ok)
    {
        mHandle = f;
        return true;
    }

    f->~PblFile();
    free(f);
    mHandle = nullptr;
    FilePlatformBase::Close();
    return false;
}

unsigned int FilePlatform::RealRead(void* pBuffer, unsigned int uiSize)
{
    if (PblFile* f = AsPblFile(mHandle))
    {
        int before = f->Tell();
        int sz = f->GetSize();
        int avail = (sz > before) ? (sz - before) : 0;
        int toRead = ((int)uiSize < avail) ? (int)uiSize : avail;
        if (toRead > 0)
        {
            f->ReadData(pBuffer, toRead);
            return (unsigned int)toRead;
        }
    }
    return 0;
}

void FilePlatform::RealSeek(unsigned int uiPos)
{
    if (PblFile* f = AsPblFile(mHandle))
    {
        f->Seek((int)uiPos, PblFile::SeekFromStart);
    }
}

unsigned int FilePlatform::RealWrite(void* pBuffer, unsigned int uiSize)
{
    if (PblFile* f = AsPblFile(mHandle))
    {
        f->WriteData(pBuffer, (int)uiSize);
        return uiSize;
    }
    return 0;
}

}  // namespace Snd

extern "C" int swbf2_sndfile_is_range_cached(const Snd::FilePlatform* fp, unsigned int offset, unsigned int length)
{
    if (!fp || !fp->mHandle || fp->mHandle == (void*)-1) return 1;
    return swbf2_pblfile_is_range_cached(static_cast<const PblFile*>(fp->mHandle), offset, length);
}

extern "C" void swbf2_sndfile_prefetch_range(const Snd::FilePlatform* fp, unsigned int offset, unsigned int length)
{
    if (!fp || !fp->mHandle || fp->mHandle == (void*)-1) return;
    swbf2_pblfile_prefetch_range(static_cast<const PblFile*>(fp->mHandle), offset, length);
}
