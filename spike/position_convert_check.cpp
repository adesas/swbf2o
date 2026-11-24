/* The position quantisation, the netLOD path, the per-client rule and the quaternion path, run as the
 * engine's own code, for the check in browser/position_convert_check.mjs.
 *
 * Everything that decides a number here is the engine's: BeginNetLOD, SetNetLOD, SetNetPrecision,
 * ReadNetLOD, NetGame::GetMyGuest, NetGame::IsLocalTo, NetVectorBits, the eight converter classes,
 * WriteUnitVector, ReadUnitVector, WritePositionVector, ReadPositionVector, WriteThrust, ReadThrust,
 * WriteAngle, ReadAngle, WriteSwitch, ReadSwitch, WriteJetFuel, ReadJetFuel, WriteQuaternion,
 * ReadQuaternion and the bit packing. They are copied verbatim out of NetState.cpp and NetGame.cpp by
 * tools/extract_position_convert.py, which records both source hashes; the packet layer is linked from
 * the built tree, and tools/net_wire_check.py is a third reading of that layer.
 *
 * The other half is tools/position_convert_check.py, a second reading of the same source. A check that
 * only round-tripped the engine against itself could not fail, so the comparison is with the model: the
 * state each side reaches, the bit width each side derives, the ranges each side reads off the class,
 * the codes each side puts on the wire, the bytes those codes make, and the values each side decodes.
 *
 * Four things are stated rather than observed, and each is marked where it is used.
 *
 *   Which converter a value goes through is not reported by the engine's own functions. The mapping (x
 *   and z through PositionXZConvert, y through Y, NetState.cpp:2177 and :2185; the quaternion's four
 *   fields in X, Y, Z, S order, :2220 and :2229) is what this file reads off the source and what the
 *   model reads off the same source independently. The bytes are what decides it, because the converters
 *   have different ranges and different divisors.
 *
 *   This file has one netLOD because the engine has one, so the reader continues from the state the
 *   writer left; the check never presents that as two machines.
 *
 *   MountedTurret::Write at NetState.cpp:1798 is game layer and cannot be run here: the assignment
 *   `oldPrecision = netLOD; netLOD = IsLocalTo(NetGame::GetCurDst(), mPlayerId) ? 1 : oldPrecision;` is
 *   quoted, and this file applies it. What is run is the rule itself, through the engine's own
 *   IsLocalTo, and the consequence for the bytes. Which branch IsLocalTo took is not observable from
 *   outside; only the value it returns is. GetMyGuest is engine code, and the nine values in its table
 *   are an input both halves are given identically.
 *
 *   ReadMatrix at NetState.cpp:2202 continues with D3DXQuaternionNormalize and
 *   D3DXMatrixRotationQuaternion at :2207 and :2208. Those are not engine code available to this check,
 *   so the quaternion half is pinned only up to the four decoded components, and nothing here normalises.
 *
 * SetNetLOD takes a bool, so it can only produce 0 or 1. The scenarios that show 2 and 3 put the field
 * on the wire as the wire would carry it and say so.
 *
 * Prints JSON with the same fields the model prints.
 *
 * Usage from node: position_report() -> pointer to a NUL-terminated JSON string.
 */
#include "NetPacket.h"
#include "NetComm.h"
#include "PebbleFL/Common/PblVector.h"
#include "PebbleFL/Common/PblQuaternion.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Defined in the generated file, which copied them from NetState.cpp. */
void BeginNetLOD();
void SetNetLOD(bool precision);
void SetNetPrecision(NetPktGroup& group);
void ReadNetLOD(NetPktGroup& group);
void WriteUnitVector(NetPktGroup& group, const PblVector3& value);
void ReadUnitVector(NetPktGroup& group, PblVector3& value);
void WritePositionVector(NetPktGroup& group, const PblVector3& value);
void ReadPositionVector(NetPktGroup& group, PblVector3& value);
void WriteSwitch(NetPktGroup& group, float value);
void ReadSwitch(NetPktGroup& group, float& value);
void WriteThrust(NetPktGroup& group, float value);
void ReadThrust(NetPktGroup& group, float& value);
void WriteAngle(NetPktGroup& group, float value);
void ReadAngle(NetPktGroup& group, float& value);
void WriteJetFuel(NetPktGroup& group, float value);
void ReadJetFuel(NetPktGroup& group, float& value);
void WriteQuaternion(NetPktGroup& group, const PblQuaternion& value);
void ReadQuaternion(NetPktGroup& group, PblQuaternion& value);
/* The two globals the generated file copied from NetState.cpp:74 and :165. */
extern int netLOD;
extern int lastNetLOD;
/* Accessors. They model nothing: they report what the engine's own converters say. */
int unitBits();
unsigned int unitMinBits();
unsigned int unitMaxBits();
int posXZBits();
unsigned int posXZMinBits();
unsigned int posXZMaxBits();
int posYBits();
unsigned int posYMinBits();
unsigned int posYMaxBits();
int quatBits();
unsigned int quatMinBits();
unsigned int quatMaxBits();
int switchBits();
unsigned int switchMinBits();
unsigned int switchMaxBits();
int thrustBits();
unsigned int thrustMinBits();
unsigned int thrustMaxBits();
int angleBits();
unsigned int angleMinBits();
unsigned int angleMaxBits();
int jetFuelBits();
unsigned int jetFuelMinBits();
unsigned int jetFuelMaxBits();

/* The same shell the generated file declares, so that the two functions it copied verbatim from
 * NetGame.cpp can be called. The table itself is not reachable from here; only the functions are. */
struct NetPlayerStub { int myGuest[1]; };
struct NetGame
{
public:
    static NetPlayerStub netPlayers[64];
    static int GetMyGuest(int iPlayer, int iSlot);
    static bool IsLocalTo(int iPlayer, int iOther);
};
void seedGuests();

