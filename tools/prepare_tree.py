#!/usr/bin/env python3
"""Prepare build/src_fixed from the pristine decompilation for the WASM build.

Every transformation here is a reconstruction of something the MSVC x86 source could not have
*not* contained, or a documented platform collapse. Nothing is deleted silently: each pass counts
what it changed and the counts are printed so the diff is auditable.

  1. operator new(unsigned int|unsigned long|UINT ...) -> operator new(size_t ...)
     C++ requires the allocation function's first parameter to be size_t.
  2. Duplicate global `operator new(size_t)` / `operator delete(void*)` definitions in one TU
     become the array forms (`operator new[]`). MSVC mangles ??2 (scalar) and ??_U (array) into
     separate definitions; the decompiler emitted both as `operator new`, which cannot coexist.
  2b. The matching `operator delete(void*, unsigned int)` size parameter follows pass 1 to size_t.
  3. Allocator function-pointer types take size_t, not unsigned int. MSVC's size_t *is*
     `unsigned int`, so `Globals::smAlloc` (size_t) and `FreeList::mAllocFunc` (unsigned int)
     were interchangeable there; emscripten's size_t is `unsigned long`, so both spellings must
     collapse to size_t.
  4. Vararg trampolines whose trailing parameter the decompiler recorded as `char*`
     (MSVC x86 va_list is literally `char*`) become `va_list`.
  5. `unsigned int(-1)` style functional casts -> `(unsigned int)(-1)`; that cast form is MSVC's.
  6. Bare `enum X;` forward declarations / `enum X {` definitions get an explicit `: int`.
  7. Missing member `operator new` / `operator delete` declarations are put back into their class.
     A class with a custom allocator must declare them; the definitions survive in the .cpp, but
     a member allocation function with no in-class declaration cannot be defined out of line.
  8. Redundant explicit destructor instantiations are dropped when the same TU already explicitly
     instantiates the whole class (which includes the destructor). The decompiler emitted both.
  9. Inside RedEngineFL/Graphics/PC headers only, the FakeD3D* software-fallback classes are
     dropped: they are the reference implementation of the very layer the browser build replaces
     (WebGPU), and they derive from D3D COM interfaces we deliberately do not model.

This tool never rewrites forward declarations into includes and never edits anything outside the
passes above - that job belongs to include_pass.py, which is purely additive.
"""
import os
import json
import re
import sys
import shutil
import collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "vendor", "openswbf2", "src")
OUT = os.path.join(ROOT, "build", "src_fixed")

OPERATOR_NEW = re.compile(r'operator new(?:\[\])?\(\s*(?:unsigned (?:int|long)|UINT)\b')
ALLOC_FNPTR  = re.compile(r'(void\s*\*\s*\(\s*(?:__cdecl\s*)?\*+\s*\w*\s*\)\s*\()\s*unsigned int\s*(\))')
OPERATOR_DEL = re.compile(r'operator delete\s*\(\s*void\s*\*\s*\w*\s*,\s*unsigned (?:int|long|char)\b')
VALIST_PARAM = re.compile(r'\bchar\s*\*\s*(pArgs|args|pArgList|argList)\b')
ALLOC_HOOK   = re.compile(r'(?P<name>\b\w*(?:Alloc|AllocPerm|Free|FreePerm|Realloc|Malloc|Calloc))'
                          r'\s*\(\s*unsigned int\b')
