#include "CellSync.hpp"
#include <algorithm>
#include <components/debug/debuglog.hpp>
namespace mwmp
{
    CellSync::CellSync(NetworkClient& client) : mClient(client) {}

    void CellSync::onCellLoaded(const CellId& cell)
    {
        mLoaded.push_back(cell);
        Log(Debug::Verbose) << "[MP] CellSync: loaded " << cell.cellName;
        // Phase 4: send PlayerCellState packet to server
    }
    void CellSync::onCellUnloaded(const CellId& cell)
    {
        mLoaded.erase(
            std::remove_if(mLoaded.begin(), mLoaded.end(),
                [&](const CellId& c){ return c.cellName == cell.cellName
                    && c.isExterior == cell.isExterior; }),
            mLoaded.end());
        Log(Debug::Verbose) << "[MP] CellSync: unloaded " << cell.cellName;
    }
} // namespace mwmp
