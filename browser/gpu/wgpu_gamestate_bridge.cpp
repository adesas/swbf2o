// Phase 4 Native GameState Driver (GameState::ShellState <-> GameState::MissionState).
//
// Replaces the temporary single-map harness with SWBF2's real retail GameState state machine:
//   - GameState::OneTimeInit() -> RedRenderer, RedInputManager, GameSoundEngine, PlayerPrefs
//   - GameState::ShellState -> core.lvl, common.lvl, shell.lvl, shell_interface Lua UI,
//     SaveGameManager / SaveDevice (OPFS /saves), GameMovie (shell.mvs)
//   - GameState::MissionState -> MISSION.lvl, full Lua mission scripts (ScriptPreInit /
//     ScriptInit / ScriptPostLoad), Instant Action Conquest / Hero Assault / Space Assault,
//     and clean MissionState::Exit() -> ShellState::Enter() round-trips.

#include "d3d9_wgpu_internal.h"

#include <stdio.h>
#include <string.h>

#include "Battlefront2/Platform/Pc/pcSpawnDisplay.h"
#include "Battlefront2/Source/Character.h"
#include "Battlefront2/Source/Controllable.h"
#include "Battlefront2/Source/FLEffect.h"
#include "Battlefront2/Source/FLGuiManager.h"
#include "Battlefront2/Source/NetGame.h"
#include "Battlefront2/Source/GameLoop.h"
#include "Battlefront2/Source/GameMovie.h"
#include "Battlefront2/Source/GameState.h"
#include "Battlefront2/Source/LoadDisplay.h"
#include "Battlefront2/Source/LuaHelper.h"
#include "RedEngineFL/Util/RedLocalizeDB.h"
#include <emscripten.h>
#include "Battlefront2/Source/MissionPlayList.h"
#include "Battlefront2/Source/NetComm.h"
#include "Battlefront2/Source/NetSetup.h"
#include "Battlefront2/Source/PlayerProfile.h"
#include "Battlefront2/Source/ShellLoop.h"
#include "Battlefront2/Source/Weapon.h"
#include "PebbleFL/Common/PblHashTable.h"
#include "Battlefront2/Source/FLInputManager.h"
#include "RedEngineFL/Graphics/PC/pcDisplaySettings.h"
#include "RedEngineFL/Graphics/PC/pcRedRenderer.h"
#include "RedEngineFL/Graphics/PC/pcRedTexture.h"
#include "RedEngineFL/Application/pc/pcFLRender.h"
#include "RedEngineFL/Interface/RedBitmapElement.h"
#include "RedEngineFL/movie/RedMovie.h"
#include "RedEngineFL/input/RedKeyboard.h"
#include "RedEngineFL/input/pc/pcRedInputManager.h"
#include "SoundFL/source/sndenginebase.h"
#include "SoundFL/source/sndfile.h"
#include "SoundFL/source/win/sndstream.h"

extern bool g_bNoSound;
extern bool g_bNoRender;
extern bool pcForceFixedFunction;
extern bool pcWindowed;
extern int pcWidth;
extern int pcHeight;
extern float g_fFixedRate;
extern bool netIsClient;
extern void RedWarningBrowserInit(void(__cdecl* pCallback)(char const*, ...));
extern void Log(const char* fmt, ...);

// FLGuiManager.cpp: the singleton that turns RedKeyboard events into GUI strokes,
// and the singleton GuiManager::CollectInputs drains every frame.
extern KeyGrabber sKeygrabber;
// GameState.cpp: set by GameState::OneTimeInit(), which is what calls RedInputManager::Init().
extern bool sbDidOneTimeInit;
// browser/platform/dinput_browser.cpp: did the engine take the DirectInput we install?
extern "C" void swbf2_input_seam_counters(unsigned long* pCreations, unsigned long* pDevices,
                               unsigned long* pKeyboardStates, unsigned long* pKeyboardDownBytes);
// browser/platform/dsound_browser.cpp: synchronous SoundFL pump & stereo mixer.
extern "C" int swbf2_audio_pump_and_mix(float dtSeconds);
namespace GameSoundEngine {
extern float gSoftMixerBufferSizeMs;
}
// browser/platform/winsock_browser.cpp: live virtual-LAN counters.
extern "C" void swbf2_net_counters(unsigned long* txPkts, unsigned long* txBytes,
                                   unsigned long* rxPkts, unsigned long* rxBytes,
                                   unsigned int* localIp);
extern "C" void swbf2_net_begin_frame(void);
extern int gAutoNetPlayers;
extern int netPlayerLimit;
extern int netNumBots;
extern int gGlobalNumBots;
extern int netDifficulty;
extern bool netTeamDamage;
extern bool netAutoAssignTeams;
extern bool netAutoAssignTeamsOriginalSetting;
extern bool netAutoAim;
extern bool netShowNames;
extern bool netHeroesEnabled;
extern int netHeroRuleUnlock;
extern int netHeroRuleUnlockVal;
extern int netHeroRulePlayer;
extern int netHeroRuleTeam;
extern int netHeroRuleRespawn;
extern int netHeroRuleRespawnVal;
extern int gSpawnInvicibility;
extern int hostVoteThreshold;
void SetNetAwardsEnabled(int);

namespace Lua_Callbacks {
extern int gDMReinforcementMult[5];
extern int gCONReinforcementMult[5];
extern int gCONNumBots[4];
extern int gCTFNumBots[4];
extern int gELINumBots[4];
extern int gASSNumBots[4];
extern int gSpaceAssaultScoreLimit[5];
extern int gCTFScore[5];
extern int gHuntScoreLimit[5];
extern int gHuntMaxTimeLimit[5];
extern int gCTFMaxTimeLimit[5];
extern int gCONMaxTimeLimit[5];
extern bool gRandomizePlaylist;
extern int gTeamSetReinforcementMult;
}  // namespace Lua_Callbacks