FUNC_CAST    = re.compile(r'\b(unsigned int|unsigned long|signed int|DWORD|UINT|USHORT)\(\s*(-?\s*\d+)\s*\)')
# One-off corrections for decompilation defects that no general pass can produce safely.
# Each entry: file -> [(pattern, replacement)] with the reason recorded next to it.
KNOWN_ARTIFACTS = {
    # Factory<TYPE, CLASS, DESC>::FindNetIndex writes `typedef PblList<FactoryType>::Node Node;`.
    # FactoryType is a dependent name, so the standard requires `typename`; MSVC accepted it
    # anyway, and the decompiler faithfully reproduced what it saw.
    'Battlefront2/Source/NetGame.cpp': [
        (r'typedef\s+(PblList<\w+>::\w+)\s+(\w+);', r'typedef typename \1 \2;'),
    ],
    # On MSVC x86 DWORD is unsigned long; in our wasm32/i386 shim DWORD is unsigned int to match
    # unsigned int* out-params across the engine. Normalize ThreadEntry's declaration to DWORD so
    # it matches sndthread.cpp's `DWORD WINAPI ThreadBase::ThreadEntry` and CreateThread.
    'SoundFL/source/sndthreadbase.h': [
        (r'static\s+unsigned\s+long\s+__stdcall\s+ThreadEntry\b', 'static DWORD __stdcall ThreadEntry'),
    ],
    # RedSegment::Read takes the software-skinned VDAT chunk *and* the hardware-skinned compressed
    # VBUF chunk when GEOMETRYMODE_COMPRESSED is on: the VBUF branch overwrites uiVertexSize /
    # uiVertexFormat (24-byte, format 0x1004) but leaves pVertexData pointing at VDAT's 20-byte CPU
    # array and bSoftwareSkinned set, so the segment is built as pcSoftwareSkinnedShaderSegment with
    # a declaration that does not match its data - hardware-skinned characters draw nothing. Once a
    # hardware VBUF has been read it is the authoritative vertex source, so drop the VDAT pair.
    'RedEngineFL/Graphics/PC/pcRedSegment.cpp': [
        (r'(if[ \t]*\([ \t]*RedRenderer::pcGetRenderCaps\(\)\.geometry[ \t]*==[ \t]*'
         r'GEOMETRYMODE_COMPRESSED\)[ \t]*\r?\n[ \t]*\{[ \t]*\r?\n)([ \t]*)uiVertexSize = uiSize;',
         r'\1\2'
         '                    /* A hardware-skinned VBUF supersedes the software-skinned VDAT read '
         'earlier\n                       for this segment: keeping both pairs VDAT\'s 20-byte CPU '
         'vertex array with\n                       VBUF\'s 24-byte hardware declaration (0x1004) and '
         'builds a\n                       pcSoftwareSkinnedShaderSegment out of them, which draws '
         'nothing. Once a\n                       hardware VBUF is in hand it is the authoritative '
         'vertex source. */\n'
         '                    if (bSoftwareSkinned)\n                    {\n'
         '                        delete[] static_cast<unsigned char*>(pVertexData);\n'
         '                        delete[] puiSkinData;\n'
         '                        pVertexData = 0;\n'
         '                        puiSkinData = 0;\n'
         '                        bSoftwareSkinned = false;\n                    }\n'
         r'\2uiVertexSize = uiSize;'),
    ],
    # Character.cpp forward-declares two anonymous-namespace helpers with `inline` after their
    # non-inline definitions earlier in the same TU (MSVC permits this, Clang rejects it).
    'Battlefront2/Source/Character.cpp': [
        (r'\binline\s+int\s+GetWeaponChannelByWeaponIndex\(', 'int GetWeaponChannelByWeaponIndex('),
        (r'\binline\s+AmmoCounter\*\s+GetAmmoCounter\(', 'AmmoCounter* GetAmmoCounter('),
    ],
    # RedMoviePlayer::Update calls CreateBinkDecoder() unconditionally after BinkOpen(), which
    # dereferences mBink->Width even if BinkOpen returned null (missing/unsupported movie).
    'RedEngineFL/movie/pc/pcRedMoviePlayer.cpp': [
        (r'([ \t]*)sOpeningBinkDecoder = false;\r?\n[ \t]*CreateBinkDecoder\(\);',
         r'\1sOpeningBinkDecoder = false;\n\1if (mBink)\n\1{\n\1    CreateBinkDecoder();\n\1}'),
    ],
    # Snd::EngineBase::Open constructs Voice and Stream objects in smVoiceStorage[0xA800] and
    # smStreamStorage[0x1446A68] using `uiOffset < size` (which only terminates at 32/6 on MSVC
    # where sizeof(Voice)==0x540 and sizeof(Stream)==0x3611BC). Under wasm32/Clang our shim
    # types are slightly smaller (1308 / 3543420), so `uiOffset < size` runs an extra 33rd/7th
    # placement-new that overflows smStreamStorage by 3.5 MB and zeroes smVoiceVirtualManager!
    'SoundFL/source/sndenginebase.cpp': [
        (r'uiOffset < 0xA800', 'uiOffset + sizeof(Voice) <= 0xA800'),
        (r'uiOffset < 0x1446A68', 'uiOffset + sizeof(Stream) <= 0x1446A68'),
    ],
    # emdawnwebgpu allocates internal C++ RefCounted wrappers via global ::operator new. When
    # called inside ShellState/MissionState (__RedCurrHeap == RunTimeHeap), those wrappers would
    # otherwise land in RunTimeHeap and get destroyed on Shell <-> Mission transitions. A sentinel
    # heap id (-2) lets WgpuSysHeapGuard route WebGPU internal allocations to system malloc while
    # RedFreeToHeap already forwards non-RedMi pointers back to free().
    'RedEngineFL/Memory/RedMemory.cpp': [
        (r'(void\* __cdecl operator new\(size_t size\)\r?\n\{\r?\n)',
         r'\1    if (__RedCurrHeap == -2) { return malloc(size ? size : 1); }\n'),
        (r'(void\* __cdecl operator new\[\]\(size_t size\)\r?\n\{\r?\n)',
         r'\1    if (__RedCurrHeap == -2) { return malloc(size ? size : 1); }\n'),
    ],
    # Single-threaded WASM has no Win32 SoundFL FileThread; complete Snd::File::Read synchronously
    # (matching Snd::File::Open) so RedMoviePlayer (.mvs) and SoundFL streams never block forever.
    'SoundFL/source/sndfile.cpp': [
        (r'    InsertFile\(this\);\r?\n    mLock\.Signal\(\);',
         '    CompleteFileOp();'),
    ],
    # pcPerPixelDiffuseLightingShader::GetState_Prog1 added both +12 (terrain 0x10000000) and +6
    # (bumpMapHash), producing out-of-range states 18..23 against core.lvl's 18-state PROG1 table
    # (0..5: mesh no-bump, 6..11: mesh bump+tangent, 12..17: terrain bump without tangent). That
    # caused LoadVertexAndPixelShader to fall back to VS=0/PS=0 (FFP) on bumped terrain (Mos Eisley),
    # leaving lighting black so pcTerrainShader2's multiplicative pass painted the ground solid black.
    'RedEngineFL/Graphics/PC/Shaders/pcPerPixelDiffuseLightingShader.cpp': [
        (r'    if \(pMaterial->materialFlags & 0x10000000\)\r?\n    \{\r?\n        iState \+= 12;\r?\n    \}\r?\n    if \(pMaterial->bumpMapHash\)\r?\n    \{\r?\n        iState \+= 6;\r?\n    \}',
         '    if (pMaterial->bumpMapHash)\n    {\n        iState += (pMaterial->materialFlags & 0x10000000) ? 12 : 6;\n    }'),
    ],
    # pcPerPixelDiffuseLightingShader::GetState_Prog1 can compute state 18..23 (0x10000000 + bump)
    # when core.lvl PROG1 perpixeldiffuselighting_shader.xml only has 18 states; guard bounds and
    # zero shader pool sizes in Reset() so repeated Shell <-> Mission transitions stay safe.
    'RedEngineFL/Graphics/PC/pcRedShader.cpp': [
        (r'(    else\r?\n    \{\r?\n)(        RedRenderer::pcLoadVertexShader\(m_states\[uiState\]\.pass\[uiPass\]\.apVertexShaders\[uiVertexShader\]\);)',
         r'\1        if (!m_states || uiState >= m_uiNumStates || uiPass >= 4 || uiVertexShader >= 255 || !m_states[uiState].pass[uiPass].apVertexShaders)\n        {\n            RedRenderer::pcLoadVertexShader(0);\n            RedRenderer::pcLoadPixelShader(0);\n            return;\n        }\n\2'),
        (r'(    delete\[\] m_vertexShaderPool;\r?\n    m_vertexShaderPool = 0;\r?\n    m_uiNumStates = 0;)',
         r'\1\n    m_uiPixelShaderPoolSize = 0;\n    m_uiVertexShaderPoolSize = 0;'),
    ],
    # PlayerProfile::tGameOpt::ApplySettings writes to RawControllerInputs via hardcoded MSVC x86
    # byte offsets (0x20bc for mCurKeyboardBindings, 0x2638/0x263d/0x2642, 0x000c for mFlip). On
    # MSVC x86 a polymorphic class containing a `double` inserts 4 bytes of padding after the vptr
    # (putting mMode at 0x0008 and mCurKeyboardBindings at 0x20bc); under Clang/Itanium C++ ABI
    # (both wasm32 and i386 Linux) mMode sits at 0x0004 and mCurKeyboardBindings at 0x20b8, so the
    # hardcoded +0x20bc write overflows by 4 bytes into m_pGuiInputs, truncating its low 16 bits to
    # 0x0000 (e.g. 0x005a5xxx -> 0x005a0000). Every subsequent RawControllerInputs::ReadLatest()
    # then memset-zeroes 0x005a0000 (corrupting whatever global lives there, such as
    # FLEffect::s_EffectFactories) and breaks all GUI mouse/button routing. Use the real C++ member
    # names instead of raw byte offsets.
    'Battlefront2/Source/PlayerProfile.cpp': [
        (r'    RawControllerInputs\* pJoystick = FLInputManager::s_instance\.GetPlayerJoystick\(iPort\);\r?\n'
         r'    unsigned char\* pRaw = \(unsigned char\*\)pJoystick;\r?\n\r?\n'
         r'    for \(int iMode = 0; iMode < 5; \+\+iMode\)\r?\n    \{\r?\n'
         r'        memcpy\(pRaw \+ 0x20bc \+ iMode \* sizeof\(mKeyboardBindings\[0\]\), mKeyboardBindings\[iMode\],\r?\n'
         r'               sizeof\(mKeyboardBindings\[0\]\)\);\r?\n    \}\r?\n\r?\n'
         r'    for \(int iMode = 0; iMode < 5; \+\+iMode\)\r?\n    \{\r?\n'
         r'        if \(!mControllerScale\[0\]\[iMode\]\)\r?\n        \{\r?\n'
         r'            mControllerScale\[0\]\[iMode\] = 4;\r?\n        \}\r?\n'
         r'        if \(!mControllerScale\[1\]\[iMode\]\)\r?\n        \{\r?\n'
         r'            mControllerScale\[1\]\[iMode\] = 4;\r?\n        \}\r?\n\r?\n'
         r'        pRaw\[0x2638 \+ iMode\] = mControllerScale\[0\]\[iMode\];\r?\n'
         r'        pRaw\[0x263d \+ iMode\] = mControllerScale\[1\]\[iMode\];\r?\n'
         r'        pRaw\[0x000c \+ iMode\] = mFlip\[iMode\];\r?\n'
         r'        pJoystick->mMouseSensitivity\[iMode\] = ConvertMouseScaleToRawScale\(mMouseSensitivity\[iMode\] / 10\.0f, iMode\);\r?\n'
         r'        pRaw\[0x2638 \+ iMode\] = mJoystickSensitivityX\[iMode\];\r?\n'
         r'        pRaw\[0x263d \+ iMode\] = mJoystickSensitivityY\[iMode\];\r?\n'
         r'        pRaw\[0x2642 \+ iMode\] = mMouseAssist\[iMode\];',
         '    RawControllerInputs* pJoystick = FLInputManager::s_instance.GetPlayerJoystick(iPort);\n\n'
         '    for (int iMode = 0; iMode < 5; ++iMode)\n    {\n'
         '        memcpy(pJoystick->mCurKeyboardBindings[iMode], mKeyboardBindings[iMode],\n'
         '               sizeof(mKeyboardBindings[0]));\n    }\n\n'
         '    for (int iMode = 0; iMode < 5; ++iMode)\n    {\n'
         '        if (!mControllerScale[0][iMode])\n        {\n'
         '            mControllerScale[0][iMode] = 4;\n        }\n'
         '        if (!mControllerScale[1][iMode])\n        {\n'
         '            mControllerScale[1][iMode] = 4;\n        }\n\n'
         '        pJoystick->mJoystickSensitivityX[iMode] = mControllerScale[0][iMode];\n'
         '        pJoystick->mJoystickSensitivityY[iMode] = mControllerScale[1][iMode];\n'
         '        pJoystick->mFlip[iMode] = mFlip[iMode] != 0;\n'
         '        pJoystick->mMouseSensitivity[iMode] = ConvertMouseScaleToRawScale(mMouseSensitivity[iMode] / 10.0f, iMode);\n'
         '        pJoystick->mJoystickSensitivityX[iMode] = mJoystickSensitivityX[iMode];\n'
         '        pJoystick->mJoystickSensitivityY[iMode] = mJoystickSensitivityY[iMode];\n'
         '        pJoystick->mMouseAssist[iMode] = mMouseAssist[iMode] != 0;'),
    ],
    # GameLoop.cpp defines SoldierAnimatorClass::PostLoad() with `inline`, which clang -O0
    # emits out-of-line but clang -O2 inlines and drops, breaking external callers in
    # spike/level_harness.cpp and browser/gpu/wgpu_engine_bridge.cpp under release builds.
    'Battlefront2/Source/GameLoop.cpp': [
        ('inline void SoldierAnimatorClass::PostLoad()',
         'void SoldierAnimatorClass::PostLoad()'),
    ],
    # Stream::Open connects mRenderer[i].StreamConsumer::Connect(&mADPCMDecoder[i], 0) (or
    # Connect(&mSource, 0) via Connect(apSinks)), writing StreamConsumer::StreamNodeSink::mConnection.
    # Because StreamRenderer inherits StreamNodeSink twice (via StreamRendererBase -> StreamConsumer ->
    # StreamNodeSink AND via DSBufferRenderer -> StreamNodeSink inside mHardwareRenderer), calling
    # mHardwareRenderer->StreamNodeSink::Connect(this, 0) in StreamRenderer::ConnectInternal made
    # StreamRenderer::GetPacket read StreamRenderer::mConnection (Wait: both point to the same
    # StreamConsumer::mConnection subobject), EXCEPT DSBufferRenderer's QueuePackets() was never
    # pumping or DSBufferRenderer::UpdateGain left mGainCurrent at 0x7FFF while mGainTarget was
    # shifted by << 16 (0x7FFF0000), AND in mixConfig_Software (1), ConnectInternal's decompilation
    # omits the software resampler branch, creating a DSBufferRenderer whose mGainCurrent[i] starts
    # at 0x7FFF (0.000015 in 16.16 fixed point!) instead of 0x7FFF0000, AND ResetGain() reads
    # out-of-bounds mGainCurrent[i - 2]!
    # Fix DSBufferRenderer::DSBufferRenderer() initial mGainTarget/mGainCurrent to 0x7FFF0000 (1.0 in
    # 16.16 fixed-point) and fix DSBufferRenderer::ResetGain() to copy mGainTarget[i] into
    # mGainCurrent[i] (instead of mGainCurrent[i - 2]).
    'SoundFL/source/win/snddsbufferrenderer.cpp': [
        (r'    mGainTarget\[0\] = 0x7FFF;\r?\n'
         r'    mGainCurrent\[0\] = 0x7FFF;\r?\n'
         r'    mGainTarget\[1\] = 0x7FFF;\r?\n'
         r'    mGainCurrent\[1\] = 0x7FFF;',
         '    mGainTarget[0] = 0x7FFF0000;\n'
         '    mGainCurrent[0] = 0x7FFF0000;\n'
         '    mGainTarget[1] = 0x7FFF0000;\n'
         '    mGainCurrent[1] = 0x7FFF0000;'),
        (r'    for \(unsigned int i = 0; i < mNumChannels; \+\+i\)\r?\n'
         r'    \{\r?\n'
         r'        mGainCurrent\[i\] = mGainCurrent\[i - 2\];\r?\n'
         r'    \}',
         '    for (unsigned int i = 0; i < mNumChannels; ++i)\n'
         '    {\n'
         '        mGainCurrent[i] = mGainTarget[i];\n'
         '    }'),
        (r'    if \(uiLockSize != 0\)\r?\n'
         r'    \{\r?\n\r?\n'
         r'        void\* pAudio1;',
         '    if (uiLockSize != 0)\n'
         '    {\n'
         '        if (!DSBuffer::IsPlaying())\n'
         '        {\n'
         '            UpdateBufferParameters(pBuffer, false, false);\n'
         '            ResetGain();\n'
         '        }\n\n'
         '        void* pAudio1;'),
    ],
    # NetMatch.cpp dereferences PlayerProfile::GetCurProfile() without a null check when answering
    # LAN PKT_QUERY or publishing Galaxy lobby data; guard it with a 30 TPS default.
    'Battlefront2/Source/NetMatch.cpp': [
        (r'unsigned char u8Turns = PlayerProfile::GetCurProfile\(\)->mOnlineOptions\.mu8TurnsPerSecond;',
         'unsigned char u8Turns = PlayerProfile::GetCurProfile() ? PlayerProfile::GetCurProfile()->mOnlineOptions.mu8TurnsPerSecond : 30;'),
    ],
    # Separate continuous Music streaming (bus 0x9f9c4fd4) from short VoiceOver/BattleChatter
    # streams so web clients can disable high-bandwidth music while keeping voiceovers active.
    'SoundFL/source/win/sndstream.h': [
        (r'static void EnableStreaming\(bool bEnable\);',
         'static void EnableStreaming(bool bEnable);\n    static void EnableMusicStreaming(bool bEnable);'),
        (r'static bool smEnableStreaming;',
         'static bool smEnableStreaming;\n    static bool smEnableMusicStreaming;'),
    ],
    'SoundFL/source/win/sndstream.cpp': [
        (r'#include "sndengine\.h"',
         '#include "sndengine.h"\n#include "SoundFL/source/sndbus.h"'),
        (r'void Stream::EnableStreaming\(bool bEnable\)\n\{\n    smEnableStreaming = bEnable;\n\}',
         'void Stream::EnableStreaming(bool bEnable)\n{\n    smEnableStreaming = bEnable;\n}\n\nvoid Stream::EnableMusicStreaming(bool bEnable)\n{\n    smEnableMusicStreaming = bEnable;\n}'),
        (r'bool Stream::smEnableStreaming = true;',
         'bool Stream::smEnableStreaming = true;\nbool Stream::smEnableMusicStreaming = true;'),
        (r'if \(!IsOpen\(\) \|\| mOpening \|\| !smEnableStreaming\)\n    \{\n        return;\n    \}',
         'if (!IsOpen() || mOpening || !smEnableStreaming)\n    {\n        return;\n    }\n    if (!smEnableMusicStreaming)\n    {\n        for (const Bus* pBus = GetBus(); pBus; pBus = pBus->mParentBus)\n        {\n            if (pBus->mHashID == 0x9f9c4fd4u /* "Music" */)\n            {\n                if (GetPlayState() != playState_stopped)\n                {\n                    Flush(true, true);\n                    mSource.Stop();\n                    SourceTransport::Stop();\n                    SetPlayState(playState_stopped);\n                }\n                return;\n            }\n        }\n    }'),
    ],
    # Non-blocking async Snd::File::Read() for StreamSource so WAN audio stream misses
    # (voiceovers, battle chatter, music) prefetch asynchronously without stalling the 60 Hz frame loop.
    'SoundFL/source/sndfile.h': [
        (r'void SetPriority\(unsigned int uiPriority\);',
         'void SetPriority(unsigned int uiPriority);\n    void CancelPendingRead();\n    static void SetAsyncStreamFile(File* pFile, bool bEnable);\n    static void PumpAsyncReads();'),
    ],
    'SoundFL/source/sndfile.cpp': [
        (r'#include "PebbleFL/Common/PblHashTable\.h"',
         '#include "PebbleFL/Common/PblHashTable.h"\n\n'
         'extern "C" int swbf2_sndfile_is_range_cached(const Snd::FilePlatform* fp, unsigned int offset, unsigned int length);\n'
         'extern "C" void swbf2_sndfile_prefetch_range(const Snd::FilePlatform* fp, unsigned int offset, unsigned int length);\n\n'
         'namespace {\n'
         'static constexpr int kMaxAsyncSndFiles = 32;\n'
         'Snd::File* g_asyncStreamFiles[kMaxAsyncSndFiles] = {};\n'
         'Snd::File* g_pendingAsyncReads[kMaxAsyncSndFiles] = {};\n'
         'bool IsAsyncStreamFile(Snd::File* f) { for (int i = 0; i < kMaxAsyncSndFiles; ++i) if (g_asyncStreamFiles[i] == f) return true; return false; }\n'
         'void EnqueuePendingAsyncRead(Snd::File* f) { for (int i = 0; i < kMaxAsyncSndFiles; ++i) if (g_pendingAsyncReads[i] == f) return; for (int i = 0; i < kMaxAsyncSndFiles; ++i) if (!g_pendingAsyncReads[i]) { g_pendingAsyncReads[i] = f; return; } }\n'
         'bool RemovePendingAsyncRead(Snd::File* f) { for (int i = 0; i < kMaxAsyncSndFiles; ++i) if (g_pendingAsyncReads[i] == f) { g_pendingAsyncReads[i] = nullptr; return true; } return false; }\n'
         '}'),
        (r'(File::~File\(\)\n\{)\s*\n(\s*if \(mFile\.IsOpen\(\)\))',
         r'\1\n    CancelPendingRead();\n    SetAsyncStreamFile(this, false);\n\2'),
        (r'(void File::Close\(\)\n\{\n)(\s*mLock\.Wait\(\);)',
         r'\1    CancelPendingRead();\n\2'),
        (r'(void File::Read\(void\* pBuffer, unsigned int uiSize\)\n\{\n)(\s*mLock\.Wait\(\);)',
         'void File::SetAsyncStreamFile(File* pFile, bool bEnable) {\n'
         '    if (!pFile) return;\n'
         '    if (bEnable) { for (int i = 0; i < kMaxAsyncSndFiles; ++i) if (g_asyncStreamFiles[i] == pFile) return; for (int i = 0; i < kMaxAsyncSndFiles; ++i) if (!g_asyncStreamFiles[i]) { g_asyncStreamFiles[i] = pFile; return; } }\n'
         '    else { for (int i = 0; i < kMaxAsyncSndFiles; ++i) if (g_asyncStreamFiles[i] == pFile) g_asyncStreamFiles[i] = nullptr; }\n'
         '}\n'
         'void File::CancelPendingRead() {\n'
         '    if (RemovePendingAsyncRead(this)) { mFileOp = fileOp_nothing; mBlockSize = 0; if (mOpCompleteFunc) mOpCompleteFunc(this, mCallbackData, false); mLock.Signal(); }\n'
         '}\n'
         'void File::PumpAsyncReads() {\n'
         '    for (int i = 0; i < kMaxAsyncSndFiles; ++i) {\n'
         '        File* pFile = g_pendingAsyncReads[i]; if (!pFile) continue;\n'
         '        if (!pFile->mFile.IsOpen() || pFile->mFileOp != fileOp_read) { g_pendingAsyncReads[i] = nullptr; pFile->mFileOp = fileOp_nothing; pFile->mBlockSize = 0; if (pFile->mOpCompleteFunc) pFile->mOpCompleteFunc(pFile, pFile->mCallbackData, false); pFile->mLock.Signal(); continue; }\n'
         '        int st = swbf2_sndfile_is_range_cached(&pFile->mFile, pFile->mFilePos, pFile->mBlockSize);\n'
         '        if (st > 0) { g_pendingAsyncReads[i] = nullptr; pFile->CompleteFileOp(); }\n'
         '        else if (st < 0) { g_pendingAsyncReads[i] = nullptr; pFile->mFileOp = fileOp_nothing; pFile->mBlockSize = 0; if (pFile->mOpCompleteFunc) pFile->mOpCompleteFunc(pFile, pFile->mCallbackData, false); pFile->mLock.Signal(); }\n'
         '    }\n'
         '}\n\n'
         r'\1    CancelPendingRead();\n\2'),
        (r'(    if \(mFileOp == fileOp_open\)\n    \{\n        CompleteFileOp\(\);\n        return;\n    \}\n\n)(\s*CompleteFileOp\(\);)',
         r'\1    if (IsAsyncStreamFile(this) && mFile.IsOpen()) {\n'
         '        int st = swbf2_sndfile_is_range_cached(&mFile, mFilePos, uiSize);\n'
         '        if (st == 0) { swbf2_sndfile_prefetch_range(&mFile, mFilePos, uiSize); EnqueuePendingAsyncRead(this); return; }\n'
         '        if (st < 0) { mFileOp = fileOp_nothing; mBlockSize = 0; if (mOpCompleteFunc) mOpCompleteFunc(this, mCallbackData, false); mLock.Signal(); return; }\n'
         '    }\n\n'
         r'\2'),
        (r'(void File::Seek\(unsigned int uiPos\)\n\{\n)(\s*mLock\.Wait\(\);)',
         r'\1    CancelPendingRead();\n\2'),
    ],
    'SoundFL/source/sndstreamsource.cpp': [
        (r'mFile\.SetFileOpCompleteFunc\(&StreamSource::FileOpComplete, this\);',
         'mFile.SetFileOpCompleteFunc(&StreamSource::FileOpComplete, this);\n    File::SetAsyncStreamFile(&mFile, true);'),
        (r'(void StreamSource::Flush\(\)\n\{[\s\S]*?)\n    mLock\.Wait\(\);',
         r'\1\n    mFile.CancelPendingRead();\n    mLock.Wait();'),
    ],
}

