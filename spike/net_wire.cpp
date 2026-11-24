/* The net packet wire layer, run as the engine's own code, for the check in
 * browser/net_wire_check.mjs. Everything here calls the engine's functions - WriteBits, ReadBits,
 * WriteFixed, ReadFixed, WriteBytes, ReadBytes, PackHeader, UnpackHeader, RecvGroup - so a
 * disagreement with tools/net_wire_check.py is a disagreement about the format, not a tautology.
 *
 * The only thing transcribed rather than called is MakeReceivedPacket, which is static in
 * pcNetSocket.cpp; its body is reproduced where it is used and named there.
 *
 * Prints JSON with the same fields the oracle prints:
 *   ops        the operations, in wire order - the input, so both sides must describe it identically
 *   datagrams  what would go on the wire, per packet, with the header fields
 *   decode     receiving the datagrams in the order given: completion, then the values read back
 *
 * Usage from node: wire_report("01") -> pointer to a NUL-terminated JSON string.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "NetPacket.h"
#include "NetComm.h"

void __fastcall PackHeader(NetPacket* pPacket);
unsigned int __fastcall UnpackHeader(NetPacket* pPacket);
bool __fastcall RecvGroup(NetPktGroup* pComplete, NetPacket* pPacket);

enum OpKind { OP_BOOL = 0, OP_BITS, OP_FIXED, OP_BYTES, OP_FLOATBITS };

struct OpEntry {
    int kind;
    int bits;              /* field width in bits; 0 for a raw byte field */
    unsigned int value;    /* the value, or the IEEE bits of f for float fields */
    unsigned int min, max; /* fixed field range, as IEEE bits */
    int bytes;             /* OP_BYTES: number of raw bytes */
    int hires;             /* doHiRes: write and read the field as a raw float, whatever bits says */
};

/* The same operations as tools/net_wire_check.py, in the same order. The last two force a second
 * packet, which is where the group numbering and the reassembly matter. */
static const OpEntry ops[] = {
    { OP_BOOL,      1, 1u,           0u,          0u,          0, 0 },
    { OP_BITS,     20, 0xdeadbeefu,  0u,          0u,          0, 0 },  /* crosses a word boundary */
    { OP_BITS,     32, 12345u,       0u,          0u,          0, 0 },
    { OP_FIXED,    10, 1048576000u,  3212836864u, 1065353216u, 0, 0 },  /* 0.25 in [-1,1]: interior */
    { OP_FIXED,    10, 1091567616u,  0u,          1092616192u, 0, 0 },  /* 9.0 in [0,10]: interior, rounds back to itself */
    { OP_FIXED,    10, 3212836864u,  0u,          1092616192u, 0, 0 },  /* below min: code 0 means fMin, not a sample */
    { OP_FIXED,    10, 1092616192u,  0u,          1092616192u, 0, 0 },  /* exactly fMax: code uMax means fMax, not 1023/1022*10 */
    { OP_FIXED,     7, 3208442743u,  3212836864u, 1065353216u, 0, 0 },  /* code 17: engine reads 0xbf3aebae, a double model reads 0xbf3aebaf */
    { OP_FIXED,     7, 1069547520u,  3212836864u, 1065353216u, 0, 1 },  /* doHiRes: the 7 bits are ignored, the field is a raw float */
    { OP_BITS,      3, 7u,            0u,          0u,          0, 0 },  /* leaves a partial word */
    { OP_BYTES,     0, 0u,            0u,          0u,          4, 0 },  /* raw bytes: word aligned */
    { OP_FLOATBITS,32, 1069547520u,  0u,          0u,          0, 0 },  /* 1.5 as raw IEEE bits */
    { OP_BYTES,     0, 0u,            0u,          0u,        100, 0 },  /* past the 116 byte limit */
    { OP_BITS,     12, 0x1234u,       0u,          0u,          0, 0 },  /* continues across the split */
};
static const int OP_COUNT = (int)(sizeof(ops) / sizeof(ops[0]));

/* Float fields: the model stores f as a float; here the values are given as IEEE bits so both sides
 * report the same integers. Reconstruct the float from the bits when calling the engine. */
static float fromBits(unsigned int bits)
{
    unsigned int u = (unsigned int)bits;
    return *(const float*)&u;
}

