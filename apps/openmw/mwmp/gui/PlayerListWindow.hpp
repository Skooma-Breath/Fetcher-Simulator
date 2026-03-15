#ifndef OPENMW_MWMP_GUI_PLAYERLISTWINDOW_HPP
#define OPENMW_MWMP_GUI_PLAYERLISTWINDOW_HPP

#include <string>
#include <vector>

namespace mwmp
{
    // Phase 3: shows connected players, ping, cell.
    struct PlayerListEntry
    {
        uint32_t    guid = 0;
        std::string name;
        std::string cell;
        float       ping = 0.f;
        bool        isDead = false;
    };

    class PlayerListWindow
    {
    public:
        void refresh(const std::vector<PlayerListEntry>& players) { mEntries = players; }
        void setVisible(bool v) { mVisible = v; }
        bool isVisible() const  { return mVisible; }
    private:
        std::vector<PlayerListEntry> mEntries;
        bool mVisible = false;
    };
} // namespace mwmp
#endif