# The decompiler re-defines classes it only partially understands inside the headers of the
# translation unit it was analysing (see dedup_partial_class_defs). Doing that tree-wide is the
# correct end state, but measured here it regressed 195 files: some of those copies sit in
# headers that the real class reaches only through a cycle, and some TUs were compiled against
# the partial view in ways the real header does not satisfy (ZephyrSkeletonShared, Factory,
# RedParticleRenderer::Renderable). So redirects are opt-in per file, and each entry is only
# added once the files including it have been compiled - the honest way to retire an artifact.
PARTIAL_COPY_REDIRECTS = [
    # Holds 15 partial "class WeaponX : public Weapon" reconstructions. They collide with the
    # real headers at link time ('duplicate symbol: vtable for WeaponDisguise' and ten more)
    # and index virtuals in the wrong order. Included by NetState.cpp, FLInputManager.cpp and
    # HUDGameEvents.cpp, all of which compile against the real classes after the redirect.
    'Battlefront2/Source/NetState.h',
]

MEM_OPDEF    = re.compile(r'^(?P<ret>void\s*\*|void|int|unsigned int)\s+__cdecl\s+'
                          r'(?P<qual>(?:[A-Za-z_]\w*::)+)operator\s+(?P<op>new|delete)\s*'
                          r'\((?P<params>[^)]*)\)\s*$', re.M)
