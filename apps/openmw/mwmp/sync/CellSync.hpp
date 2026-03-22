#ifndef OPENMW_MWMP_SYNC_CELLSYNC_HPP
#define OPENMW_MWMP_SYNC_CELLSYNC_HPP

#include <string>
#include <vector>
#include <components/openmw-mp/Base/BasePlayer.hpp>

namespace mwmp { class NetworkClient; }

namespace mwmp
{
    // Tracks which cells are currently loaded locally and syncs that list
    // with the server so it knows what to stream to us.
    // Full implementation in Phase 4.
    class CellSync
    {
    public:
        explicit CellSync(NetworkClient& client);

        void onCellLoaded  (const CellId& cell);
        void onCellUnloaded(const CellId& cell);

        const std::vector<CellId>& getLoadedCells() const { return mLoaded; }

    private:
        NetworkClient& mClient;
        std::vector<CellId> mLoaded;
    };

} // namespace mwmp
#endif // OPENMW_MWMP_SYNC_CELLSYNC_HPP
