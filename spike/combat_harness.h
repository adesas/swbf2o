#pragma once

/* The combat harness: the engine's own blaster, the engine's own teams, and the engine's own
 * bots, measured in two builds.
 *
 * This file is a *client* of the shared level harness (spike/level_harness.h): the start-up, the
 * level load and the class load are LevelHarnessInit / LevelHarnessLoad / LevelHarnessLoadClass,
 * the player's trooper is LevelHarnessSpawn, and the player's input is HostInput through
 * LevelHarnessSetInput. Nothing here edits that harness or the browser's input/camera track: the
 * combat harness only calls what that header already exports. What it adds is the combat side,
 * which that harness has no interest in:
 *
 *   - two teams, named with the engine's own side, holding the two factions' classes:
 *     Team::SetSide (the setter the mission's Lua would call) and Team::AddClass (Team.cpp:643,
 *     the C++ entry point the mission script's SetupTeams uses);
 *   - the target bots, made the engine's own way - Team::CreateCharacter (Team.cpp:569, with
 *     iPlayerId -1, which is what makes a character an AI one) and Character::Spawn
 *     (Character.cpp:952), whose own last steps are the class's Build, the team, the character
 *     link and Entity::Init (Character.cpp:1027-1049);
 *   - the blaster, fired by the engine's own trigger chain, which the harness drives and does
 *     not replace: HostInput's fire bit -> ProcessedInputs -> PlayerController::Update ->
 *     Controllable::mControlFire[0] (PlayerController.cpp:463-470) -> Weapon::Update's IDLE
 *     branch (Weapon.cpp:1832) -> WeaponCannon::CheckFire (WeaponCannon.cpp:653) ->
 *     WeaponCannon::Fire (WeaponCannon.cpp:728) -> OrdnanceLaserClass::Build
 *     (OrdnanceLaser.cpp:195) -> a bolt object that flies and hits by itself;
 *   - the measurement of what that chain did: the bolts the engine's own collision manager is
 *     holding (CollisionManager::mOrdnance), the health each unit's own damage state holds before
 *     and after, and the hits and kills that follow from those numbers.
 *
 * There is no host-side raycast here, and no host-side damage: the bolt is the engine's
 * OrdnanceLaser and the hit is the engine's OrdnanceBullet::Collide -> ApplyDamage
 * (OrdnanceBullet.cpp:619). The harness only counts what the engine did.
 *
 * The script of the fight lives in this file (CombatHarnessRunScript) so that the native build
 * and the browser build run one script by construction rather than two scripts agreeing, which
 * is how the ground, spawn and input/camera oracles work.
 */

#include "level_harness.h"

/* Room for more targets than the harness is asked for, so that a run that asks for five still
   fits, and so the result has a fixed layout in both builds. */
#define COMBAT_MAX_TARGETS 8
#define COMBAT_MAX_UNITS (1 + COMBAT_MAX_TARGETS)
/* How many of a unit's individual hits the harness keeps. */
#define COMBAT_MAX_HITS 8
/* How many of the engine's own bolts the harness describes in detail. */
#define COMBAT_MAX_ORDNANCE 96
/* How many turns of the fight the harness records turn by turn. */
#define COMBAT_MAX_TURN_RECORDS 192
/* The size of the report the harness builds. Both builds print this same text, which makes the
   comparison a comparison of the two builds' bytes and not of two printers. */
#define COMBAT_REPORT_CAPACITY 65536

/* One fight's setup. Every string is the engine's own name for a thing: a level name, or the
   "file;class" form the engine's class loader and its Lua callback use. */
struct CombatConfig
{
    /* "tat/tat2.lvl" - LevelHarnessLoad's name. */
    const char* pcLevel;
    /* The base soldier class the faction classes are derived from: "ingame.lvl;com_inf_default". */
    const char* pcBaseClassFile;

    /* The player's faction. */
    const char* pcPlayerClass;       /* "rep_inf_ep3_rifleman" */
    const char* pcPlayerClassFile;   /* "side/rep.lvl;rep_inf_ep3_rifleman" */
    int iPlayerTeam;                 /* 1 or 2 - the two teams Team::CreateAll makes */
    int iPlayerSide;                 /* the engine's Team::Side: 3 is side_Republic */

    /* The targets' faction. */
    const char* pcTargetClass;       /* "cis_inf_rifleman" */
    const char* pcTargetClassFile;   /* "side/cis.lvl;cis_inf_rifleman" */
    int iTargetTeam;
    int iTargetSide;                 /* 4 is side_CIS */

    /* How many targets to make, and where to put them: fTargetDistance metres in front of the
       player along the direction the player was spawned facing, fTargetSpacing apart. The engine
       finds the ground under each one itself (LevelHarnessDrop -> CollisionManager::RayHit). */
    int iTargetCount;
    float fTargetDistance;
    float fTargetSpacing;
    /* How much further from the player each target stands than the one before it. The targets
       are in a line that is not square to the shot: the bolts arrive at different times, so the
       fight is an exchange and not one volley. */
    float fTargetDepth;