/* Internal to NetComm.cpp, so declared here rather than pulled from a header. */
void __fastcall PackHeader(NetPacket* pPacket);
unsigned int __fastcall UnpackHeader(NetPacket* pPacket);
bool __fastcall RecvGroup(NetPktGroup* pComplete, NetPacket* pPacket);

static const int TYPE = 12;   /* an input both sides set, not a claim about which packet carries positions */
static const unsigned char SRC = 3;
static const unsigned short SEQ = 77;

enum Op { OP_BEGIN, OP_SET, OP_PRECISION, OP_WIRE, OP_READ, OP_VEC_POS, OP_VEC_UNIT,
          OP_FIELD, OP_GET, OP_DECISION, OP_GROUP };

/* The same numbering tools/position_convert_check.py uses. For OP_FIELD and OP_GET the argument is
 * converter * 8 + value row, so both halves read the same number. */
static const char* CONV_NAME[8] = {"unit", "positionXZ", "positionY", "switch", "thrust", "angle",
                                   "jetfuel", "quat"};

struct OpEntry { int op; int arg; };
/* rule, ws and rs are the same numbers tools/position_convert_check.py puts in the same scenario:
 * rule = how many value rows come from the rule both halves apply; ws = 0 the state a scenario starts
 * from is what BeginNetLOD gives, 1 carry it from the run; rs = 0 continue from the writer half,
 * 1 carry it from the run, 2 what BeginNetLOD gives. Scenarios that do not say anything get 0, which
 * is what they did before groups existed. */
struct Scenario {
    const char* name;
    int nw, nr;
    const OpEntry* w;
    const OpEntry* r;
    int nvec;
    unsigned int v[24][4];      /* float32 bit patterns, the same numbers the model uses */
    int rule, ws, rs;
};

struct Field { int conv; int bits; unsigned int min, max, code, decoded; };
static Field fieldsWrite[64], fieldsRead[64];
static int nFieldsWrite, nFieldsRead;
static int observed[8][2], nObserved;
static int completed[8], nCompleted;
static int stateAfterWrite[2], stateAfterRead[2];
static int decisions[8][6], nDecisions;   /* player, other, guest, result, netLOD before, after */

static char out[200000];
static int pos = 0;

static void emit(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pos += vsnprintf(out + pos, sizeof(out) - pos, fmt, ap);
    va_end(ap);
}

static unsigned char wire[4096];
static int wireBase[24], wireLen[24], wireCount, packetCount, packetGroup[24];

/* Per group: what each half's state was when it reached the group, what the group's field says, and
 * which packets belong to it. A group is a group boundary in the step lists, so both halves walk the
 * same structure. */
static int grpCount, grpStart[24], grpEnd[24], grpPkt[24][4], grpPktN[24];
static int grpWriterBefore[24][2], grpFirstBit[24], grpFieldBits[24];
static int grpReaderBefore[24][2], grpReaderAfter[24][2];
static int sessionState[2];

static float fromBits(unsigned int bits)
{
    unsigned int u = (unsigned int)bits;
    return *(const float*)&u;
}

static unsigned int toBits(float f)
{
    return *(const unsigned int*)&f;
}

static void converterFacts(int c, int* bits, unsigned int* mn, unsigned int* mx)
{
    if (c == 0) { *bits = unitBits(); *mn = unitMinBits(); *mx = unitMaxBits(); }
    else if (c == 1) { *bits = posXZBits(); *mn = posXZMinBits(); *mx = posXZMaxBits(); }
    else if (c == 2) { *bits = posYBits(); *mn = posYMinBits(); *mx = posYMaxBits(); }
    else if (c == 3) { *bits = switchBits(); *mn = switchMinBits(); *mx = switchMaxBits(); }
    else if (c == 4) { *bits = thrustBits(); *mn = thrustMinBits(); *mx = thrustMaxBits(); }
    else if (c == 5) { *bits = angleBits(); *mn = angleMinBits(); *mx = angleMaxBits(); }
    else if (c == 6) { *bits = jetFuelBits(); *mn = jetFuelMinBits(); *mx = jetFuelMaxBits(); }
    else { *bits = quatBits(); *mn = quatMinBits(); *mx = quatMaxBits(); }
}

/* Which converter each field goes through. This is what this file reads off the source: the three
 * position fields through XZ, Y, XZ (NetState.cpp:2177, :2185), the three unit fields through the same
 * converter (:2161, :2169), the quaternion's four in X, Y, Z, S order (:2220, :2229), and one field
 * for each single-value converter. The model reads the same source and states the same mapping. */
static void orderFor(int op, int arg, int* order, int* n)
{
    if (op == OP_VEC_POS) { order[0] = 1; order[1] = 2; order[2] = 1; *n = 3; return; }
    if (op == OP_VEC_UNIT) { order[0] = order[1] = order[2] = 0; *n = 3; return; }
    int c = arg / 8;
    *n = (c == 7) ? 4 : 1;                       /* QuatConvert is used four times, once per component */
    for (int k = 0; k < *n; ++k) order[k] = c;
}

/* How many values a row holds, which is how many fields the op that uses it writes. */
static int rowLength(const Scenario& s, int row)
{
    int n = 0;
    for (int i = 0; i < s.nw; ++i) {
        const OpEntry& e = s.w[i];
        int order[4], k;
        if (e.op == OP_FIELD || e.op == OP_VEC_POS || e.op == OP_VEC_UNIT) {
            orderFor(e.op, e.arg, order, &k);
            int r = (e.op == OP_FIELD) ? e.arg % 8 : e.arg;
            if (r == row && k > n) n = k;
        }
    }
    return n;
}

static void recordFields(int* order, int n, Field* dest, int* count)
{
    for (int k = 0; k < n; ++k) {
        int b; unsigned int mn, mx;
        converterFacts(order[k], &b, &mn, &mx);
        dest[*count + k].conv = order[k];
        dest[*count + k].bits = b;
        dest[*count + k].min = mn;
        dest[*count + k].max = mx;
        dest[*count + k].code = 0;
        dest[*count + k].decoded = 0;
    }
    *count += n;
}

