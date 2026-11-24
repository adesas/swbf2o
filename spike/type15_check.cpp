/* A type-15 (PKT_GAME_REPLICA_UPDATE) payload, written with the engine's own writers and read back
 * with the engine's own readers. Both live in NetGame.cpp: WriteDestMask, WriteTurnLowBits,
 * WriteSwitchResponse, WriteMove, and ReadDestMask, ReadTurnLowBits, ReadSwitchResponse, ReadMove.
 *
 * The point of using the engine's code rather than a transcription of it is that this check can fail.
 * A transcription can only agree with itself; the engine's readers can disagree with the model in
 * tools/type15_check.py, and when they do, the model is wrong.
 *
 * What the engine actually reads for one host and one player, in order:
 *   ReadDestMask          32 bits, then 32 bits - the mask is 64 bits wide
 *   ReadTurnLowBits       11 bits, with 0x7ff meaning "no turn" and 0x7fe meaning escape + 1 bit,
 *                         then a rollover correction against the previous turn
 *   clientTurn            ReadTurnLowBits again
 *   lag                   8 bits, then 128 is subtracted
 *   switch response       1 bit, then a turn
 *   moves                 1 bit "same" (copy the previous inputs), then 1 bit "end", then four fixed
 *                         fields - stick x and y at 7 bits in [-1,1], mouse x and y at 11 bits in
 *                         [-16,16] - then the buttons as 30 bits and 2 bits
 */
#include "NetPacket.h"
#include "NetComm.h"
#include "FLJoystick.h"
#include "NetGame.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

/* Internal to NetGame.cpp, so declared here rather than pulled from a header. */
void __fastcall PackHeader(NetPacket* pPacket);
unsigned int __fastcall UnpackHeader(NetPacket* pPacket);
bool __fastcall RecvGroup(NetPktGroup* pComplete, NetPacket* pPacket);
void WriteDestMask(NetPktGroup& group, unsigned __int64 mask);
void WriteTurnLowBits(NetPktGroup* pGroup, unsigned int uTurn);
void WriteSwitchResponse(NetPktGroup* pGroup, int iResponse, int iTurn);
void WriteMove(NetPktGroup* pGroup, ProcessedInputs* pPrev, ProcessedInputs* pCur);
void ReadDestMask(NetPktGroup& group, unsigned __int64& mask);
void ReadTurnLowBits(NetPktGroup& group, unsigned int& turn);
SwitchResponse ReadSwitchResponse(NetPktGroup& group, unsigned int turn, unsigned int);
bool ReadMove(NetPktGroup& group, const ProcessedInputs& previous, ProcessedInputs& current);

/* The engine keeps the high bits of the last turn in a global; the rollover correction reads it.
 * In the engine this global lives in NetGame.cpp. Here the extracted readers declare it and this
 * file defines it, because the check sets it to a known value before reading. */
unsigned int turnHighBits = 0;

/* Defined in another translation unit that this harness does not link. The harness only needs zeroed
 * storage, so it provides its own constructor rather than pulling in the joystick code. */
ProcessedInputs::ProcessedInputs() { memset((void*)this, 0, sizeof(*this)); }

static const int PKT_REPLICA = 15;
static const unsigned char SRC = 3;
static const unsigned short SEQ = 77;

static char out[16384];
static int pos = 0;
static char scen[8192];
static int scenPos = 0;

static void emitTo(char* buf, int* at, const char* fmt, ...)
{
    va_list ap;
    if (*at >= 8190) return;
    va_start(ap, fmt);
    *at += vsnprintf(buf + *at, 8192 - *at, fmt, ap);
    va_end(ap);
}

static unsigned char wire[512];
static int wireLen = 0;

struct Inputs {
    float stickX, stickY, mouseX, mouseY;
    unsigned int buttons;      /* mBoolInputs: 30 bits */
    unsigned int mouseMode;    /* mMouseMode: the other 2 bits of the same word */
};

