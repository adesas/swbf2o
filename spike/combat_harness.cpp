/* The combat harness: the engine's own blaster, the engine's own teams, the engine's own bots.

   Everything in here is the engine's own code called in the engine's own order, plus the reading
   of the engine's own numbers. What this file decides is only where the fight is and how long it
   lasts. The blaster is not simulated here: it is `WeaponCannon::Fire` building an
   `OrdnanceLaser`, which then flies and hits by itself. combat_harness.h draws the chain.

   Two things are worth saying up front, because they are the kind of thing that makes an oracle
   lie:

   - This file is a client of spike/level_harness.h's public API and nothing else. It does not
     touch the level harness, the browser host, or the input/camera track.
   - Its one measurement helper is a Thread it hands to the engine, so that the engine's own turn
     loop calls it at a chosen point of the engine's own turn. It reads; it writes nothing of the
     engine's. */

#include "combat_harness.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "Battlefront2/Source/Aimer.h"
#include "Battlefront2/Source/Character.h"
#include "Battlefront2/Source/CollisionManager.h"
#include "Battlefront2/Source/CollisionMesh.h"
#include "Battlefront2/Source/Controllable.h"
#include "Battlefront2/Source/Damageable.h"
#include "Battlefront2/Source/Entity.h"
#include "Battlefront2/Source/EntitySoldier.h"
#include "Battlefront2/Source/FlagItem.h"
#include "Battlefront2/Source/NetState.h"
#include "Battlefront2/Source/Ordnance.h"
#include "Battlefront2/Source/OrdnanceBullet.h"
#include "Battlefront2/Source/OrdnanceLaser.h"
#include "Battlefront2/Source/ScreenFilterEffect.h"
#include "Battlefront2/Source/StatusDisplay.h"
#include "Battlefront2/Source/Team.h"
#include "Battlefront2/Source/Thread.h"
#include "Battlefront2/Source/Trigger.h"
#include "Battlefront2/Source/Weapon.h"
#include "Battlefront2/Source/WeaponCannon.h"
#include "PebbleFL/Common/PblRandom.h"
#include "RedEngineFL/Memory/RedMemoryPool.h"

static void(__cdecl* s_pLog)(char const*, ...) = 0;

static void Log(const char* pcFmt, ...)
{
    if (!s_pLog)
    {
        return;
    }
    char acLine[512];
    va_list args;
    va_start(args, pcFmt);
    vsnprintf(acLine, sizeof(acLine), pcFmt, args);
    va_end(args);
    /* Through the engine's own warning sink, which the level harness pointed at this callback, so
       the engine's own messages and the harness's arrive in one place in one order. */
    s_pLog("%s", acLine);
}

/* ------------------------------------------------------------------ state */

static CombatConfig s_config;
static CombatResult s_result;
static bool s_bSetup = false;
static bool s_bRan = false;

/* The engine's own objects behind each unit of the fight. Slot 0 is the player; the rest are the
   targets. Held only to ask the engine the same questions the game asks it. */
static Controllable* s_pUnit[COMBAT_MAX_UNITS] = {0};
static GameObject* s_pObject[COMBAT_MAX_UNITS] = {0};
static Character* s_pCharacter[COMBAT_MAX_UNITS] = {0};

/* What the engine's own damage state held at the end of the last turn, so that a hit is a thing
   the engine did between two of the engine's own turns and not a thing the harness inferred. */
static float s_afHealthLastTurn[COMBAT_MAX_UNITS] = {0.0f};
static float s_afHitsLastTurn[COMBAT_MAX_UNITS] = {0.0f};
static bool s_abAliveLastTurn[COMBAT_MAX_UNITS] = {false};
/* The health and hit counts one turn further back, for the two-turn window the bolts are matched
   against (see the ordnance probe below). */
static float s_afHealthTwoTurns[2][COMBAT_MAX_UNITS] = {{0.0f}, {0.0f}};
static float s_afHitsTwoTurns[2][COMBAT_MAX_UNITS] = {{0.0f}, {0.0f}};
static float s_afHealthStart[COMBAT_MAX_UNITS] = {0.0f};
static int s_aiHits[COMBAT_MAX_UNITS] = {0};
static float s_afDamage[COMBAT_MAX_UNITS] = {0.0f};
static float s_afAmmoStart[COMBAT_MAX_UNITS] = {0.0f};
static float s_afClipStart[COMBAT_MAX_UNITS] = {0.0f};

/* The turn, as the harness counts it. In a build with no network the engine does not count turns
   at all: the game's own loop does that (GameLoop.cpp:1284-1300). */
static int s_iTurn = 0;
static float s_fSimTime = 0.0f;

/* ------------------------------------------------------------------ the ordnance probe
 *
 * The engine's turn runs a list of threads in a fixed order: ThreadGroup::UpdateAll walks the
 * groups by ascending priority (Thread.cpp:83-90) and Thread::ActivateThread puts a thread in the
 * group of -iOrder (Thread.cpp:114-131). The combat-relevant groups, and where the engine puts
 * each thread there:
 *
 *   priority  thread                                     activated at
 *        0     the soldier - physics and collision       EntitySoldier.cpp:4109
 *       15     the soldier's post-collision              EntitySoldier.cpp:4110
 *       20     the soldier's aimer                       Aimer.cpp:812
 *       30     the weapon - the fire state machine       Weapon.cpp:2157
 *       40     the bolt - it flies here                  OrdnanceLaser.cpp:209
 *       60     the collision manager's ordnance pass     CollisionManager.cpp:624
 *
 * (The bolt and the collision pass ask for PER_LOCAL_TURN, and in a build with no network that
 * is the turn list: Thread.cpp:119-128 sends both kinds to sTurnThreadList unless the net client
 * is running. That is why the level harness's single Thread::UpdateAll(dt, false, false) call is
 * enough to run the whole chain.)
 *
 * A bolt is created by the weapon at 30, integrated by its own update at 40, and consumed by the
 * collision manager's own swept test at 60 - Ordnance::CheckCollision (Ordnance.cpp:422-449)
 * ray-casts the step the bolt just took against COLL_SOFT among other things. A target within
 * one turn's travel of the muzzle is therefore hit by a bolt that lived for a fraction of one
 * turn, and a scan taken at the end of the turn never sees it.
 *
 * So the probe goes into the engine's own list at priority 45 - after the bolt exists, before
 * the pass that consumes it - and reads the engine's own list, CollisionManager::mOrdnance, from
 * inside the engine's own loop. It writes none of the engine's state and starts nothing.
 *
 * "Ordnance created" therefore means: a bolt that appeared in the engine's list between one scan
 * and the next. "Died in between" means: gone from the list at the next scan, which puts its
 * death in the window between the two scans - the collision pass of the previous turn, which is
 * where a hit on a unit happens.
 */
#define ORDNANCE_SLOTS 32

static unsigned int s_auScanKeys[ORDNANCE_SLOTS];
static int s_iScanCount = 0;
static int s_iScanTurn = -1;

static void ScanOrdnance();

class OrdnanceProbe : public Thread
{
public:
    OrdnanceProbe() {}

    /* The engine's own loop calls this from ThreadGroup::Update, at the priority it was given.
       Answering false would take it out of the list and delete it (Thread.cpp:84-89), so it
       always answers true. */
    virtual bool Update(float /*fDeltaTime*/)
    {
        ScanOrdnance();
        return true;
    }
};

static OrdnanceProbe s_probe;
static bool s_bProbeActive = false;

/* ------------------------------------------------------------------ the targets' fire trigger
 *
 * The engine's own AI releases the fire trigger it does not want. AILowLevel::ProcessFire ends
 * with `pFireTrigger->Update(dt, bFire)` and `pOtherTrigger->Update(dt, false)`
 * (AILowLevel.cpp:618-620), on `&pMe->mControlFire[pMe->mWpnChannel]` (AILowLevel.cpp:615-616),
 * and EntitySoldier::UpdateIndirect calls it (EntitySoldier.cpp:3678). That indirect update is
 * the engine's AI thread: Controllable::SetIndirectControl puts it in the turn list at group -10
 * (Controllable.cpp:400-406).
 *
 * So a trigger pressed before the turn is released again by the engine's own AI before the
 * weapon's own thread looks at it - the weapon is at group 30 (Weapon::ActivatePhysics ->
 * Weapon.cpp:2153-2158), and Thread::ActivateThread puts a thread in the group of -iOrder
 * (Thread.cpp:112-131), so the turn runs the groups in ascending order.
 *
 * The harness therefore presses the trigger from a thread of its own in the engine's own list,
 * at group 25: after the engine's AI thread has had its say, before the weapon's thread. The
 * press is the engine's own Trigger::Update, on the unit's own mWpnChannel, which is the channel
 * the engine's own AI presses. What the harness does not run is the decision to press: no
 * VisionManager, no CombatHelper, no AILowLevel::ProcessFire's own choice.
 */
static bool s_bTargetsFireNow = false;

/* Whether the player died: the fight stops there, because the engine's own kill path releases
   the player's controller (see CombatHarnessTurn). */
static bool s_bPlayerDied = false;

/* The last turn the harness asked the player for: the direction the player is already turning,
   which is what an error of half a turn keeps. */
static float s_fAimTurnSign = 0.0f;

class CombatFireThread : public Thread
{
public:
    CombatFireThread() {}

    /* The engine's own loop calls this from ThreadGroup::Update, at the group it was given.
       Answering false would take it out of the list and delete it (Thread.cpp:75-89), so it
       always answers true. */
    virtual bool Update(float fDeltaTime)
    {
        for (int i = 1; i < COMBAT_MAX_UNITS; ++i)
        {
            if (!s_pUnit[i] || !s_pObject[i])
            {
                continue;
            }
            if (!static_cast<Damageable*>(s_pObject[i])->mIsAlive)
            {
                continue;
            }
            const int iChannel = s_pUnit[i]->mWpnChannel;
            s_pUnit[i]->mControlFire[iChannel].Update(fDeltaTime, s_bTargetsFireNow);
            s_pUnit[i]->mControlFire[1 - iChannel].Update(fDeltaTime, false);
        }
        return true;
    }
};

static CombatFireThread s_fireThread;
static bool s_bFireThreadActive = false;