static const int TYPE = 2;   /* PKT_QUERY: below 0x1e, so the receive prefix is 3 */
static const int SRC = 64;   /* host destination, so Send numbers the group */
static const int SEQ = 7;    /* what nextGroupNum would give */

static char out[4096];
static int outPos;

static void emit(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    outPos += vsnprintf(out + outPos, sizeof(out) - outPos, fmt, ap);
    va_end(ap);
}

static void hexBytes(const unsigned char* p, int n)
{
    for (int i = 0; i < n; ++i) emit("%02x", p[i]);
}

/* The raw bytes: the same rule as the oracle's pattern(). */
static void pattern(int n, unsigned char* buf)
{
    for (int i = 0; i < n; ++i) buf[i] = (unsigned char)((i * 3 + 1) & 0xff);
}

static unsigned char wire[2048];
static int wireBase[8], wireLen[8], wireCount;
static int hdrFields[8][6];
static char values[16][256];
static int completed[8];

static void writeOp(NetPktGroup& g, const OpEntry& op)
{
    unsigned char raw[128];
    switch (op.kind) {
    case OP_BOOL: g.Write(op.value != 0); break;
    case OP_BITS: g.WriteBits((unsigned int)op.value, op.bits); break;
    case OP_FIXED:
        doHiRes = op.hires != 0;
        g.WriteFixed(fromBits(op.value), fromBits(op.min), fromBits(op.max), op.bits);
        break;
    case OP_FLOATBITS: g.WriteBits((unsigned int)op.value, 32); break;
    default:
        pattern(op.bytes, raw);
        g.WriteBytes((char*)raw, op.bytes);
        break;
    }
}

/* Read one operation back, in the same representation the oracle reports. */
static void readOp(NetPktGroup& g, const OpEntry& op, char* dst)
{
    unsigned char raw[128];
    if (op.kind == OP_BYTES) {
        memset(raw, 0, (size_t)op.bytes);
        g.ReadBytes((char*)raw, op.bytes);
        for (int i = 0; i < op.bytes; ++i) sprintf(dst + i * 2, "%02x", raw[i]);
        dst[op.bytes * 2] = 0;
        return;
    }
    unsigned int v = 0;
    if (op.kind == OP_BOOL) {
        bool b = false;
        g.Read(b);
        v = b ? 1u : 0u;
    } else if (op.kind == OP_FLOATBITS) {
        float f = 0.0f;
        g.Read(f);
        v = *(const unsigned int*)&f;
    } else if (op.kind == OP_FIXED) {
        float f = 0.0f;
        doHiRes = op.hires != 0;
        g.ReadFixed(f, fromBits(op.min), fromBits(op.max), op.bits);
        v = *(const unsigned int*)&f;
    } else {
        g.ReadBits(v, op.bits);
    }
    sprintf(dst, "%u", v);
}

static void emitOps()
{
    emit("\"ops\":[");
    for (int i = 0; i < OP_COUNT; ++i) {
        unsigned char raw[128];
        int effective = (ops[i].hires || ops[i].bits >= 32) ? 32 : ops[i].bits;
        emit("%s{\"kind\":%d,\"bits\":%d,\"value\":%u,\"min\":%u,\"max\":%u,\"bytes_count\":%d,\"bytes\":[",
             i ? "," : "", ops[i].kind, ops[i].bits, (unsigned int)ops[i].value,
             (unsigned int)ops[i].min, (unsigned int)ops[i].max, ops[i].bytes);
        if (ops[i].kind == OP_BYTES) {
            pattern(ops[i].bytes, raw);
            for (int j = 0; j < ops[i].bytes; ++j) emit("%s%d", j ? "," : "", raw[j]);
        }
        emit("],\"hires\":%d,\"effective_bits\":%d}", ops[i].hires, effective);
    }
    emit("]");
}

/* Sender side: write the operations, then number the group as NetComm::Send(iDest, uType, pGroup)
 * does, and take the bytes as Deliver would send them (mData + mCommIndex .. mMaxIndex). */
