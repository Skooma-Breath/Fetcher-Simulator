#ifndef OPENMW_MWMP_SYNC_ACTORSYNC_HPP
#define OPENMW_MWMP_SYNC_ACTORSYNC_HPP

#include <string>
#include <unordered_map>
#include <components/openmw-mp/Base/BaseActor.hpp>

namespace mwmp
{
    class NetworkClient;

    // Manages authority and synchronisation of NPC/creature actors.
    // Full implementation in Phase 4 (actor sync).
    class ActorSync
    {
    public:
        explicit ActorSync(NetworkClient& client);

        void update(float dt);

        // Called when server grants/revokes authority over a cell's actors
        void onAuthorityGrant (const std::string& cellId);
        void onAuthorityRevoke(const std::string& cellId);

        // Handle inbound actor position batch from server
        void onActorListUpdate(const ActorList& list);

        bool hasAuthority(const std::string& cellId) const;

    private:
        NetworkClient& mClient;
        std::unordered_map<std::string, ActorList> mCells; // cellId → actor states
        std::unordered_map<std::string, bool>      mAuthority;
    };

} // namespace mwmp
#endif // OPENMW_MWMP_SYNC_ACTORSYNC_HPP