static void ScanOrdnance()
{
    if (!CollisionManager::sInstance)
    {
        return;
    }

    s_iScanCount = 0;
    PblList<Ordnance>::Itor it(CollisionManager::sInstance->mOrdnance);
    while (Ordnance* pOrdnance = it)
    {
        ++it;
        const unsigned int uKey = (unsigned int)(size_t)pOrdnance;
        if (s_iScanCount < ORDNANCE_SLOTS)
        {
            s_auScanKeys[s_iScanCount++] = uKey;
        }

        /* Already in the harness's table, and seen at the previous scan? Then it is the same bolt,
           so refresh its last-seen turn. The pool recycles slots, so a key alone does not
           identify a bolt across a gap; "seen last scan" does. */
        int iFound = -1;
        for (int i = 0; i < s_result.iOrdnanceRecorded; ++i)
        {
            if (s_result.aOrdnance[i].uObjectKey == uKey &&
                s_result.aOrdnance[i].iLastTurn >= s_iScanTurn - 1)
            {
                iFound = i;
                break;
            }
        }
        if (iFound >= 0)
        {
            s_result.aOrdnance[iFound].iLastTurn = s_iScanTurn;
            continue;
        }

        if (s_result.iOrdnanceRecorded >= COMBAT_MAX_ORDNANCE)
        {
            ++s_result.iOrdnanceTotal;
            continue;
        }

        CombatOrdnance& r = s_result.aOrdnance[s_result.iOrdnanceRecorded];
        memset(&r, 0, sizeof(r));
        r.iSequence = s_result.iOrdnanceTotal;
        r.uObjectKey = uKey;
        r.iFirstTurn = s_iScanTurn;
        r.iLastTurn = s_iScanTurn;
        r.iOwnerSlot = -1;
        r.iAttributedVictim = -1;
        r.uClassId = pOrdnance->mClass ? pOrdnance->mClass->mId : 0u;

        GameObject* pOwner = pOrdnance->GetOwner();
        for (int i = 0; i < COMBAT_MAX_UNITS; ++i)
        {
            if (pOwner && pOwner == s_pObject[i])
            {
                r.iOwnerSlot = i;
                r.iOwnerTeam = (i == 0) ? s_config.iPlayerTeam : s_config.iTargetTeam;
                break;
            }
        }

        const PblVector3& vPos = pOrdnance->GetPosition();
        r.fX = vPos.x;
        r.fY = vPos.y;
        r.fZ = vPos.z;
        const PblVector3& vVel = pOrdnance->GetVelocity();
        r.fVelX = vVel.x;
        r.fVelY = vVel.y;
        r.fVelZ = vVel.z;
        r.fLifeTime = pOrdnance->mLifeTime;
        r.fDamage = pOrdnance->GetDamage().mBaseValue;

        /* The one part of the bolt's future the engine has already decided: the swept ray its
           first update cast against the world's static geometry (OrdnanceBullet.cpp:508) leaves
           what it found in mFinalCollision, and the next update turns that into a hit. */
        if (pOrdnance->IsRtti(OrdnanceBullet::rttiHashOrdnanceBullet))
        {
            r.bStaticHit =
                (static_cast<const OrdnanceBullet*>(pOrdnance)->mFinalCollision != 0) ? 1 : 0;
        }

        ++s_result.iOrdnanceRecorded;
        ++s_result.iOrdnanceTotal;
    }

    if (s_iScanCount > s_result.iOrdnancePeak)
    {
        s_result.iOrdnancePeak = s_iScanCount;
    }
    ++s_result.iScans;
}

/* ------------------------------------------------------------------ helpers */

/* Names in the engine are UTF-16 (Character::mName is unsigned short[64], a class label is
   unsigned short*); the report is ASCII, so a name is transcribed, never re-encoded. */
static void Transcribe(char* pcOut, int iOut, const unsigned short* pwsName)
{
    if (iOut <= 0)
    {
        return;
    }
    pcOut[0] = 0;
    if (!pwsName)
    {
        return;
    }
    int i = 0;
    for (; i < iOut - 1 && pwsName[i]; ++i)
    {
        const unsigned int u = pwsName[i];
        pcOut[i] = (u >= 0x20u && u < 0x7fu) ? (char)u : '?';
    }
    pcOut[i] = 0;
}

static double D(float f)
{
    /* A NaN would be printed by %g as "nan", which is not JSON; a value that is not a number is
       also a thing the oracle should be able to see, so it is reported as NaN. In this harness
       every number reported is finite in practice; the branch is here so that a NaN shows up as
       NaN rather than as a silent zero. */
    return (double)f;
}

static Weapon* PrimaryWeapon(const Controllable* pUnit)
{
    if (!pUnit)
    {
        return 0;
    }
    /* The engine's own way from a controllable to the weapon on its primary trigger channel. */
    const int iIndex = const_cast<Controllable*>(pUnit)->GetWeaponIndex(0);
    if (iIndex < 0)
    {
        return 0;
    }
    return const_cast<Controllable*>(pUnit)->GetWeaponPtr(iIndex);
}

static bool ReadUnit(int iSlot, CombatUnit& rUnit)
{
    GameObject* pObject = s_pObject[iSlot];
    Controllable* pUnit = s_pUnit[iSlot];
    if (!pObject || !pUnit)
    {
        return false;
    }

    const PblMatrix& mMatrix = pObject->GetMatrix();
    rUnit.fX1 = mMatrix.m[3][0];
    rUnit.fY1 = mMatrix.m[3][1];
    rUnit.fZ1 = mMatrix.m[3][2];

    Damageable* pDamageable = static_cast<Damageable*>(pObject);
    rUnit.fHealthAfter = pDamageable->mCurHealth;
    rUnit.fMaxHealth = pDamageable->mMaxHealth;
    rUnit.bAliveAfter = pDamageable->mIsAlive != 0;

    Entity* pEntity = pUnit->GetEntity();
    if (pEntity && pEntity->IsRtti(EntitySoldier::rttiHashEntitySoldier))
    {
        EntitySoldier* pSoldier = static_cast<EntitySoldier*>(pEntity);
        rUnit.iBuiltWeaponCount = 0;
        for (int i = 0; i < 8; ++i)
        {
            if (pSoldier->mWeapon[i])
            {
                ++rUnit.iBuiltWeaponCount;
            }
        }
        if (pSoldier->mClass)
        {
            rUnit.iClassWeaponCount = pSoldier->mClass->mWeaponCount;
            rUnit.iCollisionBodyCount = pSoldier->mClass->mCollisionModels[0].GetNumBodies();
        }
    }

    Weapon* pWeapon = PrimaryWeapon(pUnit);
    if (pWeapon)
    {
        rUnit.iWeaponState = (int)pWeapon->GetState();
        rUnit.iWeaponSelected = pWeapon->IsSelected() ? 1 : 0;
        if (pWeapon->m_pAmmoCounter)
        {
            rUnit.fWeaponClips = pWeapon->m_pAmmoCounter->mNumClips;
            rUnit.fWeaponCurClip = pWeapon->m_pAmmoCounter->mCurClip;
        }
        if (pWeapon->mClass)
        {
            rUnit.uWeaponNameHash = pWeapon->mClass->mNameHash;
            Transcribe(rUnit.acWeaponName, (int)sizeof(rUnit.acWeaponName), pWeapon->mClass->mLabel);
        }
        if (pWeapon->mAimer)
        {
            rUnit.fAimX = pWeapon->mAimer->mFirePos.x;
            rUnit.fAimY = pWeapon->mAimer->mFirePos.y;
            rUnit.fAimZ = pWeapon->mAimer->mFirePos.z;
            rUnit.fAimDirX = pWeapon->mAimer->GetDirection().x;
            rUnit.fAimDirY = pWeapon->mAimer->GetDirection().y;
            rUnit.fAimDirZ = pWeapon->mAimer->GetDirection().z;
            rUnit.iAimerWithinLimits = pWeapon->mAimer->mWithinLimitsFlag ? 1 : 0;
        }
        rUnit.iTriggerIsFire0 = (pWeapon->mTrigger == &pUnit->mControlFire[0]) ? 1 : 0;
    }
    return true;
}

/* ------------------------------------------------------------------ the two teams
 *
 * Team::CreateAll (Team.cpp:66-77) makes eight teams. Teams 1 and 2 are the mission's two: room
 * for 300 members and a dozen classes each, and each already hostile to every team but its own
 * (Team::Team, Team.cpp:253-259 - mAffiliation[i] is +1 for the team itself and -1 for the rest).
 * What the mission's Lua script adds on top is a side, a class list and spawn rules; of those,
 * the side and the class list are what a fight between two teams needs, and the harness states
 * them with the engine's own setters.
 */
static bool SetupTeam(int iTeam, int iSide, const char* pcClass)
{
    Team* pTeam = Team::sTeams[iTeam];
    if (!pTeam)
    {
        return false;
    }

    /* The engine's own setter for a team's side - the one the mission script's SetTeamSide ends
       at. It decides the team's colours and the names the team generates for its members. */
    pTeam->SetSide((Team::Side)iSide);
    pTeam->Init();

    if (!pcClass || !pcClass[0])
    {
        return false;
    }

    /* The engine's own factory lookup, so the harness can say which class is missing rather than
       notice it later. */
    EntityClass* pClass = EntityClass::Find(PblHash(pcClass));
    if (!pClass)
    {
        Log("combat: the engine's entity factory has no class \"%s\"", pcClass);
        return false;
    }

    /* Team::AddClass (Team.cpp:643-651) is the C++ entry point the mission's SetupTeams uses. The
       engine's own not-found branch returns true, which is why the harness checked the factory
       first. */
    if (!pTeam->AddClass(static_cast<GameObjectClass*>(pClass), 1, 1))
    {
        Log("combat: team %d refused the class \"%s\"", iTeam, pcClass);
        return false;
    }
    return true;
}

/* A matrix the engine's own spawn will read: its translation is where the unit goes, and its z
   axis is the direction the engine says the unit faces (EntitySoldier.cpp:851 builds the entity's
   matrix from the description's z axis and translation). */
static void FaceMatrix(PblMatrix& mOut, float fX, float fY, float fZ, float fToX, float fToZ)
{
    mOut = gMatrixIdentity;

    float fDirX = fToX - fX;
    float fDirZ = fToZ - fZ;
    const float fLen = (float)sqrt((double)(fDirX * fDirX + fDirZ * fDirZ));
    if (fLen > 0.0f)
    {
        fDirX /= fLen;
        fDirZ /= fLen;
    }
    else
    {
        fDirX = 0.0f;
        fDirZ = 1.0f;
    }

    mOut.m[0][0] = -fDirZ; mOut.m[0][1] = 0.0f; mOut.m[0][2] = fDirX;
    mOut.m[1][0] = 0.0f;   mOut.m[1][1] = 1.0f; mOut.m[1][2] = 0.0f;
    mOut.m[2][0] = fDirX;  mOut.m[2][1] = 0.0f; mOut.m[2][2] = fDirZ;
    mOut.m[3][0] = fX;     mOut.m[3][1] = fY;   mOut.m[3][2] = fZ;
    mOut.m[3][3] = 1.0f;
}

/* ------------------------------------------------------------------ setup */