namespace {

bool g_gameStateBooted = false;
bool g_audioEnabled = true;
bool g_mpBridgeActive = false;
bool g_pendingNetEndShell = false;
int g_preferredWidth = 1600;
int g_preferredHeight = 900;
bool g_applyPreferredToProfile = false;
PlayerProfile::tProfile* g_lastSyncedProfile = nullptr;
bool g_prevScreenWasLoginOrSave = false;
unsigned short g_lastSeenProfResX = 0;
unsigned short g_lastSeenProfResY = 0;
int g_lastEnforcedMissionW = 0;
int g_lastEnforcedMissionH = 0;
bool g_loadBannerActive = false;

EM_JS(void, js_emit_load_banner, (const uint8_t* rgbaPtr, int byteLen, int w, int h,
                                  const char* title, const char* mode, const char* tip), {
  if (typeof Module !== 'undefined' && typeof Module.onLoadBanner === 'function') {
    const rgba = (rgbaPtr && byteLen > 0) ? HEAPU8.slice(rgbaPtr, rgbaPtr + byteLen) : null;
    Module.onLoadBanner({
      action: 'show',
      width: w,
      height: h,
      rgba,
      title: UTF8ToString(title),
      mode: UTF8ToString(mode),
      tip: UTF8ToString(tip),
    });
  }
});

EM_JS(void, js_emit_load_asset, (const char* kind, const uint8_t* rgbaPtr, int byteLen, int w, int h), {
  if (typeof Module !== 'undefined' && typeof Module.onLoadBanner === 'function') {
    const rgba = (rgbaPtr && byteLen > 0) ? HEAPU8.slice(rgbaPtr, rgbaPtr + byteLen) : null;
    Module.onLoadBanner({
      action: 'asset',
      kind: UTF8ToString(kind),
      width: w,
      height: h,
      rgba,
    });
  }
});

EM_JS(void, js_hide_load_banner, (), {
  if (typeof Module !== 'undefined' && typeof Module.onLoadBanner === 'function') {
    Module.onLoadBanner({ action: 'hide' });
  }
});

static void utf16ToUtf8(const unsigned short* src, char* dst, size_t dstCap) {
  if (!dst || dstCap == 0) return;
  size_t o = 0;
  if (src) {
    for (size_t i = 0; src[i] && o + 4 < dstCap; ++i) {
      unsigned int ch = src[i];
      if (ch < 0x80u) {
        dst[o++] = (char)ch;
      } else if (ch < 0x800u) {
        dst[o++] = (char)(0xC0u | (ch >> 6));
        dst[o++] = (char)(0x80u | (ch & 0x3Fu));
      } else {
        dst[o++] = (char)(0xE0u | (ch >> 12));
        dst[o++] = (char)(0x80u | ((ch >> 6) & 0x3Fu));
        dst[o++] = (char)(0x80u | (ch & 0x3Fu));
      }
    }
  }
  dst[o] = '\0';
}
unsigned int g_gameStateTicks = 0;
char g_gsStatusJson[2048] = {};
char g_netStatusJson[1024] = {};

void __cdecl GameStateEngineLog(char const* fmt, ...) {
  if (fmt && fmt[0]) {
    Log("gamestate: %s", fmt);
  }
}

void jsonEscape(const char* src, char* dst, size_t dstCap) {
  size_t o = 0;
  if (src) {
    for (size_t i = 0; src[i] && o + 2 < dstCap; ++i) {
      char c = src[i];
      if (c == '"' || c == '\\') {
        dst[o++] = '\\';
        dst[o++] = c;
      } else if ((unsigned char)c >= 0x20) {
        dst[o++] = c;
      }
    }
  }
  dst[o] = '\0';
}

}  // namespace

