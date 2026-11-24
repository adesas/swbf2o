#include "NetPacket.h"

/* Stand-ins for the parts the transport check does not run.

The names here are exactly the ones the linker reports as unresolved when the socket layer is linked on
its own (see tools/build_net_transport_check.sh, which writes them to build/net_transport_undefined.txt).
They are the socket layer's dependency closure: the game layer, the Galaxy API, and the journal trace
call. The check does not exercise them, but they have to exist for the module to link, and writing them
down is what makes the seam visible: the packet and transport layers can be tested on their own, the
routing of a received packet into game state cannot.

The values are the ones the code would see with no game state: no key, no guest, no Galaxy, nothing
authorised, no join source. Only names that the linked objects do not define are written here -
`GetGalaxyMatchmakingForNetComm` is defined by NetComm.cpp itself, so it must not be duplicated. `hostshort` is not a stand-in - it is the engine's own default, taken from
NetAdmin.cpp, and the check reports it. */
int hostshort = 3658;
unsigned long long netEnteredLobbyId = 0;
int gDebugCount = 0;

void JournalReceiveTraceStub() {}

namespace NetGame {
    unsigned int GetKey(int) { return 0; }
    int GetMyGuest(int, int) { return -1; }
    bool IsEnabled() { return false; }
}

namespace NetGameP {
    void FreePackets() {}
    int GetHostIndex() { return -1; }
    void Recv(NetPacket*, const void*, int) {}
}

namespace NetSetupP {
    void Recv(NetPacket*, const void*) {}
    void RecvLan(NetPacket*, const void*) {}
}

namespace NetCommP {
    int FindJoinSource(const int*) { return -1; }
}

namespace gf { bool isInitialized() { return false; } }

void* GetGalaxyNetworking() { return 0; }

bool IsAdminPacketAuthorized(const unsigned char*) { return false; }
