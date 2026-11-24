// Browser PCM queue and the small, honest slice of the SoundFL bank reader.
//
// This file deliberately does not implement DirectSound.  The first playable browser-audio
// slice is the real PCM16 sample that the engine's own sound bank contains: the bank metadata
// is walked with PebbleFL's PblFile/PblFileChunk readers, the format is checked with SoundFL's
// SampleInfo helpers, and the sample bytes are copied into a PCM queue that the WebAudio sink
// drains.  No JavaScript sound decoder and no synthesised sound is involved.
//
// The bank format and its field ids below mirror
//   build/src_fixed/SoundFL/source/sndsamplebankbase.cpp (LoadBank/LoadSamples)
//   build/src_fixed/SoundFL/source/win/sndsamplebank.cpp    (LoadData)
// so a later change to the engine's reader has an obvious place to be mirrored.

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <new>

#include "PebbleFL/Common/PblChunk.h"
#include "PebbleFL/Common/PblFile.h"
#include "PebbleFL/Common/PblHashTable.h"
#include "SoundFL/source/sndsampleinfo.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define AUDIO_BROWSER_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define AUDIO_BROWSER_EXPORT
#endif

namespace
{

// PebbleFL chunk ids used by the SoundFL bank container.
const unsigned int kBankContainerOuter = 0x5E262256u;
const unsigned int kBankContainerInner = 0x9A644D15u;
const unsigned int kBankMetadataChunk = 0x0FB40705u;
const unsigned int kBankDataChunk = 0xD872E2A5u;

// SoundFL bank metadata ids (SoundFL/source/sndsamplebankbase.cpp).
const unsigned int kBankHash = 0x8D39BDE6u;
const unsigned int kBankTemplate = 0x694AAA0Bu;
const unsigned int kBankSize = 0x23A0D95Cu;
const unsigned int kBankNumSamples = 0x98B889CEu;
const unsigned int kBankFormat = 0xB99D8552u;
const unsigned int kBankLoadSamples = 0x8EBF7143u;

// SoundFL sample-record ids (SoundFL/source/sndsamplebankbase.cpp).
const unsigned int kSampleBegin = 0x96E382A7u;
const unsigned int kSampleHash = 0x37386AE0u;
const unsigned int kSampleFrequency = 0x2FB31C01u;
const unsigned int kSampleSize = 0x23A0D95Cu;
const unsigned int kSampleLengthSamples = 0x1D48FEEFu;
const unsigned int kSampleAlias = 0x7D268157u;
const unsigned int kSampleEnd = 0x2E789FB4u;

// Status values returned by audio_browser_load_sample*.
enum LoadStatus
{
    kLoadOk = 1,
    kLoadNotFound = 0,
    kLoadCannotOpen = -1,
    kLoadMalformed = -2,
    kLoadUnsupportedFormat = -3,
    kLoadOutOfMemory = -4,
    kLoadIoError = -5
};

struct BankLayout
{
    int mMetadataOffset;
    int mMetadataEnd;
    int mDataOffset;
    int mDataEnd;
    bool mHaveMetadata;
    bool mHaveData;

    BankLayout()
        : mMetadataOffset(0), mMetadataEnd(0), mDataOffset(0), mDataEnd(0),
          mHaveMetadata(false), mHaveData(false)
    {
    }
};

struct SampleRecord
{
    unsigned int mHash;
    unsigned int mFrequency;
    unsigned int mSize;
    unsigned int mLengthSamples;
    unsigned int mAliasHash;
    unsigned int mDataOffset;
    bool mIsAlias;
    bool mValid;

    SampleRecord()
        : mHash(0), mFrequency(0), mSize(0), mLengthSamples(0), mAliasHash(0),
          mDataOffset(0), mIsAlias(false), mValid(false)
    {
    }
};

struct PcmChunk
{
    short* mSamples;
    int mFrames;
    int mChannels;
    unsigned int mSampleRate;
    unsigned int mHash;
    int mOffset;
    PcmChunk* mNext;