static void stepPrecision(NetPktGroup& g)
{
    unsigned int v = 0;
    g.ReadBits(v, 1);
    if (!v) { v = 0; g.ReadBits(v, 2); }
}

static void writeField(NetPktGroup& g, const Scenario& s, const OpEntry& e)
{
    int order[4], n;
    orderFor(OP_FIELD, e.arg, order, &n);
    int base = nFieldsWrite;
    recordFields(order, n, fieldsWrite, &nFieldsWrite);

    int row = e.arg % 8, c = e.arg / 8;
    switch (c) {
    case 3: WriteSwitch(g, fromBits(s.v[row][0])); break;          /* NetState.cpp:3782 */
    case 4: WriteThrust(g, fromBits(s.v[row][0])); break;          /* NetState.cpp:426 */
    case 5: WriteAngle(g, fromBits(s.v[row][0])); break;           /* NetState.cpp:438 */
    case 6: WriteJetFuel(g, fromBits(s.v[row][0])); break;         /* NetState.cpp:3794 */
    case 7:                                                            /* NetState.cpp:2220 */
        WriteQuaternion(g, PblQuaternion(fromBits(s.v[row][0]), fromBits(s.v[row][1]),
                                        fromBits(s.v[row][2]), fromBits(s.v[row][3])));
        break;
    }
    for (int k = 0; k < n; ++k) fieldsWrite[base + k].code = 0;   /* filled by reading the bytes back */
}

/* The rule the engine's own IsLocalTo implements, and the effect the quoted assignment at
 * NetState.cpp:1802 has. This file applies the assignment; it does not run MountedTurret::Write. */
static void runDecision(int index)
{
    static const int CASES[6][2] = {{5, -1}, {5, 5}, {12, 44}, {12, 45}, {64, 44}, {5, 5}};
    int player = CASES[index][0], other = CASES[index][1];
    int guest = NetGame::GetMyGuest(player, 0);            /* NetGame.cpp:4702, the engine's own code */
    bool r = NetGame::IsLocalTo(player, other);            /* NetGame.cpp:4722, the engine's own code */
    int before = netLOD;
    netLOD = r ? 1 : before;                               /* the quoted assignment, applied here */
    decisions[nDecisions][0] = player;
    decisions[nDecisions][1] = other;
    decisions[nDecisions][2] = guest;
    decisions[nDecisions][3] = r ? 1 : 0;
    decisions[nDecisions][4] = before;
    decisions[nDecisions][5] = netLOD;
    nDecisions++;
}

static void writeOp(NetPktGroup& g, const Scenario& s, const OpEntry& e)
{
    switch (e.op) {
    case OP_BEGIN: BeginNetLOD(); return;                            /* NetState.cpp:184 */
    case OP_SET: SetNetLOD(e.arg != 0); return;                     /* NetState.cpp:290 */
    case OP_PRECISION: SetNetPrecision(g); return;                  /* NetState.cpp:296 */
    case OP_WIRE:                                                   /* a field the engine's writer cannot make */
        g.WriteBits(0, 1);
        g.WriteBits((unsigned int)e.arg, 2);
        return;
    case OP_READ: return;                                           /* the read half does this */
    case OP_GROUP: return;                                          /* the boundary writes nothing itself */
    case OP_DECISION: runDecision(e.arg); return;
    case OP_FIELD: writeField(g, s, e); return;
    }
    int order[4], n;
    orderFor(e.op, e.arg, order, &n);
    recordFields(order, n, fieldsWrite, &nFieldsWrite);
    PblVector3 p(fromBits(s.v[e.arg][0]), fromBits(s.v[e.arg][1]), fromBits(s.v[e.arg][2]));
    if (e.op == OP_VEC_UNIT) WriteUnitVector(g, p);                 /* NetState.cpp:2161 */
    else WritePositionVector(g, p);                                 /* NetState.cpp:2177 */
}

static void fillGenerated(Scenario& s);

static int groupRanges(const OpEntry* steps, int n, int* start, int* end)
{
    int ng = 0, at = 0;
    for (int i = 0; i <= n; ++i) {
        if (i == n || steps[i].op == OP_GROUP) { start[ng] = at; end[ng] = i; ng++; at = i + 1; }
    }
    return ng;
}

static void encodeScenario(Scenario& s)
{
    NetPacket::Create();
    if (s.ws == 1) { netLOD = sessionState[0]; lastNetLOD = sessionState[1]; } else { BeginNetLOD(); }
    seedGuests();
    doHiRes = false;
    nFieldsWrite = nFieldsRead = nObserved = nCompleted = nDecisions = 0;
    wireCount = 0;
    for (int i = 0; i < 24; ++i) grpPktN[i] = 0;

    grpCount = groupRanges(s.w, s.nw, grpStart, grpEnd);

    /* The writer half, group by group. The state it has at the start of a group is what the previous
     * group left, because an unchanged field carries no value; nothing is reset between groups. */
    if (s.rule) fillGenerated(s);
    int at = 0;
    for (int j = 0; j < grpCount; ++j) {
        grpWriterBefore[j][0] = netLOD; grpWriterBefore[j][1] = lastNetLOD;
        NetPktGroup g;
        g.Reset();
        g.isSmall = true;
        g.BeginWrite();
        for (int i = grpStart[j]; i < grpEnd[j]; ++i) writeOp(g, s, s.w[i]);
        g.EndWrite();
        int n = g.pktCount;
        for (int i = 0; i < n; ++i) {
            NetPacket* p = g.PopFront();
            NetComm::Header(p).src = SRC;
            NetComm::Header(p).type = (unsigned char)TYPE;
            NetComm::Header(p).count = (unsigned char)n;
            NetComm::Header(p).group = SEQ;
            NetComm::Header(p).idx = (unsigned char)i;
            PackHeader(p);
            const int len = p->mMaxIndex - p->mCommIndex;
            wireBase[wireCount] = at;
            wireLen[wireCount] = len;
            memcpy(wire + at, p->mData + p->mCommIndex, (size_t)len);
            at += len;
            packetGroup[wireCount] = j;
            grpPkt[j][grpPktN[j]++] = wireCount;
            wireCount++;
        }
    }
    packetCount = wireCount;
    stateAfterWrite[0] = netLOD;
    stateAfterWrite[1] = lastNetLOD;
}