CLASS_DEF    = re.compile(r'\b(?:class|struct)\s+(?P<name>[A-Za-z_]\w*)(?P<rest>[^;{}]*?)\{')
DTOR_INST    = re.compile(r'^[ \t]*template[ \t]+(?P<spec>[^;]*?)::~(?P<cls>\w+)\s*\(\s*\)\s*;[ \t]*\r?$')


INTERNAL_LINKAGE_FIXES = [
    (
        "Battlefront2/Source/FLGuiManager.cpp",
        "GuiManager sInstances[1];",
        "// [prepare_tree] Both this file and Tracker.cpp define a file-scope global the\n"
        "// decompiler named sInstances. C++ mangles object symbols by name alone, so the two\n"
        "// collided at link time even though their types differ (GuiManager[1] vs a pointer to\n"
        "// an array of 64 Trackers). Each is referenced only inside its own translation unit,\n"
        "// which is what the original binary's `dynamic initializer for 'sInstances'` implies,\n"
        "// so the fix is the static the decompiler dropped - not renaming either engine symbol.\n"
        "static GuiManager sInstances[1];",
    ),
]


def fix_internal_linkage_collisions(root, counts):
    for rel, find, repl in INTERNAL_LINKAGE_FIXES:
        path = os.path.join(root, *rel.split("/"))
        if not os.path.isfile(path):
            continue
        text = open(path, encoding="utf-8", errors="replace").read()
        if find not in text:
            continue
        open(path, "w", encoding="utf-8").write(text.replace(find, repl, 1))
        counts["internal_linkage_fixed"] += 1
    return counts