static void encode()
{
    NetPacket::Create();

    NetPktGroup g;
    g.Reset();
    g.isSmall = true;
    g.BeginWrite();
    for (int i = 0; i < OP_COUNT; ++i) writeOp(g, ops[i]);
    g.EndWrite();

    const int iCount = g.pktCount;
    int at = 0;
    wireCount = 0;
    for (int i = 0; i < iCount; ++i) {
        NetPacket* p = g.PopFront();
        NetComm::Header(p).src = (unsigned char)SRC;
        NetComm::Header(p).type = (unsigned char)TYPE;
        NetComm::Header(p).count = (unsigned char)iCount;
        NetComm::Header(p).group = (unsigned short)SEQ;
        NetComm::Header(p).idx = (unsigned char)i;
        PackHeader(p);

        const int len = p->mMaxIndex - p->mCommIndex;
        wireBase[wireCount] = at;
        wireLen[wireCount] = len;
        memcpy(wire + at, p->mData + p->mCommIndex, (size_t)len);
        const NetComm::PktHeader& h = NetComm::Header(p);
        hdrFields[wireCount][0] = h.type;
        hdrFields[wireCount][1] = h.group;
        hdrFields[wireCount][2] = h.src;
        hdrFields[wireCount][3] = h.idx;
        hdrFields[wireCount][4] = h.count;
        hdrFields[wireCount][5] = h.compressed;
        at += len;
        wireCount++;
    }
}

/* Receiver side: MakeReceivedPacket (transcribed - it is static in pcNetSocket.cpp), UnpackHeader,
 * RecvGroup, then read the same operations back through the engine's reader. */
static void receive(const int* order, int n)
{
    NetPacket::Create();

    NetPktGroup complete;
    complete.Reset();
    complete.isSmall = true;

    for (int k = 0; k < n; ++k) {
        const int i = order[k];
        const unsigned char* raw = wire + wireBase[i];
        const int len = wireLen[i];
        const int prefix = raw[0] >= 0x1e ? 0 : 3;              /* MakeReceivedPacket */
        NetPacket* p = NetPacket::Alloc(len <= 116 - prefix);
        p->BeginWrite();
        memcpy(p->mData + prefix, raw, (size_t)len);
        p->mCommIndex = (char)prefix;
        p->mMaxIndex = (short)(len + prefix);

        UnpackHeader(p);
        NetComm::Header(p).src = (unsigned char)SRC;            /* as DoRecvs sets it */
        completed[k] = RecvGroup(&complete, p) ? 1 : 0;
    }

    memset(values, 0, sizeof(values));
    if (complete.pktCount) {
        complete.BeginRead();
        for (int i = 0; i < OP_COUNT; ++i) readOp(complete, ops[i], values[i]);
    }
}

extern "C" const char* wire_report(const char* orderString)
{
    outPos = 0;
    encode();

    int order[8];
    int n = 0;
    for (const char* s = orderString; *s; ++s) {
        if (*s >= '0' && *s <= '9') order[n++] = *s - '0';
    }
    receive(order, n);

    emit("{");
    emitOps();
    emit(",\"datagrams\":[");
    for (int i = 0; i < wireCount; ++i) {
        emit("%s{\"bytes\":\"", i ? "," : "");
        hexBytes(wire + wireBase[i], wireLen[i]);
        emit("\",\"header\":{\"type\":%d,\"group\":%d,\"src\":%d,\"idx\":%d,\"count\":%d,\"compressed\":%d},\"len\":%d}",
             hdrFields[i][0], hdrFields[i][1], hdrFields[i][2], hdrFields[i][3], hdrFields[i][4],
             hdrFields[i][5], wireLen[i]);
    }
    emit("],\"decode\":{\"order\":[");
    for (int i = 0; i < n; ++i) emit("%s%d", i ? "," : "", order[i]);
    emit("],\"complete_after_each\":[");
    for (int i = 0; i < n; ++i) emit("%s%s", i ? "," : "", completed[i] ? "true" : "false");
    emit("],\"values\":[");
    for (int i = 0; i < OP_COUNT; ++i) {
        emit("%s%s%s", i ? "," : "", ops[i].kind == OP_BYTES ? "\"" : "", values[i]);
        if (ops[i].kind == OP_BYTES) emit("\"");
    }
    emit("]}");
    emit("}");
    return out;
}
