/* The shared level harness: the engine's own start-up and level load, in one place, so that the
 * browser build and the native oracle build run the same code against the same data.
 *
 * Everything in here is the engine's own code called in the engine's own order. The only thing
 * this file decides is *which* of the engine's start-up steps a browser can run: GameState::
 * PreStateInit insists on opening the mission's Lua script and exits the process when it cannot
 * (GameState.cpp:835-860), and it builds the font and texture systems on the Direct3D9 device
 * (RedFont::Init calls RedRenderer::pcGetDevice()->CreateTexture, RedFont.cpp:2342). So the
 * harness runs the pieces the simulation needs and leaves the pieces that need a Windows device
 * or a mission script alone.
 */

#include "level_harness.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The world's half-extent in metres (GameState.cpp:292). The mission's Lua script sets it
   (LuaCallbacks_Mission.cpp:2621); the harness has no mission script, so the engine's own
   default is what it gets. */
extern int WORLD_EXTENTS;

#include "Battlefront2/Source/AIUtil.h"
#include "Battlefront2/Source/Character.h"
#include "Battlefront2/Source/Chunk.h"
#include "Battlefront2/Source/CollisionManager.h"
#include "Battlefront2/Source/CollisionMesh.h"
#include "Battlefront2/Source/CommandPost.h"
#include "Battlefront2/Source/CommandPostManager.h"
#include "Battlefront2/Source/CameraManager.h"
#include "Battlefront2/Source/ConnectivityGraph.h"
#include "Battlefront2/Source/GameLoop.h"
#include "Battlefront2/Source/HUDManager.h"
#include "Battlefront2/Source/HUDEventQueue.h"
#include "Battlefront2/Source/TargetManager.h"
#include "Battlefront2/Source/MessageDisplay.h"
#include "Battlefront2/Source/ToolTips.h"
#include "Battlefront2/Source/QOSDisplay.h"
#include "Battlefront2/Source/VoiceOverManager.h"
#include "Battlefront2/Source/LockOnManager.h"
#include "Battlefront2/Source/ControllerManager.h"
#include "Battlefront2/Source/DamageRegion.h"
#include "Battlefront2/Source/GameMemory.h"
#include "Battlefront2/Source/GameState.h"
#include "Battlefront2/Source/LoadUtil.h"
#include "Battlefront2/Source/NetGame.h"
#include "Battlefront2/Source/FLJoystick.h" /* ProcessedInputs: the engine's input record */
#include "Battlefront2/Source/PlayerController.h"
#include "Battlefront2/Source/LuaHelper.h"
#include "Battlefront2/Source/NetSetup.h"
#include "Battlefront2/Source/StringDB.h"
#include "Battlefront2/Source/Thread.h"
#include "Battlefront2/Source/SoldierAnimatorLowRes.h"
#include "Battlefront2/Source/SoldierAnimationBank.h"
#include "Battlefront2/Source/SoldierAnimatorClass.h"
#include "Battlefront2/Source/BoundaryManager.h"
#include "Battlefront2/Source/Entity.h"
#include "Battlefront2/Source/EntitySoldier.h"
#include "Battlefront2/Source/EntityTerrain.h"
#include "Battlefront2/Source/EnergyBar.h"
#include "Battlefront2/Source/EventManager.h"
#include "Battlefront2/Source/FLEffect.h"
#include "Battlefront2/Source/LensFlare.h"
#include "Battlefront2/Source/LightBeam.h"
#include "Battlefront2/Source/MotionBlurEffect.h"
#include "Battlefront2/Source/SpawnManager.h"
#include "Battlefront2/Source/Team.h"
#include "Battlefront2/Source/WeaponMelee.h"
#include "Battlefront2/Source/TriggerRegionManager.h"
#include "RedEngineFL/Application/RedTerrain.h"
#include "RedEngineFL/Application/pc/pcFLRender.h"
#include "RedEngineFL/Graphics/RedCamera.h"
#include "RedEngineFL/Interface/RedInterfaceScreen.h"
#include "RedEngineFL/Interface/RedGlyphCacheElement.h"
#include "RedEngineFL/Application/pc/pcFLRender.h"
#include "RedEngineFL/Memory/RedMemory.h"
#include "RedEngineFL/Memory/RedMemoryPool.h"
#include "PebbleFL/Common/PblHashTable.h"

/* The browser seam that gives the engine's warning system a destination. It lives in
   browser/platform/redwarning_browser.cpp because the engine's own RedWarning::Init is what
   decides where a message goes; this file only says where that is. */
void RedWarningBrowserInit(void(__cdecl* pCallback)(char const*, ...));

static bool s_bInitialised = false;

/* The engine's own memory pool sizes.
 *
 * The game sizes the pools in GameLoop::Init (GameLoop.cpp:759-806): 600 soldiers, 2400
 * weapons, 2400 ammunition counters, 2400 energy bars, 550 tree-grid stacks, and so on. The
 * sizes are not decoration: the engine refuses to build a soldier from a pool that has no room
 * (EntitySoldierClass::Build returns nothing when EntitySoldier::sMemoryPool.IsFull(),
 * EntitySoldier.cpp:5832), and a pool's item size has to be at least as large as the objects
 * that are allocated from it, or the allocation runs into the next block.
 *
 * The harness calls the engine's own GameLoop::Init, which makes these calls itself; this
 * function is the harness's copy of them, for the case where the harness is run without the
 * game loop's start-up. The numbers are the engine's, not the harness's choice: the retail game
 * overrides some of them from the mission's Lua script, which the harness does not run. */
static void SetupEngineMemoryPools()
{
    static const struct
    {
        const char* pcLabel;
        unsigned int uiCount;
    } aPools[] = {
        { "EntitySoldier", 600 },
        { "EntityDroid", 16 },
        { "MountedTurret", 50 },
        { "PassengerSlot", 0 },
        { "PowerupItem", 25 },
        { "EntityMine", 40 },
        { "EntityPortableTurret", 6 },
        { "CommandFlyer", 0 },
        { "Aimer", 100 },
        { "TreeGridStack", 550 },
        { "Weapon", 2400 },
        { "AmmoCounter", 2400 },
        { "EnergyBar", 2400 },
        { "StickInfo", 60 },
        { "Ordnance", 120 },
        { "LightFlash", 16 },
        { "EntityLight", 100 },
        { "LightningBoltEffectObject", 16 },
        { "ParticleEmitterObject", 192 },
        { "ParticleEmitterInfoData", 1024 },
        { "ParticleEmitter", 1024 },
        { "ParticleTransformer::PositionTransformer", 1024 },
        { "ParticleTransformer::SizeTransformer", 1024 },
        { "ParticleTransformer::RotationTransformer", 0 },
        { "ParticleTransformer::ColorTransformer", 1536 },
        { "EntitySoundStatic", 112 },
        { "EntitySoundStream", 30 },
        { "SoundSpaceRegion", 30 },
        { "PlayerController", 64 },
        { "TeamAssist", 64 },
    };

    for (unsigned int i = 0; i < sizeof(aPools) / sizeof(aPools[0]); ++i)
    {
        MemoryPool* pPool = MemoryPool::Find(aPools[i].pcLabel);
        if (pPool)
        {
            pPool->SetCount(aPools[i].uiCount);
        }
    }

    /* The item sizes: the largest weapon class and the largest ordnance class. */
    if (MemoryPool* pPool = MemoryPool::Find("Weapon"))
    {
        pPool->SetSize(0x1c0);
    }
    if (MemoryPool* pPool = MemoryPool::Find("Ordnance"))
    {
        pPool->SetSize(0x180);
    }
}