    /* The fight's script, in engine turns of fTurnDelta seconds. */
    float fTurnDelta;                /* the engine's own turn step: 1/30 */
    int iSettleTurns;                /* turns with no input, so the troopers come to rest */
    int iPlayerVolleyTurns;          /* turns the player holds the fire button */
    int iTailTurns;                  /* turns after the volley, so the bolts reach their targets */

    /* True makes the harness hold the targets' own fire trigger down as the engine's own AI
       holds it (AILowLevel::ProcessFire, AILowLevel.cpp:607-634). The trigger is the engine's
       own Trigger::Update; the AI's *decision* to press it is not run - see the harness. */
    bool bTargetsFire;
    bool bPlayerFire;
};

/* What the engine made of one unit, and what the fight did to it. */
struct CombatUnit
{
    bool bSpawned;
    /* "player" or "target": which side of the fight the harness put it on. */
    const char* pcRole;
    /* The name the engine's Character holds, ASCII-transcribed for the report. */
    char acName[24];
    const char* pcClass;
    int iTeam;
    /* -1 is what makes the engine's own controller a UnitMobileController
       (EntitySoldier::SetupController, EntitySoldier.cpp:3719). */
    int iPlayerId;

    /* Where the harness asked for the unit and where the engine's own matrix says it is. */
    float fSpawnX;
    float fSpawnY;
    float fSpawnZ;
    float fX0;
    float fY0;
    float fZ0;
    float fX1;
    float fY1;
    float fZ1;

    /* The engine's own damage state, before the fight and after it. */
    float fMaxHealth;
    float fHealthBefore;
    float fHealthAfter;
    bool bAliveBefore;
    bool bAliveAfter;
    int iHits;
    float fDamageTaken;
    bool bKilled;
    /* The turn of each of its first COMBAT_MAX_HITS hits, and the health either side of each. */
    int aiHitTurn[COMBAT_MAX_HITS];
    float afHitHealth[2 * COMBAT_MAX_HITS];

    /* The class's own weapons and the ones the entity built (EntitySoldier.cpp:880-926), and the
       collision bodies the class's own collision scale made (EntitySoldierClass::PostReadSetup). */
    int iClassWeaponCount;
    int iBuiltWeaponCount;
    int iCollisionBodyCount;
    /* The engine's own primary weapon: its state machine's state (Weapon::WeaponState), whether
       it is the selected weapon (Weapon::mSelectedFlag), and its ammo counter. */
    int iWeaponState;
    int iWeaponSelected;
    float fWeaponClips;
    float fWeaponCurClip;
    unsigned int uWeaponNameHash;
    char acWeaponName[32];
    /* Rounds the engine's own ammo counter has given up since the fight began:
       WeaponCannon::UpdateFire spends a round only after Fire() said true
       (WeaponCannon.cpp:529-537), so rounds gone is bolts the engine says it fired - a count of
       the shots that does not depend on the harness catching the bolt in time. */
    float fShotsFired;
    /* 1 when the engine's own weapon holds the very trigger the harness drives: the weapon's
       descriptor was built with `mTrigger = &mControlFire[mClass->mWeaponChannel[i]]`
       (EntitySoldier.cpp:901) and the player's input writes mControlFire[0]. */
    int iTriggerIsFire0;
    /* Where the engine's own aimer says the shots come from, and whether the aim is inside the
       aimer's own limits - the one gate Aimer::WeaponShouldFire puts in front of Fire (Aimer.cpp:865). */
    float fAimX;
    float fAimY;
    float fAimZ;
    /* Where the weapon points: the engine's own aimer direction (Aimer::GetDirection), which is
       the eye direction for a player soldier (EntitySoldier.cpp:3117) and the AI's own aim for a
       bot (EntitySoldier.cpp:3106). */
    float fAimDirX;
    float fAimDirY;
    float fAimDirZ;
    int iAimerWithinLimits;
};

