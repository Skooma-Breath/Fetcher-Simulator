#pragma once

#include <string>

#include <components/openmw-mp/Base/BaseStructs.hpp>

namespace mwmp
{
    struct PlayerMark
    {
        std::string name;
        std::string cell;
        Position position;
    };
}
