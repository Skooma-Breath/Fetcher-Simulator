#ifndef OPENMW_MWMP_SYNC_OBJECTSYNC_HPP
#define OPENMW_MWMP_SYNC_OBJECTSYNC_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace mwmp { class NetworkClient; }

namespace mwmp
{
    class ObjectSync
    {
    public:
        explicit ObjectSync(NetworkClient& client);

        // Called by the worldimp.cpp hook when the local player activates a door.
        // Sends a PacketDoorState intent to the server.
        void onDoorStateChanged(const std::string& cellId, const std::string& refId,
                                uint32_t refNum, bool isOpen, bool isLocked, int lockLevel);

        // Called by the protocol handler when the server broadcasts a door state.
        // Attempts to apply immediately; queues for retry if cells aren't loaded yet.
        void onServerDoorState(const std::string& cellId, const std::string& refId,
                               uint32_t refNum, bool isOpen);

        // Called each frame — retries any pending door states that failed to apply.
        void update(float dt);

    private:
        // Try to find and activate a door across all active cells.
        // Returns true if the door was found and activated.
        bool tryApplyDoorState(const std::string& refId, uint32_t refNum, bool isOpen);

        NetworkClient& mClient;

        struct PendingDoor
        {
            std::string cellId;
            std::string refId;
            uint32_t    refNum;
            bool        isOpen;
            float       retryTimer = 0.f;
        };
        std::vector<PendingDoor> mPendingDoors;
    };

} // namespace mwmp

#endif // OPENMW_MWMP_SYNC_OBJECTSYNC_HPP