/* One bolt, read out of the engine's own ordnance list. */
struct CombatOrdnance
{
    int iSequence;
    /* The engine's own object, as a key: the ordnance pool reuses its slots, so the same key can
       be a different bolt in a later turn, and "alive at the previous scan" is what tells them
       apart. */
    unsigned int uObjectKey;
    /* The turns the bolt was in the engine's list, from first seen to last seen. */
    int iFirstTurn;
    int iLastTurn;
    /* The bolt's own class: the hash the engine's ordnance factory knows it by (Factory::mId). */
    unsigned int uClassId;
    /* Which unit the engine says fired it: -1 when it is none of the harness's units. */
    int iOwnerSlot;
    int iOwnerTeam;
    float fX;
    float fY;
    float fZ;
    float fVelX;
    float fVelY;
    float fVelZ;
    /* The engine's own numbers for the bolt: what is left of its life, and the damage it carries
       at that moment (OrdnanceClass::GetDamage). */
    float fLifeTime;
    float fDamage;
    /* True when the engine had already decided what the bolt will hit:
       OrdnanceBullet::Update's swept ray against the world (OrdnanceBullet.cpp:508) named it. */
    int bStaticHit;
    /* The unit whose health dropped in the turn this bolt left the engine's list, when exactly
       one bolt and exactly one unit changed in that turn. -1 when the harness could not say. */
    int iAttributedVictim;
};

/* One turn of the fight, as the engine's own objects reported it. */
struct CombatTurnRecord
{
    int iTurn;
    float fTime;
    /* The player's primary weapon and its trigger, as the engine holds them. */
    int iWeaponState;
    int iWeaponSelected;
    int iTriggerDown;
    int iTriggerAction;
    float fWeaponCurClip;
    /* How many bolts the engine's collision manager was holding this turn. */
    int iOrdnanceLive;
    /* How many shots the engine's own ammo counter says the player has fired so far. */
    int iPlayerShotsFired;
    /* The same for the first target: the engine's own fire machine on a unit the harness drives
       with the engine's own trigger, so that a fight that does not go both ways says why. */
    int iTargetWeaponState;
    int iTargetTriggerDown;
    int iTargetShotsFired;
    int iTargetAimerWithinLimits;
    /* What the engine's own controller made of the harness's turn and pitch, and where the
       trooper's own matrix says it is facing (the level harness's own reading of the engine's
       state, level_harness.cpp:995-1010). */
    float fControlTurn;
    float fControlPitch;
    float fFacingX;
    float fFacingY;
    float fFacingZ;
    /* Where the player's own weapon points: the engine's aimer direction. */
    float fAimDirX;
    float fAimDirY;
    float fAimDirZ;
    /* How many bolts the engine's list lost this turn, and the sum of every unit's health. */
    int iOrdnanceGone;
    float fPlayerHealth;
    float fTargetHealthSum;
    int iTargetsAlive;
};

/* Everything one fight measured. */
struct CombatResult
{
    /* The setup, as the harness read it back. */
    char acLevel[64];
    char acPlayerClass[48];
    char acPlayerClassFile[96];
    char acTargetClass[48];
    char acTargetClassFile[96];
    int iPlayerTeam;
    int iTargetTeam;
    int iPlayerSide;
    int iTargetSide;
    int iTargetCountAsked;
    int iTargetsSpawned;
    float fTargetDistance;
    float fTargetSpacing;
    float fTargetDepth;
    float fTurnDelta;
    int iSettleTurns;
    int iPlayerVolleyTurns;
    int iTailTurns;
    int bPlayerFire;
    int bTargetsFire;

    int iTurnsRun;
    float fSimTime;

    /* The engine's own teams, after the harness filled them. */
    int iTeam1ClassCount;
    int iTeam1Members;
    int iTeam1Alive;
    int iTeam2ClassCount;
    int iTeam2Members;
    int iTeam2Alive;

    /* What the fight did, counted from the engine's own numbers. */
    int iHits;
    /* The targets the fight killed: the player's kills. The player's own death is counted apart
       (CombatResult::iPlayerDeaths), because a harness that counts both ways into one number
       says nothing about who killed whom. */
    int iKills;
    int iPlayerDeaths;
    float fDamageDealt;
    float fDamageTaken;
    int iOrdnanceTotal;
    int iOrdnancePeak;
    /* Bolts that left the engine's list on a turn when no unit's health changed: the ground, a
       wall, or a life that ran out. */
    int iOrdnanceExpired;
    /* Bolts whose victim the harness could not name, because more than one thing changed. */
    int iHitsAmbiguous;

    /* Who the engine says hurt each unit last, from the engine's own damage state
       (Damageable::mDamageOwner, set at Damageable.cpp:424). */
    int aiLastHitterTeam[COMBAT_MAX_UNITS];
    int abLastHitterWasPlayer[COMBAT_MAX_UNITS];
    /* How many times the harness's probe read the engine's ordnance list: once per engine turn
       that ran, because the engine's own loop called the probe. */
    int iScans;

    CombatUnit aUnits[COMBAT_MAX_UNITS];
    CombatOrdnance aOrdnance[COMBAT_MAX_ORDNANCE];
    int iOrdnanceRecorded;
    CombatTurnRecord aTurns[COMBAT_MAX_TURN_RECORDS];
    int iTurnRecords;
    /* The turn the first recorded turn is: the records are kept in a ring, so the report has to
       know where the ring starts. */
    int iFirstRecordTurn;

