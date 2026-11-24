/* Phase 5 Browser DirectSound8 & SoundFL Software 3D Audio Mixer Backend.
 *
 * Replaces Battlefront2/SoundFL's Win32 DirectSound device layer (snddsbuffer.cpp &
 * sndstreamcapturesource.cpp) with a real software DirectSound8 + 3D stereo 48 kHz mixer:
 *
 *   - DirectSoundCreate8 / DirectSoundCreate return a real virtual-COM BrowserDirectSound8
 *     advertising stereo 16-bit software mixing so SoundFL runs Snd::Engine in mixConfig_Software
 *     (all 32 3D voices mixed with full distance attenuation, Doppler pitch, and stereo panning
 *     through Snd::SoftOutput + all 6 IMA-ADPCM/PCM streams through Snd::Stream::mRenderer).
 *   - Snd::DSBuffer creates real circular PCM buffers (BrowserDSBuffer, allocated via malloc/free
 *     outside GameMemory::RunTimeHeap) supporting Lock/Unlock/GetCurrentPosition/Play/Stop/
 *     SetFrequency/SetGain/SetPan/Set3DParams.
 *   - swbf2_audio_pump_and_mix(dt) synchronously pumps Snd::Engine::GetThreadUpdate() (since
 *     single-threaded WASM has no Win32 background SoundFL update thread) and mixes every active
 *     playing BrowserDSBuffer into a 48 kHz stereo float32 ring buffer drained by the browser's
 *     lock-free SharedArrayBuffer AudioWorklet (browser/webaudio-worklet.mjs).
 */

#include "SoundFL/source/win/snddsbuffer.h"
#include "SoundFL/source/win/snddsbufferrenderer.h"
#include "SoundFL/source/win/sndengine.h"
#include "SoundFL/source/win/sndstream.h"
#include "SoundFL/source/win/sndstreamcapturesource.h"
#include "SoundFL/source/sndbus.h"
#include "SoundFL/source/sndenginebase.h"
#include "SoundFL/source/sndthreadupdate.h"
#include "PebbleFL/Common/PblListSingle.h"

#include <dsound.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void Log(const char* fmt, ...);

// Defined in Snd::DSBuffer (declared static in snddsbuffer.h).
PblListDouble Snd::DSBuffer::smList;

namespace
{

constexpr uint32_t kDefaultMixSampleRate = 48000u;
constexpr uint32_t kMixRingFrames = 16384u; // stereo float32 staging ring
constexpr uint32_t kMixSubChunkFrames = 256u; // ~5.3 ms sub-stepping keeps DSBufferRenderer fed without wraparound
constexpr size_t kMaxActiveBuffers = 128;

class BrowserDSBuffer;

struct MixerState
{
    uint32_t mixSampleRate = kDefaultMixSampleRate;
    int32_t nextMixFramesOverride = -1; // >= 0 when worker drives closed-loop SAB clock sync

    BrowserDSBuffer* buffers[kMaxActiveBuffers];
    size_t bufferCount = 0;

    float ringL[kMixRingFrames];
    float ringR[kMixRingFrames];
    uint32_t ringRead = 0;
    uint32_t ringWrite = 0;