/* Feed the bytes back through the engine's receive path. MakeReceivedPacket is static in
 * pcNetSocket.cpp (line 163), so its body is reproduced here and named there. */
static NetPktGroup complete;    /* at file scope: a guarded static local needs __cxa_guard_* */
static NetPktGroup* receiveGroup(int j)
{
    complete.Reset();
    complete.isSmall = true;

    for (int k = 0; k < grpPktN[j]; ++k) {
        const int i = grpPkt[j][k];
        const unsigned char* raw = wire + wireBase[i];
        const int len = wireLen[i];
        const int prefix = raw[0] >= 0x1e ? 0 : 3;
        NetPacket* p = NetPacket::Alloc(len <= 116 - prefix);
        if (!p) return 0;
        p->BeginWrite();
        memcpy(p->mData + prefix, raw, (size_t)len);
        p->mCommIndex = (char)prefix;
        p->mMaxIndex = (short)(len + prefix);
        UnpackHeader(p);
        NetComm::Header(p).src = SRC;            /* DoRecvs sets the source before reassembling */
        completed[i] = RecvGroup(&complete, p) ? 1 : 0;
    }
    return &complete;
}

static void fillGenerated(Scenario& s)
{
    for (int i = 0; i < s.rule; ++i) {
        s.v[i][0] = 0x449A5000u + (unsigned int)i * 0x1000u;
        s.v[i][1] = 0x42290000u - (unsigned int)i * 0x100u;
        s.v[i][2] = 0xC5B17100u + (unsigned int)i * 0x100u;
    }
}

static const OpEntry wInterior[] = { {OP_SET, 0}, {OP_VEC_POS, 0}, {OP_VEC_POS, 1} };
static const OpEntry wSingle[] = { {OP_SET, 0}, {OP_VEC_POS, 0} };
static const OpEntry wHiRes[] = { {OP_SET, 1}, {OP_VEC_POS, 0}, {OP_VEC_POS, 1} };
static const OpEntry rInterior[] = { {OP_VEC_POS, 0}, {OP_VEC_POS, 1} };
static const OpEntry rSingle[] = { {OP_VEC_POS, 0} };
/* Twenty vectors, so the op lists are long; the check counts them, so both halves must
 * hold the same steps. */
static const OpEntry wBig[] = { {OP_SET, 0}, {OP_VEC_POS, 0}, {OP_VEC_POS, 1}, {OP_VEC_POS, 2}, {OP_VEC_POS, 3}, {OP_VEC_POS, 4}, {OP_VEC_POS, 5}, {OP_VEC_POS, 6}, {OP_VEC_POS, 7}, {OP_VEC_POS, 8}, {OP_VEC_POS, 9}, {OP_VEC_POS, 10}, {OP_VEC_POS, 11}, {OP_VEC_POS, 12}, {OP_VEC_POS, 13}, {OP_VEC_POS, 14}, {OP_VEC_POS, 15}, {OP_VEC_POS, 16}, {OP_VEC_POS, 17}, {OP_VEC_POS, 18}, {OP_VEC_POS, 19} };
static const OpEntry rBig[] = { {OP_VEC_POS, 0}, {OP_VEC_POS, 1}, {OP_VEC_POS, 2}, {OP_VEC_POS, 3}, {OP_VEC_POS, 4}, {OP_VEC_POS, 5}, {OP_VEC_POS, 6}, {OP_VEC_POS, 7}, {OP_VEC_POS, 8}, {OP_VEC_POS, 9}, {OP_VEC_POS, 10}, {OP_VEC_POS, 11}, {OP_VEC_POS, 12}, {OP_VEC_POS, 13}, {OP_VEC_POS, 14}, {OP_VEC_POS, 15}, {OP_VEC_POS, 16}, {OP_VEC_POS, 17}, {OP_VEC_POS, 18}, {OP_VEC_POS, 19} };

static const OpEntry wUnit[] = {
    {OP_PRECISION, 0}, {OP_VEC_UNIT, 0}, {OP_VEC_UNIT, 1}, {OP_VEC_UNIT, 2},
};
static const OpEntry rUnit[] = { {OP_READ, 0}, {OP_VEC_UNIT, 0}, {OP_VEC_UNIT, 1}, {OP_VEC_UNIT, 2} };
static const OpEntry wWire2[] = { {OP_WIRE, 2}, {OP_VEC_UNIT, 0}, {OP_VEC_POS, 1} };
static const OpEntry wWire3[] = { {OP_WIRE, 3}, {OP_VEC_UNIT, 0}, {OP_VEC_POS, 1} };
static const OpEntry rWire2[] = { {OP_READ, 0}, {OP_VEC_UNIT, 0}, {OP_VEC_POS, 1} };
static const OpEntry wUnchanged[] = {
    {OP_SET, 1}, {OP_PRECISION, 0}, {OP_PRECISION, 0}, {OP_VEC_UNIT, 0}, {OP_VEC_POS, 1},
};
static const OpEntry rUnchanged[] = { {OP_READ, 0}, {OP_READ, 0}, {OP_VEC_UNIT, 0}, {OP_VEC_POS, 1} };
static const OpEntry wFresh[] = { {OP_PRECISION, 0}, {OP_VEC_UNIT, 0} };
static const OpEntry rFresh[] = { {OP_BEGIN, 0}, {OP_READ, 0}, {OP_VEC_UNIT, 0} };

/* The argument is converter * 8 + value row, which is the same number the model reads. The op itself
 * is carried separately, so both halves agree on what is being written and from which row. */