extern "C" {

static bool g_musicStreamingEnabled = true;

void swbf2_audio_set_enabled(int enabled) {
  g_audioEnabled = (enabled != 0);
}

void swbf2_audio_set_music_streaming(int enabled) {
  g_musicStreamingEnabled = (enabled != 0);
  Snd::Stream::EnableStreaming(true);
  Snd::Stream::EnableMusicStreaming(g_musicStreamingEnabled);
}

void swbf2_gamestate_emit_load_asset(const char* kind, const uint8_t* rgba, uint32_t width, uint32_t height) {
  if (!kind || !rgba || !width || !height) return;
  js_emit_load_asset(kind, rgba, (int)(width * height * 4u), (int)width, (int)height);
}

void swbf2_gamestate_emit_load_banner(const uint8_t* rgba, uint32_t width, uint32_t height) {
  // Only show loading screen banners for mission/campaign loads (not initial shell boot, which has
  // its own portal boot progress screen).
  if (!g_gameStateBooted || GameState::GetState() == 0x11E1FC01u /* shell */) {
    return;
  }
  char titleBuf[160] = {};
  char modeBuf[96] = {};
  char tipBuf[768] = {};
  if (LoadDisplay* ld = LoadDisplay::sInstance) {
    if (ld->m_missionNameHash) {
      if (const unsigned short* s = RedLocalizeDB::Find(ld->m_missionNameHash, true)) {
        utf16ToUtf8(s, titleBuf, sizeof(titleBuf));
      }
    }
    if (ld->m_modeNameHash) {
      if (const unsigned short* s = RedLocalizeDB::Find(ld->m_modeNameHash, true)) {
        utf16ToUtf8(s, modeBuf, sizeof(modeBuf));
      }
    }
    if (ld->m_campaignLayout && ld->m_entireMissionFileName[0]) {
      char key[256];
      snprintf(key, sizeof(key), "ifs.briefing.%s", ld->m_entireMissionFileName);
      if (const unsigned short* s = RedLocalizeDB::Find(PblHash(key), true)) {
        utf16ToUtf8(s, tipBuf, sizeof(tipBuf));
      }
      if (!titleBuf[0] && ld->m_campaignNameHash) {
        if (const unsigned short* s = RedLocalizeDB::Find(ld->m_campaignNameHash, true)) {
          utf16ToUtf8(s, titleBuf, sizeof(titleBuf));
        }
      }
    } else if (LoadDisplay::sTipsDatabase.m_numLoadedIDs > 0) {
      unsigned int tipHash = LoadDisplay::sTipsDatabase.GetRandomID();
      if (const unsigned short* s = RedLocalizeDB::Find(tipHash, true)) {
        utf16ToUtf8(s, tipBuf, sizeof(tipBuf));
      }
    }
  }
  g_loadBannerActive = true;
  js_emit_load_banner(rgba, (int)(width * height * 4u), (int)width, (int)height,
                      titleBuf, modeBuf, tipBuf);
}

int swbf2_gamestate_boot(int width, int height, const char* initialMission) {
  if (!swbf2_wgpu_is_ready()) {
    Log("gamestate: swbf2_wgpu_init must complete before swbf2_gamestate_boot");
    return 0;
  }
  if (g_gameStateBooted) {
    return 1;
  }

  if (width <= 0) width = 800;
  if (height <= 0) height = 600;
  g_preferredWidth = width;
  g_preferredHeight = height;
  GetWgpuContext().preferredWidth = (uint32_t)width;
  GetWgpuContext().preferredHeight = (uint32_t)height;
  g_applyPreferredToProfile = true;
  g_lastSyncedProfile = nullptr;
  g_prevScreenWasLoginOrSave = false;
  g_lastSeenProfResX = 0;
  g_lastSeenProfResY = 0;
  g_lastEnforcedMissionW = 0;
  g_lastEnforcedMissionH = 0;

  // Use a 50 ms SoundFL software mixer buffer (instead of 100 ms / 500 ms Win32 fallback) so
  // weapon shots and explosions play immediately without perceptible audio lag.
  GameSoundEngine::gSoftMixerBufferSizeMs = 50.0f;

  // Leave pcWidth/pcHeight at 0 so DisplayLogic::ShellSizePolicy renders the classic 2005 Shell UI
  // (including ifs_missionselect / Instant Action) at its designed 800x600 layout while Missions
  // render at the user's chosen high resolution (g_preferredWidth x g_preferredHeight).
  pcWidth = 0;
  pcHeight = 0;
  pcWindowed = true;
  g_bNoRender = false;
  g_bNoSound = !g_audioEnabled;  // Phase 5: real SoundFL 3D software mixer -> WebAudio AudioWorklet
  Snd::Stream::EnableStreaming(true);
  Snd::Stream::EnableMusicStreaming(g_musicStreamingEnabled);
  pcForceFixedFunction = false;  // Phase 3 SM 1.1 -> WGSL programmable pipeline
  g_fFixedRate = 60.0f;
  GameMovie::sUseMovies = true;
  ShellLoop::SetLegalShowtype(ShellLoop::eLEGALSCREENS_SKIP);
  // Keep SWBF2's in-game software cursor (FLRenderer::mCursorImage) active in the WebGPU canvas
  // instead of hiding it behind a Win32 HCURSOR that does not exist in an OffscreenCanvas worker.
  DisplayCfg::SetFlag(DisplayCfg::gCliDisplay, DisplayCfg::kSoftwareCursor);
  DisplayCfg::SetFlag(DisplayCfg::gDisplay, DisplayCfg::kSoftwareCursor);

  const bool directMission = (initialMission && initialMission[0]);
  const int initW = directMission ? g_preferredWidth : 800;
  const int initH = directMission ? g_preferredHeight : 600;
  swbf2_wgpu_resize(initW, initH);

  RedWarningBrowserInit(&GameStateEngineLog);
  RedRenderer::pcSetDisplayMode((unsigned int)initW, (unsigned int)initH,
                                DisplayLogic::kModeWindowed,
                                (unsigned int)initW, (unsigned int)initH,
                                false, D3DMULTISAMPLE_NONE, 0);

  if (directMission) {
    GameLoop::SetNextMission(initialMission);
    GameState::SetState(0x8FF60339u /* "mission" */);
  } else {
    GameState::SetState(0x11E1FC01u /* "shell" */);
  }

  // First Update() transitions from NULL -> ShellState (or MissionState) and runs OneTimeInit +
  // PreStateInit + Init + PostLoad + first frame.
  bool alive = GameState::Update();
  // RedRenderer::InitStatic() creates the initial device at 800x600 before any profile is loaded;
  // for a direct Quick-Launch mission at >800x600, queue the live mode switch immediately.
  if (alive && directMission && (g_preferredWidth != 800 || g_preferredHeight != 600)) {
    RedRenderer::pcApplyDisplayMode((unsigned int)g_preferredWidth, (unsigned int)g_preferredHeight,
                                    DisplayLogic::kModeWindowed,
                                    (unsigned int)g_preferredWidth, (unsigned int)g_preferredHeight,
                                    false, D3DMULTISAMPLE_NONE, 0);
  }
  if (g_audioEnabled) {
    swbf2_audio_pump_and_mix(1.0f / 60.0f);
  }
  g_gameStateBooted = true;
  g_gameStateTicks = 1;
  Log("gamestate: booted (pref=%dx%d, active=%ux%u, state=0x%08x, alive=%d, sound=%d, screen=%s)",
      g_preferredWidth, g_preferredHeight,
      RedInterfaceScreen::GetScreenWidth(), RedInterfaceScreen::GetScreenHeight(),
      GameState::GetState(), alive ? 1 : 0, g_audioEnabled ? 1 : 0,
      GuiManager::Instance() ? GuiManager::Instance()->GetCurrentScreen() : "<none>");
  return alive ? 1 : 0;
}

void swbf2_gamestate_set_resolution(int width, int height) {
  if (width < 640 || height < 480) return;
  g_preferredWidth = width;
  g_preferredHeight = height;
  GetWgpuContext().preferredWidth = (uint32_t)width;
  GetWgpuContext().preferredHeight = (uint32_t)height;
  g_lastEnforcedMissionW = 0;
  g_lastEnforcedMissionH = 0;
  if (!g_gameStateBooted) return;

  pcWidth = 0;
  pcHeight = 0;
  pcWindowed = true;
  DisplayCfg::gDisplay.bBorderless = false;

  PlayerProfile::tProfile* pProf = PlayerProfile::GetCurProfile();
  if (pProf) {
    pProf->mPCOptions.mResolutionX = (unsigned short)width;
    pProf->mPCOptions.mResolutionY = (unsigned short)height;
    pProf->mPCOptions.mWindowed = true;
    pProf->UpdateProfCRC();
    g_lastSyncedProfile = pProf;
    g_lastSeenProfResX = (unsigned short)width;
    g_lastSeenProfResY = (unsigned short)height;
    g_applyPreferredToProfile = false;
    PlayerProfile::ApplyLiveDisplay();
  } else if (GameState::GetState() == 0x8FF60339u /* mission */) {
    RedRenderer::pcApplyDisplayMode((unsigned int)width, (unsigned int)height,
                                    DisplayLogic::kModeWindowed,
                                    (unsigned int)width, (unsigned int)height,
                                    false, D3DMULTISAMPLE_NONE, 0);
  } else {
    g_applyPreferredToProfile = true;
  }
}

int swbf2_gamestate_tick(float dt) {
  if (!g_gameStateBooted || !swbf2_wgpu_is_ready()) return 0;
  swbf2_net_begin_frame();
  Snd::File::PumpAsyncReads();
  float stepDt = (dt > 0.001f && dt <= 0.25f) ? dt : (1.0f / 60.0f);
  g_fFixedRate = 1.0f / stepDt;
  // Always keep D3D9 in windowed mode inside the browser canvas (fullscreen is handled by the
  // browser Fullscreen API). Prevents in-game Video Options from triggering Win32 fullscreen
  // VidMode_Usable checks that can fall back to 640x480 / 800x600.
  pcWindowed = true;
  DisplayCfg::gDisplay.bBorderless = false;
  // Sync launcher/drawer preferred mission resolution onto PlayerProfile whenever a profile is
  // newly created or loaded from OPFS (which mutates mProfiles[0] in-place!), while still
  // adopting explicit changes the user makes in SWBF2's own Options -> Video Options screen.
  const char* preScreen =
      GuiManager::Instance() ? GuiManager::Instance()->GetCurrentScreen() : "";
  const bool isLoginOrSave = preScreen &&
      (strcmp(preScreen, "ifs_login") == 0 || strcmp(preScreen, "ifs_saveop") == 0);
  const bool isVideoOpts = preScreen && strcmp(preScreen, "ifs_opt_pcvideo") == 0;
  if (PlayerProfile::tProfile* pProf = PlayerProfile::GetCurProfile()) {
    const unsigned short curResX = pProf->mPCOptions.mResolutionX;
    const unsigned short curResY = pProf->mPCOptions.mResolutionY;
    if (isVideoOpts && (curResX != g_lastSeenProfResX || curResY != g_lastSeenProfResY) &&
        curResX >= 640 && curResY >= 480) {
      g_preferredWidth = (int)curResX;
      g_preferredHeight = (int)curResY;
      GetWgpuContext().preferredWidth = (uint32_t)curResX;
      GetWgpuContext().preferredHeight = (uint32_t)curResY;
      g_lastEnforcedMissionW = 0;
      g_lastEnforcedMissionH = 0;
      pProf->mPCOptions.mWindowed = true;
      pProf->UpdateProfCRC();
      g_lastSyncedProfile = pProf;
      g_applyPreferredToProfile = false;
    } else if (pProf != g_lastSyncedProfile || g_applyPreferredToProfile ||
               !pProf->mInitialized || (g_prevScreenWasLoginOrSave && !isLoginOrSave)) {
      if (curResX != (unsigned short)g_preferredWidth ||
          curResY != (unsigned short)g_preferredHeight || !pProf->mPCOptions.mWindowed) {
        pProf->mPCOptions.mResolutionX = (unsigned short)g_preferredWidth;
        pProf->mPCOptions.mResolutionY = (unsigned short)g_preferredHeight;
        pProf->mPCOptions.mWindowed = true;
        pProf->UpdateProfCRC();
      }
      g_lastSyncedProfile = pProf;
      g_applyPreferredToProfile = false;
    } else if (!pProf->mPCOptions.mWindowed) {
      pProf->mPCOptions.mWindowed = true;
      pProf->UpdateProfCRC();
    }
    g_lastSeenProfResX = pProf->mPCOptions.mResolutionX;
    g_lastSeenProfResY = pProf->mPCOptions.mResolutionY;
  }
  g_prevScreenWasLoginOrSave = isLoginOrSave;
  if (g_pendingNetEndShell && GameState::GetState() == 0x11E1FC01u /* shell */) {
    g_pendingNetEndShell = false;
    NetSetup::EndShell();
    NetGame::EndShell(1);
    Log("mp: completed deferred NetSetup/NetGame EndShell after returning to shell");
  }
  if (g_mpBridgeActive && GameState::GetState() == 0x11E1FC01u /* shell */) {
    if (hostGame) {
      hostGame->Update();
    }
    if (findGame && !joinGame) {
      findGame->Update();
    }
    if (joinGame) {
      joinGame->Update();
      if (joinGame->IsHostPlaying()) {
        joinGame->Launch();
      }
      if (joinGame->IsLaunchReady()) {
        const char* curScreen =
            GuiManager::Instance() ? GuiManager::Instance()->GetCurrentScreen() : "";
        if (curScreen && strcmp(curScreen, "ifs_saveop") == 0) {
          // Wait for OPFS profile save operation to complete before leaving ShellState.
        } else {
          if (!PlayerProfile::GetCurProfile()) {
            Log("mp: warning — joining mission without signed-in profile (screen='%s')",
                curScreen ? curScreen : "");
          }
          delete joinGame;
          joinGame = 0;
          if (findGame) {
            findGame->Cancel();
            delete findGame;
            findGame = 0;
          }
          g_mpBridgeActive = false;
          if (GuiManager::Instance()) {
            GuiManager::Instance()->ChangeToScreen("", false, true);
          }
          GameState::SetState(0x8FF60339u /* mission */);
        }
      }
    }
  }
  bool alive = GameState::Update();
  if (g_loadBannerActive && (!LoadDisplay::sInstance || !LoadDisplay::sInstance->m_bDisplay)) {
    g_loadBannerActive = false;
    js_hide_load_banner();
  }
  // Guarantee every Mission actually runs at the player's selected resolution (g_preferredWidth x
  // g_preferredHeight), even if no profile was signed in yet during PreStateInit or a newly loaded
  // profile carried a stale 640x480 / 800x600 setting.
  if (GameState::GetState() == 0x8FF60339u /* mission */) {
    const unsigned int curW = RedInterfaceScreen::GetScreenWidth();
    const unsigned int curH = RedInterfaceScreen::GetScreenHeight();
    if ((curW != (unsigned int)g_preferredWidth || curH != (unsigned int)g_preferredHeight) &&
        !RedRenderer::pcApplyPending() &&
        (g_lastEnforcedMissionW != g_preferredWidth || g_lastEnforcedMissionH != g_preferredHeight)) {
      g_lastEnforcedMissionW = g_preferredWidth;
      g_lastEnforcedMissionH = g_preferredHeight;
      if (PlayerProfile::tProfile* pProf = PlayerProfile::GetCurProfile()) {
        pProf->mPCOptions.mResolutionX = (unsigned short)g_preferredWidth;
        pProf->mPCOptions.mResolutionY = (unsigned short)g_preferredHeight;
        pProf->mPCOptions.mWindowed = true;
        pProf->UpdateProfCRC();
      }
      RedRenderer::pcApplyDisplayMode((unsigned int)g_preferredWidth,
                                      (unsigned int)g_preferredHeight,
                                      DisplayLogic::kModeWindowed,
                                      (unsigned int)g_preferredWidth,
                                      (unsigned int)g_preferredHeight,
                                      false, D3DMULTISAMPLE_NONE, 0);
    }
  } else {
    g_lastEnforcedMissionW = 0;
    g_lastEnforcedMissionH = 0;
  }
  // Ensure the in-game cursor (FLRenderer::mCursorGroup / mCursorImage) is visible whenever a menu
  // or SpawnDisplay (soldier class / spawn point selection) is active, and hidden during live 3D
  // combat when PointerLock is wanted.
  {
    unsigned int st = GameState::GetState();
    const char* screenRaw =
        GuiManager::Instance() ? GuiManager::Instance()->GetCurrentScreen() : "";
    bool spawnVis = (st == 0x8FF60339u && SpawnDisplay::Instance(0) &&
                     SpawnDisplay::Instance(0)->mVisible);
    bool playerAlive = false;
    if (st == 0x8FF60339u) {
      int localIdx = NetGame::IsEnabled() ? NetComm::GetLocalIndex(0) : 0;
      if (localIdx < 0) localIdx = 0;
      if (Character* ch = Character::FindPlayer(localIdx)) {
        if (Controllable* ctrl = ch->GetControllable()) {
          playerAlive = true;
        }
      }
    }
    bool pointerLockWanted =
        (st == 0x8FF60339u) && playerAlive && !spawnVis && (!screenRaw || screenRaw[0] == '\0');
    DisplayCfg::gDisplay.bSoftwareCursor = true;
    FLRenderer::EnableCursor(!pointerLockWanted);
    FLRenderer::mCursorImage.Enable(!pointerLockWanted);
  }
  if (g_audioEnabled) {
    swbf2_audio_pump_and_mix(stepDt);
  }
  ++g_gameStateTicks;
  return alive ? 1 : 0;
}

int swbf2_gamestate_launch_mission(const char* scriptName) {
  if (!g_gameStateBooted || !scriptName || !scriptName[0]) return 0;
  if (g_pendingNetEndShell || NetGame::IsEnabled() || NetSetup::InMultiplayer()) {
    g_pendingNetEndShell = false;
    NetSetup::EndShell();
    NetGame::EndShell(1);
  }
  MissionPlayList::ClearEntries();
  GameLoop::SetNextMission(scriptName);
  GameState::SetState(0x8FF60339u /* "mission" */);
  return 1;
}

int swbf2_gamestate_return_to_shell(void) {
  if (!g_gameStateBooted) return 0;
  GameState::SetState(0x11E1FC01u /* "shell" */);
  return 1;
}

int swbf2_gamestate_lua_exec(const char* code) {
  if (!code || !LuaHelper::mState) return -1;
  return lua_dostring(LuaHelper::mState, code);
}

const char* swbf2_gamestate_status(void) {
  auto& ctx = GetWgpuContext();
  unsigned int st = GameState::GetState();
  const char* stateName = "none";
  if (st == 0x11E1FC01u) stateName = "shell";
  else if (st == 0x8FF60339u) stateName = "mission";

  const char* screenRaw =
      GuiManager::Instance() ? GuiManager::Instance()->GetCurrentScreen() : "";
  char screenEsc[128];
  jsonEscape(screenRaw, screenEsc, sizeof(screenEsc));

  char missionEsc[128];
  jsonEscape(GameLoop::mMissionScript, missionEsc, sizeof(missionEsc));

  bool movieOpen = GameMovie::IsOpen();
  bool moviePlaying = (GameMovie::sRedMovie && GameMovie::sRedMovie->IsOpen() &&
                       GameMovie::sRedMovie->IsPlaying());
  unsigned long movieFrame = 0;
  unsigned long movieFrames = 0;
  unsigned long movieW = 0;
  unsigned long movieH = 0;
  if (GameMovie::sRedMovie && GameMovie::sRedMovie->mBink) {
    movieFrame = GameMovie::sRedMovie->mBink->FrameNum;
    movieFrames = GameMovie::sRedMovie->mBink->Frames;
    movieW = GameMovie::sRedMovie->mBink->Width;
    movieH = GameMovie::sRedMovie->mBink->Height;
  }

  int spawnVisible = 0;
  int spawnMode = -1;
  int playerAlive = 0;
  float playerX = 0.0f, playerY = 0.0f, playerZ = 0.0f;
  float playerFwdX = 0.0f, playerFwdZ = 0.0f;
  float playerClip = -1.0f;
  float playerLastFireTime = -1.0f;
  int remoteAlive = 0;
  float remoteX = 0.0f, remoteY = 0.0f, remoteZ = 0.0f;
  int spawnAcceptX = -1;
  int spawnAcceptY = -1;
  int sideTeam1X = -1;
  int sideTeam1Y = -1;
  int sideAutoX = -1;
  int sideAutoY = -1;
  if (st == 0x8FF60339u) {
    int localPlayerIdx = NetGame::IsEnabled() ? NetComm::GetLocalIndex(0) : 0;
    if (localPlayerIdx < 0) localPlayerIdx = 0;
    if (Character* ch = Character::FindPlayer(localPlayerIdx)) {
      if (Controllable* ctrl = ch->GetControllable()) {
        playerAlive = 1;
        const PblMatrix& m = ctrl->GetMatrix();
        const PblVector3& pos = m.GetTranslation();
        const PblVector3& fwd = m.GetZAxis();
        playerX = pos.x;
        playerY = pos.y;
        playerZ = pos.z;
        playerFwdX = fwd.x;
        playerFwdZ = fwd.z;
        int wIdx = ctrl->GetWeaponIndex(0);
        if (Weapon* w = (wIdx >= 0) ? ctrl->GetWeaponPtr(wIdx) : nullptr) {
          playerLastFireTime = w->mLastFireTime;
          if (w->m_pAmmoCounter) {
            playerClip = w->m_pAmmoCounter->mCurClip;
          }
        }
      }
    }
    if (NetGame::IsEnabled()) {
      for (int p = 0; p < 16; ++p) {
        if (p == localPlayerIdx) continue;
        if (Character* rch = Character::FindPlayer(p)) {
          if (Controllable* rctrl = rch->GetControllable()) {
            remoteAlive = 1;
            const PblVector3& rpos = rctrl->GetMatrix().GetTranslation();
            remoteX = rpos.x;
            remoteY = rpos.y;
            remoteZ = rpos.z;
            break;
          }
        }
      }
    }
    int sw = (int)RedInterfaceScreen::GetScreenWidth();
    int sh = (int)RedInterfaceScreen::GetScreenHeight();
    auto findHotspot = [&](RedInterfaceElement* el, int& ox, int& oy) {
      if (!el) return;
      for (int y = 16; y < sh && ox < 0; y += 12) {
        for (int x = 16; x < sw; x += 12) {
          if (el->TestHotSpot((float)x, (float)y)) {
            ox = x;
            oy = y;
            return;
          }
        }
      }
    };
    if (SpawnDisplay* sd = SpawnDisplay::Instance(0)) {
      spawnVisible = sd->mVisible ? 1 : 0;
      spawnMode = (int)sd->mMode;
      if (sd->mVisible && sd->m_AcceptBox) {
        findHotspot(sd->m_AcceptBox, spawnAcceptX, spawnAcceptY);
      }
      if (sd->mVisible && sd->mMode == SpawnDisplay::eSIDESELECT) {
        const char* kTeam1Names[] = {
            "ifs_sideselect1.buttons.team1.label",
            "ifs_sideselect1.buttons.team1",
            "ifs_sideselect1.buttons.team1.skin",
        };
        for (const char* name : kTeam1Names) {
          findHotspot(RedInterfaceElement::Find(name), sideTeam1X, sideTeam1Y);
          if (sideTeam1X >= 0) break;
        }
        const char* kAutoNames[] = {
            "ifs_sideselect1.buttons.auto.label",
            "ifs_sideselect1.buttons.auto",
            "ifs_sideselect1.buttons.autoassign.label",
            "ifs_sideselect1.buttons.autoassign",
        };
        for (const char* name : kAutoNames) {
          findHotspot(RedInterfaceElement::Find(name), sideAutoX, sideAutoY);
          if (sideAutoX >= 0) break;
        }
        if (sideAutoX < 0 && sideTeam1X >= 0) {
          // On multiplayer ifs_sideselect1, Auto Assign sits two button rows below team1.
          sideAutoX = sideTeam1X;
          sideAutoY = sideTeam1Y + (sh * 52) / 480;
        }
      }
    }
  }

  const bool pointerLockWanted =
      (st == 0x8FF60339u) && playerAlive && !spawnVisible && (!screenRaw || screenRaw[0] == '\0');
  const unsigned int activeScreenW = RedInterfaceScreen::GetScreenWidth()
                                         ? RedInterfaceScreen::GetScreenWidth()
                                         : ctx.width;
  const unsigned int activeScreenH = RedInterfaceScreen::GetScreenHeight()
                                         ? RedInterfaceScreen::GetScreenHeight()
                                         : ctx.height;

  snprintf(g_gsStatusJson, sizeof(g_gsStatusJson),
           "{"
           "\"booted\":%s,"
           "\"ticks\":%u,"
           "\"stateHash\":\"0x%08x\","
           "\"state\":\"%s\","
           "\"currentScreen\":\"%s\","
           "\"profileSignedIn\":%s,"
           "\"missionScript\":\"%s\","
           "\"screenWidth\":%u,"
           "\"screenHeight\":%u,"
           "\"preferredWidth\":%d,"
           "\"preferredHeight\":%d,"
           "\"spawnVisible\":%d,"
           "\"spawnMode\":%d,"
           "\"playerAlive\":%s,"
           "\"pointerLockWanted\":%s,"
           "\"playerX\":%.3f,"
           "\"playerY\":%.3f,"
           "\"playerZ\":%.3f,"
           "\"playerFwdX\":%.4f,"
           "\"playerFwdZ\":%.4f,"
           "\"playerClip\":%.1f,"
           "\"playerLastFireTime\":%.3f,"
           "\"remoteAlive\":%s,"
           "\"remoteX\":%.3f,"
           "\"remoteY\":%.3f,"
           "\"remoteZ\":%.3f,"
           "\"spawnAcceptX\":%d,"
           "\"spawnAcceptY\":%d,"
           "\"sideTeam1X\":%d,"
           "\"sideTeam1Y\":%d,"
           "\"sideAutoX\":%d,"
           "\"sideAutoY\":%d,"
           "\"pipeline\":%d,"
           "\"geometryMode\":%d,"
           "\"skinningMode\":%d,"
           "\"texturesLoaded\":%d,"
           "\"draws\":%d,"
           "\"triangles\":%d,"
           "\"skinnedDraws\":%d,"
           "\"skinnedTriangles\":%d,"
           "\"movieOpen\":%s,"
           "\"moviePlaying\":%s,"
           "\"movieFrame\":%lu,"
           "\"movieFrames\":%lu,"
           "\"movieWidth\":%lu,"
           "\"movieHeight\":%lu,"
           "\"wgpuErrors\":%u"
           "}",
           g_gameStateBooted ? "true" : "false",
           g_gameStateTicks,
           st,
           stateName,
           screenEsc,
           PlayerProfile::GetCurProfile() ? "true" : "false",
           missionEsc,
           activeScreenW,
           activeScreenH,
           g_preferredWidth,
           g_preferredHeight,
           spawnVisible,
           spawnMode,
           playerAlive ? "true" : "false",
           pointerLockWanted ? "true" : "false",
           playerX,
           playerY,
           playerZ,
           playerFwdX,
           playerFwdZ,
           playerClip,
           playerLastFireTime,
           remoteAlive ? "true" : "false",
           remoteX,
           remoteY,
           remoteZ,
           spawnAcceptX,
           spawnAcceptY,
           sideTeam1X,
           sideTeam1Y,
           sideAutoX,
           sideAutoY,
           (int)RedRenderer::pcGetRenderCaps().pipeline,
           (int)RedRenderer::pcGetRenderCaps().geometry,
           (int)RedRenderer::pcGetRenderCaps().skinning,
           RedTexture::s_textureHashTable.GetNumEntries(),
           swbf2_wgpu_draw_calls(),
           swbf2_wgpu_triangles(),
           swbf2_wgpu_skinned_draws(),
           swbf2_wgpu_skinned_triangles(),
           movieOpen ? "true" : "false",
           moviePlaying ? "true" : "false",
           movieFrame,
           movieFrames,
           movieW,
           movieH,
           ctx.errorCount);
  return g_gsStatusJson;
}

// Where did the keystroke go?
//
// The path a key takes is: IDirectInputDevice8A::GetDeviceState -> RedKeyboard::Update (rising
// edge) -> RedInputEventHandler::PostKeyboardEvent -> KeyGrabber::mStroke ->
// GuiManager::CollectInputs (GetStroke() drains and clears it) -> CreateKeyInputEvent ->
// GuiManager::mEventQueue -> the Lua screen. Sampling sKeygrabber.mStroke tells us which half is
// dead: a stroke that is still sitting there after several ticks was never drained, so
// CollectInputs never ran for this controller; a stroke that is gone while the screen does not
// change died somewhere past the queue.
const char* swbf2_gamestate_gui_probe(void) {
  static char s_probe[640];
  GuiManager* gui = GuiManager::Instance();
  const KeyStroke& stroke = sKeygrabber.mStroke;
  RedKeyboard* kb = RedInputManager::GetKeyboard(0);
  RawControllerInputs* pad =
      FLInputManager::Instance() ? FLInputManager::Instance()->GetPlayerJoystick(0) : nullptr;
  GUIInputs* gi =
      FLInputManager::Instance() ? FLInputManager::Instance()->GetPlayerGuiInputs(0) : nullptr;

  unsigned long diCreations = 0, diDevices = 0, diKeyboardStates = 0, diDownBytes = 0;
  swbf2_input_seam_counters(&diCreations, &diDevices, &diKeyboardStates, &diDownBytes);

  IDirectInput8A* di = RedInputManager::pcGetDIInterface();
  RedMouse* rm = RedInputManager::GetMouse(0);
  const int downEnter = (kb && kb->pc_DownKeys[0x1c]) ? 1 : 0;
  const float guiAccept = gi ? gi->mProcessedGUIInputs[eGUIINPUT_Accept] : -1.0f;

  snprintf(s_probe, sizeof(s_probe),
           "{\"guiScreen\":\"%s\","
           "\"strokeKey\":%d,\"strokeAscii\":%d,\"grabberInited\":%d,"
           "\"diInterface\":%d,\"kbConnected\":%d,\"oneTimeInit\":%d,"
           "\"downEnter\":%d,\"guiAccept\":%.2f,"
           "\"rmX\":%d,\"rmY\":%d,\"rmBtn\":%u,\"guiMX\":%d,\"guiMY\":%d,\"giMX\":%d,\"giMY\":%d,"
           "\"netEn\":%d,\"netCli\":%d,\"mEq\":%d,\"giEq\":%d,\"rawB0\":%.1f,\"gJoy\":%d,\"team\":%d,"
           "\"diCreations\":%lu,\"diDevices\":%lu,\"diKeyboardStates\":%lu,\"diDownBytes\":%lu,"
           "\"pad\":%d,\"ticks\":%u}",
           gui ? gui->GetCurrentScreen() : "",
           (int)stroke.key,
           (int)stroke.ascii,
           sKeygrabber.mInited ? 1 : 0,
           di ? 1 : 0,
           (kb && kb->m_connected) ? 1 : 0,
           sbDidOneTimeInit ? 1 : 0,
           downEnter,
           guiAccept,
           rm ? rm->m_X : -1,
           rm ? rm->m_Y : -1,
           rm ? rm->m_uiDownButtons : 0u,
           GuiManager::mMouseX,
           GuiManager::mMouseY,
           gi ? gi->mMouseCoords.mPosX : -1,
           gi ? gi->mMouseCoords.mPosY : -1,
           NetGame::IsEnabled() ? 1 : 0,
           netIsClient ? 1 : 0,
           (pad && pad->_redMouse == rm) ? 1 : 0,
           (pad && pad->m_pGuiInputs == gi) ? 1 : 0,
           pad ? pad->mRawInputFloat[0][66] : -1.0f,
           gui ? gui->mJoystickIndex : -1,
           SpawnDisplay::Instance(0) ? SpawnDisplay::Instance(0)->mTeamNumber : -1,
           diCreations,
           diDevices,
           diKeyboardStates,
           diDownBytes,
           pad ? 1 : 0,
           g_gameStateTicks);
  return s_probe;
}

// --- Phase 6 Native LAN Multiplayer Bridge (over WebRTC/WebSocket virtual LAN) ----------------

static int parseKvInt(const char* kv, const char* key, int dflt) {
  if (!kv || !key || !key[0]) return dflt;
  size_t klen = strlen(key);
  const char* p = kv;
  while (*p) {
    while (*p == '\n' || *p == '\r' || *p == ';' || *p == ' ' || *p == '&') ++p;
    if (!*p) break;
    if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
      return atoi(p + klen + 1);
    }
    while (*p && *p != '\n' && *p != '\r' && *p != ';' && *p != '&') ++p;
  }
  return dflt;
}

