/* Stand-in for the one symbol the packet layer reaches for. The check calls the position converters
 * and the position readers; nothing here is read by the check, so nothing here is a model of engine
 * behaviour. */
namespace NetGameP { void FreePackets() {} }