def _skip_c_like(text, j):
    """Position after a string/char literal starting at j (j points at the quote)."""
    quote = text[j]
    j += 1
    n = len(text)
    while j < n:
        c = text[j]
        if c == '\\':
            j += 2
            continue
        if c == quote:
            return j + 1
        if c == '\n' and quote == "'":
            return j
        j += 1
    return j


def match_brace(text, i):
    """Index of the '}' closing the '{' at position i, or -1 when unbalanced.

    Single linear scan with a comment/string state machine. Both properties matter here:
    a naive per-character "am I in a comment" query is quadratic and this tool walks every
    header in the tree, and skipping comments/strings is not optional - this decompilation
    is annotated with lines like "// VTABLE: SWBF2 0x007b1190", and a '}' inside one of
    those makes a naive counter stop early, which truncates the header and produces dozens
    of "expected ';' after class" errors in files that merely include it.
    """
    depth, j, n = 0, i, len(text)
    while j < n:
        c = text[j]
        if c in '"\'':
            j += 1
            while j < n:
                if text[j] == '\\':
                    j += 2
                    continue
                if text[j] == c or text[j] == '\n':
                    break
                j += 1
            j += 1  # step past the closing quote; leaving j on it re-opens the literal
            continue
        if c == '/' and j + 1 < n and text[j+1] == '/':
            k = text.find('\n', j)
            j = n if k == -1 else k + 1
            continue
        if c == '/' and j + 1 < n and text[j+1] == '*':
            k = text.find('*/', j + 2)
            j = n if k == -1 else k + 2
            continue
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return j
        j += 1
    return -1


def class_spans(text, name):
    """[(body_open, body_close)] of every `class/struct name { ... }` definition."""
    spans = []
    for m in CLASS_DEF.finditer(text):
        if m.group('name') != name:
            continue
        end = match_brace(text, m.end() - 1)
        if end >= 0:
            spans.append((m.end(), end))
    return spans


def add_member_allocators(text, pending, counts):
    """Pass 7: declare in-class operator new/delete that the decompiler left undeclared.

    `pending` maps a class name to the declarations its .cpp definitions imply; it is built in a
    first pass over the .cpp files because the definitions live in translation units while the
    classes live in headers.
    """
    for cls, decls in list(pending.items()):
        for start, end in class_spans(text, cls):
            body = text[start:end]
            missing = [d for d in decls if f"operator {d['op']}" not in body]
            if not missing:
                pending.pop(cls, None)
                break
            insert = "\n    /* declared here: the decompiler kept only the out-of-line definition */\n" + \
                     "".join(f"    static {d['ret']} operator {d['op']}({d['params']});\n" for d in missing)
            text = text[:start] + insert + text[start:]
            counts['member_allocator_declared'] += len(missing)
            pending.pop(cls, None)
            break
    return text


def dedup_global_operators(text, counts):
    """Pass 2: second scalar-looking definition of operator new/delete becomes the array form."""
    for op, arr in (('new', 'new[]'), ('delete', 'delete[]')):
        pat = re.compile(r'^(?P<ret>void\s*\*|void)\s+__cdecl\s+operator\s+' + op + r'\s*\((?P<p>[^)]*)\)\s*$',
                         re.M)
        hits = list(pat.finditer(text))
        if len(hits) < 2:
            continue
        seen, out, last = set(), [], 0
        for i, h in enumerate(hits):
            key = (h.group('ret').strip(), h.group('p').strip())
            out.append(text[last:h.start()])
            if key in seen and i > 0:
                out.append(f"void {'* ' if op == 'new' else ''}__cdecl operator {arr}({h.group('p')})")
                counts['global_operator_became_array'] += 1
            else:
                out.append(h.group(0))
            seen.add(key)
            last = h.end()
        out.append(text[last:])
        text = ''.join(out)
    return text