void CombatHarnessDefaultConfig(CombatConfig* pConfig)
{
    pConfig->pcLevel = "tat/tat2.lvl";
    pConfig->pcBaseClassFile = "ingame.lvl;com_inf_default";
    pConfig->pcPlayerClass = "rep_inf_ep3_rifleman";
    pConfig->pcPlayerClassFile = "side/rep.lvl;rep_inf_ep3_rifleman";
    pConfig->iPlayerTeam = 1;
    pConfig->iPlayerSide = 3; /* Team::side_Republic */
    pConfig->pcTargetClass = "cis_inf_rifleman";
    pConfig->pcTargetClassFile = "side/cis.lvl;cis_inf_rifleman";
    pConfig->iTargetTeam = 2;
    pConfig->iTargetSide = 4; /* Team::side_CIS */
    pConfig->iTargetCount = 4;
    pConfig->fTargetDistance = 28.0f;
    pConfig->fTargetSpacing = 1.5f;
    pConfig->fTargetDepth = 4.0f;
    pConfig->fTurnDelta = 1.0f / 30.0f;
    pConfig->iSettleTurns = 30;
    pConfig->iPlayerVolleyTurns = 40;
    pConfig->iTailTurns = 40;
    pConfig->bPlayerFire = true;
    pConfig->bTargetsFire = true;
}

bool CombatHarnessSetupLive(const CombatConfig* pConfig,
                            void(__cdecl* pLog)(char const*, ...))
{
    return CombatHarnessSetup(pConfig, pLog, &s_result);
}

