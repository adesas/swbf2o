// Phase 2 Real Engine Renderer Bridge (RedRenderer + FLRenderer -> WebGPU FFP).
//
// Boots the real retail SWBF2 renderer (pcRedRenderer.cpp + pcFLRender.cpp +
// RedTexture + RedFont + RedTerrain + SkyManager + RedSceneManager +
// pcSoftwareSkinnedShaderSegment) against our WebGPU IDirect3D9/IDirect3DDevice9
// backend, streams Tatooine (tat/tat2.lvl) + Republic Rifleman (side/rep.lvl)
// on-demand through the Phase 1 OPFS VFS, and renders frames through FLRenderer::Render().

#include "d3d9_wgpu_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "Battlefront2/Platform/Pc/pcSpawnDisplay.h"
#include "Battlefront2/Source/AIUtil.h"
#include "Battlefront2/Source/BoundaryManager.h"
#include "Battlefront2/Source/CameraManager.h"
#include "Battlefront2/Source/Character.h"
#include "Battlefront2/Source/Chunk.h"
#include "Battlefront2/Source/CollisionManager.h"
#include "Battlefront2/Source/CollisionMesh.h"
#include "Battlefront2/Source/CommandPost.h"
#include "Battlefront2/Source/CommandPostManager.h"
#include "Battlefront2/Source/ConnectivityGraph.h"
#include "Battlefront2/Source/ControllerManager.h"
#include "Battlefront2/Source/DamageRegion.h"
#include "Battlefront2/Source/Entity.h"
#include "Battlefront2/Source/EntitySoldier.h"
#include "Battlefront2/Source/EntityTerrain.h"
#include "Battlefront2/Source/EventManager.h"
#include "Battlefront2/Source/GameLoop.h"
#include "Battlefront2/Source/GameMemory.h"
#include "Battlefront2/Source/GameState.h"
#include "Battlefront2/Source/HUDEventQueue.h"
#include "Battlefront2/Source/HUDManager.h"
#include "Battlefront2/Source/LoadUtil.h"
#include "Battlefront2/Source/LockOnManager.h"
#include "Battlefront2/Source/LuaHelper.h"
#include "Battlefront2/Source/MessageDisplay.h"
#include "Battlefront2/Source/NetGame.h"
#include "Battlefront2/Source/ParticleSystem.h"
#include "Battlefront2/Source/PropGenerator.h"
#include "Battlefront2/Source/QOSDisplay.h"
#include "Battlefront2/Source/SkyManager.h"
#include "Battlefront2/Source/SoldierAnimationBank.h"
#include "Battlefront2/Source/SoldierAnimatorClass.h"
#include "Battlefront2/Source/SoldierAnimatorLowRes.h"
#include "Battlefront2/Source/SpawnManager.h"
#include "Battlefront2/Source/StringDB.h"
#include "Battlefront2/Source/TargetManager.h"
#include "Battlefront2/Source/Team.h"
#include "Battlefront2/Source/Thread.h"
#include "Battlefront2/Source/ToolTips.h"
#include "Battlefront2/Source/TriggerRegionManager.h"
#include "Battlefront2/Source/VoiceOverManager.h"
#include "Battlefront2/Source/WeaponMelee.h"
#include "PebbleFL/Common/PblTimeBase.h"
#include "RedEngineFL/Application/RedDropShadow.h"
#include "RedEngineFL/Application/RedTerrain.h"
#include "RedEngineFL/Application/pc/pcFLRender.h"
#include "RedEngineFL/Application/pc/pcRedParticle.h"
#include "RedEngineFL/Graphics/PC/pcRedRenderer.h"
#include "RedEngineFL/Graphics/PC/pcRedTexture.h"
#include "RedEngineFL/Graphics/RedCamera.h"
#include "RedEngineFL/Graphics/RedLight.h"
#include "RedEngineFL/Interface/RedFont.h"
#include "RedEngineFL/Interface/RedGlyphCacheElement.h"
#include "RedEngineFL/Interface/RedInterfaceScreen.h"
#include "RedEngineFL/Interface/RedTextElement.h"
#include "RedEngineFL/Memory/RedMemory.h"
#include "RedEngineFL/Memory/RedMemoryPool.h"
#include "RedEngineFL/Portals/BaseDisplayable.h"
#include "RedEngineFL/Util/RedSceneManager.h"

