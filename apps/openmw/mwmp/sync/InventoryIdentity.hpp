#ifndef OPENMW_MWMP_SYNC_INVENTORYIDENTITY_HPP
#define OPENMW_MWMP_SYNC_INVENTORYIDENTITY_HPP

#include <cstdint>
#include <limits>
#include <unordered_map>

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

    inline std::unordered_map<ESM::RefNum, uint32_t>& inventoryInstanceAliases()
    {
        static std::unordered_map<ESM::RefNum, uint32_t> aliases;
        return aliases;
    }

    inline void setInventoryInstanceAlias(ESM::RefNum refNum, uint32_t instanceId)
    {
        if (!refNum.isSet() || instanceId == 0)
            return;
        inventoryInstanceAliases()[refNum] = instanceId;
    }

    inline void forgetInventoryInstanceAlias(ESM::RefNum refNum)
    {
        inventoryInstanceAliases().erase(refNum);
    }

    inline void clearInventoryInstanceAliases()
    {
        inventoryInstanceAliases().clear();
    }

    inline uint32_t inventoryInstanceId(ESM::RefNum refNum)
    {
        // A world round-trip can replace an older reserved inventory ID with
        // a newly allocated world ID. The alias must win so live Lua object IDs
        // remain stable while packets carry the corrected authoritative identity.
        const auto it = inventoryInstanceAliases().find(refNum);
        if (it != inventoryInstanceAliases().end())
            return it->second;
        return refNum.mContentFile == InventoryInstanceContentFile ? refNum.mIndex : 0;
    }
}

#endif
