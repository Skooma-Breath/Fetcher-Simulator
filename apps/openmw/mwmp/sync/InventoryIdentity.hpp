#ifndef OPENMW_MWMP_SYNC_INVENTORYIDENTITY_HPP
#define OPENMW_MWMP_SYNC_INVENTORYIDENTITY_HPP

#include <cstdint>
#include <limits>

#include <components/esm/formid.hpp>

namespace mwmp
{
    // Reserve one generated RefNum content-file bucket for server-issued item
    // identities. This cannot collide with content records and is far outside
    // the range reached by OpenMW's ordinary generated RefNum counter.
    inline constexpr int32_t InventoryInstanceContentFile = std::numeric_limits<int32_t>::min();

    inline ESM::RefNum inventoryInstanceRefNum(uint32_t instanceId)
    {
        if (instanceId == 0)
            return {};
        return ESM::RefNum{ .mIndex = instanceId, .mContentFile = InventoryInstanceContentFile };
    }

    inline uint32_t inventoryInstanceId(ESM::RefNum refNum)
    {
        return refNum.mContentFile == InventoryInstanceContentFile ? refNum.mIndex : 0;
    }
}

#endif