extern int WORLD_EXTENTS;
extern bool g_bNoSound;
extern bool g_bNoRender;
extern bool pcForceFixedFunction;
extern int pcWidth;
extern int pcHeight;
extern void RedWarningBrowserInit(void(__cdecl* pCallback)(char const*, ...));
extern void Log(const char* fmt, ...);

// Optional subsystem Init declarations that live in engine TUs without dedicated headers.
namespace RedVertexCache { void Init(); }
namespace RedFootprintManager { void Init(); }
namespace ClothData { void Init(); }

namespace {

bool g_renderInited = false;
bool g_tat2Loaded = false;
PblTimeBase g_renderTimeBase;
RedCamera g_renderCamera;
unsigned int g_viewHandle = (unsigned int)-1;
EntitySoldier* g_trooper = nullptr;
RedScreenGroupElement* g_hudGroup = nullptr;
RedTextElement* g_hudTitle = nullptr;
char g_statsJson[1024] = {};

void __cdecl BridgeEngineLog(char const* fmt, ...) {
  if (fmt && fmt[0]) {
    Log("wgpu-engine: %s", fmt);
  }
}

void setupMemoryPools() {
  static const struct { const char* name; unsigned int count; } kPools[] = {
    {"EntitySoldier", 600},
    {"EntityDroid", 16},
    {"MountedTurret", 50},
    {"PassengerSlot", 0},
    {"PowerupItem", 25},
    {"EntityMine", 40},
    {"EntityPortableTurret", 6},
    {"CommandFlyer", 0},
    {"Aimer", 100},
    {"TreeGridStack", 550},
    {"Weapon", 2400},
    {"AmmoCounter", 2400},
    {"EnergyBar", 2400},
    {"StickInfo", 60},
    {"Ordnance", 120},
    {"LightFlash", 16},
    {"EntityLight", 100},
    {"LightningBoltEffectObject", 16},
    {"ParticleEmitterObject", 192},
    {"ParticleEmitterInfoData", 1024},
    {"ParticleEmitter", 1024},
    {"ParticleTransformer::PositionTransformer", 1024},
    {"ParticleTransformer::SizeTransformer", 1024},
    {"ParticleTransformer::RotationTransformer", 0},
    {"ParticleTransformer::ColorTransformer", 1536},
    {"EntitySoundStatic", 112},
    {"EntitySoundStream", 30},
    {"SoundSpaceRegion", 30},
    {"PlayerController", 64},
    {"TeamAssist", 64},
    {"Obstacle", 768},
    {"SoldierAnimation", 512},
  };
  for (size_t i = 0; i < sizeof(kPools) / sizeof(kPools[0]); ++i) {
    if (MemoryPool* p = MemoryPool::Find(kPools[i].name)) {
      p->SetCount(kPools[i].count);
    }
  }
  if (MemoryPool* p = MemoryPool::Find("Weapon")) p->SetSize(0x1c0);
  if (MemoryPool* p = MemoryPool::Find("Ordnance")) p->SetSize(0x180);
}

void aimCamera(float ex, float ey, float ez, float tx, float ty, float tz) {
  // RedCamera looks down -Z in camera local space (see RedCamera.cpp:21: -point.z >= _fNear).
  PblVector3 zAxis(ex - tx, ey - ty, ez - tz);
  float zLen = sqrtf(zAxis.x * zAxis.x + zAxis.y * zAxis.y + zAxis.z * zAxis.z);
  if (zLen < 1e-4f) {
    zAxis = PblVector3(0.0f, 0.0f, 1.0f);
  } else {
    zAxis.x /= zLen; zAxis.y /= zLen; zAxis.z /= zLen;
  }
  PblVector3 worldUp(0.0f, 1.0f, 0.0f);
  // XAxis = normalize(cross(worldUp, zAxis))
  PblVector3 xAxis(
      worldUp.y * zAxis.z - worldUp.z * zAxis.y,
      worldUp.z * zAxis.x - worldUp.x * zAxis.z,
      worldUp.x * zAxis.y - worldUp.y * zAxis.x);
  float xLen = sqrtf(xAxis.x * xAxis.x + xAxis.y * xAxis.y + xAxis.z * xAxis.z);
  if (xLen < 1e-4f) {
    xAxis = PblVector3(1.0f, 0.0f, 0.0f);
  } else {
    xAxis.x /= xLen; xAxis.y /= xLen; xAxis.z /= xLen;
  }
  // YAxis = cross(zAxis, xAxis)
  PblVector3 yAxis(
      zAxis.y * xAxis.z - zAxis.z * xAxis.y,
      zAxis.z * xAxis.x - zAxis.x * xAxis.z,
      zAxis.x * xAxis.y - zAxis.y * xAxis.x);

  PblMatrix m = gMatrixIdentity;
  m.SetXAxis(xAxis.x, xAxis.y, xAxis.z, 0.0f);
  m.SetYAxis(yAxis.x, yAxis.y, yAxis.z, 0.0f);
  m.SetZAxis(zAxis.x, zAxis.y, zAxis.z, 0.0f);
  m.SetTranslation(ex, ey, ez, 1.0f);
  g_renderCamera.SetMatrix(m);
}

} // namespace

