#ifndef OPENMW_MP_DYNAMICRECORD_HPP
#define OPENMW_MP_DYNAMICRECORD_HPP

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mwmp
{
    enum class DynamicRecordAction : uint8_t
    {
        Upsert = 0,
        Remove = 1,
    };

    struct DynamicRecordEntry
    {
        std::string recordId;
        std::string data;
    };

    inline std::string normalizeDynamicRecordType(std::string_view recordType)
    {
        std::string normalized(recordType);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (normalized == "misc")
            return "miscellaneous";

        if (normalized == "activator"
            || normalized == "armor"
            || normalized == "book"
            || normalized == "clothing"
            || normalized == "container"
            || normalized == "creature"
            || normalized == "door"
            || normalized == "enchantment"
            || normalized == "light"
            || normalized == "miscellaneous"
            || normalized == "npc"
            || normalized == "potion"
            || normalized == "spell"
            || normalized == "static"
            || normalized == "weapon")
        {
            return normalized;
        }

        return {};
    }

    inline std::string normalizeDynamicRecordScope(std::string_view recordScope)
    {
        std::string normalized(recordScope);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (normalized == "generated" || normalized == "permanent")
            return normalized;

        return {};
    }
}

#endif // OPENMW_MP_DYNAMICRECORD_HPP