bool LevelHarnessInit(void(__cdecl* pLog)(char const*, ...))
{
    if (s_bInitialised)
    {
        return true;
    }
    s_bInitialised = true;

    /* The engine's own switches for a machine with no sound (the -nosound command line option
       sets g_bNoSound at CommandLine.cpp:639) and for a machine that does not draw (the switch
       at CommandLine.cpp:457). With g_bNoRender set the renderer start-up skips the Direct3D9
       textures it would otherwise create (pcFLRender.cpp:415-440) and the frame loop does not
       draw. The browser draws its own picture; the engine's simulation - collision, physics, the
       scene graph - is what the harness needs. */
    g_bNoSound = true;
    g_bNoRender = true;

    /* The engine's own log destination. RedWarning writes its messages to a callback when one is
       set (RedWarning.cpp:340); the harness sets it so the engine's warnings - the ones that
       precede a fatal error - land somewhere instead of nowhere. */
    RedWarningBrowserInit(pLog);

    /* The engine's memory heaps (GameMemory::BuildHeaps), then its renderer start-up
       (pcFLRender.cpp:361): the scene manager, the level-of-detail manager, the light system.
       The scene manager is what the loaded entities add themselves to
       (RedSceneObject::AddToScene), so the loader needs it. */
    GameMemory::BuildHeaps(true);
    RedRenderer::InitStatic();
    RedRenderer::OpenDynamic();
    static PblTimeBase s_harnessTimeBase;
    RedRenderer::SetTimeBase(&s_harnessTimeBase);
    RedTexture::Init();
    RedRenderer::Init();
    FLRenderer::Init(false);

    /* The engine's class registries (GameState.cpp:696-796): the explosions, the ordnance, the
       weapons and the entities, which is what the entity system needs in order to exist. */
    GameState::CreateBaseExplosionClasses();
    GameState::CreateBaseOrdnanceClasses();
    GameState::CreateBaseWeaponClasses();
    GameState::CreateBaseEntityClasses();

    /* The engine's melee weapon classes (WeaponMeleeClass::Init, GameState.cpp:913). */
    WeaponMeleeClass::Init();

    /* The engine's network game state (NetGame::Create, GameState.cpp:929).
     *
     * This is the engine's own line in the mission's start-up, and the simulation needs what
     * it creates: the player's input states (the host joysticks, NetGame.cpp:1955-1956). A
     * soldier asks the network layer whether the player is in mouse mode every time it is
     * updated (EntitySoldier.cpp:6571 calls NetGame::IsMouseMode), and that reads the
     * joystick.
     *
     * It is *not* the multiplayer stack: the network is off in this build (the global
     * netEnabled is false until the front-end turns it on, NetGame.cpp:1877), so the call
     * creates the local player's input and the game-state tables and opens no socket. The
     * socket work is in NetSetup::LaunchGame, which runs only when the network is enabled
     * (NetGame.cpp:2032-2045). */
    NetGame::Create(true);

    /* The mission-mode services: the collision manager (which owns the world's collision data),
       the chunk cluster and trigger region managers, the command posts, the controllers and the
       AI subsystem. These are the lines GameState.cpp:937-953 runs in mission mode. The AI
       subsystem in particular is not optional for a level load: the hints in the level are filed
       into the tree grid that HintManager::Initialize creates (AIUtil.cpp:360), and without it
       the loader writes into an unbuilt grid.

       The particle managers and the pause menu are left out: they need the renderer and the user
       interface, and the harness has neither. */
    {
        const float fExtent = (float)WORLD_EXTENTS;
        const PblVector3 vMin(-fExtent, 0.0f, -fExtent);
        const PblVector3 vMax(fExtent, 0.0f, fExtent);
        CollisionManager::Create(vMin, vMax);
    }
    /* The HUD's element factories (GameState.cpp:832) and the camera manager
       (GameState.cpp:981). The loader needs both: the HUD chunk is read by
       HUD::Manager::Load, which asks the camera manager how many views there are, and a level
       with a HUD chunk is otherwise a crash in the middle of the load. */
    HUD::Manager::Open();
    CameraManager::Create((int)GameLoop::GetNumCameras());

    /* The engine's own Lua state (LuaHelper::InitState, LuaHelper.cpp:23). The engine creates
       it in the mission's start-up (GameState.cpp:826, inside PreStateInit) and the game loop's
       start-up (GameLoop.cpp:833). The harness needs it because GameLoop::Init registers the
       engine's event callbacks with it (EventManager::Init, EventManager.cpp:39, which pushes a
       light userdata onto the Lua stack for each one). The harness does not run any mission
       script: the scripts in the data files were compiled with a four-byte Lua number and this
       build's Lua has an eight-byte one, so the engine's loader would refuse them. */
    LuaHelper::InitState();

    /* The engine's own game-loop start-up (GameLoop::Init, GameLoop.cpp:703).
     *
     * The game runs it straight after the mission's start-up and straight before the mission's
     * post-load (GameState.cpp:1233-1235), and the harness runs it in the same place.
     *
     * The harness cannot call GameLoop::Init as it stands: the function creates the display
     * managers, and the displays build their text on the font system, which is built on a
     * Direct3D9 texture (RedFont::Init, RedFont.cpp:2341-2353). There is no device in the
     * browser, and none in the native oracle either, so the harness runs the lines of
     * GameLoop::Init that the simulation needs and leaves the ones that need the device alone.
     * See SetupEngineGameLoop below for the lines it runs. */

    /* The engine's own event manager (EventManager::Init, EventManager.cpp:39, the first line
       of GameLoop::Init). It registers the engine's events with the Lua state, which the
       harness has just created. */
    EventManager::Init();

    /* The engine's own fonts and localisation, from core.lvl (the engine reads that file in
       its mission start-up, GameState.cpp:981). The display managers build their text on these
       fonts (ChatMessageDisplay::Init asks for RedFont::GAMEFONT_SMALL,
       ChatMessageDisplay.cpp:49), and the display managers are what the game's own start-up
       creates next.
     *
     * The font chunks are data only: RedVariableWidthFont::Read reads the glyph table
     * (RedFont.cpp:2376-2440) and asks for no device. It is the engine's *system* font
     * (RedFont::Init, RedFont.cpp:2335-2354) that builds a Direct3D9 texture, and the harness
     * does not need that one: the displays ask for the named fonts, which are in core.lvl. */
    {
        static const unsigned int uiIds[] = {
            0x746E6F66, /* font - the named fonts */
            0x6C636F4C, /* Locl - the localisation strings */
        };
        LoadUtil::ReadDataFileOnHeap("core.lvl", __RedTempHeap, uiIds,
                                     (unsigned int)(sizeof(uiIds) / sizeof(uiIds[0])), 0, 0, 0);
    }

    /* The engine's own glyph cache pool (RedGlyphCacheElement::InitPool, GameState.cpp:886).
     * It is the pool the text elements draw their glyphs from, and it asks for no device. The
     * engine creates it next to the system font; the harness creates only this one, because the
     * system font is the part that needs the Direct3D9 texture. */
    RedGlyphCacheElement::InitPool(0x1770);

    /* The engine's own game-loop start-up, minus the parts that need a Windows device.
     *
     * The game runs GameLoop::Init (GameLoop.cpp:703) between the mission's start-up and the
     * mission's post-load (GameState.cpp:1233-1235), and the harness runs the lines of it that
     * the simulation needs, in the engine's order.
     *
     * The harness cannot call GameLoop::Init as it stands. The function ends with the display
     * managers' Init calls (GameLoop.cpp:857-874), and those build the text on the system's own
     * font, which is a Direct3D9 texture (RedFont::Init, RedFont.cpp:2335-2354). There is no
     * device in the browser, and none in the native oracle either, so the harness creates the
     * display managers (which the simulation asks questions of) but does not initialise them.
     *
     * The same goes for the lines that need the platform layer or the network: the first-person
     * models (FirstPerson::InitPlatform), the network client (NetGame::CreateClient) and the
     * network turns (NetGame::EnterClient and NetGame::LeaveClient). */
    Team::CreateAll();
    SpawnManager::Create();
    HUD::EventQueue::Create();
    TargetManager::Init();
    MessageDisplay::Create();
    ToolTipsDisplay::Create();
    QOSDisplay::Create();
    VoiceOverManager::Create();
    BoundaryManager::Create();

    /* The engine's own memory pool sizes (GameLoop.cpp:759-806). */
    SetupEngineMemoryPools();

    /* The engine's own tool tips (ToolTipsDisplay::sInstance->Init, GameLoop.cpp:863). The
       display's Init sets up the tip classes it asks about (ToolTips.cpp:160-165), and the
       soldier asks it on every update (EntitySoldier.cpp:3275). It needs no device. */
    ToolTipsDisplay::sInstance->Init();

    /* The engine's own lock-on managers (LockOnManager::CreateAll, GameLoop.cpp:946, in the
       engine's GameLoop::PostLoad). A soldier asks the lock-on manager to drop its target
       every time it is updated (EntitySoldier.cpp:6233), so without the managers the first
       update of a soldier is a crash. */
    LockOnManager::CreateAll();

    /* The string database (GameState.cpp:830). The world loader stores the region's names in it
       - LoadUtil::ProcessRegionInfo reads a NAME chunk and calls StringDB::Store
       (LoadUtil.cpp:678) - and the store copies the text into the database's own pool. Without
       the pool the copy has no destination, which is a crash in the middle of the load.

       GameState::PreStateInit runs this line, but the harness does not call PreStateInit (it
       opens the mission's Lua script and exits the process when it cannot), so the harness runs
       the lines of that function it needs, in the order the engine runs them. */
    StringDB::Init();

    /* The boundary manager (GameLoop.cpp:754). The world loader reads the level's path chunks
       into it (LoadUtil.cpp:268 calls BoundaryManager::Read), and the manager is a singleton:
       without Create, the loader writes into a null one. */
    BoundaryManager::Create();

    /* The terrain's collision object (GameLoop.cpp:839: new EntityTerrain).
     *
     * The collision manager holds the world's collision data, and the terrain is one of the
     * things it holds: EntityTerrain's constructor registers itself with the manager
     * (EntityTerrain.cpp:16, CollisionManager::SetTerrain). Without it the manager has no
     * terrain, and a ray that should meet the ground passes through it - the engine's own ray
     * against the terrain goes through EntityTerrain::RayHit.
     *
     * It is created before the level is loaded, as the engine creates it: the terrain's data
     * arrives with the level's tern chunk and fills in the RedTerrain singleton the object
     * reads. */
    new EntityTerrain;

    ChunkClusterManager::Create();
    TriggerRegionManager::Create();
    DamageRegion::Create();
    CommandPost::Create();
    ControllerManager::Init();
    AIUtil::Init();
    ConnectivityGraphManager::Init();
    CommandPostManager::Create();

    return true;
}