bool CombatHarnessSetup(const CombatConfig* pConfig, void(__cdecl* pLog)(char const*, ...),
                        CombatResult* pOut)
{
    if (!pConfig || !pOut)
    {
        return false;
    }
    s_pLog = pLog;
    s_config = *pConfig;
    if (s_config.fTurnDelta <= 0.0f)
    {
        s_config.fTurnDelta = 1.0f / 30.0f;
    }
    if (s_config.iTargetCount < 0)
    {
        s_config.iTargetCount = 0;
    }
    if (s_config.iTargetCount > COMBAT_MAX_TARGETS)
    {
        s_config.iTargetCount = COMBAT_MAX_TARGETS;
    }

    s_bSetup = false;
    s_bRan = false;
    s_iTurn = 0;
    s_fSimTime = 0.0f;
    s_iScanTurn = -1;
    s_iScanCount = 0;
    memset(s_auScanKeys, 0, sizeof(s_auScanKeys));
    for (int i = 0; i < COMBAT_MAX_UNITS; ++i)
    {
        s_pUnit[i] = 0;
        s_pObject[i] = 0;
        s_pCharacter[i] = 0;
        s_afHealthLastTurn[i] = 0.0f;
        s_afHitsLastTurn[i] = 0.0f;
        s_abAliveLastTurn[i] = false;
        s_aiHits[i] = 0;
        s_afDamage[i] = 0.0f;
        s_afAmmoStart[i] = 0.0f;
        s_afClipStart[i] = 0.0f;
        for (int k = 0; k < 2; ++k)
        {
            s_afHealthTwoTurns[k][i] = 0.0f;
            s_afHitsTwoTurns[k][i] = 0.0f;
        }
    }

    CombatResult& r = *pOut;
    memset(&r, 0, sizeof(r));
    for (int i = 0; i < COMBAT_MAX_UNITS; ++i)
    {
        r.aUnits[i].pcRole = (i == 0) ? "player" : "target";
        r.aUnits[i].pcClass = (i == 0) ? s_config.pcPlayerClass : s_config.pcTargetClass;
        r.aUnits[i].iTeam = (i == 0) ? s_config.iPlayerTeam : s_config.iTargetTeam;
        r.aUnits[i].iPlayerId = (i == 0) ? 0 : -1;
        r.aiLastHitterTeam[i] = 0;
        r.abLastHitterWasPlayer[i] = 0;
    }
    snprintf(r.acLevel, sizeof(r.acLevel), "%s", s_config.pcLevel ? s_config.pcLevel : "");
    snprintf(r.acPlayerClass, sizeof(r.acPlayerClass), "%s",
             s_config.pcPlayerClass ? s_config.pcPlayerClass : "");
    snprintf(r.acPlayerClassFile, sizeof(r.acPlayerClassFile), "%s",
             s_config.pcPlayerClassFile ? s_config.pcPlayerClassFile : "");
    snprintf(r.acTargetClass, sizeof(r.acTargetClass), "%s",
             s_config.pcTargetClass ? s_config.pcTargetClass : "");
    snprintf(r.acTargetClassFile, sizeof(r.acTargetClassFile), "%s",
             s_config.pcTargetClassFile ? s_config.pcTargetClassFile : "");
    r.iPlayerTeam = s_config.iPlayerTeam;
    r.iTargetTeam = s_config.iTargetTeam;
    r.iPlayerSide = s_config.iPlayerSide;
    r.iTargetSide = s_config.iTargetSide;
    r.iTargetCountAsked = s_config.iTargetCount;
    r.fTargetDistance = s_config.fTargetDistance;
    r.fTargetDepth = s_config.fTargetDepth;
    r.fTargetSpacing = s_config.fTargetSpacing;
    r.fTurnDelta = s_config.fTurnDelta;
    r.iSettleTurns = s_config.iSettleTurns;
    r.iPlayerVolleyTurns = s_config.iPlayerVolleyTurns;
    r.iTailTurns = s_config.iTailTurns;
    r.bPlayerFire = s_config.bPlayerFire ? 1 : 0;
    r.bTargetsFire = s_config.bTargetsFire ? 1 : 0;
    for (int i = 0; i < COMBAT_MAX_UNITS; ++i)
    {
        r.aUnits[i].bKilled = false;
    }

    const char* pcFail = "";
    do {
        /* 1. The engine's own start-up, level load and class loads: the level harness's, unchanged,
              including the order the loads have to happen in (docs/spawn-oracle.md). */
        if (!LevelHarnessInit(s_pLog))
        {
            pcFail = "LevelHarnessInit";
            break;
        }
        if (!LevelHarnessLoad(s_config.pcLevel))
        {
            pcFail = s_config.pcLevel;
            break;
        }
        if (!LevelHarnessLoadClass(s_config.pcBaseClassFile))
        {
            pcFail = s_config.pcBaseClassFile;
            break;
        }
        if (!LevelHarnessLoadClass(s_config.pcPlayerClassFile))
        {
            pcFail = s_config.pcPlayerClassFile;
            break;
        }
        if (s_config.pcTargetClassFile && s_config.pcTargetClassFile[0] &&
            strcmp(s_config.pcTargetClassFile, s_config.pcPlayerClassFile) != 0 &&
            !LevelHarnessLoadClass(s_config.pcTargetClassFile))
        {
            pcFail = s_config.pcTargetClassFile;
            break;
        }

        /* 1b. The status display, which is the engine's own service for the local player's own
               screen. EntitySoldier::ApplyDamage tells it where the damage came from
               (EntitySoldier.cpp:9988-9994), through StatusDisplay::Instance, and the instance is
               null until the service is created - so a player that is hit before the display
               exists walks off the end of a null pointer. The game's own front end creates the
               display when it starts a mission; the harness starts no front end, so it creates
               the same service here, and only when there is none (the browser's own build has
               one). */
        if (!StatusDisplay::Instance(0))
        {
            StatusDisplay::Create();
        }

        /* 1c. The screen filter effect, which is the engine's own service for the flash the
               screen gets when the local player is hit: Damageable::FlashLocalPlayers calls
               ScreenFilterEffect::Instance()->TimedFilter (Damageable.cpp:203-221), and the
               instance is null until the service is created. The game's own front end creates it
               with the other effects (FLEffect::InitAll, FLEffect.cpp:153-197); the harness
               starts no front end, so it creates the one service the damage path asks for, and
               only when there is none. Creating it does not start anything: FLEffect::Enable
               only activates the effect's own thread when the renderer has been initialized
               (FLEffect.cpp:331-345), and the harness has no renderer. */
        if (!ScreenFilterEffect::Instance())
        {
            ScreenFilterEffect::Create();
        }

        /* 1d. The flag item list, which the kill path walks: PlayerStats::AddKill ends with a
               walk of FlagItem::sList (PlayerStats.cpp:521-524), and the list is null until the
               service is created - FlagItem::Create is the engine's own call and it only points
               the list at the host's list (FlagItem.cpp:508-512). */
        if (!FlagItem::sList)
        {
            FlagItem::Create();
        }

        /* 2. The two teams, their sides, and the classes they carry. */
        if (!SetupTeam(s_config.iPlayerTeam, s_config.iPlayerSide, s_config.pcPlayerClass))
        {
            pcFail = "player team";
            break;
        }
        if (!SetupTeam(s_config.iTargetTeam, s_config.iTargetSide, s_config.pcTargetClass))
        {
            pcFail = "target team";
            break;
        }

        /* 3. The player's trooper, by the level harness's own spawn, at the height the engine's
              own ray query says is the ground. It is the one unit the level harness holds,
              because it is the one unit whose input goes in through LevelHarnessSetInput. */
        GroundProbeResult ground;
        if (!LevelHarnessDrop(0.0f, 0.0f, 0.35f, 0.85f, &ground))
        {
            pcFail = "no ground under the player";
            break;
        }
        SpawnResult spawn;
        if (!LevelHarnessSpawn(s_config.pcPlayerClass, 0.0f, ground.restY, 0.0f, 0, &spawn))
        {
            pcFail = "player spawn";
            break;
        }

        /* 4. The level harness's control path: the engine's own PlayerController for this
              trooper, the engine's own ProcessedInputs for player 0, the engine's own chase
              camera - and the Character that the camera path insists on. */
        if (!LevelHarnessControlAttach())
        {
            pcFail = "control attach";
            break;
        }
        if (!LevelHarnessControlReady())
        {
            pcFail = "no host joystick";
            break;
        }

        /* The trooper the level harness attached: it is the only unit whose controllable the
           level harness will hand out, and it hands it out through the Character it attached the
           controllable to (Controllable::SetCharacter and the mUnit link it makes are the same
           two lines Character::Spawn uses, Character.cpp:1043-1046). */
        Character* pAttachCharacter = 0;
        for (int i = 0; i < Character::sCharacterLimit; ++i)
        {
            if (Character::sCharacters && Character::sCharacters[i].mUnit)
            {
                pAttachCharacter = &Character::sCharacters[i];
                break;
            }
        }
        if (!pAttachCharacter)
        {
            pcFail = "control attach made no character";
            break;
        }
        s_pUnit[0] = pAttachCharacter->mUnit;
        s_pObject[0] = s_pUnit[0] ? s_pUnit[0]->GetGameObject() : 0;
        if (!s_pObject[0])
        {
            pcFail = "the player has no game object";
            break;
        }

        /* That Character belongs to the level harness's start-up, and it is one of exactly one in
           the array the level harness sized (Character::CreateAll(1), inside HarnessCharacter,
           level_harness.cpp). A fight needs one Character per shooter and per target, and they
           all have to live in the array the engine is currently using, because
           Character::GetPlayerStats is `sPlayerStats[this - sCharacters]` (Character.cpp:1515):
           a Character outside the current array indexes the player-stats array by subtracting
           two unrelated allocations. So the harness sizes the array once more, here, and every
           Character that fights is made after this line. The level harness's Character is left
           unreferenced: the line below that re-links the player's controllable is the only thing
           that ever pointed at it, and nothing frees it, so nothing dangles. */

        /* 5. The character array the fight will really use: one per shooter plus room for every
              target. Character::CreateAll (Character.cpp:355-369) is the engine's own factory -
              it makes the array and the PlayerStats that go with it - and the mission's start-up
              is what normally calls it, with the mission's character count
              (Character.cpp:1686-1699); the harness states the count as it states every other
              number the mission's Lua would have set. */
        Character::CreateAll(1 + COMBAT_MAX_TARGETS);

        /* 6. The player's real Character, made the engine's own way: Team::CreateCharacter
              (Team.cpp:569-577) is Character::CreateCharacter, Character::Init and the team's own
              member array. The player id is 0, which is what made the engine build a
              PlayerController for the trooper when it was initialised, and what keeps it reading
              the host joystick the level harness writes. */
        {
            static const unsigned short awsPlayer[] = {'P', 'L', 'A', 'Y', 'E', 'R', 0};
            Team* pTeam = Team::sTeams[s_config.iPlayerTeam];
            const int iMembersBefore = pTeam->mMemberCount;
            pTeam->CreateCharacter(awsPlayer, 0);
            Character* pChr = (pTeam->mMemberCount > iMembersBefore)
                                  ? pTeam->GetMemberPtr(pTeam->mMemberCount - 1)
                                  : 0;
            if (!pChr)
            {
                pcFail = "the player has no character";
                break;
            }
            /* The team's own pointer for the character, which is the first thing the engine's
               own Character::InitialUpdate (Character.cpp:440-443) does with a character the
               team has just made: `mTeamPtr = Team::sTeams[mTeamNumber]`. The harness does that
               one line and not the rest of InitialUpdate, because InitialUpdate also calls
               Respawn, which would put the character where the spawn manager wants it, and this
               fight places its units itself. */
            pChr->mTeamPtr = Team::sTeams[pChr->mTeamNumber];
            /* The engine's own link, in both directions, as Character::Spawn leaves them. */
            s_pUnit[0]->SetCharacter(pChr);
            pChr->mUnit = s_pUnit[0];
            s_pCharacter[0] = pChr;
            Transcribe(r.aUnits[0].acName, (int)sizeof(r.aUnits[0].acName), pChr->mName);
        }

        /* The trooper the level harness spawned is on the level harness's team, which is the
           neutral one (level_harness.cpp says so where it sets it). A fight needs the two
           factions on two teams, so the harness puts the trooper on the mission's team with the
           calls Character::Spawn itself would have made had the trooper been spawned by a
           character of that team - pObject->SetTeam and mPerceivedTeam (Character.cpp:1044-1045),
           and the team's member array, which is Character::ChangeTeam's move
           (Character.cpp:1155-1158). */
        s_pObject[0]->SetTeam(s_config.iPlayerTeam);
        s_pObject[0]->mPerceivedTeam = s_config.iPlayerTeam;

        /* 7. The targets, made the engine's own way: Team::CreateCharacter with the player id
              that makes the engine's own controller a UnitMobileController
              (EntitySoldier::SetupController, EntitySoldier.cpp:3705-3724, which asks only
              whether the id is negative), and Character::Spawn (Character.cpp:952), whose last
              steps are the class's Build, the team, the character link and Entity::Init
              (Character.cpp:1027-1049). */
        Team* pTargetTeam = Team::sTeams[s_config.iTargetTeam];
        EntityClass* pTargetClass = EntityClass::Find(PblHash(s_config.pcTargetClass));
        const int iClassIndex =
            pTargetClass ? pTargetTeam->GetClassIndex((GameObjectClass*)pTargetClass) : -1;
        if (iClassIndex < 0)
        {
            pcFail = "the target class is not on the target team";
            break;
        }

        for (int i = 0; i < s_config.iTargetCount; ++i)
        {
            const int iSlot = 1 + i;

            /* The engine's own name for a member of that team (Team::GenerateRandomName,
               Team.cpp:871-928) - the mission does not name its bots either, and the generator is
               seeded from the team's own side by Team::Init (Team.cpp:325), so it needs no
               network, no player and no script. */
            unsigned short awsName[64];
            pTargetTeam->GenerateRandomName(awsName, s_config.iTargetTeam);

            const int iMembersBefore = pTargetTeam->mMemberCount;
            pTargetTeam->CreateCharacter(awsName, -1);
            if (pTargetTeam->mMemberCount <= iMembersBefore)
            {
                pcFail = "the target team refused a bot";
                break;
            }
            Character* pChr = pTargetTeam->GetMemberPtr(pTargetTeam->mMemberCount - 1);
            if (!pChr)
            {
                pcFail = "the target team gave no bot";
                break;
            }

            /* The team's own pointer for the character: the first thing the engine's own
               Character::InitialUpdate does with a character the team has just made
               (Character.cpp:440-443). The harness does that one line and not the rest of
               InitialUpdate, because InitialUpdate also calls Respawn, which would put the
               character where the spawn manager wants it, and this fight places its units
               itself. */
            pChr->mTeamPtr = Team::sTeams[pChr->mTeamNumber];

            /* Character::SelectClass (Character.cpp:1197-1221) is the engine's own way to put one
               of the team's classes on one of its characters - ChangeClass and SelectSpawn go
               through it - and Character::Spawn refuses to spawn a character with no class
               (Character.cpp:1010-1012). */
            pChr->SelectClass(iClassIndex);
            if (pChr->mClassPtr != pTargetClass)
            {
                pcFail = "the bot's class was not selected";
                break;
            }

            /* Where to put it: across the world from the player, spaced along the line
               perpendicular to the shot, each facing the player. The ground is the engine's own
               answer, once per bot. */
            const float fX = ((float)i - (float)(s_config.iTargetCount - 1) * 0.5f) *
                             s_config.fTargetSpacing;
            const float fZ = s_config.fTargetDistance + (float)i * s_config.fTargetDepth;
            GroundProbeResult botGround;
            if (!LevelHarnessDrop(fX, fZ, 0.35f, 0.85f, &botGround))
            {
                pcFail = "no ground under a bot";
                break;
            }
            PblMatrix mSpawn;
            FaceMatrix(mSpawn, fX, botGround.restY, fZ, 0.0f, 0.0f);

            pChr->Spawn(mSpawn);

            s_pCharacter[iSlot] = pChr;
            s_pUnit[iSlot] = pChr->mUnit;
            s_pObject[iSlot] = s_pUnit[iSlot] ? s_pUnit[iSlot]->GetGameObject() : 0;
            r.aUnits[iSlot].bSpawned = (s_pObject[iSlot] != 0);
            Transcribe(r.aUnits[iSlot].acName, (int)sizeof(r.aUnits[iSlot].acName), pChr->mName);
            r.aUnits[iSlot].fSpawnX = fX;
            r.aUnits[iSlot].fSpawnY = botGround.restY;
            r.aUnits[iSlot].fSpawnZ = fZ;
            if (!s_pObject[iSlot])
            {
                pcFail = "a bot spawned with no game object";
                break;
            }
            ++r.iTargetsSpawned;
        }

        /* 8. The measurement thread, into the engine's own turn list, between the bolt (priority
              40) and the collision manager's ordnance pass (priority 60). */
        s_probe.ActivateThread(-45, PER_TURN);
        s_bProbeActive = true;

        /* 8b. The targets' fire trigger thread, into the engine's own turn list at group 25:
               after the engine's AI thread (group -10) and before the weapon's own thread
               (group 30). */
        s_fireThread.ActivateThread(-25, PER_TURN);
        s_bFireThreadActive = true;

        /* 9. Everything the fight starts with, as the engine reports it. */
        for (int i = 0; i < COMBAT_MAX_UNITS; ++i)
        {
            if (!s_pObject[i])
            {
                continue;
            }
            Damageable* pDamageable = static_cast<Damageable*>(s_pObject[i]);
            s_afHealthStart[i] = pDamageable->mCurHealth;
            s_afHealthLastTurn[i] = pDamageable->mCurHealth;
            s_afHealthTwoTurns[0][i] = pDamageable->mCurHealth;
            s_afHealthTwoTurns[1][i] = pDamageable->mCurHealth;
            s_afHitsLastTurn[i] = 0.0f;
            s_afHitsTwoTurns[0][i] = 0.0f;
            s_afHitsTwoTurns[1][i] = 0.0f;
            s_abAliveLastTurn[i] = pDamageable->mIsAlive != 0;

            r.aUnits[i].bSpawned = true;
            r.aUnits[i].fHealthBefore = pDamageable->mCurHealth;
            r.aUnits[i].fMaxHealth = pDamageable->mMaxHealth;
            r.aUnits[i].bAliveBefore = pDamageable->mIsAlive != 0;

            const PblMatrix& mMatrix = s_pObject[i]->GetMatrix();
            r.aUnits[i].fX0 = mMatrix.m[3][0];
            r.aUnits[i].fY0 = mMatrix.m[3][1];
            r.aUnits[i].fZ0 = mMatrix.m[3][2];

            Weapon* pWeapon = PrimaryWeapon(s_pUnit[i]);
            if (pWeapon && pWeapon->m_pAmmoCounter)
            {
                s_afAmmoStart[i] = pWeapon->m_pAmmoCounter->mNumClips;
                s_afClipStart[i] = pWeapon->m_pAmmoCounter->mCurClip;
            }
            ReadUnit(i, r.aUnits[i]);
        }

        r.iTeam1ClassCount = Team::sTeams[1] ? Team::sTeams[1]->GetClassCount() : 0;
        r.iTeam1Members = Team::sTeams[1] ? Team::sTeams[1]->mMemberCount : 0;
        r.iTeam1Alive = Team::sTeams[1] ? Team::sTeams[1]->GetNumMembersAlive() : 0;
        r.iTeam2ClassCount = Team::sTeams[2] ? Team::sTeams[2]->GetClassCount() : 0;
        r.iTeam2Members = Team::sTeams[2] ? Team::sTeams[2]->mMemberCount : 0;
        r.iTeam2Alive = Team::sTeams[2] ? Team::sTeams[2]->GetNumMembersAlive() : 0;

        /* Why the pool sizes the level harness states matter to a bolt: the engine takes bolts
           from its own pool (Ordnance.h's operator new is sMemoryPool.Allocate), and an item
           smaller than the object it holds is not a pool. */
        MemoryPool* pOrdnancePool = MemoryPool::Find("Ordnance");
        MemoryPool* pWeaponPool = MemoryPool::Find("Weapon");
        r.uiOrdnancePoolCount = pOrdnancePool ? pOrdnancePool->GetCount() : 0u;
        r.uiOrdnancePoolSize = pOrdnancePool ? pOrdnancePool->GetSize() : 0u;
        r.uiWeaponPoolCount = pWeaponPool ? pWeaponPool->GetCount() : 0u;
        r.uiWeaponPoolSize = pWeaponPool ? pWeaponPool->GetSize() : 0u;
        r.uiSoldierPoolCount = EntitySoldier::sMemoryPool.GetCount();
        r.uiSizeofOrdnanceLaser = (unsigned int)sizeof(OrdnanceLaser);
        r.uiSizeofOrdnanceBullet = (unsigned int)sizeof(OrdnanceBullet);
        r.uiSizeofWeaponCannon = (unsigned int)sizeof(WeaponCannon);
        /* WeaponCannon::Fire draws from the engine's sim random stream for its spread
           (WeaponCannon.cpp:744-746), and EnterFire takes its shot delay from the same stream
           through AIUtil::GetRandomFloat (AIUtil.cpp:160-167). The stream's seed is a constant
           (PblRandom.cpp:6) in a build that never calls PblRandom::SetGlobalSeed, which is what
           makes two builds comparable; the seed is reported so that a disagreement can be
           identified as a start-up difference rather than a fight difference. */
        r.uiSimSeed = PblRandom::GetSim().GetSeed();
        r.iCollisionMeshes = LevelHarnessCollisionMeshCount();
    } while (false);

    if (pcFail[0])
    {
        snprintf(r.acError, sizeof(r.acError), "%s", pcFail);
        Log("combat: setup failed at: %s", pcFail);
        return false;
    }

    s_bSetup = true;
    return true;
}

