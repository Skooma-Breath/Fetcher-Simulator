#include "ObjectSync.hpp"
#include <components/debug/debuglog.hpp>
namespace mwmp
{
    ObjectSync::ObjectSync(NetworkClient& client) : mClient(client) {}
    void ObjectSync::onDoorStateChanged(const std::string& cellId, const std::string& refId,
                                        uint32_t /*refNum*/, bool open, bool /*locked*/, int /*lockLevel*/)
    {
        Log(Debug::Verbose) << "[MP] ObjectSync: door " << refId
                            << " in " << cellId << " open=" << open;
        // Phase 4: send PacketDoorState
    }
} // namespace mwmp