static void parseKvStr(const char* kv, const char* key, char* out, size_t outCap) {
  if (!out || outCap == 0) return;
  out[0] = '\0';
  if (!kv || !key || !key[0]) return;
  size_t klen = strlen(key);
  const char* p = kv;
  while (*p) {
    while (*p == '\n' || *p == '\r' || *p == ';' || *p == ' ' || *p == '&') ++p;
    if (!*p) break;
    if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
      const char* val = p + klen + 1;
      size_t n = 0;
      while (val[n] && val[n] != '\n' && val[n] != '\r' && val[n] != ';' && val[n] != '&' &&
             n + 1 < outCap) {
        out[n] = val[n];
        ++n;
      }
      out[n] = '\0';
      return;
    }
    while (*p && *p != '\n' && *p != '\r' && *p != ';' && *p != '&') ++p;
  }
}

int swbf2_net_set_password(const char* password) {
  NetSetup::SetPassword(password ? password : "");
  return 1;
}

int swbf2_net_disconnect(void) {
  if (!g_gameStateBooted) return 0;
  if (hostGame) {
    hostGame->Cancel();
    delete hostGame;
    hostGame = 0;
  }
  if (joinGame) {
    joinGame->Cancel();
    delete joinGame;
    joinGame = 0;
  }
  if (findGame) {
    findGame->Cancel();
    delete findGame;
    findGame = 0;
  }
  g_mpBridgeActive = false;
  NetSetup::SetPassword("");
  MissionPlayList::ClearEntries();
  NetSetup::SetInNetGame(false);
  if (GameState::GetState() == 0x8FF60339u /* mission */) {
    g_pendingNetEndShell = true;
    GameState::SetState(0x11E1FC01u /* shell */);
  } else {
    g_pendingNetEndShell = false;
    NetSetup::EndShell();
    NetGame::EndShell(1);
  }
  Log("mp: disconnected and cleaned up LAN session");
  return 1;
}