struct Scenario {
    const char* name;
    unsigned int turnHighBits;
    unsigned __int64 destMask;
    unsigned int turn;             /* the full turn, as the writer is given it */
    unsigned int clientTurn;
    unsigned int lag;
    int switchResponse;            /* 0 none, 1 can, 2 cannot */
    unsigned int switchTurn;
    Inputs first;
    Inputs second;                /* identical to first, so the engine writes "same" */
};

/* Write one scenario into a group, pack it as NetComm::Send would, and keep the bytes. */
static void encodeScenario(const Scenario& s)
{
    NetPacket::Create();

    NetPktGroup g;
    g.Reset();
    g.isSmall = true;
    g.BeginWrite();

    WriteDestMask(g, s.destMask);
    WriteTurnLowBits(&g, s.turn);
    WriteTurnLowBits(&g, s.clientTurn);
    g.WriteBits(s.lag, 8);
    WriteSwitchResponse(&g, s.switchResponse, s.switchTurn);

    ProcessedInputs prev;
    memset((void*)&prev, 0, sizeof(prev));
    ProcessedInputs cur;
    memset((void*)&cur, 0, sizeof(cur));
    cur.mFloatInputs[0] = s.first.stickX;
    cur.mFloatInputs[1] = s.first.stickY;
    cur.mFloatInputs[2] = s.first.mouseX;
    cur.mFloatInputs[3] = s.first.mouseY;
    cur.bits.mBoolInputs = s.first.buttons;
    cur.bits.mMouseMode = s.first.mouseMode;

    WriteMove(&g, &prev, &cur);

    ProcessedInputs cur2;
    memset((void*)&cur2, 0, sizeof(cur2));
    cur2.mFloatInputs[0] = s.second.stickX;
    cur2.mFloatInputs[1] = s.second.stickY;
    cur2.mFloatInputs[2] = s.second.mouseX;
    cur2.mFloatInputs[3] = s.second.mouseY;
    cur2.bits.mBoolInputs = s.second.buttons;
    cur2.bits.mMouseMode = s.second.mouseMode;

    WriteMove(&g, &cur, &cur2);        /* identical inputs: the engine writes "same" */
    WriteMove(&g, &cur2, 0);           /* the end marker */

    g.EndWrite();

    const int iCount = g.pktCount;
    int at = 0;
    for (int i = 0; i < iCount; ++i) {
        NetPacket* p = g.PopFront();
        NetComm::Header(p).src = SRC;
        NetComm::Header(p).type = (unsigned char)PKT_REPLICA;
        NetComm::Header(p).count = (unsigned char)iCount;
        NetComm::Header(p).group = SEQ;
        NetComm::Header(p).idx = (unsigned char)i;
        PackHeader(p);
        const int len = p->mMaxIndex - p->mCommIndex;
        memcpy(wire + at, p->mData + p->mCommIndex, (size_t)len);
        at += len;
    }
    wireLen = at;
}

