#ifndef OPENMW_MWMP_SYNC_OBJECTSYNC_HPP
#define OPENMW_MWMP_SYNC_OBJECTSYNC_HPP
#include <string>
namespace mwmp { class NetworkClient; }
namespace mwmp
{
    // Phase 4: door/container/object synchronisation.
    class ObjectSync
    {
    public:
        explicit ObjectSync(NetworkClient& client);
        void onDoorStateChanged(const std::string& cellId, const std::string& refId,
                                uint32_t refNum, bool open, bool locked, int lockLevel);
    private:
        NetworkClient& mClient;
    };
} // namespace mwmp
#endif