int swbf2_net_host_lan_config(const char* roomName, const char* playlistCsv,
                              const char* playerName, const char* configKv) {
  if (!g_gameStateBooted) return 0;
  if (hostGame) {
    hostGame->Cancel();
    delete hostGame;
    hostGame = 0;
  }
  if (joinGame) {
    joinGame->Cancel();
    delete joinGame;
    joinGame = 0;
  }
  if (findGame) {
    findGame->Cancel();
    delete findGame;
    findGame = 0;
  }

  const char* rName = (roomName && roomName[0]) ? roomName : "SWBF2-Web";
  const char* pName = (playerName && playerName[0]) ? playerName : "Host";
  const char* kv = configKv ? configKv : "";

  int maxPlayers = parseKvInt(kv, "maxPlayers", 16);
  if (maxPlayers < 2) maxPlayers = 2;
  if (maxPlayers > 64) maxPlayers = 64;

  int numBots = parseKvInt(kv, "numBots", 16);
  if (numBots < 0) numBots = 0;
  if (numBots > 32) numBots = 32;

  int difficulty = parseKvInt(kv, "difficulty", 2);
  if (difficulty < eDIFFICULTY_EASY) difficulty = eDIFFICULTY_EASY;
  if (difficulty > eDIFFICULTY_HARD) difficulty = eDIFFICULTY_HARD;

  int reinfMult = parseKvInt(kv, "reinfMult", 100);
  if (reinfMult < 25) reinfMult = 25;
  if (reinfMult > 500) reinfMult = 500;

  bool teamDamage = parseKvInt(kv, "teamDamage", 0) != 0;
  bool autoAssign = parseKvInt(kv, "autoAssign", 0) != 0;
  bool showNames = parseKvInt(kv, "showNames", 1) != 0;
  bool autoAim = parseKvInt(kv, "autoAim", 0) != 0;
  int spawnInvinc = parseKvInt(kv, "spawnInvinc", 2);
  if (spawnInvinc < 0) spawnInvinc = 0;
  if (spawnInvinc > 15) spawnInvinc = 15;
  int warmUp = parseKvInt(kv, "warmUp", 0);
  if (warmUp < 0) warmUp = 0;
  if (warmUp > 60) warmUp = 60;
  bool awardsEnabled = parseKvInt(kv, "awards", 1) != 0;

  bool heroesEnabled = parseKvInt(kv, "heroes", 1) != 0;
  int heroUnlock = parseKvInt(kv, "heroUnlock", 5);       // 3 = time, 5 = points, 7 = scripted/instant
  int heroUnlockVal = parseKvInt(kv, "heroUnlockVal", 10);
  int heroPlayer = parseKvInt(kv, "heroPlayer", 1);       // 1=mostpoints, 2=mostkills, 4=leastpoints, 7=random, 8=heroslayer
  int heroTeam = parseKvInt(kv, "heroTeam", 3);           // 1=one, 3=both, 4=cycle
  int heroRespawnVal = parseKvInt(kv, "heroRespawn", 60); // -1 = never, >=0 = seconds

  int conTime = parseKvInt(kv, "conTime", 0);
  int ctfScore = parseKvInt(kv, "ctfScore", 5);
  int ctfTime = parseKvInt(kv, "ctfTime", 0);
  int assScore = parseKvInt(kv, "assScore", 180);
  int huntScore = parseKvInt(kv, "huntScore", 50);
  int huntTime = parseKvInt(kv, "huntTime", 5);
  bool randomizePlaylist = parseKvInt(kv, "randomize", 0) != 0;

  char password[128];
  parseKvStr(kv, "password", password, sizeof(password));

  NetComm::SetConnectType(CT_LAN);
  NetSetup::EnableOnlineService(false);
  NetSetup::SetLoginName(0, pName);
  NetGame::BeginShell(1);
  NetSetup::SetAsHost();
  NetSetup::SetAsDedicated(false);
  NetSetup::SetGameName(rName);
  NetSetup::SetPassword(password);

  netPlayerLimit = maxPlayers;
  gAutoNetPlayers = maxPlayers;
  netNumBots = numBots;
  gGlobalNumBots = numBots;
  for (int i = 0; i < 4; ++i) {
    Lua_Callbacks::gCONNumBots[i] = numBots;
    Lua_Callbacks::gCTFNumBots[i] = numBots;
    Lua_Callbacks::gELINumBots[i] = numBots;
    Lua_Callbacks::gASSNumBots[i] = numBots;
  }
  for (int i = 0; i < 5; ++i) {
    Lua_Callbacks::gCONReinforcementMult[i] = reinfMult;
    Lua_Callbacks::gDMReinforcementMult[i] = reinfMult;
    Lua_Callbacks::gCONMaxTimeLimit[i] = conTime;
    Lua_Callbacks::gCTFScore[i] = ctfScore;
    Lua_Callbacks::gCTFMaxTimeLimit[i] = ctfTime;
    Lua_Callbacks::gSpaceAssaultScoreLimit[i] = assScore;
    Lua_Callbacks::gHuntScoreLimit[i] = huntScore;
    Lua_Callbacks::gHuntMaxTimeLimit[i] = huntTime;
  }
  Lua_Callbacks::gRandomizePlaylist = randomizePlaylist;
  Lua_Callbacks::gTeamSetReinforcementMult = 0;

  netDifficulty = difficulty;
  if (PlayerProfile::tProfile* pProf = PlayerProfile::GetCurProfile()) {
    pProf->mGameOptions.mCurDiffLevel = (eGAME_DIFFICULTY)difficulty;
    pProf->mGameOptions.mbHerosEnabled = heroesEnabled;
  }
  netTeamDamage = teamDamage;
  netAutoAssignTeams = autoAssign;
  netAutoAssignTeamsOriginalSetting = autoAssign;
  GameLoop::SetCanSwitchSides(!autoAssign);
  netShowNames = showNames;
  netAutoAim = autoAim;
  gSpawnInvicibility = spawnInvinc;
  NetGame::SetMaxPreGameTime(warmUp);
  SetNetAwardsEnabled(awardsEnabled ? 1 : 0);

  netHeroesEnabled = heroesEnabled;
  netHeroRuleUnlock = heroUnlock;
  netHeroRuleUnlockVal = heroUnlockVal;
  netHeroRulePlayer = heroPlayer;
  netHeroRuleTeam = heroTeam;
  netHeroRuleRespawn = (heroRespawnVal >= 0) ? 2 : 1;
  netHeroRuleRespawnVal = heroRespawnVal;

  MissionPlayList::ClearEntries();
  int baseTickets = (150 * reinfMult) / 100;
  if (baseTickets < 50) baseTickets = 50;
  char firstMission[64] = "tat2g_con";
  int addedMaps = 0;
  if (playlistCsv && playlistCsv[0]) {
    const char* p = playlistCsv;
    while (*p) {
      while (*p == ',' || *p == ' ' || *p == ';') ++p;
      if (!*p) break;
      char mBuf[64];
      size_t n = 0;
      while (p[n] && p[n] != ',' && p[n] != ';' && p[n] != ' ' && n + 1 < sizeof(mBuf)) {
        mBuf[n] = p[n];
        ++n;
      }
      mBuf[n] = '\0';
      if (n > 0) {
        if (addedMaps == 0) {
          snprintf(firstMission, sizeof(firstMission), "%s", mBuf);
        }
        MissionPlayList::AddEntry(mBuf, baseTickets, baseTickets);
        ++addedMaps;
      }
      p += n;
    }
  }
  if (addedMaps == 0) {
    MissionPlayList::AddEntry("tat2g_con", baseTickets, baseTickets);
    addedMaps = 1;
  }
  if (randomizePlaylist && addedMaps > 1) {
    MissionPlayList::RandomizeOrder();
    const char* nextLua = nullptr;
    int t1 = 0, t2 = 0;
    MissionPlayList::GetEntry(0, nextLua, t1, t2);
    if (nextLua && nextLua[0]) {
      snprintf(firstMission, sizeof(firstMission), "%s", nextLua);
    }
  }
  MissionPlayList::SetLoopForever(true);
  GameLoop::SetNextMission(firstMission);

  hostGame = NetSetup::HostGame();
  g_mpBridgeActive = (hostGame != 0);
  Log("mp: host LAN started room='%s' firstMission='%s' maps=%d player='%s' maxP=%d bots=%d diff=%d reinf=%d%% heroes=%d ff=%d directPort=%d",
      rName, firstMission, addedMaps, pName, maxPlayers, numBots, difficulty, reinfMult,
      heroesEnabled ? 1 : 0, teamDamage ? 1 : 0, NetComm::GetDirectPort());
  return hostGame ? 1 : 0;
}

