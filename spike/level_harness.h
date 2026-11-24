#pragma once

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

struct GroundProbeResult
{
    /* The height the engine's own terrain reports at (x, z). NaN when the level has no terrain. */
    float terrainY;
    /* The height of the first surface the engine's own collision query finds below (x, z):
       the terrain or a collision mesh. NaN when the ray finds nothing. */
    float groundY;
    /* Where a capsule of the given radius and half height comes to rest when it is dropped on
       that surface: the surface plus the capsule's own half height and radius. */
    float restY;
    /* The number of the steps the drop took. */
    int steps;
};

/* What the engine's own spawn produced. Every number here is the engine's: the position comes
   from the entity's own matrix, the health from its own damage state, the weapon count from
   the class the engine built it from. */
struct SpawnResult
{
    /* True when the engine built an entity and it survived its own start-up. */
    bool spawned;
    /* The position the entity's own matrix reports after the spawn. */
    float x;
    float y;
    float z;
    /* The entity's own health and its maximum, as the engine's damage state holds them. */
    float health;
    float maxHealth;
    /* The number of weapons the entity's class carries, and how many of them the entity built. */
    int classWeaponCount;
    int builtWeaponCount;
    /* The number of collision bodies the engine's class built for the soldier (the head, the
       torso, the body cylinder and the lower and upper body spheres - EntitySoldierClass::
       PostReadSetup). */
    int collisionBodyCount;
    /* The number of times the entity's own physics moved it. */
    int updateCount;
};

bool LevelHarnessInit(void(__cdecl* pLog)(char const*, ...));

/* Loads a level through the engine's own loader (LoadUtil::ReadDataFile). The name is the
 * engine's: "tat/tat2.lvl". Returns true when the engine's loader accepted it. */
bool LevelHarnessLoad(const char* pcName);

/* Loads an entity class through the engine's own loader, the way the game names one.
 *
 * The name is the engine's own "file;level" form: LoadUtil::ReadDataFileOnHeap splits the name
 * at the semicolon (LoadUtil.cpp:441-451), takes the part after it as the name of the nested
 * level to read and selects it by its hash (LoadUtil.cpp:497-500). The file is named the way
 * the engine names it - relative to the data directory, with the directory separated by a
 * slash: "side/cis.lvl;cis_inf_rifleman" reads the nested level "cis_inf_rifleman" out of the
 * CIS side file.
 *
 * The loader reads only the nested level that was named: the chunks of the other levels in the
 * file are skipped by seeking over them, so the megabytes of the other classes are never
 * parsed. */
bool LevelHarnessLoadClass(const char* pcName);

/* Spawns one entity of the named class at a point in the world, through the engine's own
 * factory: EntityClass::Find, then the class's own Build (EntitySoldierClass::Build, which
 * allocates the soldier, adds it to the collision manager and activates its physics), and then
 * the same three steps Character::Spawn takes after the build (Character.cpp:1034-1039): the
 * team is set, the character is attached, and the entity is initialised.
 *
 * The class must already be loaded (LevelHarnessLoadClass) and the level must already be in
 * the world (LevelHarnessLoad): the spawn puts the entity at a point in the loaded world. */
bool LevelHarnessSpawn(const char* pcClassName, float x, float y, float z, int iPlayerId,
                        SpawnResult* pOut);

/* Runs the engine's own update for the spawned entity: the physics, the collision and the
 * weapons, at the engine's own delta. Returns the number of updates it ran. */
int LevelHarnessUpdate(float fDeltaTime);

/* Drops a capsule at (x, z) and reports where it comes to rest. The drop is a constant
 * acceleration over a fixed step; every number that decides where the capsule stops comes from
 * the engine: the terrain's height and the collision manager's own ray query. */
bool LevelHarnessDrop(float x, float z, float fRadius, float fHalfHeight, GroundProbeResult* pOut);

/* The number of collision meshes the engine's own table holds after a load. */
int LevelHarnessCollisionMeshCount();