bool LevelHarnessLoad(const char* pcName)
{
    if (!pcName || !pcName[0])
    {
        return false;
    }

    char acLevel[256];
    snprintf(acLevel, sizeof(acLevel), "%s", pcName);
    const size_t n = strlen(acLevel);
    if (n < 4 || strcmp(acLevel + n - 4, ".lvl") != 0)
    {
        /* The engine's loader takes the file name as it is and prefixes the data directory
           (LoadUtil.cpp:468), so the extension is part of the name. */
        if (n + 4 < sizeof(acLevel))
        {
            memcpy(acLevel + n, ".lvl", 5);
        }
    }

    /* The chunks the simulation needs, and only those.
     *
     * LoadUtil::ReadDataFileOnHeap takes the list of chunk ids it should read
     * (LoadUtil.cpp:222-236): a chunk whose id is not in the list is skipped. The browser reads
     * the world's collision data, its terrain, its entities and its AI, and leaves the rest
     * alone:
     *
     *   - the textures: the browser draws its own picture and reads those itself
     *     (browser/main.cpp's texture reader), so the engine's copies would be a second copy of
     *     the same megabytes in a linear memory that has to hold the level. The textures are
     *     the largest thing in a level file: eleven megabytes of the fifteen in ingame.lvl.
     *   - the skeletons: the animation rig. The browser's own model reader has it; the
     *     simulation does not move a skeleton.
     *   - the fonts: RedFont::Read builds the engine's font textures on the Direct3D9 device.
     *   - the HUD: the browser's page has its own HUD, and the engine's HUD parser reads past
     *     the end of its chunk buffer in this build - it takes whatever follows in memory, so
     *     the level load then depends on the heap layout (which is why it works at -O0 and not
     *     at -O1).
     *   - the scripts: the browser does not run the mission's Lua.
     *   - the sounds: there is no DirectSound device and no WebAudio sink yet.
     *
     * The model entries (gmod) and the models themselves (modl) are the exception, and they
     * are not there for the picture: the engine refuses to build a prop that has no model
     * (EntityProp.cpp:898), and the level's walls and platforms are props. Without them the
     * level loads, but it has no buildings in it: the collision manager holds no static bodies
     * at all, and the ground under a building is the terrain rather than the building.
     *
     * The models' own vertex data is not read into memory. The engine asks the renderer
     * whether it can hold compressed geometry (pcRedSegment.cpp:592) and this build's renderer
     * says no, so the engine skips the vertex buffers of the compressed models and reads only
     * their metadata. An uncompressed model would need a Direct3D9 vertex buffer, which this
     * build has no device to create.
     *
     * The ids are the engine's own four-character codes, read as the little-endian integers the
     * loader's switch statement uses.
     */
    static const unsigned int uiIds[] = {
        0x63746E65, /* entc - the entity classes */
        0x646C7277, /* wrld - the world: the entities, the regions, the hints */
        0x5F707270, /* prp_ - the prop classes */
        0x6364726F, /* ordc - the ordnance classes */
        0x626D6F63, /* comb - the combination weapon classes */
        0x63707865, /* expc - the explosion classes */
        0x636E7077, /* wnpc - the weapon classes */
        0x6C6C6F63, /* coll - the collision meshes */
        0x6D697270, /* prim - the collision primitives */
        0x6E726574, /* tern - the terrain */
        0x68746170, /* path - the AI paths */
        0x6E616C70, /* plan - the AI plans */
        0x706E6C70, /* plnp - the connectivity graph patches */
        0x74726F70, /* port - the PVS portals */
        0x646F6D67, /* gmod - the model entries */
        0x6C646F6D, /* modl - the models */
    };

    /* The engine's own loader with the engine's own chunk filter.
     *
     * The filter is the third and fourth arguments of ReadDataFileOnHeap (LoadUtil.h:23), which
     * is the form the game itself uses when it wants only some of a file's chunks
     * (GameSoundEngine.cpp:501). The shorter ReadDataFile takes a list of *level* hashes
     * instead (LoadUtil.h:18), so a chunk list passed there is read as a list of level names and
     * filters nothing. */
    /* TEMPORARY EXPERIMENT REMOVED */
    return LoadUtil::ReadDataFileOnHeap(acLevel, __RedTempHeap, uiIds,
                                        (unsigned int)(sizeof(uiIds) / sizeof(uiIds[0])), 0, 0, 0);
}

