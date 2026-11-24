/* Stubs for what the wire layer references but does not own. The check never calls these; they
 * are here only to link. The names the linker asks for are themselves a result: the wire layer's
 * dependency closure is the QOS queues, the time base, and the game-layer packet release. */
namespace NetGameP { void FreePackets() {} }