    // Telemetry for Phase 5 verification & debug overlay
    uint32_t totalBuffersCreated = 0;
    uint32_t mixCalls = 0;
    uint64_t totalMixedFrames = 0;
    uint64_t nonZeroFrames = 0;
    uint32_t peakPlayingBuffers = 0;
    uint32_t lastPlayingBuffers = 0;
    uint32_t peakStreamBuffers = 0;
    uint32_t lastStreamBuffers = 0;
    uint64_t streamNonZeroFrames = 0;
    float lastPeakAbs = 0.0f;
    float lastRmsL = 0.0f;
    float lastRmsR = 0.0f;
    float maxPeakAbs = 0.0f;
    double accumEnergyL = 0.0;
    double accumEnergyR = 0.0;
    uint64_t accumEnergyFrames = 0;
    bool initializedLogged = false;
};

MixerState g_mix;

void RegisterBuffer(BrowserDSBuffer* buf)
{
    if (!buf) return;
    for (size_t i = 0; i < g_mix.bufferCount; ++i)
    {
        if (g_mix.buffers[i] == buf) return;
    }
    if (g_mix.bufferCount < kMaxActiveBuffers)
    {
        g_mix.buffers[g_mix.bufferCount++] = buf;
    }
}

void UnregisterBuffer(BrowserDSBuffer* buf)
{
    for (size_t i = 0; i < g_mix.bufferCount; ++i)
    {
        if (g_mix.buffers[i] == buf)
        {
            g_mix.buffers[i] = g_mix.buffers[--g_mix.bufferCount];
            g_mix.buffers[g_mix.bufferCount] = nullptr;
            return;
        }
    }
}

static inline float ClampF(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Smooth soft-knee limiter: passes normal signal (|x| <= 0.85) untouched and
// gently saturates peaks above 0.85 toward +/-0.99 to prevent harsh digital clipping crackles.
static inline float SoftLimitSample(float x)
{
    const float a = fabsf(x);
    if (a <= 0.85f) return x;
    const float excess = a - 0.85f;
    const float limited = 0.85f + 0.14f * tanhf(excess / 0.14f);
    return x < 0.0f ? -limited : limited;
}

static inline float DbHundredthsToLinear(long lHundredthsDb)
{
    if (lHundredthsDb <= DSBVOLUME_MIN) return 0.0f;
    if (lHundredthsDb >= DSBVOLUME_MAX) return 1.0f;
    return powf(10.0f, (float)lHundredthsDb / 2000.0f);
}

class BrowserDS3DListener final : public IDirectSound3DListener
{
public:
    static void* operator new(size_t n) { return malloc(n); }
    static void operator delete(void* p) noexcept { free(p); }
    static void operator delete(void* p, size_t) noexcept { free(p); }

    long __stdcall QueryInterface(const IID&, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = static_cast<IDirectSound3DListener*>(this);
        AddRef();
        return DS_OK;
    }
    unsigned long __stdcall AddRef() override { return ++mRef; }
    unsigned long __stdcall Release() override
    {
        unsigned long r = --mRef;
        if (r == 0) delete this;
        return r;
    }

    long __stdcall SetDistanceFactor(float f, unsigned long) override { mDistanceFactor = f; return DS_OK; }
    long __stdcall SetDopplerFactor(float f, unsigned long) override { mDopplerFactor = f; return DS_OK; }
    long __stdcall SetRolloffFactor(float f, unsigned long) override { mRolloffFactor = f; return DS_OK; }
    long __stdcall GetDistanceFactor(float* f) override { if (f) *f = mDistanceFactor; return DS_OK; }
    long __stdcall GetDopplerFactor(float* f) override { if (f) *f = mDopplerFactor; return DS_OK; }
    long __stdcall GetRolloffFactor(float* f) override { if (f) *f = mRolloffFactor; return DS_OK; }
    long __stdcall CommitDeferredSettings() override { return DS_OK; }

private:
    unsigned long mRef = 1;
    float mDistanceFactor = 1.0f;
    float mDopplerFactor = 1.0f;
    float mRolloffFactor = 1.0f;
};

class BrowserDS3DBuffer final : public IDirectSound3DBuffer
{
public:
    static void* operator new(size_t n) { return malloc(n); }
    static void operator delete(void* p) noexcept { free(p); }
    static void operator delete(void* p, size_t) noexcept { free(p); }

    BrowserDS3DBuffer()
    {
        memset(&mParams, 0, sizeof(mParams));
        mParams.dwSize = sizeof(DS3DBUFFER);
        mParams.dwMode = DS3DMODE_HEADRELATIVE;
        mParams.flMinDistance = 1.0f;
        mParams.flMaxDistance = 100.0f;
    }

    long __stdcall QueryInterface(const IID&, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = static_cast<IDirectSound3DBuffer*>(this);
        AddRef();
        return DS_OK;
    }
    unsigned long __stdcall AddRef() override { return ++mRef; }
    unsigned long __stdcall Release() override
    {
        unsigned long r = --mRef;
        if (r == 0) delete this;
        return r;
    }

    long __stdcall SetAllParameters(LPCDS3DBUFFER p, unsigned long) override
    {
        if (p) mParams = *p;
        return DS_OK;
    }
    long __stdcall SetPosition(float x, float y, float z, unsigned long) override
    {
        mParams.vPosition.x = x; mParams.vPosition.y = y; mParams.vPosition.z = z;
        return DS_OK;
    }
    long __stdcall SetVelocity(float x, float y, float z, unsigned long) override
    {
        mParams.vVelocity.x = x; mParams.vVelocity.y = y; mParams.vVelocity.z = z;
        return DS_OK;
    }
    long __stdcall SetMinDistance(float f, unsigned long) override { mParams.flMinDistance = f; return DS_OK; }
    long __stdcall SetMaxDistance(float f, unsigned long) override { mParams.flMaxDistance = f; return DS_OK; }
    long __stdcall SetMode(unsigned long m, unsigned long) override { mParams.dwMode = (int)m; return DS_OK; }

    DS3DBUFFER mParams;

private:
    unsigned long mRef = 1;
};

class BrowserDSBuffer final : public IDirectSoundBuffer8
{
public:
    static void* operator new(size_t n) { return malloc(n); }
    static void operator delete(void* p) noexcept { free(p); }
    static void operator delete(void* p, size_t) noexcept { free(p); }

    BrowserDSBuffer(Snd::SampleInfo::Format eFormat, uint32_t channels, uint32_t sampleRate,
                    uint32_t lengthBytes, bool is3D, bool isPrimary, uint32_t flags)
        : mFormat(eFormat),
          mChannels(channels ? channels : 2),
          mSampleRate(sampleRate ? sampleRate : g_mix.mixSampleRate),
          mFrequency(sampleRate ? sampleRate : g_mix.mixSampleRate),
          mIs3D(is3D),
          mIsPrimary(isPrimary),
          mFlags(flags)
    {
        mBitsPerSample = (eFormat == Snd::SampleInfo::format_PCM8) ? 8u : 16u;
        mFrameBytes = (mBitsPerSample / 8u) * mChannels;
        if (mFrameBytes == 0) mFrameBytes = 4u;

        if (!mIsPrimary)
        {
            if (lengthBytes < mFrameBytes * 64u) lengthBytes = mFrameBytes * 64u;
            lengthBytes = (lengthBytes / mFrameBytes) * mFrameBytes;
            mBufferBytes = lengthBytes;
            mTotalFrames = mBufferBytes / mFrameBytes;
            mData = static_cast<uint8_t*>(calloc(1, mBufferBytes));
            if (mIs3D)
            {
                m3D = new BrowserDS3DBuffer();
            }
            RegisterBuffer(this);
            ++g_mix.totalBuffersCreated;
        }
        else
        {
            mBufferBytes = 0;
            mTotalFrames = 0;
        }
    }

    ~BrowserDSBuffer()
    {
        UnregisterBuffer(this);
        if (m3D)
        {
            m3D->Release();
            m3D = nullptr;
        }
        free(mData);
        mData = nullptr;
    }

    long __stdcall QueryInterface(const IID& riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (memcmp(&riid, &IID_IDirectSound3DListener, sizeof(GUID)) == 0)
        {
            *ppv = new BrowserDS3DListener();
            return DS_OK;
        }
        if (memcmp(&riid, &IID_IDirectSound3DBuffer, sizeof(GUID)) == 0)
        {
            if (!m3D) m3D = new BrowserDS3DBuffer();
            m3D->AddRef();
            *ppv = static_cast<IDirectSound3DBuffer*>(m3D);
            return DS_OK;
        }
        *ppv = static_cast<IDirectSoundBuffer8*>(this);
        AddRef();
        return DS_OK;
    }
    unsigned long __stdcall AddRef() override { return ++mRef; }
    unsigned long __stdcall Release() override
    {
        unsigned long r = --mRef;
        if (r == 0) delete this;
        return r;
    }

    long __stdcall GetCaps(DSBCAPS* pCaps) override
    {
        if (!pCaps) return DSERR_INVALIDPARAM;
        pCaps->dwSize = sizeof(DSBCAPS);
        pCaps->dwFlags = mFlags;
        pCaps->dwBufferBytes = mBufferBytes;
        pCaps->dwUnlockTransferRate = 0;
        pCaps->dwPlayCpuOverhead = 0;
        return DS_OK;
    }

    long __stdcall GetCurrentPosition(unsigned long* pPlay, unsigned long* pWrite) override
    {
        uint32_t bytePos = 0;
        if (mTotalFrames > 0)
        {
            uint32_t frameIdx = ((uint32_t)mPlayFrame) % mTotalFrames;
            bytePos = frameIdx * mFrameBytes;
        }
        if (pPlay) *pPlay = bytePos;
        if (pWrite) *pWrite = bytePos;
        return DS_OK;
    }

    long __stdcall GetFormat(WAVEFORMATEX* pwfx, unsigned long dwSize, unsigned long* pWritten) override
    {
        if (pWritten) *pWritten = sizeof(WAVEFORMATEX);
        if (!pwfx || dwSize < sizeof(WAVEFORMATEX)) return DS_OK;
        memset(pwfx, 0, sizeof(WAVEFORMATEX));
        pwfx->wFormatTag = WAVE_FORMAT_PCM;
        pwfx->nChannels = (unsigned short)mChannels;
        pwfx->nSamplesPerSec = mSampleRate;
        pwfx->wBitsPerSample = (unsigned short)mBitsPerSample;
        pwfx->nBlockAlign = (unsigned short)mFrameBytes;
        pwfx->nAvgBytesPerSec = mSampleRate * mFrameBytes;
        return DS_OK;
    }

    long __stdcall GetStatus(unsigned long* pStatus) override
    {
        if (!pStatus) return DSERR_INVALIDPARAM;
        unsigned long s = DSBCAPS_LOCSOFTWARE ? DSBSTATUS_LOCSOFTWARE : 0;
        if (mPlaying)
        {
            s |= DSBSTATUS_PLAYING;
            if (mLooping) s |= DSBSTATUS_LOOPING;
        }
        if (mTerminated) s |= DSBSTATUS_TERMINATED;
        *pStatus = s;
        return DS_OK;
    }

    long __stdcall Lock(unsigned long dwOffset, unsigned long dwBytes,
                        void** ppAudio1, unsigned long* pBytes1,
                        void** ppAudio2, unsigned long* pBytes2,
                        unsigned long) override
    {
        if (!mData || mBufferBytes == 0 || !ppAudio1 || !pBytes1) return DSERR_INVALIDCALL;
        uint32_t off = dwOffset % mBufferBytes;
        uint32_t bytes = dwBytes;
        if (bytes == 0 || bytes > mBufferBytes) bytes = mBufferBytes;
        uint32_t first = mBufferBytes - off;
        if (first >= bytes)
        {
            *ppAudio1 = mData + off;
            *pBytes1 = bytes;
            if (ppAudio2) *ppAudio2 = nullptr;
            if (pBytes2) *pBytes2 = 0;
        }
        else
        {
            *ppAudio1 = mData + off;
            *pBytes1 = first;
            if (ppAudio2) *ppAudio2 = mData;
            if (pBytes2) *pBytes2 = bytes - first;
        }
        return DS_OK;
    }

    long __stdcall Unlock(void*, unsigned long, void*, unsigned long) override { return DS_OK; }

    long __stdcall Play(unsigned long, unsigned long, unsigned long dwFlags) override
    {
        mLooping = (dwFlags & DSBPLAY_LOOPING) != 0;
        mPlaying = true;
        mTerminated = false;
        return DS_OK;
    }

    long __stdcall Stop() override
    {
        mPlaying = false;
        return DS_OK;
    }

    long __stdcall SetCurrentPosition(unsigned long dwNewPos) override
    {
        if (mBufferBytes > 0 && mFrameBytes > 0)
        {
            uint32_t off = dwNewPos % mBufferBytes;
            mPlayFrame = (double)(off / mFrameBytes);
        }
        else
        {
            mPlayFrame = 0.0;
        }
        return DS_OK;
    }

    long __stdcall SetFormat(LPCWAVEFORMATEX pwfx) override
    {
        if (pwfx && pwfx->nSamplesPerSec > 0)
        {
            mSampleRate = pwfx->nSamplesPerSec;
            mFrequency = pwfx->nSamplesPerSec;
        }
        return DS_OK;
    }

    long __stdcall SetVolume(long lVolume) override
    {
        mGain = DbHundredthsToLinear(lVolume);
        return DS_OK;
    }

    long __stdcall SetPan(long lPan) override
    {
        if (lPan == 0)
        {
            mPan = 0.0f;
        }
        else if (lPan < 0)
        {
            mPan = ClampF(-(1.0f - DbHundredthsToLinear(lPan)), -1.0f, 0.0f);
        }
        else
        {
            mPan = ClampF(1.0f - DbHundredthsToLinear(-lPan), 0.0f, 1.0f);
        }
        return DS_OK;
    }

    long __stdcall SetFrequency(unsigned long dwFreq) override
    {
        if (dwFreq == 0) dwFreq = mSampleRate;
        if (dwFreq < 100u) dwFreq = 100u;
        if (dwFreq > 200000u) dwFreq = 200000u;
        mFrequency = dwFreq;
        return DS_OK;
    }

    void SetGainLinear(float g) { mGain = ClampF(g, 0.0f, 2.0f); }
    void SetPanLinear(float p) { mPan = ClampF(p, -1.0f, 1.0f); }

    BrowserDS3DBuffer* Get3D() const { return m3D; }

    void MixAdd(float* outL, float* outR, uint32_t outFrames)
    {
        if (!mPlaying || mIsPrimary || !mData || mTotalFrames == 0 || outFrames == 0) return;

        float pan = mPan;
        if (mIs3D && m3D && m3D->mParams.dwMode != DS3DMODE_DISABLE)
        {
            // Engine::Calculate3DParams supplies listener-local coordinates (+x right, -z front).
            const float x = m3D->mParams.vPosition.x;
            const float z = m3D->mParams.vPosition.z;
            const float horiz = sqrtf(x * x + z * z);
            if (horiz > 1e-4f)
            {
                pan = ClampF(x / horiz, -1.0f, 1.0f);
            }
        }

        // Equal-power-like stereo pan law for mono; balance law for stereo.
        const float gainL = mGain * (pan > 0.0f ? (1.0f - pan) : 1.0f);
        const float gainR = mGain * (pan < 0.0f ? (1.0f + pan) : 1.0f);

        const double step = (double)mFrequency / (double)g_mix.mixSampleRate;
        const uint32_t total = mTotalFrames;

        for (uint32_t i = 0; i < outFrames; ++i)
        {
            if (mPlayFrame >= (double)total)
            {
                if (mLooping)
                {
                    mPlayFrame = fmod(mPlayFrame, (double)total);
                }
                else
                {
                    mPlaying = false;
                    mTerminated = true;
                    break;
                }
            }

            const uint32_t idx0 = (uint32_t)mPlayFrame;
            const uint32_t idx1 = (idx0 + 1u < total) ? (idx0 + 1u) : (mLooping ? 0u : idx0);
            const float frac = (float)(mPlayFrame - (double)idx0);

            float sL = 0.0f;
            float sR = 0.0f;
            if (mBitsPerSample == 16u)
            {
                const int16_t* pcm = reinterpret_cast<const int16_t*>(mData);
                if (mChannels == 1u)
                {
                    float a = pcm[idx0] * (1.0f / 32768.0f);
                    float b = pcm[idx1] * (1.0f / 32768.0f);
                    float s = a + (b - a) * frac;
                    sL = s;
                    sR = s;
                }
                else
                {
                    float aL = pcm[idx0 * mChannels + 0] * (1.0f / 32768.0f);
                    float bL = pcm[idx1 * mChannels + 0] * (1.0f / 32768.0f);
                    float aR = pcm[idx0 * mChannels + 1] * (1.0f / 32768.0f);
                    float bR = pcm[idx1 * mChannels + 1] * (1.0f / 32768.0f);
                    sL = aL + (bL - aL) * frac;
                    sR = aR + (bR - aR) * frac;
                }
            }
            else
            {
                if (mChannels == 1u)
                {
                    float a = ((int)mData[idx0] - 128) * (1.0f / 128.0f);
                    float b = ((int)mData[idx1] - 128) * (1.0f / 128.0f);
                    float s = a + (b - a) * frac;
                    sL = s;
                    sR = s;
                }
                else
                {
                    float aL = ((int)mData[idx0 * mChannels + 0] - 128) * (1.0f / 128.0f);
                    float bL = ((int)mData[idx1 * mChannels + 0] - 128) * (1.0f / 128.0f);
                    float aR = ((int)mData[idx0 * mChannels + 1] - 128) * (1.0f / 128.0f);
                    float bR = ((int)mData[idx1 * mChannels + 1] - 128) * (1.0f / 128.0f);
                    sL = aL + (bL - aL) * frac;
                    sR = aR + (bR - aR) * frac;
                }
            }

            outL[i] += sL * gainL;
            outR[i] += sR * gainR;
            if (fabsf(sL * gainL) > 1e-5f || fabsf(sR * gainR) > 1e-5f)
            {
                ++mNonZeroFrames;
            }
            mPlayFrame += step;
        }
    }

    Snd::SampleInfo::Format mFormat;
    uint32_t mChannels = 2;
    uint32_t mBitsPerSample = 16;
    uint32_t mFrameBytes = 4;
    uint32_t mSampleRate = kDefaultMixSampleRate;
    uint32_t mFrequency = kDefaultMixSampleRate;
    uint32_t mBufferBytes = 0;
    uint32_t mTotalFrames = 0;
    uint8_t* mData = nullptr;
    double mPlayFrame = 0.0;
    float mGain = 1.0f;
    float mPan = 0.0f;
    bool mIs3D = false;
    bool mIsPrimary = false;
    bool mPlaying = false;
    bool mLooping = false;
    bool mTerminated = false;
    uint32_t mFlags = 0;
    uint64_t mNonZeroFrames = 0;

private:
    unsigned long mRef = 1;
    BrowserDS3DBuffer* m3D = nullptr;
};

class BrowserDirectSound8 final : public IDirectSound8
{
public:
    static void* operator new(size_t n) { return malloc(n); }
    static void operator delete(void* p) noexcept { free(p); }
    static void operator delete(void* p, size_t) noexcept { free(p); }

    long __stdcall QueryInterface(const IID&, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = static_cast<IDirectSound8*>(this);
        AddRef();
        return DS_OK;
    }
    unsigned long __stdcall AddRef() override { return ++mRef; }
    unsigned long __stdcall Release() override
    {
        unsigned long r = --mRef;
        if (r == 0) delete this;
        return r;
    }

    long __stdcall GetCaps(DSCAPS* pCaps) override
    {
        if (!pCaps) return DSERR_INVALIDPARAM;
        memset(pCaps, 0, sizeof(*pCaps));
        pCaps->dwSize = sizeof(DSCAPS);
        pCaps->dwFlags = DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARYMONO |
                         DSCAPS_SECONDARY16BIT | DSCAPS_SECONDARY8BIT |
                         DSCAPS_SECONDARYSTEREO | DSCAPS_SECONDARYMONO |
                         DSCAPS_CONTINUOUSRATE;
        pCaps->dwMinSecondarySampleRate = 100;
        pCaps->dwMaxSecondarySampleRate = 200000;
        pCaps->dwPrimaryBuffers = 1;
        // Report non-zero dwMaxHwMixingAllBuffers so SoundFL DSBufferRenderer::SetFormat()
        // honors GameSoundEngine::gSoftMixerBufferSizeMs (~50 ms) instead of forcing a 500 ms
        // latency floor (snddsbufferrenderer.cpp:888). DSBuffer::CreateBuffers() still returns 0
        // so Snd::Engine stays cleanly in mixConfig_Software.
        pCaps->dwMaxHwMixingAllBuffers = 64;
        return DS_OK;
    }

    long __stdcall SetCooperativeLevel(HWND, unsigned long) override { return DS_OK; }

    long __stdcall GetSpeakerConfig(unsigned long* pCfg) override
    {
        if (pCfg) *pCfg = mSpeakerConfig;
        return DS_OK;
    }

    long __stdcall SetSpeakerConfig(unsigned long cfg) override
    {
        mSpeakerConfig = cfg;
        return DS_OK;
    }

    long __stdcall Initialize(const GUID*) override { return DS_OK; }

private:
    unsigned long mRef = 1;
    unsigned long mSpeakerConfig = DSSPEAKER_STEREO;
};

void PumpSndThreadUpdateOnce()
{
    Snd::ThreadUpdate* tu = Snd::Engine::GetThreadUpdate();
    if (!tu || !tu->mUpdating || tu->mPausing) return;

    for (PblListSingle::Itor it(tu->mUpdateList);
         it.GetNode() != tu->mUpdateList.GetTerminator(); ++it)
    {
        auto* pObject = reinterpret_cast<Snd::ThreadUpdate::Object*>(
            it.GetNode()->GetObjectA(&tu->mUpdateList,
                                     offsetof(Snd::ThreadUpdate::Object, mNode)));
        if (pObject && !pObject->mDestructing)
        {
            pObject->UpdateObject();
        }
    }
}

void PushMixedStereo(const float* srcL, const float* srcR, uint32_t frames)
{
    for (uint32_t i = 0; i < frames; ++i)
    {
        uint32_t nextWrite = (g_mix.ringWrite + 1u) % kMixRingFrames;
        if (nextWrite == g_mix.ringRead)
        {
            // Drop oldest frame if consumer hasn't drained yet (keeps latest audio live).
            g_mix.ringRead = (g_mix.ringRead + 1u) % kMixRingFrames;
        }
        g_mix.ringL[g_mix.ringWrite] = srcL[i];
        g_mix.ringR[g_mix.ringWrite] = srcR[i];
        g_mix.ringWrite = nextWrite;
    }
}

}  // namespace

// --- DirectSound C entry points ---------------------------------------------------------------

long DirectSoundCreate(const GUID*, IDirectSound** ppDS, IUnknown*)
{
    if (!ppDS) return DSERR_INVALIDPARAM;
    *ppDS = new BrowserDirectSound8();
    if (!g_mix.initializedLogged)
    {
        g_mix.initializedLogged = true;
        Log("ds: BrowserDirectSound8 software 3D mixer active (%u Hz stereo)", g_mix.mixSampleRate);
    }
    return DS_OK;
}

long DirectSoundCreate8(const GUID*, IDirectSound8** ppDS8, IUnknown*)
{
    if (!ppDS8) return DSERR_INVALIDPARAM;
    *ppDS8 = new BrowserDirectSound8();
    if (!g_mix.initializedLogged)
    {
        g_mix.initializedLogged = true;
        Log("ds: BrowserDirectSound8 software 3D mixer active (%u Hz stereo)", g_mix.mixSampleRate);
    }
    return DS_OK;
}

long DirectSoundEnumerateA(DSDENUMCALLBACK* pCallback, void* ref)
{
    if (pCallback)
    {
        GUID g = DSDEVID_DefaultPlayback;
        pCallback(&g, "WebAudio Software 3D Mixer", "swbf2-web", ref);
    }
    return DS_OK;
}

long DirectSoundCaptureEnumerateA(DSDENUMCALLBACK*, void*)
{
    return DS_OK;
}

long GetDeviceID(const GUID* pcGuidDevice, GUID* pcGuidDeviceID)
{
    if (!pcGuidDeviceID) return DSERR_INVALIDPARAM;
    *pcGuidDeviceID = pcGuidDevice ? *pcGuidDevice : DSDEVID_DefaultPlayback;
    return DS_OK;
}

// --- Snd::DSBuffer ----------------------------------------------------------------------------

Snd::DSBuffer::DSBuffer()
{
    mBuffer = 0;
    mBuffer3D = 0;
    mProperties = 0;
    mDirectSound = 0;
    mFlags = 0;
    mSampleRate = g_mix.mixSampleRate;
    mFrequency = g_mix.mixSampleRate;
    mTargetFrequency = g_mix.mixSampleRate;
    mGain = 1.0f;
    mTargetGain = 1.0f;
    mLength = 0;
    mNode.mNext = 0;
    mNode.mPrev = 0;
    mResourcesAcquired = 0;
    mInterpolation = 0;
}

Snd::DSBuffer::~DSBuffer()
{
    Destroy();
}

bool Snd::DSBuffer::AcquireResources()
{
    mResourcesAcquired = (mBuffer != 0) ? 1 : 0;
    return mResourcesAcquired != 0;
}

bool Snd::DSBuffer::Create(SampleInfo::Format eFormat, unsigned int uiNumChannels,
                            unsigned int uiSampleRate, unsigned int uiLength, bool b3D,
                            bool /*bProperties*/, unsigned int uiFlags, bool bPrimary,
                            bool /*bSetFormat*/, IDirectSound* pDirectSound,
                            unsigned long /*ulChannelMask*/, bool /*bRetryInSoftware*/)
{
    Destroy();

    if (uiNumChannels == 0) uiNumChannels = 2;
    if (uiSampleRate == 0) uiSampleRate = g_mix.mixSampleRate;

    BrowserDSBuffer* buf = new BrowserDSBuffer(eFormat, uiNumChannels, uiSampleRate,
                                               uiLength, b3D && !bPrimary, bPrimary, uiFlags);
    mBuffer = buf;
    mBuffer3D = buf->Get3D();
    if (mBuffer3D) mBuffer3D->AddRef();
    mProperties = 0;
    mDirectSound = pDirectSound;
    mFlags = uiFlags;
    mSampleRate = uiSampleRate;
    mFrequency = uiSampleRate;
    mTargetFrequency = uiSampleRate;
    mGain = 1.0f;
    mTargetGain = 1.0f;
    mLength = buf->mBufferBytes;
    mResourcesAcquired = 1;
    return true;
}

void Snd::DSBuffer::Destroy()
{
    if (mProperties)
    {
        mProperties->Release();
        mProperties = 0;
    }
    if (mBuffer3D)
    {
        mBuffer3D->Release();
        mBuffer3D = 0;
    }
    if (mBuffer)
    {
        mBuffer->Stop();
        mBuffer->Release();
        mBuffer = 0;
    }
    mResourcesAcquired = 0;
    mLength = 0;
}

bool Snd::DSBuffer::GetFormat(SampleInfo::Format& reFormat, unsigned int& ruiNumChannels,
                               unsigned int& ruiSampleRate) const
{
    if (!mBuffer) return false;
    auto* buf = static_cast<BrowserDSBuffer*>(mBuffer);
    reFormat = buf->mFormat;
    ruiNumChannels = buf->mChannels;
    ruiSampleRate = buf->mSampleRate;
    return true;
}

bool Snd::DSBuffer::IsPlaying()
{
    if (!mBuffer) return false;
    return static_cast<BrowserDSBuffer*>(mBuffer)->mPlaying;
}

bool Snd::DSBuffer::IsTerminated()
{
    if (!mBuffer) return true;
    return static_cast<BrowserDSBuffer*>(mBuffer)->mTerminated;
}

bool Snd::DSBuffer::Play(bool bLooping, unsigned int /*uiPriority*/, bool /*bImmediate*/,
                          bool bRewind) const
{
    if (!mBuffer) return false;
    auto* buf = static_cast<BrowserDSBuffer*>(mBuffer);
    if (bRewind) buf->SetCurrentPosition(0);
    return buf->Play(0, 0, bLooping ? DSBPLAY_LOOPING : 0) == DS_OK;
}

void Snd::DSBuffer::ResetBuffers() {}
void Snd::DSBuffer::ResetFrequencyInterpolation() {}
void Snd::DSBuffer::ResetGainInterpolation() {}

void Snd::DSBuffer::SetFrequency(unsigned int uiFrequency)
{
    if (uiFrequency == 0) uiFrequency = mSampleRate;
    mFrequency = uiFrequency;
    mTargetFrequency = uiFrequency;
    if (mBuffer) mBuffer->SetFrequency(uiFrequency);
}

void Snd::DSBuffer::SetGain(float fGain)
{
    mGain = fGain;
    mTargetGain = fGain;
    if (mBuffer)
    {
        static_cast<BrowserDSBuffer*>(mBuffer)->SetGainLinear(fGain);
    }
}

void Snd::DSBuffer::SetPan(float fPan) const
{
    if (mBuffer)
    {
        static_cast<BrowserDSBuffer*>(mBuffer)->SetPanLinear(fPan);
    }
}

bool Snd::DSBuffer::SetProperty(const GUID&, unsigned long, void*, unsigned long) const
{
    return false;
}

void Snd::DSBuffer::Stop()
{
    if (mBuffer) mBuffer->Stop();
}

bool Snd::DSBuffer::TestProperty(const GUID&, unsigned long) const
{
    return false;
}

unsigned int Snd::DSBuffer::CreateBuffers(bool, bool, unsigned int, DSBuffer*, unsigned int, IDirectSound*)
{
    return 0;
}

void Snd::DSBuffer::DestroyBuffers(DSBuffer*, unsigned int& ruiCount)
{
    ruiCount = 0;
}

bool Snd::DSBuffer::Set3DParams(DS3DBUFFER& rParams, bool bImmediate) const
{
    if (!mBuffer3D) return false;
    return mBuffer3D->SetAllParameters(&rParams, bImmediate ? DS3D_IMMEDIATE : DS3D_DEFERRED) == DS_OK;
}

Snd::DSBuffer::WaveFormat::WaveFormat(SampleInfo::Format eFormat, unsigned int uiNumChannels,
                                       unsigned int uiSampleRate, bool bExtensible,
                                       unsigned long ulChannelMask)
{
    memset(this, 0, sizeof(*this));
    const unsigned short usBitsPerSample = (unsigned short)SampleInfo::GetBitDepth(eFormat);
    Format.nChannels = (unsigned short)uiNumChannels;
    Format.nSamplesPerSec = uiSampleRate;
    Format.wBitsPerSample = usBitsPerSample;
    Format.nBlockAlign = (unsigned short)((usBitsPerSample >> 3) * uiNumChannels);
    Format.nAvgBytesPerSec = Format.nBlockAlign * uiSampleRate;
    if (!bExtensible && ulChannelMask == 0)
    {
        Format.wFormatTag = WAVE_FORMAT_PCM;
        Format.cbSize = 14;
        return;
    }
    Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    Format.cbSize = 40;
    SubFormat = GUID();
    dwChannelMask = ulChannelMask;
}

// --- Snd::StreamCaptureSource (voice capture remains disabled until Phase 6) ------------------

Snd::StreamCaptureSource::StreamCaptureSource()
    : mCaptureDevice(0), mCaptureBuffer(0), mCaptureBufferSize(0), mNextLockPos(0), mFrameSize(0)
{
}

Snd::StreamCaptureSource::~StreamCaptureSource() { Close(); }
void Snd::StreamCaptureSource::Close() { mCaptureDevice = 0; mCaptureBuffer = 0; mCaptureBufferSize = 0; mNextLockPos = 0; }
void Snd::StreamCaptureSource::Flush(bool) { mNextLockPos = 0; }
bool Snd::StreamCaptureSource::Open(unsigned char, StreamFormat const&, unsigned int, unsigned char, StreamPacket*) { return false; }
void Snd::StreamCaptureSource::UpdateObject() {}

// --- Phase 5 Audio Mixer Pump & JS/WebAudio Exports -------------------------------------------

extern "C"
{

void swbf2_audio_set_sample_rate(int sampleRate)
{
    if (sampleRate >= 22050 && sampleRate <= 96000)
    {
        g_mix.mixSampleRate = (uint32_t)sampleRate;
    }
}

void swbf2_audio_set_next_mix_frames(int frames)
{
    if (frames < 0)
    {
        g_mix.nextMixFramesOverride = -1;
    }
    else
    {
        if (frames > 4096) frames = 4096;
        g_mix.nextMixFramesOverride = frames;
    }
}

int swbf2_audio_pump_and_mix(float dtSeconds)
{
    const int32_t overrideFrames = g_mix.nextMixFramesOverride;
    g_mix.nextMixFramesOverride = -1;

    if (Snd::Engine::GetMixConfig() == Snd::Engine::mixConfig_Disabled)
    {
        return 0;
    }

    uint32_t frames = 0;
    if (overrideFrames >= 0)
    {
        frames = (uint32_t)overrideFrames;
    }
    else
    {
        if (dtSeconds <= 0.0005f || dtSeconds > 0.1f) dtSeconds = 1.0f / 60.0f;
        frames = (uint32_t)lroundf(dtSeconds * (float)g_mix.mixSampleRate);
        if (frames < 128u) frames = 128u;
        if (frames > 4096u) frames = 4096u;
    }

    // Even if the AudioWorklet ring is already topped up (e.g., high-refresh-rate display
    // or suspended AudioContext), keep SoundFL's thread update list pumped once.
    if (frames == 0u)
    {
        PumpSndThreadUpdateOnce();
        return 0;
    }

    static float s_tmpL[4096];
    static float s_tmpR[4096];
    memset(s_tmpL, 0, frames * sizeof(float));
    memset(s_tmpR, 0, frames * sizeof(float));

    // Two-pass initial pump so newly triggered SFX/gunshots transition into playing state
    // immediately on the same frame with zero startup delay.
    PumpSndThreadUpdateOnce();
    PumpSndThreadUpdateOnce();

    // Advance & mix in small sub-chunks (~5.3 ms) with SoundFL thread updates between chunks
    // so low-latency (~50 ms) DSBufferRenderers always refill ahead of the play cursor without
    // wrapping over stale samples or stuttering after a frame hitch.
    uint32_t playingCount = 0;
    uint32_t streamPlayingCount = 0;
    uint32_t mixedSoFar = 0;
    while (mixedSoFar < frames)
    {
        uint32_t chunk = frames - mixedSoFar;
        if (chunk > kMixSubChunkFrames) chunk = kMixSubChunkFrames;
        if (mixedSoFar > 0u)
        {
            PumpSndThreadUpdateOnce();
        }
        uint32_t chunkPlaying = 0;
        uint32_t chunkStream = 0;
        for (size_t i = 0; i < g_mix.bufferCount; ++i)
        {
            BrowserDSBuffer* b = g_mix.buffers[i];
            if (b && b->mPlaying)
            {
                ++chunkPlaying;
                uint64_t beforeNz = b->mNonZeroFrames;
                b->MixAdd(s_tmpL + mixedSoFar, s_tmpR + mixedSoFar, chunk);
                if (i > 0)
                {
                    ++chunkStream;
                    g_mix.streamNonZeroFrames += (b->mNonZeroFrames - beforeNz);
                }
            }
        }
        if (chunkPlaying > playingCount) playingCount = chunkPlaying;
        if (chunkStream > streamPlayingCount) streamPlayingCount = chunkStream;
        mixedSoFar += chunk;
    }

    float peak = 0.0f;
    double sumL2 = 0.0;
    double sumR2 = 0.0;
    uint32_t nz = 0;
    for (uint32_t i = 0; i < frames; ++i)
    {
        float l = SoftLimitSample(s_tmpL[i]);
        float r = SoftLimitSample(s_tmpR[i]);
        s_tmpL[i] = l;
        s_tmpR[i] = r;
        float al = fabsf(l);
        float ar = fabsf(r);
        if (al > peak) peak = al;
        if (ar > peak) peak = ar;
        if (al > 1e-5f || ar > 1e-5f) ++nz;
        sumL2 += (double)l * (double)l;
        sumR2 += (double)r * (double)r;
    }

    PushMixedStereo(s_tmpL, s_tmpR, frames);

    ++g_mix.mixCalls;
    g_mix.totalMixedFrames += frames;
    g_mix.nonZeroFrames += nz;
    g_mix.lastPlayingBuffers = playingCount;
    if (playingCount > g_mix.peakPlayingBuffers) g_mix.peakPlayingBuffers = playingCount;
    g_mix.lastStreamBuffers = streamPlayingCount;
    if (streamPlayingCount > g_mix.peakStreamBuffers) g_mix.peakStreamBuffers = streamPlayingCount;
    g_mix.lastPeakAbs = peak;
    if (peak > g_mix.maxPeakAbs) g_mix.maxPeakAbs = peak;
    g_mix.lastRmsL = sqrtf((float)(sumL2 / (double)frames));
    g_mix.lastRmsR = sqrtf((float)(sumR2 / (double)frames));
    g_mix.accumEnergyL += sumL2;
    g_mix.accumEnergyR += sumR2;
    g_mix.accumEnergyFrames += frames;

    // One more pump so DSBufferRenderers immediately observe the play-cursor advance and refill.
    PumpSndThreadUpdateOnce();
    return (int)frames;
}

int swbf2_audio_mix_pending_frames(void)
{
    uint32_t r = g_mix.ringRead;
    uint32_t w = g_mix.ringWrite;
    return (int)((w >= r) ? (w - r) : (kMixRingFrames - r + w));
}

int swbf2_audio_drain_stereo(float* outL, float* outR, int maxFrames)
{
    if (!outL || !outR || maxFrames <= 0) return 0;
    int avail = swbf2_audio_mix_pending_frames();
    int n = (avail < maxFrames) ? avail : maxFrames;
    for (int i = 0; i < n; ++i)
    {
        outL[i] = g_mix.ringL[g_mix.ringRead];
        outR[i] = g_mix.ringR[g_mix.ringRead];
        g_mix.ringRead = (g_mix.ringRead + 1u) % kMixRingFrames;
    }
    return n;
}

void swbf2_audio_reset_stats(void)
{
    g_mix.mixCalls = 0;
    g_mix.totalMixedFrames = 0;
    g_mix.nonZeroFrames = 0;
    g_mix.peakPlayingBuffers = g_mix.lastPlayingBuffers;
    g_mix.peakStreamBuffers = g_mix.lastStreamBuffers;
    g_mix.streamNonZeroFrames = 0;
    g_mix.lastPeakAbs = 0.0f;
    g_mix.maxPeakAbs = 0.0f;
    g_mix.lastRmsL = 0.0f;
    g_mix.lastRmsR = 0.0f;
    g_mix.accumEnergyL = 0.0;
    g_mix.accumEnergyR = 0.0;
    g_mix.accumEnergyFrames = 0;
}

const char* swbf2_audio_stats(void)
{
    static char s_json[512];
    const float overallRmsL = g_mix.accumEnergyFrames
        ? sqrtf((float)(g_mix.accumEnergyL / (double)g_mix.accumEnergyFrames)) : 0.0f;
    const float overallRmsR = g_mix.accumEnergyFrames
        ? sqrtf((float)(g_mix.accumEnergyR / (double)g_mix.accumEnergyFrames)) : 0.0f;
    snprintf(s_json, sizeof(s_json),
             "{\"mixConfig\":%d,\"sampleRate\":%u,\"activeBuffers\":%u,"
             "\"playingBuffers\":%u,\"peakPlayingBuffers\":%u,"
             "\"streamBuffers\":%u,\"peakStreamBuffers\":%u,\"streamNonZeroFrames\":%llu,"
             "\"totalBuffersCreated\":%u,"
             "\"mixCalls\":%u,\"totalMixedFrames\":%llu,\"nonZeroFrames\":%llu,"
             "\"pendingFrames\":%d,\"lastPeak\":%.4f,\"maxPeak\":%.4f,"
             "\"rmsL\":%.4f,\"rmsR\":%.4f,\"overallRmsL\":%.4f,\"overallRmsR\":%.4f}",
             (int)Snd::Engine::GetMixConfig(),
             g_mix.mixSampleRate,
             (unsigned)g_mix.bufferCount,
             g_mix.lastPlayingBuffers,
             g_mix.peakPlayingBuffers,
             g_mix.lastStreamBuffers,
             g_mix.peakStreamBuffers,
             (unsigned long long)g_mix.streamNonZeroFrames,
             g_mix.totalBuffersCreated,
             g_mix.mixCalls,
             (unsigned long long)g_mix.totalMixedFrames,
             (unsigned long long)g_mix.nonZeroFrames,
             swbf2_audio_mix_pending_frames(),
             g_mix.lastPeakAbs,
             g_mix.maxPeakAbs,
             g_mix.lastRmsL,
             g_mix.lastRmsR,
             overallRmsL,
             overallRmsR);
    return s_json;
}

}  // extern "C"