int LevelHarnessCollisionMeshCount()
{
    return CollisionMesh::_HashTable.GetNumEntries();
}

/* The entity the harness last spawned. The engine owns it: the class's Build allocates it from
   the engine's own memory pool (EntitySoldier::sMemoryPool) and the engine's own update loop
   moves it. The harness only keeps the pointer so it can ask the engine's own update to run. */
static Entity* s_pSpawned = 0;
static int s_iUpdateCount = 0;

/* The class load reads the class's own file and nothing else.
 *
 * The model entries (gmod) and the models themselves (modl) are in the list because the
 * engine's own class readers ask the model for things and do not ask whether it is there:
 * a soldier class names its model and uses the result (EntitySoldier.cpp:5532), an item class
 * asks it for its bounding box (EntityItem.cpp:197), and an explosion class asks its chunk
 * descriptor's model for the same (Explosion.cpp:316).
 *
 * The skeletons (skel) and the animation banks (zaa_, zaf_) are in the list because the first
 * soldier class that is derived creates the animator class, and the animator class wants the
 * "human" animation bank and its skeleton (SoldierAnimatorClass.cpp:105-107). It uses the
 * skeleton without asking whether it is there, so without the skeletons it walks off the end
 * of a null pointer.
 *
 * The models' own vertex data is not read into memory. The engine asks the renderer whether it
 * can hold compressed geometry (pcRedSegment.cpp:592) and this build's renderer says no, so
 * the engine skips the vertex buffers of the compressed models and reads only their metadata.
 * An uncompressed model would need a Direct3D9 vertex buffer, which this build has no device
 * to create. */
static bool LoadClassFromFile(const char* pcName)
{
    if (!pcName || !pcName[0])
    {
        return false;
    }

    /* The engine's own naming: the file, then a semicolon, then the name of the class. The
       game names its own loads the same way when it reads a mission file (GameState.cpp:835:
       "MISSION.lvl"), and the part after the semicolon is the name of the nested level to
       read (LoadUtil.cpp:441-451). */
    char acFile[256];
    char acClass[128];
    acFile[0] = 0;
    acClass[0] = 0;
    {
        const char* pcSemicolon = strchr(pcName, ';');
        if (pcSemicolon)
        {
            const size_t n = (size_t)(pcSemicolon - pcName);
            if (n >= sizeof(acFile))
            {
                return false;
            }
            memcpy(acFile, pcName, n);
            acFile[n] = 0;
            snprintf(acClass, sizeof(acClass), "%s", pcSemicolon + 1);
        }
        else
        {
            snprintf(acFile, sizeof(acFile), "%s", pcName);
        }
    }
    if (!acFile[0] || !acClass[0])
    {
        return false;
    }

    /* The class is selected by its hash, the way the game selects one: the engine's own call
       for a class file is ReadDataFile("SIDE\\snw.lvl", 0, &uiClassHash, 1, &pcClass)
       (LuaCallbacks_Mission.cpp:1374). The hash is of the class's name, and the loader reads
       the nested level whose id is that hash (LoadUtil.cpp:288-300), so the megabytes of the
       other classes in the file are skipped rather than parsed. */
    const unsigned int uiHash = PblHash(acClass);
    const char* pcClassName = acClass;
    /* The chunk list is the simulation's: the entity class, the weapon classes, the ordnance
       and the explosion classes, the collision primitives, and (for a class that names a model)
       the model entries.

       The models' own data (the vertex data in the modl chunks), the textures and the
       animations are left out: the browser reads and draws those itself, and the engine's
       copies would be a second copy of the same megabytes in a linear memory that has to hold
       the level. */
    static const unsigned int uiIds[] = {
        0x5F6C766C, /* lvl_ - the nested level that holds the class */
        0x63746E65, /* entc - the entity class */
        0x6364726F, /* ordc - the ordnance classes */
        0x626D6F63, /* comb - the combination weapon classes */
        0x63707865, /* expc - the explosion classes */
        0x636E7077, /* wnpc - the weapon classes */
        0x6D697270, /* prim - the collision primitives */
        0x646F6D67, /* gmod - the model entries */
        0x6C646F6D, /* modl - the models */
        0x6C656B73, /* skel - the skeletons */
        0x5F61617A, /* zaa_ - the animation banks */
        0x5F66617A, /* zaf_ - the animation banks, low resolution */
    };

    const unsigned int uiCount = (unsigned int)(sizeof(uiIds) / sizeof(uiIds[0]));
    const bool bOk = LoadUtil::ReadDataFileOnHeap(acFile, __RedTempHeap, uiIds, uiCount, &uiHash,
                                                  1, &pcClassName);

    /* The engine's own low-resolution animator class.
     *
     * The game creates it right after it reads the file that holds the shared animation banks:
     * LuaCallbacks_Mission.cpp:619-622 reads "ingame.lvl" and then calls
     * SoldierAnimatorLowResClass::Create(). A soldier class names its own low-resolution
     * animation bank in its properties (EntitySoldier.cpp:5591), and the engine adds that bank
     * to this class, so the class has to exist before a soldier's class file is read.
     *
     * The class can only be created when the "humanlz" bank is there - the constructor looks it
     * up and uses the result without asking whether it is there
     * (SoldierAnimatorLowRes.cpp:538-544) - so the file that holds the shared banks has to be
     * read first. The game reads ingame.lvl before the faction's side file, and so does the
     * harness: the base classes come first, the soldier's own class second. */
    static bool s_bLowResAnimatorCreated = false;
    if (bOk && !s_bLowResAnimatorCreated)
    {
        SoldierAnimatorLowResClass::Create();
        s_bLowResAnimatorCreated = true;
    }

    return bOk;
}

