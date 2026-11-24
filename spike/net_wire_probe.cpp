/* What the packet wire layer actually demands. Build with the net objects only and let the
 * linker name what is missing - that is the dependency closure of the transport seam, not a guess.
 */
#include <stdio.h>

#include "NetPacket.h"
#include "NetComm.h"

extern "C" int probe_wire(void)
{
    NetPacket::Create();

    NetPktGroup g;
    g.isSmall = true;
    NetPacket* p = NetPacket::Alloc(true);
    g.PushBack(p);
    g.BeginWrite();
    g.Write(true);
    g.Write(12345);
    g.WriteBits(0xdeadbeefu, 20);
    float f = 0.25f;
    g.WriteFixed(f, -1.0f, 1.0f, 10);
    g.EndWrite();

    /* Re-read the same bytes as they would arrive on the wire: the packed header first. */
    char* bytes = p->mData + p->mCommIndex;
    int len = p->mMaxIndex - p->mCommIndex;
    printf("wire bytes %d, first byte type %d\n", len, (unsigned char)bytes[0]);

    NetPacket* q = NetPacket::Alloc(true);
    memcpy(q->mData, p->mData, (size_t)(p->mMaxIndex + 4));
    unsigned int second = NetComm::Header(q).group;
    (void)second;
    return len;
}