    /* Why the pool sizes the level harness states matter to a bolt: the engine allocates the
       bolt from its own pool, and a pool whose item is smaller than the object is not a pool. */
    unsigned int uiOrdnancePoolCount;
    unsigned int uiOrdnancePoolSize;
    unsigned int uiSizeofOrdnanceLaser;
    unsigned int uiSizeofOrdnanceBullet;
    unsigned int uiWeaponPoolCount;
    unsigned int uiWeaponPoolSize;
    unsigned int uiSizeofWeaponCannon;
    unsigned int uiSoldierPoolCount;
    /* The seeds the engine's own random streams hold when the fight starts. WeaponCannon::Fire
       draws from the sim stream for its spread (WeaponCannon.cpp:744-746), so a difference here
       between two builds is a difference in the start-up, not in the fight. */
    unsigned int uiSimSeed;
    unsigned int uiGlobalSeed;
    int iCollisionMeshes;
    /* The engine's own start-up said this when the fight could not be set up; empty otherwise. */
    char acError[96];
};

/* Sets the fight up: the engine's start-up and level and class loads (the level harness), the two
 * teams and their sides and classes, the player's trooper (LevelHarnessSpawn +
 * LevelHarnessControlAttach) and the target bots (Team::CreateCharacter + Character::Spawn).
 *
 * Returns false, and says why in pOut->acError, when any step of it did not happen. */
bool CombatHarnessSetup(const CombatConfig* pConfig, void(__cdecl* pLog)(char const*, ...),
                        CombatResult* pOut);

/* The live form: uses the harness's own result, which CombatHarnessLiveReport then measures.
 * Passing a second result here would leave the live report with ordnance but no unit records. */
bool CombatHarnessSetupLive(const CombatConfig* pConfig,
                            void(__cdecl* pLog)(char const*, ...));

/* One engine turn of the fight, measured: the targets' triggers first (which is where the
 * engine's own AI presses them, in ControllerManager::Update's place), then the player's input,
 * then the player's controller, then the engine's turn threads - which is where the weapon, the
 * bolt and the hit all run. */
bool CombatHarnessTurn(CombatConfig* pConfig, bool bPlayerFire, bool bTargetsFire,
                       CombatTurnRecord* pOutRecord, const HostInput* pLiveInput = 0);

/* The units of the current fight, for the renderer and the live HUD. Slot 0 is the player;
 * slots 1..iTargetCount are the bots. These are reads of the engine's own objects, not copies
 * of the harness's spawn parameters. */
int CombatHarnessUnitCount(void);
unsigned int CombatHarnessUnitModelHash(int iSlot);
bool CombatHarnessUnitMatrix(int iSlot, float* pOut, int iCount);
const char* CombatHarnessUnitClass(int iSlot);
bool CombatHarnessUnitAlive(int iSlot);
float CombatHarnessUnitHealth(int iSlot);
bool CombatHarnessUnitAmmo(int iSlot, float* pCurClip, float* pNumClips);
bool CombatHarnessSelectWeapon(int iSlot, int iChannel, int iIndex);
bool CombatHarnessRespawnPlayer(void);
bool CombatHarnessRespawnTargets(void);
void CombatHarnessSetAIFire(bool bEnable);
int CombatHarnessGetLiveOrdnance(float* pOut, int iMaxCount);
int CombatHarnessUnitHitCount(int iSlot);

/* One live turn: the host's current input is used unchanged, and the targets use the config's
 * own fire policy. This is the playable counterpart of CombatHarnessTurn's scripted aiming. */
bool CombatHarnessLiveTurn(const HostInput* pInput);

/* Reads the engine back: every unit, every bolt still in the engine's list, the teams. */
bool CombatHarnessMeasure(CombatResult* pOut);

/* The whole scripted fight: setup, the settle turns, the volley, the tail, and the measurement.
 * Fills pOut and builds the report, and returns false when any step of the setup failed. */
bool CombatHarnessRun(const CombatConfig* pConfig, void(__cdecl* pLog)(char const*, ...),
                      CombatResult* pOut);

/* Takes the harness's measurement thread out of the engine's own thread list. Done by
 * CombatHarnessRun; a host that drives the turns itself should call it when it is finished. */
bool CombatHarnessFinish(void);

/* The fight's result as JSON, built by this file, so that the native oracle and the browser
 * probe print byte-identical text for a build that agrees with itself. Valid after
 * CombatHarnessRun; empty before it. */
const char* CombatHarnessReport(void);

/* Measures the live fight now and returns the same JSON report. Unlike CombatHarnessReport,
 * which returns the last scripted run's text, this includes live turns driven by the host. */
const char* CombatHarnessLiveReport(void);

/* The default script for a named pair of factions, so that the native build and the browser
 * build ask for the same fight by construction. */
void CombatHarnessDefaultConfig(CombatConfig* pConfig);