bool LevelHarnessLoadClass(const char* pcName)
{
    return LoadClassFromFile(pcName);
}

bool LevelHarnessSpawn(const char* pcClassName, float x, float y, float z, int iPlayerId,
                        SpawnResult* pOut)
{
    if (pOut)
    {
        memset(pOut, 0, sizeof(*pOut));
    }
    if (!pcClassName || !pcClassName[0])
    {
        return false;
    }

    /* The class the engine's own factory holds. The name is the class's name in the game's own
       spelling: EntityClass::Find hashes it (Entity.cpp:166-181, Factory.h:81-91). */
    EntityClass* pClass = EntityClass::Find(PblHash(pcClassName));
    if (!pClass)
    {
        return false;
    }

    /* The entity description the engine's own spawn builds (Character.cpp:1001-1004): the world
       matrix it is to appear at, the player it belongs to and the priority the game gives it.
       The matrix is the engine's own: a translation and a facing direction, with the engine's
       own convention that the facing is the matrix's z axis (EntitySoldier.cpp:851 builds the
       entity's matrix from the description's z axis and translation). */
    PblMatrix mMatrix = gMatrixIdentity;
    mMatrix.m[3][0] = x;
    mMatrix.m[3][1] = y;
    mMatrix.m[3][2] = z;
    mMatrix.m[3][3] = 1.0f;

    EntityDesc desc;
    desc.mMatrix = mMatrix;
    desc.mPlayerId = iPlayerId;

    /* The engine's own animation maps (GameLoop.cpp:825 and :844).
     *
     * The class read creates the animator instance (EntitySoldier.cpp:4618). PostLoad then
     * builds the action-animation table that a roll asks for: EntitySoldier's ROLL update
     * calls GetLowerBodyActionAnimation(DIVE, HUMAN_RIFLE) and uses it immediately
     * (EntitySoldier.cpp:7258-7260). Without this step that answer is null, so the first roll
     * crashes instead of rolling. This runs once, after every class file has been read and
     * before the first soldier is built, which is where the game runs it. */
    static bool s_bAnimationPostLoad = false;
    if (!s_bAnimationPostLoad)
    {
        SoldierAnimationBank::Reset();
        SoldierAnimatorClass::PostLoad();
        s_bAnimationPostLoad = true;
    }

    /* The engine's own pools, created from the counts GameLoop::Init set (GameLoop.cpp:925,
       MemoryPool::CreateAll - the one line of the engine's GameLoop::PostLoad that the
       simulation needs). The pools are what the soldier is allocated from
       (EntitySoldier::sMemoryPool), so this has to happen before the build. */
    MemoryPool::CreateAll();

    /* The engine's own factory: EntitySoldierClass::Build allocates the soldier from the
       engine's memory pool, adds it to the collision manager and activates its physics
       (EntitySoldier.cpp:5830-5851). */
    Entity* pEntity = pClass->Build(desc);
    if (!pEntity)
    {
        return false;
    }

    /* The three steps Character::Spawn takes after the build (Character.cpp:1034-1039): the
       entity is put on a team, the character is attached to it, and the entity is initialised.
       The harness has no character - a character is a member of a team, and a team is a
       multiplayer concept - so the team is the engine's own neutral side (Team::side_Neutral)
       and the entity is initialised exactly as the game initialises it. */
    GameObject* pObject = pEntity->GetGameObject();
    if (!pObject)
    {
        return false;
    }
    pObject->SetTeam(0);
    pObject->mPerceivedTeam = 0;
    pEntity->Init();

    s_pSpawned = pEntity;
    s_iUpdateCount = 0;

    if (pOut)
    {
        /* The position is the entity's own matrix: EntityGeometry holds it and hands it out
           through GetPosition (EntityGeometry.h:212). */
        const PblVector3& vPos = pObject->GetPosition();
        pOut->spawned = true;
        pOut->x = vPos.x;
        pOut->y = vPos.y;
        pOut->z = vPos.z;
        pOut->health = pObject->mCurHealth;
        pOut->maxHealth = pObject->mMaxHealth;

        /* The weapons are the class's own: the class says how many it carries, and the entity
           built one weapon object for each of them (EntitySoldier.cpp:880-926). */
        const EntitySoldier* pSoldier = static_cast<const EntitySoldier*>(pEntity);
        pOut->classWeaponCount = pSoldier->mClass->mWeaponCount;
        for (int i = 0; i < pSoldier->mClass->mWeaponCount; ++i)
        {
            if (pSoldier->mWeapon[i])
            {
                ++pOut->builtWeaponCount;
            }
        }

        /* The collision bodies are the class's own: EntitySoldierClass::PostReadSetup builds
           them out of the class's own collision scale (EntitySoldier.cpp:4727-4800). */
        pOut->collisionBodyCount = pSoldier->mClass->mCollisionModels[0].GetNumBodies();
    }

    return true;
}

/* ---------------------------------------------------------------- player control and camera */

/* The engine's own input record for the local player. PlayerController's constructor takes it
   once (PlayerController.cpp:157: mJoystick = NetGame::GetJoystick(pOwner->mPlayerId)), and
   NetGame::GetJoystick hands back hostJoysticks[player], which NetGame::Create made
   (NetGame.cpp:1955-1956) and which the engine's own no-net turn fills from the device layer
   (NetGame.cpp:5753-5759). The harness writes the same object the same way. */
static ProcessedInputs* s_pHostJoystick = 0;

/* The trooper's own controller, as the engine made it, and the trooper's own controllable. */
static Controllable* s_pControllable = 0;
static PlayerController* s_pPlayerController = 0;

/* What the harness last injected, so a host can read its own input back. */
static HostInput s_input = { 0.0f, 0.0f, 0.0f, 0.0f, 0u, 2u };

/* The Character the engine's own camera code insists on having for a soldier with a player id.
 *
 * EntitySoldier::SetupCameraTrackMatrix computes the third-person track matrix and then, at its
 * foot, asks the soldier's Character for its vehicle in order to start a motion blur
 * (EntitySoldier.cpp:3863-3879: `if (Controllable* pVehicle = mCharacter->mVehicle)`), with no
 * guard on the Character itself - because in the game a soldier with a player id always belongs to
 * one (Controllable::SetCharacter, Character.cpp:434-442, is the only thing that puts a player id
 * on a controllable). The harness spawns without a Character, and this is the one place in the
 * camera path where that shows. The engine's own object factory is Character::CreateAll
 * (Character.cpp:355-369), so that is what the harness uses. */
static Character* s_pCharacter = 0;