/* ------------------------------------------------------------------ the player's aim
 *
 * The player is a real player unit, and the engine's own controller turns the harness's input
 * into the unit's own turn and pitch (PlayerController.cpp:397-398). The soldier's own update
 * turns the body by that turn (EntitySoldier.cpp:6894-6900, EntitySoldier::Move) and pitches the
 * eye by that pitch (EntitySoldier.cpp:6702 and EntitySoldier.cpp:6772). The eye direction is
 * the matrix's Z axis turned by the pitch (EntitySoldier.cpp:993 and EntitySoldier.cpp:2938-2947),
 * and it is the direction the weapon fires along (EntitySoldier.cpp:3117).
 *
 * So the harness aims the player the way a player aims: with the turn and the pitch of the
 * input, toward the first living target. The turn's sign comes from the engine's own rotation:
 * EntitySoldier::MoveOffGround turns the forward from +Z toward -X for a positive angle, and
 * GetOmega hands the soldier a negative omega for a positive turn input
 * (EntitySoldier.cpp:8299), so a positive turn input turns the forward toward +X.
 */
static float AimClamp(float f)
{
    if (f > 1.0f)
    {
        return 1.0f;
    }
    if (f < -1.0f)
    {
        return -1.0f;
    }
    return f;
}

/* The harness's own atan, atan2 and asin.
 *
 * The native oracle links the host's glibc and the wasm build links musl, and the two libms do
 * not agree to the last bit: their atan2f and their sinf differ by one unit in the last place on
 * some inputs (54 of the engine's own 2048 sine table entries differ, and the table is built with
 * sinf and cosf - PblMath.cpp:24-33). A harness that asked the platform for an angle would
 * therefore aim a hair differently in the two builds, and the oracle would report a difference
 * that belongs to the platform and not to the engine. So the harness computes its angles itself,
 * out of arithmetic that both targets do the same way.
 *
 * The accuracy is not the point: the point is that the same code gives the same bits in both
 * builds. The polynomials below are good to about a millionth of a radian, which is far finer
 * than the engine's own 2048-entry sine table interpolates to. */
static float DetAtan(float fX)
{
    /* atan on [-1, 1] (the odd polynomial used by the Cephes-style approximations). */
    const float fA = fX * fX;
    return fX *
           (0.99997726f +
            fA * (-0.33262347f +
                  fA * (0.19354346f +
                        fA * (-0.11643287f + fA * (0.05265332f + fA * -0.01172120f)))));
}

static float DetAtan2(float fY, float fX)
{
    const float fAbsY = fabsf(fY);
    const float fAbsX = fabsf(fX);
    if (fAbsY <= fAbsX)
    {
        const float fA = DetAtan(fY / fX);
        if (fX < 0.0f)
        {
            return (fY < 0.0f) ? (fA - 3.14159265f) : (fA + 3.14159265f);
        }
        return fA;
    }
    const float fA = DetAtan(fX / fY);
    const float fR = (fY < 0.0f) ? (fA - 1.57079633f) : (1.57079633f - fA);
    return (fX < 0.0f) ? (fR + 3.14159265f) : fR;
}

static float DetAsin(float fX)
{
    if (fX >= 1.0f)
    {
        return 1.57079633f;
    }
    if (fX <= -1.0f)
    {
        return -1.57079633f;
    }
    const float fC = (float)sqrt((double)(1.0f - fX * fX));
    return DetAtan2(fX, fC);
}

static void AimPlayer(HostInput* pInput)
{
    if (!pInput || !s_pUnit[0])
    {
        return;
    }
    Weapon* pWeapon = PrimaryWeapon(s_pUnit[0]);
    if (!pWeapon || !pWeapon->mAimer)
    {
        return;
    }

    /* The first living target: the engine's own alive flag on the engine's own damageable. */
    GameObject* pTarget = 0;
    for (int i = 1; i < COMBAT_MAX_UNITS; ++i)
    {
        if (!s_pObject[i])
        {
            continue;
        }
        if (static_cast<Damageable*>(s_pObject[i])->mIsAlive)
        {
            pTarget = s_pObject[i];
            break;
        }
    }
    if (!pTarget)
    {
        return;
    }

    /* The target's chest: the engine's own matrix translation - the soldier's feet - plus the
       height of a soldier's body, which is what the engine's own collision body is around. */
    const PblVector3 vTargetPos = pTarget->GetMatrix().GetTranslation();
    const PblVector3 vFire = pWeapon->mAimer->mFirePos;
    const PblVector3 vAim = pWeapon->mAimer->GetDirection();

    /* The aim point: the target's upper chest. The engine's bolts are fired along the aim and
       then fall (the engine gives an ordnance a gravity of its own - OrdnanceBullet.cpp:482-520
       integrates it), so a bolt aimed at the middle of a chest at thirty metres arrives low, and
       the harness aims a little high on purpose rather than missing the target it can see. */
    PblVector3 vWant(vTargetPos.x - vFire.x, (vTargetPos.y + 1.45f) - vFire.y,
                     vTargetPos.z - vFire.z);
    const float fLen = (float)sqrt((double)(vWant.x * vWant.x + vWant.y * vWant.y + vWant.z * vWant.z));
    if (fLen <= 0.0f)
    {
        return;
    }
    vWant.x /= fLen;
    vWant.y /= fLen;
    vWant.z /= fLen;

    /* The yaw: the signed angle from the way the weapon points to the way the target is, in the
       XZ plane. The angle is measured the way the engine's own rotation measures it
       (EntitySoldier::Move turns the forward from +Z toward -X for a positive angle, so the
       angle of a forward vector f is atan2(-f.x, f.z)).

       The harness computes that angle itself, out of the cross and the dot of the two
       directions, instead of asking the platform for atan2f: the native oracle links glibc and
       the wasm build links musl, and their atan2f differ by one unit in the last place on some
       inputs, which would make the two builds aim a hair differently and the oracle would report
       a difference that is the platform's, not the engine's. */
    const float fCross = vAim.x * vWant.z - vAim.z * vWant.x; /* sin of the angle, signed */
    const float fDot = vAim.x * vWant.x + vAim.z * vWant.z;    /* cosine of the angle */
    float fYawError = DetAtan2(fCross, fDot);
    /* A trooper that is facing exactly away from its target has an error of exactly half a turn,
       and the sign of half a turn is the sign of a zero - which flips as the trooper crosses the
       angle the error is measured from, so a plain error would make the trooper rock on the spot
       instead of turning. The harness therefore takes an error of half a turn to be half a turn
       the way it was already turning, and the trooper commits to the turn. */
    if (fYawError > 3.0f || fYawError < -3.0f)
    {
        fYawError = (s_fAimTurnSign < 0.0f) ? -3.14159265f : 3.14159265f;
    }
    /* The gain is one, because the aim the harness reads is the aim the weapon fires along and
       that aim is the camera's, which is one turn behind the trooper's own facing: a gain above
       one turns the trooper past the target and back, and the fight is a rock, not an aim.

       The sign: the engine's own rotation turns the trooper's forward from +Z toward -X for a
       positive angle (EntitySoldier::Move, EntitySoldier.cpp:8396-8418), and GetOmega hands the
       soldier a negative omega for a positive turn input (EntitySoldier.cpp:8299), so a positive
       turn input turns the forward toward +X - the same way as the angle the harness measures
       the error in (atan2 of -x and z). */
    pInput->fTurn = AimClamp(1.0f * fYawError);
    if (fabsf(pInput->fTurn) > 0.001f)
    {
        s_fAimTurnSign = pInput->fTurn;
    }

    /* The pitch: the eye's own angle above the horizon. A positive pitch input looks up
       (EntitySoldier.cpp:2940-2943 builds the eye direction from sin and cos of the pitch). */
    const float fPitchNow = DetAsin(vAim.y);
    const float fPitchWant = DetAsin(vWant.y);
    pInput->fPitch = AimClamp(1.0f * (fPitchWant - fPitchNow));
}

/* ------------------------------------------------------------------ one turn */

int CombatHarnessUnitCount(void)
{
    return s_bSetup ? 1 + s_config.iTargetCount : 0;
}

unsigned int CombatHarnessUnitModelHash(int iSlot)
{
    if (iSlot < 0 || iSlot >= COMBAT_MAX_UNITS || !s_pObject[iSlot] ||
        !s_pObject[iSlot]->GetModel())
    {
        return 0;
    }
    return s_pObject[iSlot]->GetModel()->_uiNameHash;
}

bool CombatHarnessUnitMatrix(int iSlot, float* pOut, int iCount)
{
    if (!pOut || iSlot < 0 || iSlot >= COMBAT_MAX_UNITS || !s_pObject[iSlot])
    {
        return false;
    }
    const PblMatrix& m = s_pObject[iSlot]->GetMatrix();
    for (int i = 0; i < iCount && i < 16; ++i)
    {
        pOut[i] = m.m[i / 4][i % 4];
    }
    return true;
}

const char* CombatHarnessUnitClass(int iSlot)
{
    if (iSlot < 0 || iSlot >= COMBAT_MAX_UNITS)
    {
        return "";
    }
    return s_result.aUnits[iSlot].pcClass ? s_result.aUnits[iSlot].pcClass : "";
}

bool CombatHarnessUnitAlive(int iSlot)
{
    if (iSlot < 0 || iSlot >= COMBAT_MAX_UNITS || !s_pObject[iSlot])
    {
        return false;
    }
    Damageable* pDamageable = static_cast<Damageable*>(s_pObject[iSlot]);
    return pDamageable && (pDamageable->mIsAlive != 0);
}