    PcmChunk()
        : mSamples(0), mFrames(0), mChannels(0), mSampleRate(0), mHash(0), mOffset(0), mNext(0)
    {
    }
};

// The queue is single-threaded, like the rest of the browser host.  It owns its PCM bytes, so
// the JavaScript side can copy a small block into WebAudio without holding a wasm pointer.
PcmChunk* g_pQueueHead = 0;
PcmChunk* g_pQueueTail = 0;
int g_iQueuedFrames = 0;
int g_iSinkReady = 0;

unsigned int g_uiLoadedSampleHash = 0;
unsigned int g_uiLoadedSampleRate = 0;
unsigned int g_uiLoadedSampleBytes = 0;
int g_iLoadedSampleChannels = 0;
int g_iLoadedSampleFrames = 0;
int g_iLastLoadStatus = kLoadNotFound;

const int kMaxQueuedBytes = 16 * 1024 * 1024;

void ClearQueue()
{
    PcmChunk* p = g_pQueueHead;
    while (p)
    {
        PcmChunk* const pNext = p->mNext;
        free(p->mSamples);
        free(p);
        p = pNext;
    }
    g_pQueueHead = 0;
    g_pQueueTail = 0;
    g_iQueuedFrames = 0;
}

int QueueBytes()
{
    int iBytes = 0;
    for (PcmChunk* p = g_pQueueHead; p; p = p->mNext)
    {
        iBytes += (p->mFrames - p->mOffset) * p->mChannels * (int)sizeof(short);
    }
    return iBytes;
}

// Returns false only when the queue's memory ceiling would be exceeded.
bool EnqueuePcm(const short* pSamples, int iFrames, int iChannels, unsigned int uiSampleRate,
                unsigned int uiHash)
{
    if (!pSamples || iFrames <= 0 || iChannels <= 0 || iChannels > 2 ||
        iFrames > INT_MAX / (iChannels * (int)sizeof(short)))
    {
        return false;
    }

    const int iBytes = iFrames * iChannels * (int)sizeof(short);
    if (iBytes <= 0 || QueueBytes() > kMaxQueuedBytes - iBytes)
    {
        return false;
    }

    PcmChunk* const pChunk = (PcmChunk*)malloc(sizeof(PcmChunk));
    if (!pChunk)
    {
        return false;
    }
    new (pChunk) PcmChunk();

    pChunk->mSamples = (short*)malloc((size_t)iBytes);
    if (!pChunk->mSamples)
    {
        free(pChunk);
        return false;
    }
    memcpy(pChunk->mSamples, pSamples, (size_t)iBytes);
    pChunk->mFrames = iFrames;
    pChunk->mChannels = iChannels;
    pChunk->mSampleRate = uiSampleRate;
    pChunk->mHash = uiHash;

    if (g_pQueueTail)
    {
        g_pQueueTail->mNext = pChunk;
    }
    else
    {
        g_pQueueHead = pChunk;
    }
    g_pQueueTail = pChunk;
    g_iQueuedFrames += iFrames;
    return true;
}

// The two SoundFL bank containers are plain chunk trees.  Locate the metadata and the PCM data
// without guessing at file offsets: PblFileChunk gives us the exact payload position.
bool FindBankChunks(PblFileChunk& parent, BankLayout& layout, int iDepth)
{
    if (iDepth > 8)
    {
        return false;
    }

    while (!parent.IsDone())
    {
        PblFileChunk child = parent.ReadNextChild();
        if (child._iId == kBankMetadataChunk)
        {
            layout.mMetadataOffset = child._iNext;
            layout.mMetadataEnd = child._iEnd;
            layout.mHaveMetadata = true;
        }
        else if (child._iId == kBankDataChunk)
        {
            layout.mDataOffset = child._iNext;
            layout.mDataEnd = child._iEnd;
            layout.mHaveData = true;
        }
        else if (child._iId == kBankContainerOuter || child._iId == kBankContainerInner)
        {
            FindBankChunks(child, layout, iDepth + 1);
        }
    }
    return layout.mHaveMetadata && layout.mHaveData;
}

int LoadSampleByHash(const char* pcBankPath, unsigned int uiSampleHash)
{
    g_iLastLoadStatus = kLoadNotFound;
    g_uiLoadedSampleHash = 0;
    g_uiLoadedSampleRate = 0;
    g_uiLoadedSampleBytes = 0;
    g_iLoadedSampleChannels = 0;
    g_iLoadedSampleFrames = 0;
    ClearQueue();

    if (!pcBankPath || !pcBankPath[0] || !uiSampleHash)
    {
        g_iLastLoadStatus = kLoadMalformed;
        return g_iLastLoadStatus;
    }

    PblFile file;
    if (!file.Open(pcBankPath, false, false, false))
    {
        g_iLastLoadStatus = kLoadCannotOpen;
        return g_iLastLoadStatus;
    }

    BankLayout layout;
    PblFileChunk root(&file);
    const bool bFound = FindBankChunks(root, layout, 0);
    if (!bFound)
    {
        file.Close();
        g_iLastLoadStatus = kLoadMalformed;
        return g_iLastLoadStatus;
    }

    // Read the bank header exactly as SoundFL's LoadBank does.
    unsigned int uiBankHash = 0;
    unsigned int uiBankSize = 0;
    unsigned int uiNumSamples = 0;
    unsigned int uiFormat = 0;
    bool bTemplate = false;
    bool bSawLoadSamples = false;

    PblFile* const pFile = &file;
    pFile->Seek(layout.mMetadataOffset, PblFile::SeekFromStart);
    for (;;)
    {
        const unsigned int uiId = (unsigned int)pFile->Read32();
        if (uiId == kBankHash)
        {
            uiBankHash = (unsigned int)pFile->Read32();
        }
        else if (uiId == kBankTemplate)
        {
            bTemplate = pFile->Read32() == 1;
        }
        else if (uiId == kBankSize)
        {
            uiBankSize = (unsigned int)pFile->Read32();
        }
        else if (uiId == kBankNumSamples)
        {
            uiNumSamples = (unsigned int)pFile->Read32();
        }
        else if (uiId == kBankFormat)
        {
            uiFormat = (unsigned int)pFile->Read32();
        }
        else if (uiId == kBankLoadSamples)
        {
            bSawLoadSamples = true;
            break;
        }
        else
        {
            // The bank has a nameless marker here; it carries no value.
        }
    }

    (void)uiBankHash;
    if (!bSawLoadSamples || !uiNumSamples || uiNumSamples > 1000000u)
    {
        file.Close();
        g_iLastLoadStatus = kLoadMalformed;
        return g_iLastLoadStatus;
    }
    if (bTemplate || uiFormat != (unsigned int)Snd::SampleInfo::format_PCM16)
    {
        file.Close();
        g_iLastLoadStatus = kLoadUnsupportedFormat;
        return g_iLastLoadStatus;
    }

    SampleRecord* const pRecords =
        (SampleRecord*)malloc((size_t)uiNumSamples * sizeof(SampleRecord));
    if (!pRecords)
    {
        file.Close();
        g_iLastLoadStatus = kLoadOutOfMemory;
        return g_iLastLoadStatus;
    }
    for (unsigned int i = 0; i < uiNumSamples; ++i)
    {
        new (pRecords + i) SampleRecord();
    }

    unsigned int uiDataOffset = 0;
    unsigned int uiParsed = 0;
    int iTarget = -1;
    for (; uiParsed < uiNumSamples; ++uiParsed)
    {
        SampleRecord record;
        const unsigned int uiBegin = (unsigned int)pFile->Read32();
        if (uiBegin != kSampleBegin)
        {
            break;
        }

        bool bEnded = false;
        for (;;)
        {
            const unsigned int uiId = (unsigned int)pFile->Read32();
            if (uiId == kSampleHash)
            {
                record.mHash = (unsigned int)pFile->Read32();
            }
            else if (uiId == kSampleFrequency)
            {
                record.mFrequency = (unsigned int)pFile->Read32();
            }
            else if (uiId == kSampleSize)
            {
                record.mSize = (unsigned int)pFile->Read32();
            }
            else if (uiId == kSampleLengthSamples)
            {
                record.mLengthSamples = (unsigned int)pFile->Read32();
            }
            else if (uiId == kSampleAlias)
            {
                record.mAliasHash = (unsigned int)pFile->Read32();
                record.mIsAlias = true;
            }
            else if (uiId == kSampleEnd)
            {
                bEnded = true;
                break;
            }
            else
            {
                // 0x809608b6 and 0 are record markers without values in the engine reader.
            }
        }
        if (!bEnded)
        {
            break;
        }

        if (record.mIsAlias)
        {
            bool bAliasFound = false;
            for (unsigned int i = 0; i < uiParsed; ++i)
            {
                if (pRecords[i].mHash == record.mAliasHash && pRecords[i].mValid)
                {
                    record.mSize = pRecords[i].mSize;
                    record.mLengthSamples = pRecords[i].mLengthSamples;
                    record.mFrequency = pRecords[i].mFrequency;
                    record.mDataOffset = pRecords[i].mDataOffset;
                    bAliasFound = true;
                    break;
                }
            }
            if (!bAliasFound)
            {
                continue;
            }
        }
        else
        {
            record.mDataOffset = uiDataOffset;
            uiDataOffset += record.mSize;
        }
        record.mValid = true;
        pRecords[uiParsed] = record;

        if (record.mHash == uiSampleHash)
        {
            iTarget = (int)uiParsed;
        }
    }

    if (uiParsed != uiNumSamples || !uiBankSize || uiDataOffset != uiBankSize)
    {
        free(pRecords);
        file.Close();
        g_iLastLoadStatus = kLoadMalformed;
        return g_iLastLoadStatus;
    }
    if (iTarget < 0)
    {
        free(pRecords);
        file.Close();
        g_iLastLoadStatus = kLoadNotFound;
        return g_iLastLoadStatus;
    }

    const SampleRecord& target = pRecords[iTarget];
    const unsigned int uiFrameSize = Snd::SampleInfo::GetFrameSize(Snd::SampleInfo::format_PCM16);
    const unsigned int uiSamplesPerFrame =
        Snd::SampleInfo::GetSamplesPerFrame(Snd::SampleInfo::format_PCM16);
    if (uiFrameSize != 2u || uiSamplesPerFrame != 1u || !target.mFrequency ||
        target.mLengthSamples > UINT_MAX / uiFrameSize ||
        target.mSize != target.mLengthSamples * uiFrameSize || target.mSize > (unsigned int)INT_MAX)
    {
        free(pRecords);
        file.Close();
        g_iLastLoadStatus = kLoadMalformed;
        return g_iLastLoadStatus;
    }
    const long long iPcmEnd = (long long)layout.mDataOffset + target.mDataOffset + target.mSize;
    if (iPcmEnd > file.GetSize())
    {
        free(pRecords);
        file.Close();
        g_iLastLoadStatus = kLoadMalformed;
        return g_iLastLoadStatus;
    }

    short* const pPcm = (short*)malloc(target.mSize);
    if (!pPcm)
    {
        free(pRecords);
        file.Close();
        g_iLastLoadStatus = kLoadOutOfMemory;
        return g_iLastLoadStatus;
    }

    // SampleBank::LoadData reads from the data chunk's payload start plus the per-sample offset.
    pFile->Seek(layout.mDataOffset + (int)target.mDataOffset, PblFile::SeekFromStart);
    pFile->ReadData(pPcm, (int)target.mSize);

    const int iFrames = (int)(target.mSize / uiFrameSize);
    if (!EnqueuePcm(pPcm, iFrames, 1, target.mFrequency, target.mHash))
    {
        free(pPcm);
        free(pRecords);
        file.Close();
        g_iLastLoadStatus = kLoadOutOfMemory;
        return g_iLastLoadStatus;
    }

    g_uiLoadedSampleHash = target.mHash;
    g_uiLoadedSampleRate = target.mFrequency;
    g_uiLoadedSampleBytes = target.mSize;
    g_iLoadedSampleChannels = 1;
    g_iLoadedSampleFrames = iFrames;
    g_iLastLoadStatus = kLoadOk;

    free(pPcm);
    free(pRecords);
    file.Close();
    return g_iLastLoadStatus;
}

}  // namespace

