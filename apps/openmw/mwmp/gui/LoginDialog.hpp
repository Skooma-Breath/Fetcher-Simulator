#ifndef OPENMW_MWMP_GUI_LOGINDIALOG_HPP
#define OPENMW_MWMP_GUI_LOGINDIALOG_HPP

#include <string>
#include <functional>

namespace mwmp
{
    // Phase 3: MyGUI login dialog shown before connect.
    // Stub stores the values; real UI wired in Phase 3.
    class LoginDialog
    {
    public:
        struct Credentials
        {
            std::string serverAddress;
            uint16_t    port         = 25565;
            std::string playerName;
            std::string passwordHash; // SHA-256 hex
        };

        using AcceptCallback = std::function<void(const Credentials&)>;
        using CancelCallback = std::function<void()>;

        void setAcceptCallback(AcceptCallback cb) { mAcceptCb = std::move(cb); }
        void setCancelCallback(CancelCallback cb)  { mCancelCb = std::move(cb); }

        void show() { mVisible = true; }
        void hide() { mVisible = false; }
        bool isVisible() const { return mVisible; }

        // Pre-populate from CLI args so the dialog shows sensible defaults
        void setDefaults(const Credentials& creds) { mDefaults = creds; }

    private:
        bool           mVisible  = false;
        Credentials    mDefaults;
        AcceptCallback mAcceptCb;
        CancelCallback mCancelCb;
    };

} // namespace mwmp
#endif // OPENMW_MWMP_GUI_LOGINDIALOG_HPP