def drop_redundant_dtor_inst(text, counts):
    """Pass 8: keep `template class X<...>;`, drop the extra `template X<...>::~T();`."""
    lines = text.split('\n')
    kept = []
    for line in lines:
        m = DTOR_INST.match(line)
        if m and re.search(r'template\s+class\s+' + re.escape(m.group('spec').strip()) + r'\s*;', text):
            counts['redundant_dtor_instantiation_dropped'] += 1
            kept.append('/* dropped: destructor already instantiated by the explicit class instantiation */')
            continue
        kept.append(line)
    return '\n'.join(kept)


SPECIAL_DEF = re.compile(r'^(?P<ret>[A-Za-z_][\w:<>,\*&\s~]*?)\s+(?P<q>[A-Za-z_]\w*<[^;{}]*>\s*::\s*[~]?\w+)\s*\(', re.M)


# Translation units where the decompiler placed explicit member-specialization definitions at the
# bottom of the file (legal for MSVC, which instantiates at end of TU). Keep this list explicit:
# hoisting blindly reorders code that other parts of the TU may depend on.
HOIST_SPECIALIZATION_FILES = {'Battlefront2/Source/NetGame.cpp'}


# NetGame.cpp reconstructs `template <class T, int N> class BblList` itself and then defines two
# explicit specializations about 5500 lines below the first thing that instantiates the primary
# template (`class NetEntityList : public BblList<NetEntity, 512>`). MSVC instantiates at end of
# translation unit, so the original source could legally sit there; clang instantiates on first
# use and rejects a specialization that arrives afterwards. Only these two members move. The
# tree-wide version (hoist_specializations) reorders far too much and regressed 195 files.
NETGAME_HOIST_MEMBERS = [
    r'void BblList<NetEntity, 512>::Remove\s*\(\s*NetEntity&\s*item\s*\)',
    r'void BblList<NetEntity, 512>::Itor::Advance\s*\(\s*\)',
]
# Land above *every* out-of-line member of the specialization, not merely above the first class
# that derives from it: defining BblList<NetEntity, 512>::Itor::Itor instantiates the nested Itor,
# and a specialization of Itor::Advance arriving after that is rejected just the same.
NETGAME_HOIST_ANCHOR = 'BblList<NetEntity, 512>::Itor::Itor'


def hoist_netgame_specializations(text, counts):
    """Move NetGame.cpp's two late BblList specializations above their first instantiation."""
    spans = []
    for pattern in NETGAME_HOIST_MEMBERS:
        m = re.search(pattern, text)
        if not m:
            return text
        # walk back over the signature to include its `template <>` and the decompiler's
        # // FUNCTION: comment, so the block moves as one unit
        start = text.rfind('template', 0, m.start())
        back = text.rfind('//', 0, m.start())
        if back > start - 3 and back != -1 and text.rfind('\n', 0, back) > text.rfind('\n', 0, start):
            start = back
        brace = text.find('{', m.end())
        end = match_brace(text, brace)
        if start < 0 or brace < 0 or end < 0:
            return text
        spans.append((start, end + 1))

    anchor_at = text.find(NETGAME_HOIST_ANCHOR)
    # The blocks must lie *below* the anchor - that is the whole point of the move. If any of
    # them is already above it, the file is not the layout this fix describes, so back off.
    if anchor_at < 0 or any(sp[1] < anchor_at for sp in spans):
        return text

    spans.sort(reverse=True)
    blocks = []
    for a, b in spans:
        blocks.insert(0, text[a:b].strip('\n'))
        # swallow the blank line that separated the block from what followed it
        after = b
        while after < len(text) and text[after] in '\r\n':
            after += 1
        text = text[:a] + text[after:]

    moved = (
        '// [prepare_tree] Moved from below, immediately before the first use of the primary\n'
        '// template: clang instantiates on first use, and MSVC instantiated at end of TU.\n'
        + '\n\n'.join(blocks) + '\n\n'
    )
    at = text.find(NETGAME_HOIST_ANCHOR)
    # The anchor is a signature, and the two lines above it (`// FUNCTION:` and `template <>`)
    # belong to it: inserting between them would orphan the declaration. Back up to the blank
    # line that separates the preceding construct.
    boundary = text.rfind('\n\n', 0, at)
    if boundary > 0:
        at = boundary + 2
    text = text[:at] + moved + text[at:]
    counts['netgame_specializations_hoisted'] = counts.get('netgame_specializations_hoisted', 0) + len(blocks)
    return text


def hoist_specializations(text, counts):
    """Move out-of-line definitions of template specializations above their first use.

    MSVC instantiates at end of translation unit, so the original source could place these at the
    bottom; clang instantiates on first use and rejects a specialization defined after that point.
    The definitions are re-emitted immediately before the first point that mentions the primary
    template - never above the include block, because the template itself may be declared in the
    body of this very file.
    """
    bodies = [b for b in SPECIAL_DEF.finditer(text)]
    blocks = []
    for m in bodies:
        if m.start() and text[m.start() - 1] != '\n':
            continue
        j = text.find('{', m.end() - 1)
        if j < 0:
            continue
        depth, k = 0, j
        while k < len(text):
            c = text[k]
            if c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    k += 1
                    break
            elif c in '"\'':
                k += 1
                while k < len(text) and text[k] != c:
                    if text[k] == '\\':
                        k += 1
                    k += 1
            elif c == ';' and depth == 0:
                break
            k += 1
        else:
            continue
        # key on the full specialization (Class<Args>) rather than the primary template: the
        # primary may be declared long before the specialization is first referenced, and hoisting
        # to the primary's first mention can outrun the types in its own argument list.
        spec = re.sub(r'\s+', ' ', m.group('q').split('::')[0].strip())
        blocks.append((m.start(), k, text[m.start():k], spec))
    if not blocks:
        return text

    def first_use(spec, own_start):
        """earliest position, before own_start, where this specialization is named outside a definition"""
        pat = re.compile(r'(?<![\w:>)])' + re.escape(spec).replace(r'\ ', r'\s+') + r'(?![\w])')
        for m in pat.finditer(text):
            if m.start() >= own_start:
                break
            if any(b[0] <= m.start() < b[1] for b in blocks):
                continue
            return m.start()
        return None

    def decl_end(name):
        """end of the in-file definition of `name`, so we never hoist above a type it needs"""
        m = re.search(r'^[ \t]*(class|struct)\s+' + re.escape(name) + r'\b[^;{]*\{', text, re.M)
        if not m:
            return 0
        depth, j = 0, m.end() - 1
        while j < len(text):
            if text[j] == '{':
                depth += 1
            elif text[j] == '}':
                depth -= 1
                if depth == 0:
                    return j + 1
            j += 1
        return 0

    targets = []
    for s, e, body, spec in blocks:
        u = first_use(spec, s)
        if u is None:
            continue
        for arg in re.findall(r'[A-Za-z_]\w*', spec.split('<', 1)[-1]):
            u = max(u, decl_end(arg))
        last_include = [m.end() for m in re.finditer(r'^#\s*include', text[:s], re.M)]
        targets.append((u, s, e, body))
    if not targets:
        return text
    # never hoist into the include block: everything these definitions name is declared there
    last_include = [text.rfind('\n', 0, m.end()) + 1 for m in
                    re.finditer(r'^#\s*include\s*[<"][^>"]*[>"][^\n]*', text, re.M)]
    include_end = max(last_include, default=0)

    plan = []
    for pos, s, e, body in targets:
        plan.append((max(pos, include_end), s, e, body))

    marker = '/* hoisted above first use: clang instantiates eagerly, MSVC instantiated at EOF */'
    # remove first (descending), then insert (descending) - each block moves to its own point,
    # because one common point would outrun a type another block needs
    for _, s, e, _ in sorted(plan, key=lambda x: -x[1]):
        text = text[:s] + text[e:]
    for pos, s, e, _ in sorted(plan, key=lambda x: -x[0]):
        at = text.rfind('\n', 0, pos) + 1
        text = text[:at] + marker + "\n" + "\n".join(b for p, _, _, b in plan if p == pos) + "\n" + text[at:]

    counts['specialization_definitions_hoisted'] += len(targets)
    return text