int swbf2_net_host_lan(const char* roomName, const char* missionScript, const char* playerName) {
  return swbf2_net_host_lan_config(roomName, missionScript, playerName, "numBots=8");
}

int swbf2_net_find_lan(const char* playerName) {
  if (!g_gameStateBooted) return 0;
  if (joinGame) {
    joinGame->Cancel();
    delete joinGame;
    joinGame = 0;
  }
  if (findGame) {
    findGame->Cancel();
    delete findGame;
    findGame = 0;
  }

  const char* pName = (playerName && playerName[0]) ? playerName : "Client";
  NetComm::SetConnectType(CT_LAN);
  NetSetup::EnableOnlineService(false);
  NetSetup::SetLoginName(0, pName);
  NetGame::BeginShell(1);
  NetSetup::SetAsClient();

  findGame = NetSetup::FindGame(FT_NORMAL, 0, 0);
  g_mpBridgeActive = (findGame != 0);
  Log("mp: find LAN started player='%s' directPort=%d", pName, NetComm::GetDirectPort());
  return findGame ? 1 : 0;
}

int swbf2_net_join_found(int index) {
  if (!g_gameStateBooted || !findGame) return 0;
  int count = findGame->GetGameCount();
  if (count <= 0) return 0;
  if (index < 0 || index >= count) index = 0;
  findGame->SelectFirst();
  if (index > 0) findGame->Select(index);
  if (joinGame) {
    joinGame->Cancel();
    delete joinGame;
    joinGame = 0;
  }
  joinGame = NetSetup::JoinGame(findGame);
  g_mpBridgeActive = (joinGame != 0);
  Log("mp: join LAN index=%d/%d started", index, count);
  return joinGame ? 1 : 0;
}