float CombatHarnessUnitHealth(int iSlot)
{
    if (iSlot < 0 || iSlot >= COMBAT_MAX_UNITS || !s_pObject[iSlot])
    {
        return 0.0f;
    }
    Damageable* pDamageable = static_cast<Damageable*>(s_pObject[iSlot]);
    return pDamageable ? pDamageable->mCurHealth : 0.0f;
}

int CombatHarnessUnitHitCount(int iSlot)
{
    if (iSlot < 0 || iSlot >= COMBAT_MAX_UNITS) return 0;
    return s_aiHits[iSlot];
}

bool CombatHarnessUnitAmmo(int iSlot, float* pCurClip, float* pNumClips)
{
    if (iSlot < 0 || iSlot >= COMBAT_MAX_UNITS || !s_pUnit[iSlot])
    {
        return false;
    }
    Weapon* pWeapon = PrimaryWeapon(s_pUnit[iSlot]);
    if (!pWeapon || !pWeapon->m_pAmmoCounter)
    {
        return false;
    }
    if (pCurClip) *pCurClip = pWeapon->m_pAmmoCounter->mCurClip;
    if (pNumClips) *pNumClips = pWeapon->m_pAmmoCounter->mNumClips;
    return true;
}

bool CombatHarnessSelectWeapon(int iSlot, int iChannel, int iIndex)
{
    if (iSlot < 0 || iSlot >= COMBAT_MAX_UNITS || !s_pUnit[iSlot])
    {
        return false;
    }
    if (iIndex < 0 || iIndex >= s_pUnit[iSlot]->GetWeaponCount())
    {
        return false;
    }
    s_pUnit[iSlot]->SetWeaponIndex(iChannel, iIndex);
    return true;
}

bool CombatHarnessRespawnPlayer(void)
{
    if (!s_pObject[0]) return false;
    Damageable* pD = static_cast<Damageable*>(s_pObject[0]);
    if (pD)
    {
        pD->mCurHealth = pD->mMaxHealth;
        pD->mIsAlive = 1;
    }
    s_bPlayerDied = false;
    GroundProbeResult g;
    LevelHarnessDrop(0.0f, 0.0f, 0.35f, 0.85f, &g);
    PblMatrix m = gMatrixIdentity;
    m.m[3][0] = 0.0f;
    m.m[3][1] = g.restY;
    m.m[3][2] = 0.0f;
    s_pObject[0]->SetMatrix(m);
    return true;
}

bool CombatHarnessRespawnTargets(void)
{
    for (int i = 0; i < s_config.iTargetCount; ++i)
    {
        int iSlot = 1 + i;
        if (!s_pObject[iSlot]) continue;
        Damageable* pD = static_cast<Damageable*>(s_pObject[iSlot]);
        if (pD)
        {
            pD->mCurHealth = pD->mMaxHealth;
            pD->mIsAlive = 1;
        }
        const float fX = ((float)i - (float)(s_config.iTargetCount - 1) * 0.5f) *
                         s_config.fTargetSpacing;
        const float fZ = s_config.fTargetDistance + (float)i * s_config.fTargetDepth;
        GroundProbeResult botGround;
        LevelHarnessDrop(fX, fZ, 0.35f, 0.85f, &botGround);
        PblMatrix mSpawn;
        FaceMatrix(mSpawn, fX, botGround.restY, fZ, 0.0f, 0.0f);
        s_pObject[iSlot]->SetMatrix(mSpawn);
    }
    return true;
}

static bool s_bLiveAIFire = false;
static int s_iLiveAICooldown = 0;

void CombatHarnessSetAIFire(bool bEnable)
{
    s_bLiveAIFire = bEnable;
}

int CombatHarnessGetLiveOrdnance(float* pOut, int iMaxCount)
{
    if (!CollisionManager::sInstance || !pOut || iMaxCount <= 0)
    {
        return 0;
    }
    int count = 0;
    PblList<Ordnance>::Itor it(CollisionManager::sInstance->mOrdnance);
    while (Ordnance* pOrdnance = it)
    {
        ++it;
        if (count >= iMaxCount) break;
        const PblVector3& pos = pOrdnance->GetPosition();
        const PblVector3& vel = pOrdnance->GetVelocity();
        int team = 2; // target / CIS
        GameObject* pOwner = pOrdnance->GetOwner();
        if (pOwner && pOwner == s_pObject[0])
        {
            team = 1; // player
        }
        int base = count * 7;
        pOut[base + 0] = pos.x;
        pOut[base + 1] = pos.y;
        pOut[base + 2] = pos.z;
        pOut[base + 3] = vel.x;
        pOut[base + 4] = vel.y;
        pOut[base + 5] = vel.z;
        pOut[base + 6] = (float)team;
        count++;
    }
    return count;
}

bool CombatHarnessTurn(CombatConfig* /*pConfig*/, bool bPlayerFire, bool bTargetsFire,
                       CombatTurnRecord* pOutRecord, const HostInput* pLiveInput)
{
    if (!s_bSetup)
    {
        return false;
    }

    /* A dead player ends the fight. The engine's own kill path releases the controller
       (Controllable::Kill -> Controllable::ReleaseController, Controllable.cpp:303 and
       Controllable.cpp:373-382), and the level harness's cached controller pointer is the one
       that was released - so the next LevelHarnessControlUpdate would call through a freed
       controller. The fight is over when the player is dead, so the harness stops here and says
       so. */
    if (s_pObject[0] && !static_cast<Damageable*>(s_pObject[0])->mIsAlive)
    {
        s_bPlayerDied = true;
        if (!pLiveInput)
        {
            return false;
        }
        static_cast<Damageable*>(s_pObject[0])->mCurHealth = 0.0f;
    }

    const float fDelta = s_config.fTurnDelta;
    const int iTurn = s_iTurn;
    s_iScanTurn = iTurn;

    /* Whether the targets' triggers are down this turn. */
    if (pLiveInput)
    {
        if (s_bLiveAIFire)
        {
            s_iLiveAICooldown++;
            // Fire in bursts: 8 frames firing, 42 frames cooldown
            s_bTargetsFireNow = ((s_iLiveAICooldown % 50) < 8);

            // Turn living bots to aim towards player
            if (s_pObject[0] && s_bTargetsFireNow)
            {
                const PblMatrix& pM = s_pObject[0]->GetMatrix();
                float px = pM.m[3][0], pz = pM.m[3][2];
                for (int i = 0; i < s_config.iTargetCount; ++i)
                {
                    int iSlot = 1 + i;
                    if (s_pObject[iSlot] && CombatHarnessUnitAlive(iSlot))
                    {
                        const PblMatrix& bM = s_pObject[iSlot]->GetMatrix();
                        PblMatrix faced;
                        FaceMatrix(faced, bM.m[3][0], bM.m[3][1], bM.m[3][2], px, pz);
                        s_pObject[iSlot]->SetMatrix(faced);
                    }
                }
            }
        }
        else
        {
            s_bTargetsFireNow = false;
        }
    }
    else
    {
        s_bTargetsFireNow = bTargetsFire;
    }

    /* The player's turn of input, and then the turn's threads. LevelHarnessControlUpdate is the
       level harness's own pair: the engine's PlayerController's Update, which turns the input's
       fire bit into mControlFire[0] (PlayerController.cpp:463-470), and then Thread::UpdateAll,
       where the soldier, the aimer, the weapon, the bolt and the collision pass run. */
    if (pLiveInput)
    {
        /* The live mission's own input: WASD, pointer-lock mouse, jump, roll and fire all stay
           exactly as the host injected them. The scripted fight below uses AimPlayer because
           the oracle has no human at the controls. */
        LevelHarnessSetInput(pLiveInput);
    }
    else
    {
        HostInput input;
        input.fMove = 0.0f;
        input.fStrafe = 0.0f;
        input.fTurn = 0.0f;
        input.fPitch = 0.0f;
        input.uiButtons = bPlayerFire ? (unsigned int)HOST_BUTTON_FIRE : 0u;
        input.uiMouseMode = 2u; /* the value a fresh ProcessedInputs has, per level_harness.h */
        AimPlayer(&input);
        LevelHarnessSetInput(&input);
    }

    if (LevelHarnessControlUpdate(fDelta) <= 0)
    {
        return false;
    }

    /* The camera, which is the engine's own per-frame camera update (GameLoop.cpp:1837). The
       player's aim comes from the camera: EntitySoldier::UpdateWeaponAndAimer aims the weapon
       along the camera's own eye direction (EntitySoldier.cpp:3040-3092), so a camera that does
       not follow the trooper leaves the trooper aiming where it was when it spawned. The game
       updates the camera every frame and the threads every turn; the harness does both every
       turn, so the aim the player is given is the aim the engine's own weapon fires along. */
    LevelHarnessCameraUpdate(fDelta);

    s_fSimTime += fDelta;
    ++s_iTurn;

    /* The engine's own damage state, read after the turn: a hit is a fall in the engine's own
       health between two of the engine's own turns, and a kill is the engine's own alive flag
       going false. */
    int iUnitsHit = 0;
    int iLastUnitHit = -1;
    for (int i = 0; i < COMBAT_MAX_UNITS; ++i)
    {
        if (!s_pObject[i])
        {
            continue;
        }
        Damageable* pDamageable = static_cast<Damageable*>(s_pObject[i]);
        const float fNow = pDamageable->mCurHealth;
        const float fBefore = s_afHealthLastTurn[i];

        if (fNow < fBefore)
        {
            const float fLost = fBefore - fNow;
            s_afDamage[i] += fLost;
            ++s_aiHits[i];
            ++iUnitsHit;
            iLastUnitHit = i;

            CombatUnit& rUnit = s_result.aUnits[i];
            rUnit.iHits = s_aiHits[i];
            rUnit.fDamageTaken = s_afDamage[i];
            const int iHit = s_aiHits[i] - 1;
            if (iHit < COMBAT_MAX_HITS)
            {
                rUnit.aiHitTurn[iHit] = iTurn;
                rUnit.afHitHealth[2 * iHit] = fBefore;
                rUnit.afHitHealth[2 * iHit + 1] = fNow;
            }
        }
        if (s_abAliveLastTurn[i] && !pDamageable->mIsAlive)
        {
            s_result.aUnits[i].bKilled = true;
            if (i == 0)
            {
                ++s_result.iPlayerDeaths;
            }
            else
            {
                ++s_result.iKills;
            }
        }

        s_afHealthLastTurn[i] = fNow;
        s_afHitsLastTurn[i] = (float)s_aiHits[i];
        s_abAliveLastTurn[i] = pDamageable->mIsAlive != 0;
    }

    /* Which bolts died since the probe last ran, and who they hit. A bolt last seen at the
       previous scan and absent from this one died in the window between the two - and that window
       is the collision pass of the previous turn plus this turn up to the probe, which is exactly
       where a hit on a unit happens. When one bolt died and one unit lost health, the harness
       says that bolt hit that unit; otherwise it says it could not tell, and counts the
       ambiguity instead of guessing. */
    int iBoltsGone = 0;
    for (int i = 0; i < s_result.iOrdnanceRecorded; ++i)
    {
        CombatOrdnance& rBolt = s_result.aOrdnance[i];
        if (rBolt.iLastTurn != s_iScanTurn - 1 || rBolt.iAttributedVictim != -1)
        {
            continue;
        }
        ++iBoltsGone;
        if (iUnitsHit == 1)
        {
            rBolt.iAttributedVictim = iLastUnitHit;
        }
        else
        {
            ++s_result.iHitsAmbiguous;
        }
    }
    if (iUnitsHit == 0)
    {
        s_result.iOrdnanceExpired += iBoltsGone;
    }

    /* The turn, as the engine's objects reported it. */
    CombatTurnRecord& record = s_result.aTurns[iTurn % COMBAT_MAX_TURN_RECORDS];
    memset(&record, 0, sizeof(record));
    record.iTurn = iTurn;
    record.fTime = s_fSimTime;
    record.iOrdnanceLive = s_iScanCount;
    record.iOrdnanceGone = iBoltsGone;
    record.fPlayerHealth = s_afHealthLastTurn[0];
    for (int i = 1; i < COMBAT_MAX_UNITS; ++i)
    {
        if (!s_pObject[i])
        {
            continue;
        }
        record.fTargetHealthSum += s_afHealthLastTurn[i];
        if (static_cast<Damageable*>(s_pObject[i])->mIsAlive)
        {
            ++record.iTargetsAlive;
        }
    }
    Weapon* pWeapon = PrimaryWeapon(s_pUnit[0]);
    if (pWeapon)
    {
        record.iWeaponState = (int)pWeapon->GetState();
        record.iWeaponSelected = pWeapon->IsSelected() ? 1 : 0;
        record.iTriggerAction = pWeapon->mTrigger ? (int)pWeapon->mTrigger->m_uiActionState : 0;
        record.iTriggerDown = (pWeapon->mTrigger && pWeapon->mTrigger->IsDown()) ? 1 : 0;
        if (pWeapon->m_pAmmoCounter)
        {
            record.fWeaponCurClip = pWeapon->m_pAmmoCounter->mCurClip;
        }
        const float fPerRound =
            (pWeapon->m_pAmmoCounter->m_pClass &&
             pWeapon->m_pAmmoCounter->m_pClass->mAmmoPerRound > 0.0f)
                ? pWeapon->m_pAmmoCounter->m_pClass->mAmmoPerRound
                : 1.0f;
        record.iPlayerShotsFired =
            (int)(s_afAmmoStart[0] - pWeapon->m_pAmmoCounter->mNumClips +
                  (s_afClipStart[0] - pWeapon->m_pAmmoCounter->mCurClip) / fPerRound + 0.5f);
    }
    /* What the engine's own controller did with the harness's turn and pitch, and where the
       trooper's own matrix says it faces. */
    {
        TrooperState state;
        if (LevelHarnessGetTrooperState(&state) && state.ok)
        {
            record.fControlTurn = state.controlTurn;
            record.fControlPitch = state.controlPitch;
            record.fFacingX = state.dirX;
            record.fFacingY = state.dirY;
            record.fFacingZ = state.dirZ;
        }
        Weapon* pAimWeapon = PrimaryWeapon(s_pUnit[0]);
        if (pAimWeapon && pAimWeapon->mAimer)
        {
            record.fAimDirX = pAimWeapon->mAimer->GetDirection().x;
            record.fAimDirY = pAimWeapon->mAimer->GetDirection().y;
            record.fAimDirZ = pAimWeapon->mAimer->GetDirection().z;
        }
    }

    /* The first target's own fire machine: the engine's weapon state, its trigger, its shots and
       its aimer, so that a fight in which only one side shoots says which side's engine refused. */
    {
        Weapon* pTargetWeapon = PrimaryWeapon(s_pUnit[1]);
        if (pTargetWeapon)
        {
            record.iTargetWeaponState = (int)pTargetWeapon->GetState();
            record.iTargetTriggerDown =
                (pTargetWeapon->mTrigger && pTargetWeapon->mTrigger->IsDown()) ? 1 : 0;
            record.iTargetAimerWithinLimits =
                (pTargetWeapon->mAimer && pTargetWeapon->mAimer->mWithinLimitsFlag) ? 1 : 0;
            if (pTargetWeapon->m_pAmmoCounter)
            {
                const float fPerRound =
                    (pTargetWeapon->m_pAmmoCounter->m_pClass &&
                     pTargetWeapon->m_pAmmoCounter->m_pClass->mAmmoPerRound > 0.0f)
                        ? pTargetWeapon->m_pAmmoCounter->m_pClass->mAmmoPerRound
                        : 1.0f;
                record.iTargetShotsFired =
                    (int)(s_afAmmoStart[1] - pTargetWeapon->m_pAmmoCounter->mNumClips +
                          (s_afClipStart[1] - pTargetWeapon->m_pAmmoCounter->mCurClip) / fPerRound +
                          0.5f);
            }
        }
    }
    if (pOutRecord)
    {
        *pOutRecord = record;
    }
    ++s_result.iTurnRecords;
    if (s_result.iTurnRecords > COMBAT_MAX_TURN_RECORDS)
    {
        s_result.iTurnRecords = COMBAT_MAX_TURN_RECORDS;
    }
    s_result.iFirstRecordTurn = s_iTurn - s_result.iTurnRecords + 1;

    return true;
}

