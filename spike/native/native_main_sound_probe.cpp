/* Resolve a weapon's sound name through the engine's own sound-property records. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../spike/level_harness.h"
#include "Battlefront2/Source/GameSoundEngine.h"
#include "Battlefront2/Source/LoadUtil.h"
#include "SoundFL/source/sndglobals.h"
#include "SoundFL/source/sndsound.h"
#include "SoundFL/source/sndsoundlayered.h"
#include "SoundFL/source/sndsoundparameterized.h"
#include "SoundFL/source/sndsoundstream.h"
#include "RedEngineFL/Graphics/RedCamera.h"
#include "RedEngineFL/Memory/RedMemory.h"

void __cdecl Log(char const* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

RedCamera* BrowserSceneCamera()
{
    static RedCamera sCamera;
    return &sCamera;
}

static void __cdecl NativeLog(char const* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void DumpDirectSounds()
{
    int i = 0;
    for (PblListSingle::Itor it(Snd::Sound::Properties::smPropertyList); !it.IsDone(); ++it)
    {
        const Snd::Sound::Properties* p =
            (const Snd::Sound::Properties*)((const char*)it.GetNode() -
                                            offsetof(Snd::Sound::Properties, mNode));
        printf("  direct property %08x\n", p->mHashID);
        if (++i >= 12) break;
    }
    printf("direct properties shown: %d\n", i);
}

static void ProbeSound(unsigned int uiHash)
{
    const Snd::Sound::Properties* pSound = Snd::Sound::Properties::FindByHashID(uiHash);
    const Snd::SoundLayered::Properties* pLayered =
        Snd::SoundLayered::Properties::FindByHashID(uiHash);
    const Snd::SoundStream::Properties* pStream =
        Snd::SoundStream::Properties::FindByHashID(uiHash);
    const Snd::SoundParameterized::Properties* pParameterized =
        Snd::SoundParameterized::Properties::FindByHashID(uiHash);
    printf("sound %08x: direct=%p layered=%p stream=%p parameterized=%p\n", uiHash,
           (const void*)pSound, (const void*)pLayered, (const void*)pStream,
           (const void*)pParameterized);
    if (pSound)
    {
        printf("  direct selected object: %p\n", pSound->mSelObjectWeightList->GetObjectA());
    }
    if (pLayered)
    {
        for (int i = 0; i < 32; ++i)
        {
            const Snd::SoundLayered::Properties::Layer* pLayer = pLayered->FindLayerByIndex(i);
            if (!pLayer) break;
            printf("  layer %d: sound props %08x, selected object %p\n", i,
                   pLayer->mSoundProps ? pLayer->mSoundProps->mHashID : 0,
                   pLayer->mSoundProps
                       ? pLayer->mSoundProps->mSelObjectWeightList->GetObjectA()
                       : 0);
        }
    }
    if (pStream)
    {
        printf("  stream id: %08x, selected segment: %08x\n", pStream->mStreamID,
               (unsigned int)(size_t)pStream->mSelObjectWeightList->GetObjectA());
    }
}

int main()
{
    if (!LevelHarnessInit(NativeLog))
    {
        return 1;
    }
    /* Property records only: no mixer is opened. The allocation hooks are the same ones
       GameSoundEngine::Init sets before it opens the engine (GameSoundEngine.cpp:151-157). */
    Snd::Globals::smAlloc = malloc;
    Snd::Globals::smFree = free;
    Snd::Globals::smAllocPerm = malloc;
    Snd::Globals::smFreePerm = free;
    Snd::Globals::smTempAlloc = malloc;
    Snd::Globals::smTempFree = free;
    Snd::Globals::smWarningFunction = 0;

    static const unsigned int uiIds[] = {
        0x5F6C766Cu, /* lvl_ - the nested sound set */
        0x5F646E73u, /* snd_ - its sound-property records */
    };
    if (!LoadUtil::ReadDataFileOnHeap("core.lvl", __RedTempHeap, uiIds, 1, 0, 0, 0))
    {
        fprintf(stderr, "could not read core.lvl\n");
        return 1;
    }
    if (!LoadUtil::ReadDataFile("sound/tat.lvl;tat2gcw", 0, 0, 0, 0))
    {
        fprintf(stderr, "could not read sound/tat.lvl;tat2cw\n");
        return 1;
    }

    DumpDirectSounds();
    ProbeSound(0x96547673u); /* imp_weap_inf_rifle_fire */
    ProbeSound(0xB1AD507Cu); /* its parent property */
    return 0;
}