extern "C" {

int swbf2_engine_render_init(int width, int height) {
  if (!swbf2_wgpu_is_ready()) {
    Log("wgpu-engine: swbf2_wgpu_init must complete before swbf2_engine_render_init");
    return 0;
  }
  if (g_renderInited) {
    return 1;
  }

  if (width <= 0) width = 1280;
  if (height <= 0) height = 720;
  swbf2_wgpu_resize(width, height);

  g_bNoSound = true;
  g_bNoRender = false;
  pcForceFixedFunction = false; // Phase 3: PIPELINE_PROG1 + compressed geometry + hardware skinning
  pcWidth = width;
  pcHeight = height;

  RedWarningBrowserInit(&BridgeEngineLog);
  GameMemory::BuildHeaps(true);
  pvs::IManager::ReserveDisplayables();

  RedRenderer::InitStatic();
  RedRenderer::pcSetDisplayMode((unsigned int)width, (unsigned int)height,
                                DisplayLogic::kModeWindowed,
                                (unsigned int)width, (unsigned int)height,
                                false, D3DMULTISAMPLE_NONE, 0);
  RedRenderer::OpenDynamic();
  RedRenderer::SetTimeBase(&g_renderTimeBase);

  RedTexture::Init();
  RedRenderer::Init();
  RedInterfaceScreen::s_screenFull.SetDimensions((unsigned int)width, (unsigned int)height);
  RedFont::Init();
  RedGlyphCacheElement::InitPool(0x1770);

  RedRenderer::SetFogEnable(true);
  RedRenderer::SetSky(true);
  RedVertexCache::Init();
  RedParticleRenderer::Init();
  RedFootprintManager::Init();
  RedDropShadowManager::Init();
  ParticleSystem::Init();
  ClothData::Init();

  GameState::CreateBaseExplosionClasses();
  GameState::CreateBaseOrdnanceClasses();
  GameState::CreateBaseWeaponClasses();
  GameState::CreateBaseEntityClasses();
  WeaponMeleeClass::Init();

  NetGame::Create(true);

  const float fExtent = (float)WORLD_EXTENTS;
  CollisionManager::Create(PblVector3(-fExtent, 0.0f, -fExtent), PblVector3(fExtent, 0.0f, fExtent));
  HUD::Manager::Open();
  CameraManager::Create(1);
  LuaHelper::InitState();
  EventManager::Init();
  StringDB::Init();

  Team::CreateAll();
  SpawnManager::Create();
  SpawnDisplay::Create();
  HUD::EventQueue::Create();
  TargetManager::Init();
  MessageDisplay::Create();
  ToolTipsDisplay::Create();
  QOSDisplay::Create();
  VoiceOverManager::Create();
  BoundaryManager::Create();
  setupMemoryPools();
  ToolTipsDisplay::sInstance->Init();
  LockOnManager::CreateAll();
  new EntityTerrain;
  ChunkClusterManager::Create();
  TriggerRegionManager::Create();
  DamageRegion::Create();
  CommandPost::Create();
  ControllerManager::Init();
  AIUtil::Init();
  ConnectivityGraphManager::Init();
  CommandPostManager::Create();
  PropGenerator::Init();
  SkyManager::Init();

  FLRenderer::Init(false);

  // Set up perspective camera (RedCamera takes aspect = height / width).
  g_renderCamera.SetPerspective(0.7f, 1500.0f, 1.0471976f, (float)height / (float)width);
  aimCamera(0.0f, 8.0f, -12.0f, 0.0f, 2.0f, 10.0f);

  RedViewport fullVp = {0.0f, 0.0f, 1.0f, 1.0f};
  g_viewHandle = FLRenderer::AddView(fullVp, &g_renderCamera);

  g_renderInited = true;
  Log("wgpu-engine: RedRenderer initialized (%dx%d, pipeline=%d, geom=%d, skin=%d, bc=%d)",
      width, height,
      (int)RedRenderer::pcGetRenderCaps().pipeline,
      (int)RedRenderer::pcGetRenderCaps().geometry,
      (int)RedRenderer::pcGetRenderCaps().skinning,
      swbf2_wgpu_has_bc());
  return 1;
}

int swbf2_engine_render_load_tat2(void) {
  if (!g_renderInited) return 0;
  if (g_tat2Loaded) return 1;

  // 1. Load core.lvl (textures, fonts, localization, shaders).
  static const unsigned int kCoreIds[] = {
    CHUNK_ID('t', 'e', 'x', '_'),
    CHUNK_ID('f', 'o', 'n', 't'),
    CHUNK_ID('L', 'o', 'c', 'l'),
    CHUNK_ID('S', 'H', 'D', 'R'),
  };
  LoadUtil::ReadDataFileOnHeap("core.lvl", __RedTempHeap, kCoreIds,
                               (unsigned int)(sizeof(kCoreIds) / sizeof(kCoreIds[0])), 0, 0, 0);

  // 2. Load common.lvl & ingame.lvl shared assets & base infantry class.
  SoldierAnimationBank::Reset();
  static const unsigned int kSharedIds[] = {
    CHUNK_ID('l', 'v', 'l', '_'),
    CHUNK_ID('t', 'e', 'x', '_'),
    CHUNK_ID('m', 'o', 'd', 'l'),
    CHUNK_ID('g', 'm', 'o', 'd'),
    CHUNK_ID('s', 'k', 'e', 'l'),
    CHUNK_ID('z', 'a', 'f', '_'),
    CHUNK_ID('z', 'a', 'a', '_'),
    CHUNK_ID('s', 'a', 'n', 'm'),
    CHUNK_ID('e', 'n', 't', 'c'),
    CHUNK_ID('p', 'r', 'p', '_'),
    CHUNK_ID('w', 'p', 'n', 'c'),
    CHUNK_ID('o', 'r', 'd', 'c'),
    CHUNK_ID('e', 'x', 'p', 'c'),
    CHUNK_ID('c', 'o', 'm', 'b'),
    CHUNK_ID('c', 'o', 'l', 'l'),
    CHUNK_ID('p', 'r', 'i', 'm'),
  };
  LoadUtil::ReadDataFileOnHeap("common.lvl", __RedTempHeap, kSharedIds,
                               (unsigned int)(sizeof(kSharedIds) / sizeof(kSharedIds[0])), 0, 0, 0);

  unsigned int comInfHash = PblHash("com_inf_default");
  const char* comInfName = "com_inf_default";
  LoadUtil::ReadDataFileOnHeap("ingame.lvl", __RedTempHeap, kSharedIds,
                               (unsigned int)(sizeof(kSharedIds) / sizeof(kSharedIds[0])),
                               &comInfHash, 1, &comInfName);
  SoldierAnimatorLowResClass::Create();

  // 3. Load Tatooine Mos Eisley (tat/tat2.lvl): textures, lights, sky, terrain, models, world!
  static const unsigned int kLevelIds[] = {
    CHUNK_ID('t', 'e', 'x', '_'),
    CHUNK_ID('l', 'g', 'h', 't'),
    CHUNK_ID('s', 'k', 'y', '_'),
    CHUNK_ID('t', 'e', 'r', 'n'),
    CHUNK_ID('m', 'o', 'd', 'l'),
    CHUNK_ID('g', 'm', 'o', 'd'),
    CHUNK_ID('s', 'k', 'e', 'l'),
    CHUNK_ID('z', 'a', 'f', '_'),
    CHUNK_ID('z', 'a', 'a', '_'),
    CHUNK_ID('e', 'n', 't', 'c'),
    CHUNK_ID('p', 'r', 'p', '_'),
    CHUNK_ID('w', 'p', 'n', 'c'),
    CHUNK_ID('o', 'r', 'd', 'c'),
    CHUNK_ID('e', 'x', 'p', 'c'),
    CHUNK_ID('c', 'o', 'm', 'b'),
    CHUNK_ID('c', 'o', 'l', 'l'),
    CHUNK_ID('p', 'r', 'i', 'm'),
    CHUNK_ID('p', 'a', 't', 'h'),
    CHUNK_ID('p', 'l', 'a', 'n'),
    CHUNK_ID('p', 'l', 'n', 'p'),
    CHUNK_ID('p', 'o', 'r', 't'),
    CHUNK_ID('w', 'r', 'l', 'd'),
  };
  bool levelOk = LoadUtil::ReadDataFileOnHeap("tat/tat2.lvl", __RedTempHeap, kLevelIds,
                                              (unsigned int)(sizeof(kLevelIds) / sizeof(kLevelIds[0])),
                                              0, 0, 0);
  if (!levelOk) {
    Log("wgpu-engine: failed to load tat/tat2.lvl");
    return 0;
  }

  // 4. Stream Republic rifleman (textures + models + animations + class) out of side/rep.lvl.
  unsigned int repHash = PblHash("rep_inf_ep3_rifleman");
  const char* repName = "rep_inf_ep3_rifleman";
  LoadUtil::ReadDataFileOnHeap("side/rep.lvl", __RedTempHeap, kSharedIds,
                               (unsigned int)(sizeof(kSharedIds) / sizeof(kSharedIds[0])),
                               &repHash, 1, &repName);

  // 5. Retail PostLoad / Activate sequence.
  MemoryPool::CreateAll();
  SoldierAnimatorClass::PostLoad();
  RedTerrain::Activate();
  SkyManager::Activate();
  RedRenderer::PostLoad();
  SoldierAnimatorLowResClass::PostLoad();
  RedSceneManager::SetupStaticWorld();

  // 6. Spawn a real Republic rifleman in the open Mos Eisley courtyard right in front of Camera A.
  const float trooperX = -5.4f;
  const float trooperZ = -7.2f;
  float groundY = RedTerrain::GetInterpolatedHeight(trooperX, trooperZ);
  if (EntityClass* cls = EntityClass::Find(repHash)) {
    PblMatrix spawnMat = gMatrixIdentity;
    // Face toward Camera A (-6.0, ..., -10.5)
    spawnMat.m[0][0] = -0.98387f; spawnMat.m[0][2] =  0.17889f;
    spawnMat.m[2][0] = -0.17889f; spawnMat.m[2][2] = -0.98387f;
    spawnMat.SetTranslation(trooperX, groundY + 0.05f, trooperZ, 1.0f);
    EntityDesc desc;
    desc.mMatrix = spawnMat;
    desc.mPlayerId = 0;
    Entity* ent = cls->Build(desc);
    if (ent) {
      if (GameObject* obj = ent->GetGameObject()) {
        obj->SetTeam(0);
        obj->mPerceivedTeam = 0;
      }
      ent->Init();
      g_trooper = static_cast<EntitySoldier*>(ent);
      /* EntitySoldier's constructor already did SetPortalActive(true) and
         pvs::IManager::RegisterDynamic (EntitySoldier.cpp:933-934). Clearing the flag here - an
         old leftover from when a host-side viewer drew the scene itself - takes the soldier out
         of the PVS visible set, and RedSceneManager::Update (RedSceneManager.cpp:972) never makes
         it visible again: the trooper was spawned, updated and never drawn. */
    }
  }

  // Aim Camera A across Mos Eisley courtyard with the Clone Trooper framed head-to-toe in foreground.
  aimCamera(-6.0f, groundY + 1.5f, -12.0f, 0.5f, groundY + 0.9f, 4.0f);

  // 7. Add a real engine RedFont / RedTextElement HUD label onto RedInterfaceScreen.
  g_hudGroup = new RedScreenGroupElement("WgpuHudGroup");
  g_hudTitle = new RedTextElement("WgpuHudTitle");
  g_hudTitle->SetFont(RedFont::GAMEFONT_MEDIUM);
  g_hudTitle->SetTextAlign(RedTextElement::TEXT_HALIGN_LEFT, RedTextElement::TEXT_VALIGN_TOP);
  g_hudTitle->SetTextStyle(RedGlyphCacheElement::TEXT_STYLE_SHADOW);
  g_hudTitle->SetColor(RedColor(255, 224, 96, 255));
  g_hudTitle->SetPosition(24.0f, 20.0f);
  g_hudTitle->SetText("STAR WARS BATTLEFRONT II - WEBGPU SM 1.1", true);
  g_hudTitle->Enable(true);
  g_hudGroup->AddElement(g_hudTitle);
  g_hudGroup->Enable(true);
  FLRenderer::AddInterfaceElement(g_hudGroup, 0.0f, 0.0f, false);

  g_tat2Loaded = true;
  Log("wgpu-engine: tat/tat2 loaded (textures=%d, terrainLayers=%d, collisionMeshes=%d, trooper=%s)",
      RedTexture::s_textureHashTable.GetNumEntries(),
      RedTerrain::_iNumLayers,
      CollisionMesh::_HashTable.GetNumEntries(),
      g_trooper ? "spawned" : "none");
  return 1;
}

void swbf2_engine_render_set_camera(float ex, float ey, float ez, float tx, float ty, float tz) {
  if (!g_renderInited) return;
  aimCamera(ex, ey, ez, tx, ty, tz);
}

void swbf2_engine_render_set_trooper_visible(int visible) {
  if (!g_trooper) return;
  if (visible) {
    if (!g_trooper->IsInScene()) g_trooper->Activate();
  } else {
    if (g_trooper->IsInScene()) g_trooper->Deactivate();
  }
}

int swbf2_engine_render_frame(float dt) {
  if (!g_renderInited || !swbf2_wgpu_is_ready()) return 0;
  if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;
  GameLoop::sGameOver = false;
  GameLoop::sClientDeltaTime = dt;
  g_renderTimeBase.AdvanceTime(dt);
  FLRenderer::Render();
  return swbf2_wgpu_draw_calls();
}

const char* swbf2_engine_render_stats(void) {
  auto& ctx = GetWgpuContext();
  snprintf(g_statsJson, sizeof(g_statsJson),
           "{"
           "\"initialized\":%s,"
           "\"levelLoaded\":%s,"
           "\"pipeline\":%d,"
           "\"geometryMode\":%d,"
           "\"skinningMode\":%d,"
           "\"texturesLoaded\":%d,"
           "\"terrainLayers\":%d,"
           "\"collisionMeshes\":%d,"
           "\"trooperSpawned\":%s,"
           "\"vertexShadersCreated\":%d,"
           "\"pixelShadersCreated\":%d,"
           "\"vsTranslated\":%d,"
           "\"psTranslated\":%d,"
           "\"lastFrameDraws\":%d,"
           "\"lastFrameTriangles\":%d,"
           "\"lastFrameSkinnedDraws\":%d,"
           "\"lastFrameSkinnedTriangles\":%d,"
           "\"wgpuErrors\":%d,"
           "\"hasBC\":%s"
           "}",
           g_renderInited ? "true" : "false",
           g_tat2Loaded ? "true" : "false",
           (int)RedRenderer::pcGetRenderCaps().pipeline,
           (int)RedRenderer::pcGetRenderCaps().geometry,
           (int)RedRenderer::pcGetRenderCaps().skinning,
           RedTexture::s_textureHashTable.GetNumEntries(),
           RedTerrain::_iNumLayers,
           CollisionMesh::_HashTable.GetNumEntries(),
           g_trooper ? "true" : "false",
           (int)ctx.allVertexShaders.size(),
           (int)ctx.allPixelShaders.size(),
           (int)GetTranslatedVsCount(),
           (int)GetTranslatedPsCount(),
           swbf2_wgpu_draw_calls(),
           swbf2_wgpu_triangles(),
           swbf2_wgpu_skinned_draws(),
           swbf2_wgpu_skinned_triangles(),
           swbf2_wgpu_error_count(),
           swbf2_wgpu_has_bc() ? "true" : "false");
  return g_statsJson;
}

} // extern "C"