int swbf2_net_launch_match(void) {
  if (!g_gameStateBooted) return 0;
  if (hostGame && hostGame->IsLaunchReady()) {
    const char* curScreen =
        GuiManager::Instance() ? GuiManager::Instance()->GetCurrentScreen() : "";
    if (curScreen && strcmp(curScreen, "ifs_saveop") == 0) {
      Log("mp: deferring host launch while ifs_saveop is active");
      return 0;
    }
    if (!PlayerProfile::GetCurProfile()) {
      Log("mp: warning — host launching mission without signed-in profile (screen='%s')",
          curScreen ? curScreen : "");
    }
    delete hostGame;
    hostGame = 0;
    g_mpBridgeActive = false;
    if (GuiManager::Instance()) {
      GuiManager::Instance()->ChangeToScreen("", false, true);
    }
    GameState::SetState(0x8FF60339u /* mission */);
    Log("mp: host launching multiplayer mission");
    return 1;
  }
  if (joinGame) {
    joinGame->Launch();
    return 1;
  }
  return 0;
}

const char* swbf2_net_status(void) {
  unsigned long txPkts = 0, txBytes = 0, rxPkts = 0, rxBytes = 0;
  unsigned int localIp = 0;
  swbf2_net_counters(&txPkts, &txBytes, &rxPkts, &rxBytes, &localIp);
  char ipStr[32];
  snprintf(ipStr, sizeof(ipStr), "%u.%u.%u.%u",
           localIp & 0xffu, (localIp >> 8) & 0xffu,
           (localIp >> 16) & 0xffu, (localIp >> 24) & 0xffu);

  int connectedPlayers = 0;
  for (int i = 0; i < 64; ++i) {
    if (NetComm::IsConnected(i)) ++connectedPlayers;
  }
  const char* gameErr = nullptr;
  int gameErrLvl = 0;
  NetSetup::GetGameError(gameErr, gameErrLvl);
  char errEsc[128];
  jsonEscape(gameErr ? gameErr : "", errEsc, sizeof(errEsc));

  char foundNameEsc[128] = {};
  int findCount = findGame ? findGame->GetGameCount() : 0;
  if (findGame && findCount > 0) {
    findGame->SelectFirst();
    const char* n = findGame->GetName();
    jsonEscape(n ? n : "", foundNameEsc, sizeof(foundNameEsc));
  }

  snprintf(g_netStatusJson, sizeof(g_netStatusJson),
           "{"
           "\"localIp\":\"%s\","
           "\"isLAN\":%s,"
           "\"isEnabled\":%s,"
           "\"isHost\":%s,"
           "\"isClient\":%s,"
           "\"inShell\":%s,"
           "\"directPort\":%d,"
           "\"connectedPlayers\":%d,"
           "\"localIdx\":%d,"
           "\"clientTurn\":%u,"
           "\"ackedTurn\":%u,"
           "\"readyToPlay\":%s,"
           "\"isPreGame\":%s,"
           "\"preGameTime\":%d,"
           "\"hostKnowsILoaded\":%s,"
           "\"findGameCount\":%d,"
           "\"foundName\":\"%s\","
           "\"hostActive\":%s,"
           "\"hostPlayerCount\":%d,"
           "\"hostLaunchReady\":%s,"
           "\"joinActive\":%s,"
           "\"joinConnected\":%s,"
           "\"joinLaunchReady\":%s,"
           "\"joinHostPlaying\":%s,"
           "\"joinFailed\":%s,"
           "\"txPackets\":%lu,"
           "\"txBytes\":%lu,"
           "\"rxPackets\":%lu,"
           "\"rxBytes\":%lu,"
           "\"gameError\":\"%s\""
           "}",
           ipStr,
           NetComm::IsLAN() ? "true" : "false",
           NetGame::IsEnabled() ? "true" : "false",
           NetGame::IsHost() ? "true" : "false",
           NetGame::IsClient() ? "true" : "false",
           NetGame::InShell() ? "true" : "false",
           NetComm::GetDirectPort(),
           connectedPlayers,
           NetComm::GetLocalIndex(0),
           NetGame::GetClientTurnNumber(),
           NetGame::GetClientAckedTurnNumber(),
           NetGame::IsReadyToPlay() ? "true" : "false",
           NetGame::IsPreGame() ? "true" : "false",
           NetGame::GetPreGameTime(),
           NetGame::DoesHostKnowILoaded() ? "true" : "false",
           findCount,
           foundNameEsc,
           hostGame ? "true" : "false",
           hostGame ? hostGame->GetPlayerCount() : 0,
           (hostGame && hostGame->IsLaunchReady()) ? "true" : "false",
           joinGame ? "true" : "false",
           (joinGame && joinGame->IsConnected()) ? "true" : "false",
           (joinGame && joinGame->IsLaunchReady()) ? "true" : "false",
           (joinGame && joinGame->IsHostPlaying()) ? "true" : "false",
           (joinGame && joinGame->IsFailed()) ? "true" : "false",
           txPkts,
           txBytes,
           rxPkts,
           rxBytes,
           errEsc);
  return g_netStatusJson;
}


}  // extern "C"