def strip_classes(text, pred, removed):
    """Pass 9: delete class bodies whose name satisfies pred."""
    out, i = [], 0
    while True:
        m = CLASS_DEF.search(text, i)
        if not m:
            out.append(text[i:]); break
        if not pred(m.group('name')):
            out.append(text[i:m.end()]); i = m.end(); continue
        depth, j = 0, m.end() - 1
        while j < len(text):
            c = text[j]
            if c in '"\'':
                j += 1
                while j < len(text) and text[j] != c:
                    if text[j] == '\\':
                        j += 1
                    j += 1
            elif c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        k = j + 1
        while k < len(text) and text[k] in ' \t\r\n':
            k += 1
        if k < len(text) and text[k] == ';':
            k += 1
        out.append(text[i:m.start()])
        out.append(f'/* class {m.group("name")} (Direct3D software fallback) omitted in the browser build */')
        removed.append(m.group('name'))
        i = k
    return ''.join(out)


def _include_graph(root):
    """file -> set of files it includes, resolved the way the compiler resolves them.

    Needed by dedup_partial_class_defs: redirecting a partial class copy at the real header is
    only safe when that header cannot get back to the file being edited, otherwise the include
    guard of one of the two makes the class disappear and every file that relies on it fails.
    """
    files = []
    by_name = {}
    for d, _, fs in os.walk(root):
        for f in fs:
            if f.endswith(('.h', '.hpp', '.inl')):
                path = os.path.join(d, f)
                files.append(path)
                by_name.setdefault(f, []).append(path)

    inc_re = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"', re.M)
    graph = {}
    for path in files:
        text = open(path, encoding='utf-8', errors='replace').read()
        edges = set()
        for spec in inc_re.findall(text):
            spec = spec.replace('\\', '/')
            cand = [os.path.normpath(os.path.join(os.path.dirname(path), spec))]
            cand.append(os.path.normpath(os.path.join(root, spec)))
            cand += by_name.get(os.path.basename(spec), [])
            for c in cand:
                c = os.path.abspath(c)
                if c != os.path.abspath(path) and os.path.isfile(c):
                    edges.add(c)
        graph[os.path.abspath(path)] = edges
    return graph


def _reaches(graph, start, target, limit=40000):
    seen, stack, steps = set(), [start], 0
    while stack and steps < limit:
        steps += 1
        node = stack.pop()
        for nxt in graph.get(node, ()):
            if nxt == target:
                return True
            if nxt not in seen:
                seen.add(nxt)
                stack.append(nxt)
    return False


def dedup_partial_class_defs(root):
    """Replace decompiler-reconstructed partial class copies with the real header.

    The decompiler works one translation unit at a time, so headers end up re-defining classes
    they only partially understand. Battlefront2/Source/NetState.h is the clearest case: it
    carries fifteen "class WeaponX : public Weapon" bodies containing just the members netcode
    happens to touch, with Write/Read as the first virtuals, while WeaponX.h carries the real
    class with all of its virtuals.

    That is not cosmetic - it breaks both the link and, more quietly, correctness:

    * Both copies mangle to the same name, and clang emits a class's vtable in the translation
      unit defining its *key function*, the first non-inline, non-pure virtual. In the real
      header that is the destructor (so WeaponDisguise.cpp emits the vtable); in the partial
      copy the destructor is missing, so the key function is Write and NetState.cpp emits a
      second one. Result: 'wasm-ld: error: duplicate symbol: vtable for WeaponDisguise'.
    * Even if the link were fixed by hand, code compiled against the partial copy indexes
      virtuals by the partial copy's ordering. NetState.cpp calling WeaponDisguise::Write
      through a vtable would land on the wrong slot of the real vtable, because the real class
      has a dozen virtuals ahead of it. A partial copy is therefore unsafe to *use*, not just
      unsafe to link twice.

    So the copy is deleted and the authoritative header included in its place. The one file
    named after the class is treated as authoritative - that is the file the decompiler
    reconstructed from the class's own vtable. Where the redirect would create an include
    cycle, the copy is left alone and reported, because then the TU genuinely cannot see the
    real class and silently breaking 264 files is worse than a known gap; those cases land in
    build/class_copy_report.json and in docs/upstream-gaps.md.
    """
    counts = collections.Counter()
    full = re.compile(r'^([ \t]*)(?:template\s*<[^>]*>\s*)?(class|struct)\s+'
                      r'(?P<name>[A-Za-z_]\w*)\s*(?:final\s*)?(?::[^;{}]*)?\{', re.M)

    defs = collections.defaultdict(list)
    for d, _, fs in os.walk(root):
        for f in fs:
            if not f.endswith('.h'):
                continue
            path = os.path.join(d, f)
            text = open(path, encoding='utf-8', errors='replace').read()
            for m in full.finditer(text):
                end = match_brace(text, m.end() - 1)
                if end < 0:
                    continue
                semi = text.find(';', end)
                if semi != -1 and semi - end < 4:
                    end = semi + 1
                defs[m['name']].append((os.path.abspath(path), m.start(), end))

    graph = _include_graph(root)
    plans = collections.defaultdict(list)
    skipped = []

    allowed = {os.path.abspath(os.path.join(root, rel)) for rel in PARTIAL_COPY_REDIRECTS}

    for name, ds in sorted(defs.items()):
        if len(ds) < 2:
            continue
        owners = {p for p, _, _ in ds if os.path.splitext(os.path.basename(p))[0] == name}
        if len(owners) != 1:
            continue
        owner = owners.pop()
        for path, start, end in ds:
            if path == owner or path not in allowed:
                continue
            if _reaches(graph, owner, path):
                skipped.append({'class': name, 'copy': path, 'authoritative': owner,
                                'reason': 'including the authoritative header would cycle'})
                counts['partial_class_copies_left_in_place'] += 1
                continue
            rel = os.path.relpath(owner, os.path.dirname(path)).replace(os.sep, '/')
            plans[path].append((start, end, rel, name))
            counts['partial_class_defs_removed'] += 1

    # A header owns every class it defines, not just the one it is named after: WeaponGrenade.h
    # defines both WeaponGrenadeClass and WeaponGrenade. Once a copy is redirected at that
    # header, any local copy of the header's other classes is now a redefinition, so those go
    # too. Repeated until the set stops growing.
    def class_names_of(path):
        text = open(path, encoding='utf-8', errors='replace').read()
        return {m['name'] for m in full.finditer(text) if match_brace(text, m.end() - 1) >= 0}

    owner_cache = {}
    grew = True
    while grew:
        grew = False
        for path, items in list(plans.items()):
            owners_here = {os.path.abspath(os.path.normpath(
                os.path.join(os.path.dirname(path), rel))) for _, _, rel, _ in items}
            for owner in owners_here:
                if owner not in owner_cache:
                    owner_cache[owner] = class_names_of(owner)
                already = {it[3] for it in items}
                for cname in owner_cache[owner]:
                    if cname in already:
                        continue
                    for opath, start, end in defs.get(cname, []):
                        if opath != path:
                            continue
                        rel = os.path.relpath(owner, os.path.dirname(path)).replace(os.sep, '/')
                        plans[path].append((start, end, rel, cname))
                        counts['partial_class_defs_removed'] += 1
                        grew = True

    for path, items in plans.items():
        text = open(path, encoding='utf-8', errors='replace').read()
        includes = []
        # descending so earlier offsets stay valid; nested spans are skipped rather than
        # double-removed, which would eat text between the two classes
        removed_after = len(text)
        for start, end, rel, name in sorted(items, reverse=True):
            if end > removed_after:
                continue
            note = ("// [prepare_tree] partial reconstruction of '%s' removed; the real class "
                    "is defined in %s\n" % (name, rel))
            text = text[:start] + note + text[end:]
            removed_after = start
            includes.append('#include "%s"' % rel)
        lines = text.split('\n')
        at = 0
        for i, line in enumerate(lines[:120]):
            if line.startswith('#include') or line.startswith('#pragma once'):
                at = i + 1
        for inc in includes:
            lines.insert(at, inc)
        open(path, 'w', encoding='utf-8').write('\n'.join(lines))
        counts['headers_deduped'] += 1

    report = os.path.join(root, '..', 'class_copy_report.json')
    with open(report, 'w', encoding='utf-8') as fh:
        json.dump({'removed': counts['partial_class_defs_removed'],
                   'left_in_place': skipped}, fh, indent=2)
    return counts