#define F(c, row) ((c) * 8 + (row))                  /* the argument for OP_FIELD */
#define D(index) (index)                             /* the argument for OP_DECISION is the case index */

static const OpEntry wNotLocal[] = { {OP_DECISION, D(0)}, {OP_FIELD, F(4, 0)}, {OP_FIELD, F(5, 0)} };
static const OpEntry rNotLocal[] = { {OP_GET, F(4, 0)}, {OP_GET, F(5, 0)} };
static const OpEntry wLocal[] = { {OP_DECISION, D(1)}, {OP_FIELD, F(4, 0)}, {OP_FIELD, F(5, 0)} };
static const OpEntry rLocal[] = { {OP_GET, F(4, 0)}, {OP_GET, F(5, 0)} };
static const OpEntry wBothWays[] = {
    {OP_SET, 0}, {OP_DECISION, D(0)}, {OP_FIELD, F(4, 0)}, {OP_FIELD, F(5, 0)},
    {OP_FIELD, F(6, 0)}, {OP_FIELD, F(3, 0)},
    {OP_DECISION, D(1)}, {OP_FIELD, F(4, 0)}, {OP_FIELD, F(5, 0)},
    {OP_FIELD, F(6, 0)}, {OP_FIELD, F(3, 0)},
};
static const OpEntry rBothWays[] = {
    {OP_GET, F(4, 0)}, {OP_GET, F(5, 0)}, {OP_GET, F(6, 0)}, {OP_GET, F(3, 0)},
    {OP_GET, F(4, 0)}, {OP_GET, F(5, 0)}, {OP_GET, F(6, 0)}, {OP_GET, F(3, 0)},
};
static const OpEntry wGuestMatch[] = { {OP_DECISION, D(2)}, {OP_FIELD, F(4, 0)} };
static const OpEntry rGuestMatch[] = { {OP_GET, F(4, 0)} };
static const OpEntry wGuestMiss[] = { {OP_DECISION, D(3)}, {OP_FIELD, F(4, 0)} };
static const OpEntry rGuestMiss[] = { {OP_GET, F(4, 0)} };
static const OpEntry wPastArray[] = { {OP_DECISION, D(4)}, {OP_FIELD, F(4, 0)} };
static const OpEntry rPastArray[] = { {OP_GET, F(4, 0)} };
static const OpEntry wNotGated[] = {
    {OP_SET, 1}, {OP_DECISION, D(5)}, {OP_FIELD, F(6, 0)}, {OP_FIELD, F(3, 0)}, {OP_FIELD, F(7, 1)},
};
static const OpEntry rNotGated[] = { {OP_GET, F(6, 0)}, {OP_GET, F(3, 0)}, {OP_GET, F(7, 1)} };
static const OpEntry wQuat[] = { {OP_FIELD, F(7, 0)} };
static const OpEntry rQuat[] = { {OP_GET, F(7, 0)} };

static const OpEntry wThreeGroups[] = {
    {OP_SET, 1}, {OP_PRECISION, 0}, {OP_FIELD, F(4, 0)}, {OP_GROUP, 0},
    {OP_PRECISION, 0}, {OP_FIELD, F(4, 0)}, {OP_GROUP, 0},
    {OP_SET, 0}, {OP_PRECISION, 0}, {OP_FIELD, F(4, 0)},
};
static const OpEntry rThreeGroups[] = {
    {OP_READ, 0}, {OP_GET, F(4, 0)}, {OP_GROUP, 0},
    {OP_READ, 0}, {OP_GET, F(4, 0)}, {OP_GROUP, 0},
    {OP_READ, 0}, {OP_GET, F(4, 0)},
};
static const OpEntry wSplitThenUnchanged[] = {
    {OP_SET, 0}, {OP_PRECISION, 0},
    {OP_VEC_POS, 0}, {OP_VEC_POS, 1}, {OP_VEC_POS, 2}, {OP_VEC_POS, 3}, {OP_VEC_POS, 4},
    {OP_VEC_POS, 5}, {OP_VEC_POS, 6}, {OP_VEC_POS, 7}, {OP_VEC_POS, 8}, {OP_VEC_POS, 9},
    {OP_VEC_POS, 10}, {OP_VEC_POS, 11}, {OP_VEC_POS, 12}, {OP_VEC_POS, 13}, {OP_VEC_POS, 14},
    {OP_GROUP, 0}, {OP_PRECISION, 0}, {OP_VEC_POS, 15},
};
static const OpEntry rSplitThenUnchanged[] = {
    {OP_READ, 0},
    {OP_VEC_POS, 0}, {OP_VEC_POS, 1}, {OP_VEC_POS, 2}, {OP_VEC_POS, 3}, {OP_VEC_POS, 4},
    {OP_VEC_POS, 5}, {OP_VEC_POS, 6}, {OP_VEC_POS, 7}, {OP_VEC_POS, 8}, {OP_VEC_POS, 9},
    {OP_VEC_POS, 10}, {OP_VEC_POS, 11}, {OP_VEC_POS, 12}, {OP_VEC_POS, 13}, {OP_VEC_POS, 14},
    {OP_GROUP, 0}, {OP_READ, 0}, {OP_VEC_POS, 15},
};
static const OpEntry wChangeAfterField[] = {
    {OP_SET, 0}, {OP_PRECISION, 0}, {OP_FIELD, F(4, 0)}, {OP_DECISION, D(1)},
    {OP_FIELD, F(4, 0)}, {OP_FIELD, F(3, 0)},
};
static const OpEntry rChangeAfterField[] = {
    {OP_READ, 0}, {OP_GET, F(4, 0)}, {OP_GET, F(4, 0)}, {OP_GET, F(3, 0)},
};
static const OpEntry wChangeBeforeField[] = {
    {OP_SET, 0}, {OP_DECISION, D(1)}, {OP_PRECISION, 0}, {OP_FIELD, F(4, 0)}, {OP_FIELD, F(3, 0)},
};
static const OpEntry rChangeBeforeField[] = {
    {OP_READ, 0}, {OP_GET, F(4, 0)}, {OP_GET, F(3, 0)},
};
static const OpEntry wTwoGroupsJoin[] = {
    {OP_SET, 1}, {OP_PRECISION, 0}, {OP_FIELD, F(4, 0)}, {OP_GROUP, 0},
    {OP_PRECISION, 0}, {OP_FIELD, F(4, 0)},
};
static const OpEntry rTwoGroupsJoin[] = {
    {OP_GROUP, 0}, {OP_READ, 0}, {OP_GET, F(4, 0)},
};

