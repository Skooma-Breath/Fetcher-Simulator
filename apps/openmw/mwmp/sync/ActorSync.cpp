#include "ActorSync.hpp"
#include <components/debug/debuglog.hpp>

namespace mwmp
{
    ActorSync::ActorSync(NetworkClient& client) : mClient(client) {}

    void ActorSync::update(float /*dt*/) { /* Phase 4 */ }

    void ActorSync::onAuthorityGrant(const std::string& cellId)
    {
        mAuthority[cellId] = true;
        Log(Debug::Info) << "[MP] ActorSync: authority granted for " << cellId;
    }

    void ActorSync::onAuthorityRevoke(const std::string& cellId)
    {
        mAuthority[cellId] = false;
        Log(Debug::Info) << "[MP] ActorSync: authority revoked for " << cellId;
    }

    void ActorSync::onActorListUpdate(const ActorList& list)
    {
        mCells[list.cellId] = list;
        // Phase 4: apply positions/stats to live NPC Ptrs
    }

    bool ActorSync::hasAuthority(const std::string& cellId) const
    {
        auto it = mAuthority.find(cellId);
        return (it != mAuthority.end()) && it->second;
    }
} // namespace mwmp