extern "C"
{

// Load one real PCM16 sample from a SoundFL bank and put it at the head of the PCM queue.
// Return values: 1 loaded, 0 hash not found, negative values are errors (see LoadStatus).
AUDIO_BROWSER_EXPORT int audio_browser_load_sample(const char* pcBankPath,
                                                    unsigned int uiSampleHash)
{
    return LoadSampleByHash(pcBankPath, uiSampleHash);
}

// The same operation, but the name is hashed by the engine's own PblHash implementation.
AUDIO_BROWSER_EXPORT int audio_browser_load_sample_by_name(const char* pcBankPath,
                                                            const char* pcSampleName)
{
    if (!pcSampleName || !pcSampleName[0])
    {
        g_iLastLoadStatus = kLoadMalformed;
        return g_iLastLoadStatus;
    }
    const PblHash hash(pcSampleName);
    return LoadSampleByHash(pcBankPath, (unsigned int)hash);
}

AUDIO_BROWSER_EXPORT int audio_browser_last_load_status()
{
    return g_iLastLoadStatus;
}

AUDIO_BROWSER_EXPORT unsigned int audio_browser_sample_hash()
{
    return g_uiLoadedSampleHash;
}

AUDIO_BROWSER_EXPORT unsigned int audio_browser_sample_rate()
{
    return g_uiLoadedSampleRate;
}

AUDIO_BROWSER_EXPORT unsigned int audio_browser_sample_bytes()
{
    return g_uiLoadedSampleBytes;
}

AUDIO_BROWSER_EXPORT int audio_browser_sample_channels()
{
    return g_iLoadedSampleChannels;
}

AUDIO_BROWSER_EXPORT int audio_browser_sample_frames()
{
    return g_iLoadedSampleFrames;
}

// Queue an arbitrary interleaved PCM16 block.  This is the seam the engine's mixer can use
// later; it is deliberately separate from the one-sample loader above.
AUDIO_BROWSER_EXPORT int audio_browser_push_pcm(const short* pPcm, int iFrames, int iChannels,
                                                 unsigned int uiSampleRate)
{
    if (!EnqueuePcm(pPcm, iFrames, iChannels, uiSampleRate, 0))
    {
        return 0;
    }
    return 1;
}

// Copy up to iMaxFrames interleaved frames from the head of the queue.  The optional rate and
// channel pointers describe the chunk that was copied.  The return value is the number of frames
// copied (zero means the queue is empty).
AUDIO_BROWSER_EXPORT int audio_browser_drain_pcm(short* pOut, int iMaxFrames,
                                                  int* pSampleRate, int* pChannels)
{
    if (!pOut || iMaxFrames <= 0 || !g_pQueueHead)
    {
        return 0;
    }

    PcmChunk* const pChunk = g_pQueueHead;
    const int iRemaining = pChunk->mFrames - pChunk->mOffset;
    const int iFrames = iRemaining < iMaxFrames ? iRemaining : iMaxFrames;
    memcpy(pOut,
           pChunk->mSamples + (size_t)pChunk->mOffset * (size_t)pChunk->mChannels,
           (size_t)iFrames * (size_t)pChunk->mChannels * sizeof(short));

    if (pSampleRate)
    {
        *pSampleRate = (int)pChunk->mSampleRate;
    }
    if (pChannels)
    {
        *pChannels = pChunk->mChannels;
    }

    pChunk->mOffset += iFrames;
    g_iQueuedFrames -= iFrames;
    if (pChunk->mOffset == pChunk->mFrames)
    {
        g_pQueueHead = pChunk->mNext;
        if (!g_pQueueHead)
        {
            g_pQueueTail = 0;
        }
        free(pChunk->mSamples);
        free(pChunk);
    }
    return iFrames;
}

AUDIO_BROWSER_EXPORT int audio_browser_pending_frames()
{
    return g_iQueuedFrames;
}

AUDIO_BROWSER_EXPORT void audio_browser_clear()
{
    ClearQueue();
}

// The JavaScript sink sets this only after its AudioContext is running and its output node is
// connected.  A C++ caller can use audio_browser_sink_ready() before handing PCM to the engine.
AUDIO_BROWSER_EXPORT void audio_browser_set_sink_ready(int iReady)
{
    g_iSinkReady = iReady ? 1 : 0;
}

AUDIO_BROWSER_EXPORT int audio_browser_sink_ready()
{
    return g_iSinkReady;
}

}  // extern "C"