static const Scenario scenarios[] = {
    {"ordinary interior values", 3, 2, wInterior, rInterior, 2, {
        {0x449A5000u, 0x42290000u, 0xC5B17100u},        /* 1234.5, 42.25, -5678.125 */
        {0xC2C80000u, 0x00000000u, 0x42C80000u},        /* -100.0, 0.0, 100.0: 0.0 is an interior code */
    }},
    {"the endpoints are codes, not values", 3, 2, wInterior, rInterior, 2, {
        {0xC6200000u, 0xC4800000u, 0xC6200000u},        /* exactly min on all three: code 0 */
        {0x46200000u, 0x44800000u, 0x46200000u},        /* exactly max: code (1<<21)-1 */
    }},
    {"values outside the range", 3, 2, wInterior, rInterior, 2, {
        {0xC63B8000u, 0xC59C4000u, 0xC63B8000u},        /* -12000, -5000, -12000: below min */
        {0x463B8000u, 0x459C4000u, 0x463B8000u},        /* 12000, 5000, 12000: above max, y is outside */
    }},
    {"an interior value that rounds onto the reserved code 0", 2, 1, wSingle, rSingle, 1, {
        {0xC61FFFFFu, 0xC47FFFFFu, 0xC61FFFFFu},         /* one float32 step inside the range */
    }},
    {"float32 rounding, where a double model answers differently", 2, 1, wSingle, rSingle, 1, {
        {0x46180000u, 0x44780004u, 0xC6180000u},         /* 9728.0, 992.000244140625, -9728.00048828125 */
    }},
    {"netLOD 1: the same values as raw IEEE bits", 3, 2, wHiRes, rInterior, 2, {
        {0x46180000u, 0x44780004u, 0xC6180000u},
        {0xC63B8000u, 0xC59C4000u, 0xC63B8000u},         /* outside the range, and nothing is lost */
    }},
    /* Twenty vectors is 1260 bits, past the 116 byte small-packet limit, so the group splits. */
    {"twenty vectors: the group splits on a word boundary", 21, 20, wBig, rBig, 20, {
        {0x449A5000u, 0x42290000u, 0xC5B17100u},
    }, 20, 0, 0},
    {"UnitConvert at 11 bits: interior, code 0, the top code", 4, 4, wUnit, rUnit, 3, {
        {0x3F000000u, 0xBF800000u, 0x3F800000u},         /* 0.5, -1.0, 1.0: interior, 0, (1<<11)-1 */
        {0x00000000u, 0xBF800000u, 0x3F800000u},         /* 0.0 is interior */
        {0xBFCCCCCDu, 0xBF77EDFBu, 0x3F000000u},         /* -1.6, -0.9684750437736511, 0.5 */
    }},
    {"the wire carries 2: UnitConvert and the position converters both take the long path",
     3, 3, wWire2, rWire2, 2, {
        {0x3F000000u, 0xBF800000u, 0x3F800000u}, {0x449A5000u, 0x42290000u, 0xC5B17100u},
    }},
    {"the wire carries 3", 3, 3, wWire3, rWire2, 2, {
        {0x3F000000u, 0xBF800000u, 0x3F800000u}, {0x449A5000u, 0x42290000u, 0xC5B17100u},
    }},
    {"the second field says unchanged: the reader keeps what it has", 5, 4, wUnchanged, rUnchanged, 2, {
        {0x3F000000u, 0xBF800000u, 0x3F800000u}, {0x449A5000u, 0x42290000u, 0xC5B17100u},
    }},
    {"a reader that starts fresh, then reads what the wire carries", 2, 3, wFresh, rFresh, 1, {
        {0x3F000000u, 0xBF800000u, 0x3F800000u},
    }},

    /* The per-client decision, run as the engine's own IsLocalTo, and what it does to the same value.
     * The assignment at NetState.cpp:1802 is quoted and applied here, not run. */
    {"not local to this client: the gated converters stay short", 3, 2, wNotLocal, rNotLocal, 1, {
        {0x3E99999Au},                                     /* 0.3, the same value on both sides */
    }},
    {"the same client: the gated converters take raw IEEE bits, the other two do not", 3, 2, wLocal,
     rLocal, 1, {
        {0x3E99999Au},
    }},
    {"the same float, encoded both ways in one group through the engine's own WriteFixed", 11, 8,
     wBothWays, rBothWays, 1, {
        {0x3E99999Au},
    }},
    {"a guest slot: the rule reaches one entry and matches", 2, 1, wGuestMatch, rGuestMatch, 1, {
        {0x3E99999Au},
    }},
    {"a guest table that does not match: the rule says no", 2, 1, wGuestMiss, rGuestMiss, 1, {
        {0x3E99999Au},
    }},
    {"a player id past the array: the bounds check answers before the table", 2, 1, wPastArray,
     rPastArray, 1, {
        {0x3E99999Au},
    }},
    {"three converters that do not change with precision", 5, 3, wNotGated, rNotGated, 2, {
        {0x3E99999Au},
        {0xBF7FFCB9u, 0x3ECCCCCDu, 0x3F333333u, 0x3F800000u},
    }},

    /* The quaternion half of ReadMatrix only: four fields, X, Y, Z, S, at 13 bits. What follows in the
     * source is D3DXQuaternionNormalize and D3DXMatrixRotationQuaternion, which are not engine code here.
     * Before any normalisation the decoded components are not unit length. */
    {"quaternion X, Y, Z, S in that order: code 0 and the top code, and an interior rounding", 1, 1,
     wQuat, rQuat, 1, {
        {0xBF7FFCB9u, 0x3ECCCCCDu, 0x3F333333u, 0x3F800000u},   /* code 0, 5733, 6962, 8191 */
    }},
    {"a quaternion component that rounds onto the reserved code 0", 1, 1, wQuat, rQuat, 1, {
        {0xBF7FFCB9u, 0x3F333333u, 0x3ECCCCCDu, 0xBF3FEC57u},   /* -0.99995, then 1025: the rounding case */
    }},

    /* Across a sequence of groups, through the engine's own four functions. The order the source uses at
     * NetState.cpp:1077 is SetNetLOD then SetNetPrecision at the group start and then the fields, and at
     * :1162 ReadNetLOD at the group start and then the fields. A group boundary is a step, so both halves
     * walk the same structure. Rows the model builds from a rule are the same rule both files apply. */
    {"three groups in order: A sets the precision, B says unchanged, C changes it again", 10, 8,
     wThreeGroups, rThreeGroups, 1, {{0x3E99999Au}}, 0, 0, 2},
    {"a group that arrives in two packets, then a group that says unchanged", 20, 19,
     wSplitThenUnchanged, rSplitThenUnchanged, 16, {
        {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0},
        {0xBF800000u, 0x3F000000u, 0xBF400000u},   /* -1.0, 0.5, -0.75: the row the second group writes */
     }, 15, 0, 2},
    /* The shape MountedTurret::Write has at NetState.cpp:1798: the value is changed inside the group and
     * the fields written after it use the new value. Here the change comes after the field, so the field
     * cannot carry it, and the reader that read the field first has nothing to tell it. */
    {"the change happens inside the group, after the field: the field cannot carry it", 6, 4,
     wChangeAfterField, rChangeAfterField, 1, {{0x3E99999Au}}, 0, 0, 2},
    {"the same group with the change before the field: the field carries it, and the reader agrees", 5, 3,
     wChangeBeforeField, rChangeBeforeField, 1, {{0x3E99999Au}}, 0, 0, 2},
    {"the same bytes, read by a reader that carries the state from the earlier group", 6, 3,
     wTwoGroupsJoin, rTwoGroupsJoin, 1, {{0x3E99999Au}}, 0, 0, 1},
    /* What a reader that joined and never saw the group that established the value starts with is measured
     * through the engine's own BeginNetLOD here, not assumed to be 0. */
    {"the same bytes, read by a reader that joined and never saw an earlier group", 6, 3,
     wTwoGroupsJoin, rTwoGroupsJoin, 1, {{0x3E99999Au}}, 0, 0, 2},
};