/* ------------------------------------------------------------------ player control and camera
 *
 * The engine's own chain, as far as the harness can take it without a device layer:
 *
 *   FLInputManager::Update(dt)            the engine reads the devices and turns the raw key and
 *      |                                   mouse state into one ProcessedInputs per player
 *      v                                   (FLInputManager.cpp:398 -> RawControllerInputs::
 *   ProcessedInputs m_inputs[0]            ReadLatest, pcFLJoystick.cpp:651, which writes the
 *      |                                   buffer its mProcessDestination points at - and
 *      |                                   FLInputManager::Init wires that destination to
 *      |                                   m_inputs[0], the same object, at
 *      |                                   FLInputManager.cpp:239-241 + 139-148)
 *      v
 *   NetGame::IsTurnReady()                the no-net branch copies that buffer into the host
 *      |                                   joystick: *hostJoysticks[0] = *GetPlayerInputs(0)
 *      |                                   (NetGame.cpp:5753-5759)
 *      v
 *   NetGame::GetJoystick(0) -> hostJoysticks[0]   the PlayerController's own mJoystick
 *      |                                   (PlayerController.cpp:157)
 *      v
 *   PlayerController::Update(dt)          reads the four axes and the button bits and writes the
 *      |                                   controllable's mControlMove / mControlStrafe /
 *      |                                   mControlTurn / mControlPitch and its mControl* triggers
 *      |                                   (PlayerController.cpp:247-500)
 *      v
 *   EntitySoldier::Update(dt)             turns the controls into a heading, a speed and a pitch,
 *      |                                   and runs the physics (EntitySoldier.cpp:6520-6531 and
 *      |                                   what follows it)
 *      v
 *   CameraManager::Update(dt)             the chase camera asks the soldier for its track matrix
 *      |                                   (EntitySoldier::SetupCameraTrackMatrix,
 *      |                                   EntitySoldier.cpp:3793, from the class's own camera
 *      |                                   settings) and puts the result on the RedCamera
 *      v                                   (ChaseCamera::SetupCamera, ChaseCamera.cpp:304-431)
 *
 * The one link the browser build cannot run is the first: it has no keyboard and no mouse, because
 * the engine's device layer is DirectInput and this build's DirectInput8Create fails - see
 * browser/platform/dinput_browser.cpp, which is the honest seam for "no device on this machine".
 * So the harness injects at the link the net layer itself uses: a ProcessedInputs written by the
 * engine's own ProcessedInputs::Set, exactly as the moves of a remote player are written when they
 * arrive off the wire (NetGame.cpp:2952, 12187). The axes and the button bits are the engine's
 * layout, the units are the engine's, and everything downstream of them is the engine's code.
 */

class RedCamera;

/* One turn of the local player's input, in the engine's own layout and units:
 * the four ProcessedInputs axes (mFloatInputs[0..3]) and the two halves of its bit field. */
struct HostInput
{
    /* -1..1, the engine's own clamp (PlayerController.cpp:262-274). +1 is forward (W). */
    float fMove;
    /* -1..1, +1 is right (D). */
    float fStrafe;
    /* Turn and pitch. In mouse mode - the value the engine's own ProcessedInputs constructor puts
       in bits.mMouseMode, 2 - the engine clamps these to +-16 and treats them as a rate
       (PlayerController.cpp:276-300); the page produces them from pointer-lock deltas the way
       RawControllerInputs::ReadLatest does: pixels divided by the frame's seconds, times the
       engine's own 1/2048 mouse scale. Outside mouse mode the engine clamps them to +-1. */
    float fTurn;
    float fPitch;
    /* The held buttons, one bit each, in bits.mBoolInputs order. */
    unsigned int uiButtons;
    /* bits.mMouseMode: 2 is the engine's own default for a fresh joystick; 0 makes the engine
       treat turn and pitch as +-1 stick axes. */
    unsigned int uiMouseMode;
};

/* The button bits, named for what the engine's own PlayerController does with them. */
enum HostInputButton
{
    HOST_BUTTON_FIRE = 1u << 0,           /* mControlFire[0]  (PlayerController.cpp:465-476) */
    HOST_BUTTON_FIRE_SECONDARY = 1u << 1, /* mControlFire[1] */
    HOST_BUTTON_SPRINT = 1u << 2,         /* mControlSprint */
    HOST_BUTTON_JUMP = 1u << 3,           /* mControlJump */
    HOST_BUTTON_CROUCH = 1u << 4,         /* mControlCrouch */
    HOST_BUTTON_ZOOM = 1u << 5,           /* mControlZoom */
    HOST_BUTTON_VIEW = 1u << 6,           /* mControlView: first/third person switch */
    HOST_BUTTON_RELOAD = 1u << 7,         /* mControlReload */
    HOST_BUTTON_USE = 1u << 8,            /* mControlUse */
    /* The roll. The engine's own line is
       mOwner->mControlSpecialMove.Update(dt, (mJoystick->bits.mBoolInputs & 0x20000000) != 0)
       (PlayerController.cpp:480), and EntitySoldier::Roll is what the special move does for a
       soldier (EntitySoldier.cpp:9705). */
    HOST_BUTTON_ROLL = 1u << 29,
};

/* What the engine reports about the trooper it is controlling, and about the camera it is
 * pointing at it with. Every number is read out of the engine's own objects. */
struct TrooperState
{
    bool ok;
    /* The engine's own turn counter for this trooper. */
    int updates;
    /* The entity's own matrix: its translation, and its z axis, which is the engine's own
       convention for the direction an entity faces (EntitySoldier.cpp:851). */
    float x;
    float y;
    float z;
    float dirX;
    float dirY;
    float dirZ;
    /* The collision object's own velocity. */
    float velX;
    float velY;
    float velZ;
    /* The controllable's own control values, after the engine has scaled them: what
       PlayerController wrote and EntitySoldier then adjusted. */
    float controlMove;
    float controlStrafe;
    float controlTurn;
    float controlPitch;
    /* The soldier's own state (SoldierState: STAND, CROUCH, ROLL, JUMP, ...). */
    int iState;
    float fHealth;
    /* The controllable's own triggers, as the engine's Trigger class holds them: whether the fire
       trigger and the special-move (roll) trigger are down. */
    int iFireDown;
    int iSpecialMoveDown;
    /* The engine's own input record for the local player, as it stands: the button bits the
       engine's ProcessedInputs holds (bits.mBoolInputs) and its mouse-mode field. */
    unsigned int uiHostButtons;
    unsigned int uiHostMouseMode;
    /* The class's own roll gate and the trooper's own energy: the two numbers the engine's
       RollUsingEnergy asks before it rolls (EntitySoldier.cpp:10727-10740: the energy bar's
       SpendEnergy, then EntitySoldier::Roll's mClass->mRollSpeedFactor). */
    float fRollSpeedFactor;
    float fEnergy;
    /* The engine's camera: its world matrix's translation and z axis, and its own field of view
       and zoom, which ChaseCamera::SetupCamera sets from the soldier's class. */
    float camX;
    float camY;
    float camZ;
    float camDirX;
    float camDirY;
    float camDirZ;
    float camFov;
    float camZoom;
};