def main():
    if os.path.exists(OUT):
        shutil.rmtree(OUT)
    counts = collections.Counter()
    enum_names = set()
    files = []
    for d, _, fs in os.walk(SRC):
        rel_dir = os.path.relpath(d, SRC).replace(os.sep, '/')
        if rel_dir == 'mimalloc' or rel_dir.startswith('mimalloc/'):
            continue
        for f in fs:
            if f.endswith(('.c', '.cpp', '.h', '.inl')):
                p = os.path.join(d, f)
                files.append(p)
                if f.endswith('.h'):
                    t = open(p, encoding='utf-8', errors='replace').read()
                    enum_names |= set(re.findall(r'\benum\s+([A-Za-z_]\w*)\s*;', t))

    removed = []
    # definitions live in .cpp files, the classes live in headers: collect first, patch second
    pending = collections.defaultdict(list)
    for p in files:
        if not p.endswith('.cpp'):
            continue
        t = open(p, encoding='utf-8', errors='replace').read()
        for m in MEM_OPDEF.finditer(t):
            if '::operator' not in m.group(0):
                continue
            cls = m.group('qual').rstrip(':').split('::')[-1]
            decl = {'ret': m.group('ret').strip(), 'op': m.group('op'), 'params': m.group('params').strip()}
            if decl not in pending[cls]:
                pending[cls].append(decl)

    for p in files:
        rel = os.path.relpath(p, SRC).split(os.sep)
        text = open(p, encoding='utf-8', errors='replace').read()

        counts['operator_new_size_t'] += len(OPERATOR_NEW.findall(text))
        text = OPERATOR_NEW.sub('operator new(size_t', text)

        counts['allocator_fnptr_size_t'] += len(ALLOC_FNPTR.findall(text))
        text = ALLOC_FNPTR.sub(r'\1size_t\2', text)

        # The deallocation function must mirror the allocation function's parameter list: since
        # pass 1 normalizes `operator new(unsigned int)` to size_t, the matching size parameter on
        # the placement `operator delete` has to move with it.
        counts['operator_delete_size_t'] += len(OPERATOR_DEL.findall(text))
        text = OPERATOR_DEL.sub(lambda m: m.group(0).replace('unsigned int', 'size_t')
                                                 .replace('unsigned long', 'size_t')
                                                 .replace('unsigned char', 'size_t'), text)

        # Allocator hooks: the engine stores them in size_t function pointers, so their parameter
        # must be size_t. On MSVC that was free (size_t *is* unsigned int there).
        counts['allocator_hook_size_t'] += len(ALLOC_HOOK.findall(text))
        text = ALLOC_HOOK.sub(lambda m: m.group('name') + '(size_t', text)

        counts['msvc_valist_param'] += len(VALIST_PARAM.findall(text))
        text = VALIST_PARAM.sub(r'va_list \1', text)

        counts['msvc_functional_cast'] += len(FUNC_CAST.findall(text))
        text = FUNC_CAST.sub(r'(\1)(\2)', text)

        rel_str = os.path.relpath(p, SRC).replace(os.sep, '/')
        for pattern, repl in KNOWN_ARTIFACTS.get(rel_str, []):
            counts['known_decomp_artifact_fixed'] += len(re.findall(pattern, text))
            text = re.sub(pattern, repl, text)

        if p.endswith('.cpp'):
            # hoist_specializations() exists but is deliberately not run: it reorders code and the
            # underlying problem is a class specialization the decompiler never emitted.
            text = dedup_global_operators(text, counts)
            text = drop_redundant_dtor_inst(text, counts)
            if rel_str == 'Battlefront2/Source/NetGame.cpp':
                text = hoist_netgame_specializations(text, counts)
        else:
            text = add_member_allocators(text, pending, counts)

        for name in enum_names:
            if re.search(r'\benum\s+' + name + r'\s*;', text):
                counts['enum_forward_decl_typed'] += 1
                text = re.sub(r'\benum\s+' + name + r'\s*;', 'enum ' + name + ' : int;', text)
            if re.search(r'\benum\s+' + name + r'\s*\{', text):
                counts['enum_definition_typed'] += 1
                text = re.sub(r'\benum\s+' + name + r'\s*\{', 'enum ' + name + ' : int {', text)

        # FakeD3D* used to be stripped from the RedEngineFL/Graphics/PC headers on the theory
        # that a browser build has no use for the software fallback classes. That was wrong and
        # it cost a translation unit: pcRedRenderer.cpp *defines* their members, so removing the
        # declarations left it with 326 'undeclared identifier FakeD3D' errors and lost the 91
        # RedRenderer:: symbols the whole game links against. They compile now that the shim
        # carries D3D9's vocabulary, so they stay - the browser backend replaces what they call,
        # not what they are.

        dest = os.path.join(OUT, os.path.relpath(p, SRC))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        open(dest, 'w', encoding='utf-8').write(text)

    # The browser build's own support translation units live next to the shims but belong in the
    # core archive: the engine calls into them (D3DX-named maths, Lua allocator hooks).
    shim_src = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "spike", "shim"))
    extra = os.path.join(OUT, "_browser_shim")
    os.makedirs(extra, exist_ok=True)
    copied = []
    for f in sorted(os.listdir(shim_src)):
        if f.endswith('.cpp'):
            shutil.copy(os.path.join(shim_src, f), os.path.join(extra, f))
            copied.append(f)
    print(f"prepared {len(files)} files -> {OUT} (+browser shim TUs: {', '.join(copied)})")
    counts.update(dedup_partial_class_defs(OUT))
    fix_internal_linkage_collisions(OUT, counts)
    print("fixes:", dict(counts))


if __name__ == '__main__':
    sys.exit(main())
