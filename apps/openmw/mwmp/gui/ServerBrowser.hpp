#ifndef OPENMW_MWMP_GUI_SERVERBROWSER_HPP
#define OPENMW_MWMP_GUI_SERVERBROWSER_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace mwmp
{
    // Phase 6: server browser with master-server query.
    struct ServerEntry
    {
        std::string address;
        uint16_t    port        = 25565;
        std::string name;
        std::string version;
        int         playerCount = 0;
        int         maxPlayers  = 0;
        int         ping        = 0;
    };

    class ServerBrowser
    {
    public:
        void refresh() { /* Phase 6: query master server */ }
        const std::vector<ServerEntry>& getServers() const { return mServers; }
        void setVisible(bool v) { mVisible = v; }
        bool isVisible() const  { return mVisible; }
    private:
        std::vector<ServerEntry> mServers;
        bool mVisible = false;
    };
} // namespace mwmp
#endif