static Character* HarnessCharacter(void)
{
    if (s_pCharacter)
    {
        return s_pCharacter;
    }

    /* The engine's own factory: the array of Characters, sized the way the game sizes it, plus its
       PlayerStats. The game calls this from GameState::InitPlayerCharacter before MakePlayer
       (Character.cpp:1686-1699); the harness makes one, for one local player. */
    Character::CreateAll(1);
    s_pCharacter = Character::CreateCharacter();
    if (!s_pCharacter)
    {
        return 0;
    }

    /* Character::Init (Character.cpp:406-420): the team number, the name and the player id. It is
       not MakePlayer, which would change team, pick a class and respawn - and the harness has
       already spawned its trooper, through the engine's own class factory. */
    static const unsigned short awsName[] = { 'P', '1', 0 };
    s_pCharacter->Init(0, awsName, 0);

    /* The team pointer MakePlayer gives a player (Character.cpp:1694: mTeamPtr =
       Team::sTeams[mTeamNumber]). The harness cannot use ChangeTeam, which removes the character
       from the team it is on first (Character.cpp:1144-1158) and there is no team to be removed
       from. Team 0 is the neutral team the trooper's own team was set to when it was spawned. */
    s_pCharacter->mTeamPtr = Team::sTeams[s_pCharacter->mTeamNumber];
    return s_pCharacter;
}

/* The engine's own view and screen dimensions.
 *
 * The chase camera asks the renderer and the interface screen how big the picture is
 * (ChaseCamera.cpp:306-309):
 *
 *   fAspectRatio = RedRenderer::GetScreenAspectRatio()
 *                  * (FLRenderer::GetViewHeight(0) * RedInterfaceScreen::GetScreenWidth())
 *                  / (FLRenderer::GetViewWidth(0)  * RedInterfaceScreen::GetScreenHeight());
 *
 * A machine with a display has all four numbers; a headless build has none of them, the ratio is
 * 0/0, and the camera's field of view becomes NaN. The engine's own setters are what a display
 * would have used: RedInterfaceScreen::SetDimensions for the screen
 * (RedInterfaceScreen.cpp:114-118) and the same call on the view's interface screen, which is
 * FLRenderer::m_pViewInterfaceScreen[0] (pcFLRender.cpp:312, and the array is declared in
 * pcFLRender.h:117). The host tells the harness the size of its own picture; the harness gives it
 * to the engine.
 */
bool LevelHarnessSetViewport(unsigned int uiWidth, unsigned int uiHeight)
{
    if (uiWidth == 0 || uiHeight == 0)
    {
        return false;
    }

    RedInterfaceScreen::s_screenFull.SetDimensions(uiWidth, uiHeight);
    if (FLRenderer::m_pViewInterfaceScreen && FLRenderer::m_pViewInterfaceScreen[0])
    {
        FLRenderer::m_pViewInterfaceScreen[0]->SetDimensions(uiWidth, uiHeight);
    }
    return true;
}

bool LevelHarnessControlAttach(void)
{
    if (!s_pSpawned)
    {
        return false;
    }

    GameObject* pObject = s_pSpawned->GetGameObject();
    if (!pObject)
    {
        return false;
    }
    s_pControllable = pObject->GetControllable();
    if (!s_pControllable)
    {
        return false;
    }

    /* The engine's own effect system.
     *
     * The effect singletons are created by FLEffect::InitAll (FLEffect.cpp:153-190), which the
     * game runs from the mission start-up (GameState.cpp:971). The harness does not run the whole
     * of it: HDREffect::Init asks the renderer for the sky buffer (pcHDREffect.cpp:28) and this
     * build has no Direct3D9 device to give it. But a trooper that fires does need some of them:
     * the laser ordnance makes a light, the light's colour update activates a light beam and a
     * lens flare, and those live in singletons that are null until they are created
     * (LensFlare.cpp:120, LightBeam.cpp:222). The camera path asks for the motion blur
     * (EntitySoldier.cpp:3869-3878).
     *
     * So the harness starts those three, each with the engine's own factory - the same calls
     * FLEffect::InitAll makes (FLEffect.cpp:156-158 and 187) - and leaves the rest of the effect
     * system to a build that has a device.
     *
     * This is done here, in the control path, and not in LevelHarnessInit, so that the ground and
     * spawn oracles run exactly the start-up they ran before. */
    static bool s_bEffectsStarted = false;
    if (!s_bEffectsStarted)
    {
        FLEffectObject::Init();
        LightBeamEffect::Create();
        LensFlareEffect::Create();
        MotionBlurEffect::Create();
        s_bEffectsStarted = true;
    }

    /* The Character, for the reason given above. SetCharacter is the engine's own link: it puts
       the player id on the controllable from the Character (Controllable.cpp:434-442) - the same
       id the spawn description already gave it, so nothing here changes who the trooper belongs
       to. mUnit is what Character::Spawn sets when it attaches a trooper (Character.cpp:1037). */
    Character* pCharacter = HarnessCharacter();
    if (!pCharacter)
    {
        return false;
    }
    if (s_pControllable->GetCharacter() != pCharacter)
    {
        s_pControllable->SetCharacter(pCharacter);
    }
    if (!pCharacter->mUnit)
    {
        pCharacter->mUnit = s_pControllable;
    }

    /* The controller the engine's own EntitySoldier::Init made (EntitySoldier.cpp:1130, which
       calls SetupController for every controllable the entity has, and EntitySoldier.cpp:3705-3724,
       which makes a PlayerController for any controllable with a player id and adds it with
       AddController). The harness does not make one: it finds the one that is already there. */
    s_pPlayerController = PlayerControllerPtr(s_pControllable);
    if (!s_pPlayerController)
    {
        return false;
    }
    /* The controller grabbed its joystick when the engine made it, before the harness attached a
       Character; NetGame::GetJoystick hands back the same object the controller reads. */
    s_pHostJoystick = s_pPlayerController->mJoystick;
    if (!s_pHostJoystick)
    {
        return false;
    }

    /* The engine's third-person camera: put the chase camera on this trooper and make it the
       active camera. A Controllable *is* a Trackable (Controllable.h:143), which is what the
       chase camera chases. */
    CameraManager* pCameraManager = CameraManager::Instance();
    if (!pCameraManager)
    {
        return false;
    }
    pCameraManager->Chase(static_cast<Trackable*>(s_pControllable), 0);
    pCameraManager->ChaseMode(0);
    return true;
}

bool LevelHarnessControlReady(void)
{
    return s_pPlayerController != 0 && s_pHostJoystick != 0;
}