/* Puts the engine's third-person chase camera on the spawned trooper. Engine-native:
 * CameraManager::Chase and ChaseMode (CameraManager.cpp:422-450). Returns false when there is no
 * spawned trooper or no camera manager. */
bool LevelHarnessControlAttach(void);

/* Gives the engine the size of the picture the host is drawing, with the engine's own setters
 * (RedInterfaceScreen::SetDimensions on the screen and on the view's interface screen). The
 * chase camera's field of view is computed from those numbers (ChaseCamera.cpp:306-309), so a
 * headless build without them gets a NaN field of view. Returns false for a zero size. */
bool LevelHarnessSetViewport(unsigned int uiWidth, unsigned int uiHeight);

/* True once the trooper is under the engine's own player control: the engine made the
 * PlayerController for it during EntitySoldier::Init (EntitySoldier.cpp:1130 ->
 * EntitySoldier::SetupController, EntitySoldier.cpp:3705-3724) because the spawn description gave
 * it a player id, and the controller reads the engine's own host joystick. */
bool LevelHarnessControlReady(void);

/* Writes one turn of the host's input into the engine's own ProcessedInputs for the local player
 * (NetGame::GetJoystick(0)), using the engine's own ProcessedInputs::Clear and ::Set. It does not
 * step anything. */
void LevelHarnessSetInput(const HostInput* pInput);

/* Reads the input back, so a host can show what it has injected. */
void LevelHarnessGetInput(HostInput* pOut);

/* One engine turn with the player's input live: the trooper's own PlayerController's Update - the
 * exact call ControllerManager::Update makes for each player controller (ControllerManager.cpp:739-
 * 745) - and then the engine's turn threads, which is what LevelHarnessUpdate runs.
 *
 * It does not call ControllerManager::Update as a whole: that function also runs the AI managers
 * and, every sixteenth call, DoPeriodicUpdate, whose first act is to dereference
 * pOwner->mCharacter->mAdrenaline (ControllerManager.cpp:729). The harness has no Character - a
 * Character is a multiplayer player record, with a team, a class index, medals and a spawn queue -
 * so the harness runs the controller itself and leaves the AI managers alone. */
int LevelHarnessControlUpdate(float fDeltaTime);

/* The engine's own camera update: CameraManager::Update, which the game calls once per frame
 * (GameLoop.cpp:1837). */
void LevelHarnessCameraUpdate(float fDeltaTime);

/* Reads the trooper and the engine's camera. Safe at any time; ok is false before a spawn. */
bool LevelHarnessGetTrooperState(TrooperState* pOut);

/* The spawned trooper's own world matrix, in the engine's row-major order. The renderer uses
 * this exact matrix for the model instance; it does not reconstruct an orientation from a
 * heading, so a roll or a jump keeps the engine's full transform. Returns false before a spawn. */
bool LevelHarnessGetTrooperMatrix(float* pOut, int iCount);

/* The hash of the GameModel the spawned trooper's class gave it (GameModel::_uiNameHash). The
 * renderer compares this against the hash of each modl NAME in the class record, so it draws the
 * class's own high-resolution model rather than a weapon, a chunk or the low-resolution LOD. */
unsigned int LevelHarnessTrooperModelHash(void);

/* The class's own first/third-person FOV values, for the camera's diagnostic path. */
bool LevelHarnessGetTrooperFOVInputs(float* pOut);

/* The engine's own camera object - the one CameraManager::Create made and the chase camera writes
 * (CameraManager.cpp:83, ChaseCamera.cpp:428). Null before the harness initialises. */
RedCamera* LevelHarnessCamera(void);

/* The movement script: a fixed sequence of injected input, in the harness itself, so that the
 * native build and the browser build drive the same trooper with the same input by construction
 * rather than by two scripts agreeing. Records the trooper and the camera after every turn.
 *
 * Returns false when there is nothing to drive; otherwise fills pOutStates (up to iMaxStates) and
 * says how many turns it ran in *pOutCount - it reports the turns it ran even when the buffer was
 * too small, so a caller can tell the difference. */
bool LevelHarnessMovementScript(TrooperState* pOutStates, int iMaxStates, int* pOutCount);

/* The number of turns LevelHarnessMovementScript would record. */
int LevelHarnessMovementScriptLength(void);