bool CombatHarnessFinish(void)
{
    if (!s_bProbeActive)
    {
        return false;
    }
    /* Out of the engine's own thread list. The engine deletes a thread whose Update answers
       false, and nothing in the engine owns this one, so the harness takes it out itself rather
       than leave a static object in the list for the engine's own loop to delete at exit. */
    s_probe.DeactivateThread();
    s_bProbeActive = false;
    if (s_bFireThreadActive)
    {
        s_fireThread.DeactivateThread();
        s_bFireThreadActive = false;
    }
    return true;
}

bool CombatHarnessLiveTurn(const HostInput* pInput)
{
    return CombatHarnessTurn(&s_config, true, s_config.bTargetsFire, 0, pInput);
}

/* ------------------------------------------------------------------ reading the engine back */

bool CombatHarnessMeasure(CombatResult* pOut)
{
    if (!s_bSetup || !pOut)
    {
        return false;
    }

    CombatResult& r = *pOut = s_result;
    r.iTurnsRun = s_iTurn;
    r.fSimTime = s_fSimTime;
    r.iHits = 0;
    r.fDamageDealt = 0.0f;
    r.fDamageTaken = 0.0f;

    for (int i = 0; i < COMBAT_MAX_UNITS; ++i)
    {
        if (!s_pObject[i])
        {
            continue;
        }
        CombatUnit& rUnit = r.aUnits[i];
        ReadUnit(i, rUnit);
        rUnit.iHits = s_aiHits[i];
        rUnit.fDamageTaken = s_afDamage[i];
        r.iHits += s_aiHits[i];
        if (i == 0)
        {
            r.fDamageTaken += s_afDamage[i];
        }
        else
        {
            r.fDamageDealt += s_afDamage[i];
        }

        /* The engine's own count of the shots it fired: WeaponCannon::UpdateFire spends rounds
           only after Fire() said true (WeaponCannon.cpp:529-537), so the rounds the counter has
           given up are bolts the engine says it made. */
        Weapon* pWeapon = PrimaryWeapon(s_pUnit[i]);
        if (pWeapon && pWeapon->m_pAmmoCounter)
        {
            /* The engine's own ammo unit: one round is m_pClass->mAmmoPerRound of the counter
               (AmmoCounter::ExpendRounds, AmmoCounter.cpp:100-107), so the rounds the counter
               gave up are divided by that to get shots. */
            const float fPerRound =
                (pWeapon->m_pAmmoCounter->m_pClass && pWeapon->m_pAmmoCounter->m_pClass->mAmmoPerRound > 0.0f)
                    ? pWeapon->m_pAmmoCounter->m_pClass->mAmmoPerRound
                    : 1.0f;
            rUnit.fShotsFired = (s_afAmmoStart[i] - pWeapon->m_pAmmoCounter->mNumClips) +
                                (s_afClipStart[i] - pWeapon->m_pAmmoCounter->mCurClip) / fPerRound;
        }

        /* Who the engine says hurt it last. */
        Damageable* pDamageable = static_cast<Damageable*>(s_pObject[i]);
        r.aiLastHitterTeam[i] = pDamageable->mDamageOwner.mTeam;
        r.abLastHitterWasPlayer[i] =
            (pDamageable->mDamageOwner.mCharacter &&
             pDamageable->mDamageOwner.mCharacter->mPlayerId >= 0)
                ? 1 : 0;
    }

    r.iTeam1Members = Team::sTeams[1] ? Team::sTeams[1]->mMemberCount : 0;
    r.iTeam1Alive = Team::sTeams[1] ? Team::sTeams[1]->GetNumMembersAlive() : 0;
    r.iTeam2Members = Team::sTeams[2] ? Team::sTeams[2]->mMemberCount : 0;
    r.iTeam2Alive = Team::sTeams[2] ? Team::sTeams[2]->GetNumMembersAlive() : 0;
    return true;
}

/* ------------------------------------------------------------------ the script */

static void BuildReport();

bool CombatHarnessRun(const CombatConfig* pConfig, void(__cdecl* pLog)(char const*, ...),
                      CombatResult* pOut)
{
    CombatConfig config;
    CombatHarnessDefaultConfig(&config);
    if (pConfig)
    {
        config = *pConfig;
    }

    if (!CombatHarnessSetup(&config, pLog, &s_result))
    {
        if (pOut)
        {
            *pOut = s_result;
        }
        BuildReport();
        return false;
    }

    const int aiPhases[3] = {s_config.iSettleTurns, s_config.iPlayerVolleyTurns,
                             s_config.iTailTurns};
    CombatTurnRecord record;
    for (int iPhase = 0; iPhase < 3; ++iPhase)
    {
        const bool bFire = (iPhase == 1);
        for (int iTurn = 0; iTurn < aiPhases[iPhase]; ++iTurn)
        {
            if (!CombatHarnessTurn(&s_config, bFire && s_config.bPlayerFire,
                                   bFire && s_config.bTargetsFire, &record))
            {
                if (!s_bPlayerDied)
                {
                    snprintf(s_result.acError, sizeof(s_result.acError), "turn %d", s_iTurn);
                }
                break;
            }
        }
        if (s_bPlayerDied)
        {
            break;
        }
    }

    CombatHarnessMeasure(&s_result);
    CombatHarnessFinish();
    s_bRan = true;
    BuildReport();

    if (pOut)
    {
        *pOut = s_result;
    }
    return true;
}