/* Feed the bytes back through the engine's receive path, then read them with the engine's readers. */
static void decodeScenario(const Scenario& s)
{
    NetPacket::Create();

    NetPktGroup complete;
    complete.Reset();
    complete.isSmall = true;

    const int prefix = wire[0] >= 0x1e ? 0 : 3;
    NetPacket* p = NetPacket::Alloc(wireLen <= 116 - prefix);
    p->BeginWrite();
    memcpy(p->mData + prefix, wire, (size_t)wireLen);
    p->mCommIndex = (char)prefix;
    p->mMaxIndex = (short)(wireLen + prefix);
    UnpackHeader(p);
    NetComm::Header(p).src = SRC;
    RecvGroup(&complete, p);

    turnHighBits = s.turnHighBits;
    complete.BeginRead();

    unsigned __int64 mask = 0;
    ReadDestMask(complete, mask);

    unsigned int turn = 0;
    ReadTurnLowBits(complete, turn);

    unsigned int clientTurn = 0;
    ReadTurnLowBits(complete, clientTurn);

    unsigned int lag = 0;
    complete.ReadBits(lag, 8);

    const SwitchResponse response = ReadSwitchResponse(complete, turn, (unsigned int)-1);

    ProcessedInputs previous;
    memset((void*)&previous, 0, sizeof(previous));
    ProcessedInputs current;
    memset((void*)&current, 0, sizeof(current));

    int movesRead = 0;
    bool ended = false;
    unsigned int fx[4], fy[4], mx[4], my[4], buttons[4], mouseMode[4];
    for (int i = 0; i < 4; ++i) { fx[i] = fy[i] = mx[i] = my[i] = buttons[i] = mouseMode[i] = 0; }
    for (int i = 0; i < 4; ++i) {
        if (!ReadMove(complete, previous, current)) { ended = true; break; }
        memcpy(&fx[i], &current.mFloatInputs[0], 4);
        memcpy(&fy[i], &current.mFloatInputs[1], 4);
        memcpy(&mx[i], &current.mFloatInputs[2], 4);
        memcpy(&my[i], &current.mFloatInputs[3], 4);
        buttons[i] = current.bits.mBoolInputs;
        mouseMode[i] = current.bits.mMouseMode;
        previous = current;
        movesRead++;
    }

    scenPos = 0;
    emitTo(scen, &scenPos, "{\"name\":\"%s\",\"bytes\":\"", s.name);
    for (int i = 0; i < wireLen; ++i) emitTo(scen, &scenPos, "%s%02x", i ? "" : "", wire[i]);
    emitTo(scen, &scenPos,
           "\",\"mask\":%llu,\"turn\":%u,\"client_turn\":%u,\"lag\":%d,\"switch_response\":%d,"
           "\"moves\":%d,\"ended\":%d,\"inputs\":[",
           (unsigned long long)mask, turn, clientTurn, (int)lag - 128, (int)response,
           movesRead, ended ? 1 : 0);
    for (int i = 0; i < movesRead; ++i) {
        emitTo(scen, &scenPos,
               "%s{\"stick_x\":%u,\"stick_y\":%u,\"mouse_x\":%u,\"mouse_y\":%u,\"buttons\":%u,"
               "\"mouse_mode\":%u}",
               i ? "," : "", fx[i], fy[i], mx[i], my[i], buttons[i], mouseMode[i]);
    }
    emitTo(scen, &scenPos, "]}");
}

extern "C" const char* type15_report(void)
{
    Scenario scenarios[2];

    Scenario& a = scenarios[0];
    a.name = "ordinary";
    a.turnHighBits = 0;
    a.destMask = 0x0000000000000003ull;
    a.turn = 0x123;
    a.clientTurn = 0x120;
    a.lag = 200;
    a.switchResponse = 1;            /* can switch */
    a.switchTurn = 0x123;            /* the same turn, so the reader answers SR_CAN */
    a.first.stickX = -0.5f; a.first.stickY = 0.25f;
    a.first.mouseX = -3.0f; a.first.mouseY = 12.0f;
    a.first.buttons = 0x12345678u;
    a.first.mouseMode = 0;
    a.second = a.first;

    Scenario& b = scenarios[1];
    b.name = "reserved";
    b.turnHighBits = 0x400;
    b.destMask = 0x0000000000000001ull;
    b.turn = (unsigned int)-1;        /* written as 0x7ff: no turn */
    b.clientTurn = 0x7fe;            /* written as 0x7fe plus the escape bit */
    b.lag = 128;
    b.switchResponse = 0;            /* none: the turn becomes -1 */
    b.switchTurn = 0x123;
    b.first.stickX = 0.75f; b.first.stickY = -1.0f;
    b.first.mouseX = 16.0f; b.first.mouseY = -16.0f;
    b.first.buttons = 0x3fffffffu;   /* all 30 button bits */
    b.first.mouseMode = 3;            /* and the other two bits of the same word */
    b.second = b.first;

    pos = 0;
    emitTo(out, &pos, "{\"scenarios\":[");
    for (int i = 0; i < 2; ++i) {
        encodeScenario(scenarios[i]);
        decodeScenario(scenarios[i]);
        emitTo(out, &pos, "%s%s", i ? "," : "", scen);
    }
    emitTo(out, &pos, "]}");
    return out;
}