void LevelHarnessSetInput(const HostInput* pInput)
{
    if (!pInput || !s_pHostJoystick)
    {
        return;
    }
    s_input = *pInput;

    /* The engine's own mutators on the engine's own record: Clear, then Set for each axis and each
       held button. ProcessedInputs::Set is what RawControllerInputs::StandardInputProcess calls for
       every bound action it has just read off a device (pcFLJoystick.cpp:1015-1016, and the
       default arm of its switch, pcFLJoystick.cpp:299-305, is the held-button case), so the axes
       and bits land in the engine's own layout with the engine's own rules. */
    s_pHostJoystick->Clear();
    s_pHostJoystick->bits.mMouseMode = pInput->uiMouseMode & 3u;

    bool bLocked = false;
    // OpenSWBF2's ProcessedInputs::Set takes (eType, bLocked, fLastValue, fValue) matching the
    // retail binary (FLJoystick.cpp:180).
    s_pHostJoystick->Set(ePROCESSEDINPUT_MOVE_AXIS, bLocked, 0.0f, pInput->fMove);
    s_pHostJoystick->Set(ePROCESSEDINPUT_STRAFE_AXIS, bLocked, 0.0f, pInput->fStrafe);
    s_pHostJoystick->Set(ePROCESSEDINPUT_TURN_AXIS, bLocked, 0.0f, pInput->fTurn);
    s_pHostJoystick->Set(ePROCESSEDINPUT_PITCH_AXIS, bLocked, 0.0f, pInput->fPitch);

    for (unsigned int iBit = 0; iBit < 30; ++iBit)
    {
        if ((pInput->uiButtons & (1u << iBit)) == 0)
        {
            continue;
        }
        s_pHostJoystick->Set(static_cast<ePROCESSEDINPUT_TYPE>(iBit), bLocked, 0.0f, 1.0f);
    }
}

void LevelHarnessGetInput(HostInput* pOut)
{
    if (!pOut)
    {
        return;
    }
    *pOut = s_input;
}

int LevelHarnessControlUpdate(float fDeltaTime)
{
    if (!s_pSpawned)
    {
        return 0;
    }

    /* The engine's own host turn (GameLoop.cpp:1290-1300) is ControllerManager::Update then
       Thread::UpdateAll. The harness runs the one line of the first that concerns this trooper -
       its own controller's Update, ControllerManager.cpp:739-745 - and then the turn threads.
       Why the controller and not the whole manager is documented in level_harness.h. */
    if (s_pPlayerController)
    {
        s_pPlayerController->Update(fDeltaTime);
    }

    Thread::UpdateAll(fDeltaTime, false, false);
    ++s_iUpdateCount;
    return s_iUpdateCount;
}

void LevelHarnessCameraUpdate(float fDeltaTime)
{
    /* The game's own per-frame camera update (GameLoop.cpp:1837). */
    if (CameraManager* pCameraManager = CameraManager::Instance())
    {
        pCameraManager->Update(fDeltaTime);
    }
}

RedCamera* LevelHarnessCamera(void)
{
    CameraManager* pCameraManager = CameraManager::Instance();
    if (!pCameraManager)
    {
        return 0;
    }
    return pCameraManager->GetCamera(0);
}

bool LevelHarnessGetTrooperState(TrooperState* pOut)
{
    if (!pOut)
    {
        return false;
    }
    memset(pOut, 0, sizeof(*pOut));
    if (!s_pSpawned || !s_pControllable)
    {
        return false;
    }
    GameObject* pObject = s_pSpawned->GetGameObject();
    if (!pObject)
    {
        return false;
    }

    pOut->ok = true;
    pOut->updates = s_iUpdateCount;

    const PblMatrix& mMatrix = pObject->GetMatrix();
    pOut->x = mMatrix.m[3][0];
    pOut->y = mMatrix.m[3][1];
    pOut->z = mMatrix.m[3][2];
    /* The engine's own convention: an entity faces along its matrix's z axis. */
    pOut->dirX = mMatrix.m[2][0];
    pOut->dirY = mMatrix.m[2][1];
    pOut->dirZ = mMatrix.m[2][2];

    const PblVector3& vVel = pObject->GetVelocity();
    pOut->velX = vVel.x;
    pOut->velY = vVel.y;
    pOut->velZ = vVel.z;

    pOut->controlMove = s_pControllable->mControlMove;
    pOut->controlStrafe = s_pControllable->mControlStrafe;
    pOut->controlTurn = s_pControllable->mControlTurn;
    pOut->controlPitch = s_pControllable->mControlPitch;

    pOut->iState = static_cast<const EntitySoldier*>(s_pSpawned)->mState;
    pOut->fHealth = pObject->mCurHealth;
    pOut->iFireDown = s_pControllable->mControlFire[0].IsDown() ? 1 : 0;
    pOut->iSpecialMoveDown = s_pControllable->mControlSpecialMove.IsDown() ? 1 : 0;
    if (s_pHostJoystick)
    {
        pOut->uiHostButtons = s_pHostJoystick->bits.mBoolInputs;
        pOut->uiHostMouseMode = s_pHostJoystick->bits.mMouseMode;
    }

    /* The class's own roll gate and the trooper's own energy, so that a roll that did not happen
       can be read against the engine's own numbers rather than guessed at. */
    {
        const EntitySoldier* pSoldier = static_cast<const EntitySoldier*>(s_pSpawned);
        pOut->fRollSpeedFactor = pSoldier->mClass->mRollSpeedFactor;
        pOut->fEnergy = pSoldier->mEnergyBar.GetEnergy();
    }

    if (RedCamera* pCamera = LevelHarnessCamera())
    {
        const PblMatrix& mCam = pCamera->GetMatrix();
        pOut->camX = mCam.m[3][0];
        pOut->camY = mCam.m[3][1];
        pOut->camZ = mCam.m[3][2];
        pOut->camDirX = mCam.m[2][0];
        pOut->camDirY = mCam.m[2][1];
        pOut->camDirZ = mCam.m[2][2];
        pOut->camFov = pCamera->GetFOV();
        pOut->camZoom = pCamera->GetZoom();
    }
    return true;
}

bool LevelHarnessGetTrooperMatrix(float* pOut, int iCount)
{
    if (!pOut || !s_pSpawned)
    {
        return false;
    }
    GameObject* pObject = s_pSpawned->GetGameObject();
    if (!pObject)
    {
        return false;
    }
    const PblMatrix& m = pObject->GetMatrix();
    for (int i = 0; i < iCount && i < 16; ++i)
    {
        pOut[i] = m.m[i / 4][i % 4];
    }
    return true;
}

bool LevelHarnessGetTrooperFOVInputs(float* pOut)
{
    if (!pOut || !s_pSpawned)
    {
        return false;
    }
    const ControllableClass* pClass =
        static_cast<const EntitySoldier*>(s_pSpawned)->GetControllableClass();
    if (!pClass)
    {
        return false;
    }
    pOut[0] = pClass->mThirdPersonFOV;
    pOut[1] = pClass->mFirstPersonFOV;
    return true;
}

unsigned int LevelHarnessTrooperModelHash(void)
{
    if (!s_pSpawned)
    {
        return 0;
    }
    GameObject* pObject = s_pSpawned->GetGameObject();
    if (!pObject || !pObject->GetModel())
    {
        return 0;
    }
    return pObject->GetModel()->_uiNameHash;
}

/* The movement script: a turn-by-turn list of injected input, held here so that the two builds run
 * the same trooper through the same input by construction. */