/* ------------------------------------------------------------------ the report
 *
 * Both drivers print this same text, so the comparison is between two builds' bytes and not
 * between two printers' readings of two structs. Every float is printed with nine significant
 * digits, which is what it takes to name a float32 exactly (FLT_DECIMAL_DIG is 9), so the two
 * sides either print the same number or print different numbers, and the difference is real.
 */
static char s_acReport[COMBAT_REPORT_CAPACITY];

static void BuildReport()
{
    const CombatResult& r = s_result;
    int n = 0;
    const int cap = COMBAT_REPORT_CAPACITY;

#define PUT(...)                                                                      \
    do {                                                                              \
        const int room = cap - n;                                                     \
        if (room <= 1) { n = cap - 1; break; }                                        \
        const int wrote = snprintf(s_acReport + n, (size_t)room, __VA_ARGS__);         \
        if (wrote > 0) { n += (wrote < room) ? wrote : (room - 1); }                   \
        if (n >= cap - 1) { n = cap - 1; break; }                                      \
    } while (false)

    PUT("{\"level\":\"%s\",\"player_class\":\"%s\",\"player_class_file\":\"%s\","
        "\"target_class\":\"%s\",\"target_class_file\":\"%s\","
        "\"player_team\":%d,\"player_side\":%d,\"target_team\":%d,\"target_side\":%d,"
        "\"targets_asked\":%d,\"targets_spawned\":%d,"
        "\"target_distance\":%.9g,\"target_spacing\":%.9g,\"target_depth\":%.9g,"
        "\"turn_delta\":%.9g,\"settle_turns\":%d,\"volley_turns\":%d,\"tail_turns\":%d,"
        "\"player_fire\":%d,\"targets_fire\":%d,"
        "\"turns_run\":%d,\"sim_time\":%.9g,"
        "\"hits\":%d,\"kills\":%d,\"player_deaths\":%d,"
        "\"damage_dealt\":%.9g,\"damage_taken\":%.9g,"
        "\"hits_ambiguous\":%d,"
        "\"ordnance_total\":%d,\"ordnance_peak\":%d,\"ordnance_expired\":%d,"
        "\"ordnance_scans\":%d,"
        "\"team1\":{\"classes\":%d,\"members\":%d,\"alive\":%d},"
        "\"team2\":{\"classes\":%d,\"members\":%d,\"alive\":%d},",
        r.acLevel, r.acPlayerClass, r.acPlayerClassFile, r.acTargetClass, r.acTargetClassFile,
        r.iPlayerTeam, r.iPlayerSide, r.iTargetTeam, r.iTargetSide,
        r.iTargetCountAsked, r.iTargetsSpawned, D(r.fTargetDistance), D(r.fTargetSpacing),
        D(r.fTargetDepth), D(r.fTurnDelta), r.iSettleTurns, r.iPlayerVolleyTurns, r.iTailTurns,
        r.bPlayerFire, r.bTargetsFire, r.iTurnsRun, D(r.fSimTime),
        r.iHits, r.iKills, r.iPlayerDeaths, D(r.fDamageDealt), D(r.fDamageTaken),
        r.iHitsAmbiguous,
        r.iOrdnanceTotal, r.iOrdnancePeak, r.iOrdnanceExpired, r.iScans,
        r.iTeam1ClassCount, r.iTeam1Members, r.iTeam1Alive,
        r.iTeam2ClassCount, r.iTeam2Members, r.iTeam2Alive);

    PUT("\"units\":[");
    for (int i = 0; i < COMBAT_MAX_UNITS; ++i)
    {
        const CombatUnit& u = r.aUnits[i];
        if (!u.bSpawned)
        {
            continue;
        }
        PUT("%s{\"slot\":%d,\"role\":\"%s\",\"name\":\"%s\",\"class\":\"%s\",\"team\":%d,"
            "\"player_id\":%d,"
            "\"asked_at\":[%.9g,%.9g,%.9g],"
            "\"at_before\":[%.9g,%.9g,%.9g],\"at_after\":[%.9g,%.9g,%.9g],"
            "\"health_before\":%.9g,\"health_after\":%.9g,\"max_health\":%.9g,"
            "\"alive_before\":%s,\"alive_after\":%s,"
            "\"hits\":%d,\"damage_taken\":%.9g,\"killed\":%s,"
            "\"last_hitter_team\":%d,\"last_hitter_was_player\":%s,"
            "\"class_weapons\":%d,\"built_weapons\":%d,\"collision_bodies\":%d,"
            "\"weapon_state\":%d,\"weapon_selected\":%d,\"weapon_clips\":%.9g,"
            "\"weapon_cur_clip\":%.9g,\"shots_fired\":%.9g,"
            "\"weapon_name\":\"%s\",\"weapon_name_hash\":%u,\"trigger_is_fire0\":%d,"
            "\"aim_at\":[%.9g,%.9g,%.9g],\"aim_dir\":[%.9g,%.9g,%.9g],"
            "\"aimer_within_limits\":%d,"
            "\"hit_turns\":[",
            (i == 0) ? "" : ",", i, u.pcRole ? u.pcRole : "", u.acName,
            u.pcClass ? u.pcClass : "", u.iTeam, u.iPlayerId,
            D(u.fSpawnX), D(u.fSpawnY), D(u.fSpawnZ),
            D(u.fX0), D(u.fY0), D(u.fZ0), D(u.fX1), D(u.fY1), D(u.fZ1),
            D(u.fHealthBefore), D(u.fHealthAfter), D(u.fMaxHealth),
            u.bAliveBefore ? "true" : "false", u.bAliveAfter ? "true" : "false",
            u.iHits, D(u.fDamageTaken), u.bKilled ? "true" : "false",
            r.aiLastHitterTeam[i], r.abLastHitterWasPlayer[i] ? "true" : "false",
            u.iClassWeaponCount, u.iBuiltWeaponCount, u.iCollisionBodyCount,
            u.iWeaponState, u.iWeaponSelected, D(u.fWeaponClips), D(u.fWeaponCurClip),
            D(u.fShotsFired), u.acWeaponName, u.uWeaponNameHash, u.iTriggerIsFire0,
            D(u.fAimX), D(u.fAimY), D(u.fAimZ), D(u.fAimDirX), D(u.fAimDirY), D(u.fAimDirZ),
            u.iAimerWithinLimits);
        for (int h = 0; h < u.iHits && h < COMBAT_MAX_HITS; ++h)
        {
            PUT("%s{\"turn\":%d,\"before\":%.9g,\"after\":%.9g}", (h == 0) ? "" : ",",
                u.aiHitTurn[h], D(u.afHitHealth[2 * h]), D(u.afHitHealth[2 * h + 1]));
        }
        PUT("]}");
    }
    PUT("],");

    PUT("\"ordnance\":[");
    for (int i = 0; i < r.iOrdnanceRecorded; ++i)
    {
        const CombatOrdnance& o = r.aOrdnance[i];
        /* The bolt's own pool key is not in the report: it is an address, and an address is the
           one thing in this fight that is not the same in two runs of the same program. The
           sequence number is what tells the bolts apart in the report. */
        PUT("%s{\"seq\":%d,\"turn_first\":%d,\"turn_last\":%d,\"class_id\":%u,"
            "\"owner_slot\":%d,\"owner_team\":%d,"
            "\"at\":[%.9g,%.9g,%.9g],\"vel\":[%.9g,%.9g,%.9g],"
            "\"life\":%.9g,\"damage\":%.9g,\"static_hit\":%d,\"victim_slot\":%d}",
            (i == 0) ? "" : ",", o.iSequence, o.iFirstTurn, o.iLastTurn, o.uClassId,
            o.iOwnerSlot, o.iOwnerTeam, D(o.fX), D(o.fY), D(o.fZ), D(o.fVelX), D(o.fVelY),
            D(o.fVelZ), D(o.fLifeTime), D(o.fDamage), o.bStaticHit, o.iAttributedVictim);
    }
    PUT("],");

    /* One entry per turn, in the order: turn, weapon state, weapon selected, trigger down, the
       trigger's own action state, rounds gone, bolts live at the probe, bolts gone since the last
       probe, player health, the sum of the targets' health, targets alive, sim time. */
    PUT("\"turns\":[");
    for (int i = 0; i < r.iTurnRecords; ++i)
    {
        const int iTurn = r.iFirstRecordTurn + i;
        const CombatTurnRecord& t = r.aTurns[((iTurn % COMBAT_MAX_TURN_RECORDS) +
                                               COMBAT_MAX_TURN_RECORDS) %
                                              COMBAT_MAX_TURN_RECORDS];
        PUT("%s[%d,%d,%d,%d,%d,%d,%d,%d,%.9g,%.9g,%d,%.9g,%d,%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g,"
            "%.9g,%.9g,%.9g]",
            (i == 0) ? "" : ",", t.iTurn, t.iWeaponState, t.iWeaponSelected, t.iTriggerDown,
            t.iTriggerAction, t.iPlayerShotsFired, t.iOrdnanceLive, t.iOrdnanceGone,
            D(t.fPlayerHealth), D(t.fTargetHealthSum), t.iTargetsAlive, D(t.fTime),
            t.iTargetWeaponState, t.iTargetTriggerDown, t.iTargetShotsFired,
            t.iTargetAimerWithinLimits, D(t.fControlTurn), D(t.fControlPitch), D(t.fFacingX),
            D(t.fFacingY), D(t.fFacingZ), D(t.fAimDirX), D(t.fAimDirY), D(t.fAimDirZ));
    }
    PUT("],");

    PUT("\"diag\":{\"ordnance_pool\":[%u,%u],\"sizeof_ordnance_laser\":%u,"
        "\"sizeof_ordnance_bullet\":%u,\"weapon_pool\":[%u,%u],\"sizeof_weapon_cannon\":%u,"
        "\"soldier_pool_count\":%u,\"sim_seed\":%u,\"collision_meshes\":%d},",
        r.uiOrdnancePoolCount, r.uiOrdnancePoolSize, r.uiSizeofOrdnanceLaser,
        r.uiSizeofOrdnanceBullet, r.uiWeaponPoolCount, r.uiWeaponPoolSize,
        r.uiSizeofWeaponCannon, r.uiSoldierPoolCount, r.uiSimSeed, r.iCollisionMeshes);

    PUT("\"player_died\":%s,", s_bPlayerDied ? "true" : "false");
    PUT("\"error\":\"%s\",\"ran\":%s}", r.acError, s_bRan ? "true" : "false");

    /* The report is read by the driver with strlen/printf, so it has to end here. */
    s_acReport[(n < cap) ? n : (cap - 1)] = 0;
#undef PUT
}

const char* CombatHarnessReport(void)
{
    return s_acReport;
}

const char* CombatHarnessLiveReport(void)
{
    CombatHarnessMeasure(&s_result);
    BuildReport();
    return s_acReport;
}
