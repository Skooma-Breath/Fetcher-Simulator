#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_MULTIPLAYER_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_MULTIPLAYER_H

#include <components/settings/sanitizerimpl.hpp>
#include <components/settings/settingvalue.hpp>

#include <string>
#include <string_view>

namespace Settings
{
    struct MultiplayerCategory : WithIndex
    {
        using WithIndex::WithIndex;

        /// Last address typed in the Direct Connect dialog (persisted across sessions).
        SettingValue<std::string> mLastServerAddress{ mIndex, "Multiplayer", "last server address" };

        /// Last port typed in the Direct Connect dialog (clamped 1–65535).
        SettingValue<int> mLastServerPort{ mIndex, "Multiplayer", "last server port",
            makeClampSanitizerInt(1, 65535) };

        /// Base URL of the master server polled by the Server Browser.
        /// e.g. "https://master.openmw-mp.org"
        SettingValue<std::string> mMasterServerUrl{ mIndex, "Multiplayer", "master server url" };
    };
}

#endif // OPENMW_COMPONENTS_SETTINGS_CATEGORIES_MULTIPLAYER_H