static const struct ScriptPhase
{
    const char* pcName;
    int iTurns;
    float fMove;
    float fStrafe;
    float fTurn;
    float fPitch;
    unsigned int uiButtons;
} aScriptPhases[] = {
    /* Let the engine settle the trooper on the ground it was spawned on. */
    { "settle", 30, 0.0f, 0.0f, 0.0f, 0.0f, 0u },
    /* W: the engine's own forward axis. */
    { "forward", 60, 1.0f, 0.0f, 0.0f, 0.0f, 0u },
    /* S and A: the engine turns those into a negative axis and a smaller one. */
    { "back-strafe", 30, -0.5f, 1.0f, 0.0f, 0.0f, 0u },
    /* The mouse: turn while still moving forward. */
    { "turn", 40, 0.3f, 0.0f, 2.0f, 0.0f, 0u },
    /* Space, held for three turns of the engine's own 30 Hz turn rate. */
    { "jump", 30, 0.0f, 0.0f, 0.0f, 0.0f, HOST_BUTTON_JUMP },
    /* Let the jump's action input lock expire: the engine clears the special-move trigger while
       INPUTLOCK_ACTION is set (EntitySoldier.cpp:6360-6369), so a roll pressed in the air or
       during the landing lock is deliberately rejected. The roll is tested on the ground. */
    { "recover", 30, 0.0f, 0.0f, 0.0f, 0.0f, 0u },
    /* The roll - the engine's own special-move bit. The engine only rolls when the move control
       is big enough and pointing forward (EntitySoldier::CheckMoveControlForRoll,
       EntitySoldier.cpp:10845-10853), so the phase moves forward while the button is held. */
    { "roll", 40, 1.0f, 0.0f, 0.0f, 0.0f, HOST_BUTTON_ROLL },
    /* The primary fire button. */
    { "fire", 30, 0.0f, 0.0f, 0.0f, 0.0f, HOST_BUTTON_FIRE },
};

int LevelHarnessMovementScriptLength(void)
{
    int iCount = 0;
    for (unsigned int i = 0; i < sizeof(aScriptPhases) / sizeof(aScriptPhases[0]); ++i)
    {
        iCount += aScriptPhases[i].iTurns;
    }
    return iCount;
}

bool LevelHarnessMovementScript(TrooperState* pOutStates, int iMaxStates, int* pOutCount)
{
    if (pOutCount)
    {
        *pOutCount = 0;
    }
    if (!LevelHarnessControlAttach())
    {
        return false;
    }

    const float fDelta = 1.0f / 30.0f;
    int iRecorded = 0;
    int iTurns = 0;

    for (unsigned int iPhase = 0; iPhase < sizeof(aScriptPhases) / sizeof(aScriptPhases[0]); ++iPhase)
    {
        const ScriptPhase& rPhase = aScriptPhases[iPhase];
        for (int iTurn = 0; iTurn < rPhase.iTurns; ++iTurn)
        {
            HostInput input;
            input.fMove = rPhase.fMove;
            input.fStrafe = rPhase.fStrafe;
            input.fTurn = rPhase.fTurn;
            input.fPitch = rPhase.fPitch;
            /* The one-shot buttons are held for three turns only: the engine's own turn rate is
               30 Hz, and a button held for the whole phase would be the engine's own
               "button held down", which for jump means nothing further. */
            input.uiButtons = (iTurn < 3) ? rPhase.uiButtons : 0u;
            input.uiMouseMode = 2u;
            LevelHarnessSetInput(&input);

            LevelHarnessControlUpdate(fDelta);
            LevelHarnessCameraUpdate(fDelta);
            ++iTurns;

            if (pOutStates && iRecorded < iMaxStates)
            {
                LevelHarnessGetTrooperState(&pOutStates[iRecorded]);
                ++iRecorded;
            }
        }
    }

    if (pOutCount)
    {
        *pOutCount = iTurns;
    }
    return true;
}

int LevelHarnessUpdate(float fDeltaTime)
{
    if (!s_pSpawned)
    {
        return 0;
    }

    /* The engine's own update, through the engine's own thread machinery.

       A spawned entity is put on the turn thread by ActivatePhysics (EntitySoldier.cpp:4109:
       ActivateThread(0, PER_TURN)), and the game's own loop advances those threads with
       Thread::UpdateAll(delta, false, false) - the "false" is the turn list rather than the
       frame list (GameLoop.cpp:1299). The harness asks the engine for the same thing, so the
       physics, the collision and the weapons are the engine's own code running on the engine's
       own data. */
    Thread::UpdateAll(fDeltaTime, false, false);
    ++s_iUpdateCount;
    return s_iUpdateCount;
}

bool LevelHarnessDrop(float x, float z, float fRadius, float fHalfHeight, GroundProbeResult* pOut)
{
    if (!pOut)
    {
        return false;
    }

    memset(pOut, 0, sizeof(*pOut));
    pOut->terrainY = NAN;
    pOut->groundY = NAN;
    pOut->restY = NAN;

    /* The engine's own terrain height at this point. The terrain is optional: a level without a
       tern chunk has none, and the engine's own answer then is whatever the terrain's reader
       left behind, which is not a height. */
    if (RedTerrain::_bEnable)
    {
        pOut->terrainY = RedTerrain::GetInterpolatedHeight(x, z);
    }

    /* The drop. A constant acceleration over a fixed step: the motion is the harness's, because
       the engine does not integrate a free body on its own - its entities do that - but every
       number that decides where the capsule stops comes from the engine.

       The surface is found by the engine's own collision query: the same downward ray the engine
       uses to ask what is under a chunk (Chunk.cpp:590) and under a droid (EntityDroid.cpp:1537),
       with the same filter (COLL_STATIC | COLL_TERRAIN | COLL_RIGID). */
    const float fStep = 1.0f / 60.0f;
    const float fGravity = -9.8f;
    float fY = 200.0f;
    float fVelocity = 0.0f;
    int iSteps = 0;

    for (iSteps = 0; iSteps < 20000; ++iSteps)
    {
        fVelocity += fGravity * fStep;
        fY += fVelocity * fStep;

        const PblVector3 vFrom(x, fY, z);
        const PblVector3 vTo(x, fY - fStep - fHalfHeight - fRadius, z);
        CollisionObject* pHit = 0;
        PblVector3 vNormal;
        const float fHit = CollisionManager::RayHit(vFrom, vTo, pHit, &vNormal, 0, 0,
                                                    COLL_STATIC | COLL_TERRAIN | COLL_RIGID, false);
        if (pHit && fHit < 1.0f)
        {
            /* The capsule's centre comes to rest a half height and a radius above the surface
               it landed on. */
            const float fSurfaceY = vFrom.y + (vTo.y - vFrom.y) * fHit;
            pOut->groundY = fSurfaceY;
            pOut->restY = fSurfaceY + fHalfHeight + fRadius;
            pOut->steps = iSteps + 1;
            return true;
        }
    }

    pOut->steps = iSteps;
    return false;
}