extern "C" const char* position_report(void)
{
    emit("{\"input\":{\"type\":%d,\"src\":%d,\"group_sequence\":%d,\"dohires\":0", TYPE, SRC, SEQ);
    emit(",\"note\":\"the engine's own BeginNetLOD, SetNetLOD, SetNetPrecision, ReadNetLOD, "
         "NetGame::GetMyGuest and NetGame::IsLocalTo are called here; SetNetLOD takes a bool so it can "
         "only make 0 or 1, and the check puts 2 and 3 on the wire directly and says so. The assignment "
         "at NetState.cpp:1802 is quoted and applied by this file, not run. Which branch IsLocalTo took "
         "is not observable from outside, so only what it returns is compared. The guest table is an "
         "input both halves are given\"}");

    emit(",\"scenarios\":[");
    for (int i = 0; i < (int)(sizeof(scenarios) / sizeof(scenarios[0])); ++i) {
        Scenario s = scenarios[i];
        encodeScenario(s);                                   /* the writer half, group by group */
        int rStart[24], rEnd[24];
        int nrGroups = groupRanges(s.r, s.nr, rStart, rEnd);

        if (s.rs == 1) { netLOD = sessionState[0]; lastNetLOD = sessionState[1]; }
        else if (s.rs == 2) { BeginNetLOD(); }               /* what a reader that joins has, measured */
        else { netLOD = stateAfterWrite[0]; lastNetLOD = stateAfterWrite[1]; }

        bool bad = false;
        int idxWrite = 0;
        for (int j = 0; j < grpCount; ++j) {
            grpReaderBefore[j][0] = netLOD; grpReaderBefore[j][1] = lastNetLOD;
            NetPktGroup* group = receiveGroup(j);
            if (!group) { bad = true; break; }
            group->BeginRead();
            unsigned int v = 0;
            group->ReadBits(v, 1);
            grpFirstBit[j] = (int)v;
            grpFieldBits[j] = 1;
            if (!v) { unsigned int w = 0; group->ReadBits(w, 2); grpFieldBits[j] = 3; }

            /* Pass one: the codes, read back out of the bytes with the widths the writer used. */
            group->BeginRead();
            for (int k = grpStart[j]; k < grpEnd[j]; ++k) {
                const OpEntry& e = s.w[k];
                if (e.op == OP_READ || e.op == OP_SET || e.op == OP_BEGIN || e.op == OP_DECISION) continue;
                if (e.op == OP_PRECISION || e.op == OP_WIRE) { stepPrecision(*group); continue; }
                int order[4], n;
                orderFor(e.op, e.arg, order, &n);
                for (int c = 0; c < n; ++c, ++idxWrite) {
                    unsigned int x = 0;
                    group->ReadBits(x, fieldsWrite[idxWrite].bits);
                    fieldsWrite[idxWrite].code = x;
                }
            }

            /* Pass two: the same bytes, read with the engine's own reader. A decision is a writer-side
             * act, so it is skipped here; the reader's state comes from the wire, and from what it
             * already had when the field said unchanged. */
            group->BeginRead();
            for (int k = rStart[j]; k < rEnd[j]; ++k) {
                const OpEntry& e = s.r[k];
                if (e.op == OP_BEGIN) { BeginNetLOD(); continue; }
                if (e.op == OP_DECISION) continue;
                if (e.op == OP_READ) {
                    ReadNetLOD(*group);                       /* NetState.cpp:309, the engine's own reading */
                    observed[nObserved][0] = netLOD;
                    observed[nObserved][1] = lastNetLOD;
                    nObserved++;
                    continue;
                }
                int order[4], n;
                orderFor(e.op, e.arg, order, &n);
                int base = nFieldsRead;
                recordFields(order, n, fieldsRead, &nFieldsRead);
                if (e.op == OP_GET) {
                    float val = 0.0f;
                    int c = e.arg / 8;
                    if (c == 3) { ReadSwitch(*group, val); fieldsRead[base].decoded = toBits(val); }
                    else if (c == 4) { ReadThrust(*group, val); fieldsRead[base].decoded = toBits(val); }
                    else if (c == 5) { ReadAngle(*group, val); fieldsRead[base].decoded = toBits(val); }
                    else if (c == 6) { ReadJetFuel(*group, val); fieldsRead[base].decoded = toBits(val); }
                    else {
                        PblQuaternion q;
                        ReadQuaternion(*group, q);
                        float part[4] = {q._fX, q._fY, q._fZ, q._fS};
                        for (int c = 0; c < n; ++c) fieldsRead[base + c].decoded = toBits(part[c]);
                    }
                    continue;
                }
                PblVector3 p;
                if (e.op == OP_VEC_UNIT) ReadUnitVector(*group, p);   /* NetState.cpp:2169 */
                else ReadPositionVector(*group, p);                   /* NetState.cpp:2185 */
                for (int c = 0; c < n; ++c)
                    fieldsRead[base + c].decoded = toBits(c == 0 ? p.x : (c == 1 ? p.y : p.z));
            }
            grpReaderAfter[j][0] = netLOD; grpReaderAfter[j][1] = lastNetLOD;
        }
        if (bad) { emit("%s{\"name\":\"%s\",\"error\":\"the group never completed\"}", i ? "," : "", s.name); continue; }
        stateAfterRead[0] = netLOD;
        stateAfterRead[1] = lastNetLOD;
        sessionState[0] = stateAfterRead[0];
        sessionState[1] = stateAfterRead[1];

        emit("%s{\"name\":\"%s\",", i ? "," : "", s.name);
        emit("\"steps\":{\"w\":[");
        for (int k = 0; k < s.nw; ++k) emit("%s[%d,%d]", k ? "," : "", s.w[k].op, s.w[k].arg);
        emit("],\"r\":[");
        for (int k = 0; k < s.nr; ++k) emit("%s[%d,%d]", k ? "," : "", s.r[k].op, s.r[k].arg);
        emit("]}");
        emit(",\"values\":[");
        for (int v = 0; v < s.nvec; ++v) {
            int n = rowLength(s, v);
            emit("%s[", v ? "," : "");
            for (int k = 0; k < n; ++k) emit("%s%u", k ? "," : "", s.v[v][k]);
            emit("]");
        }
        emit("],\"packets\":%d", packetCount);
        emit(",\"bytes\":[");
        for (int d = 0; d < wireCount; ++d) {
            emit("%s\"", d ? "," : "");
            for (int k = 0; k < wireLen[d]; ++k) emit("%02x", wire[wireBase[d] + k]);
            emit("\"");
        }
        emit("],\"complete_after_each\":[");
        for (int k = 0; k < wireCount; ++k) emit("%s%s", k ? "," : "", completed[k] ? "true" : "false");
        emit("]");
        emit(",\"state_after_write\":[%d,%d]", stateAfterWrite[0], stateAfterWrite[1]);
        emit(",\"state_after_read\":[%d,%d]", stateAfterRead[0], stateAfterRead[1]);
        emit(",\"observed\":[");
        for (int k = 0; k < nObserved; ++k) emit("%s[%d,%d]", k ? "," : "", observed[k][0], observed[k][1]);
        emit("]");
        emit(",\"fields_write\":[");
        for (int f = 0; f < nFieldsWrite; ++f)
            emit("%s{\"name\":\"%s\",\"bits\":%d,\"min\":%u,\"max\":%u,\"code\":%u}",
                 f ? "," : "", CONV_NAME[fieldsWrite[f].conv], fieldsWrite[f].bits,
                 fieldsWrite[f].min, fieldsWrite[f].max, fieldsWrite[f].code);
        emit("],\"fields_read\":[");
        for (int f = 0; f < nFieldsRead; ++f)
            emit("%s{\"name\":\"%s\",\"bits\":%d,\"decoded\":%u}",
                 f ? "," : "", CONV_NAME[fieldsRead[f].conv], fieldsRead[f].bits, fieldsRead[f].decoded);
        emit("]");
        if (grpCount > 1) {
            emit(",\"groups\":[");
            for (int j = 0; j < grpCount; ++j)
                emit("%s{\"idx\":%d,\"writer_before\":[%d,%d],\"first_bit\":%d,\"field_bits\":%d,"
                     "\"reader_before\":[%d,%d],\"reader_after\":[%d,%d]}",
                     j ? "," : "", j, grpWriterBefore[j][0], grpWriterBefore[j][1], grpFirstBit[j],
                     grpFieldBits[j], grpReaderBefore[j][0], grpReaderBefore[j][1],
                     grpReaderAfter[j][0], grpReaderAfter[j][1]);
            emit("]");
        }
        if (nDecisions) {
            emit(",\"decision\":[");
            for (int d = 0; d < nDecisions; ++d)
                emit("%s{\"player\":%d,\"other\":%d,\"guest\":%d,\"result\":%s,\"netLOD_before\":%d,"
                     "\"netLOD_after\":%d}", d ? "," : "", decisions[d][0], decisions[d][1],
                     decisions[d][2], decisions[d][3] ? "true" : "false",
                     decisions[d][4], decisions[d][5]);
            emit("]");
        }
        emit("}");
    }
    emit("]}");
    return out;
}
