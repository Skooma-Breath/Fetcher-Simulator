#ifndef OPENMW_MWMP_GUI_CHATWINDOW_HPP
#define OPENMW_MWMP_GUI_CHATWINDOW_HPP

#include <string>
#include <vector>
#include <functional>

namespace mwmp
{
    class NetworkClient;

    // Phase 3: full MyGUI chat window.
    // For now this is a plain data sink that other systems can call.
    class ChatWindow
    {
    public:
        explicit ChatWindow(NetworkClient& client);

        // Add a message to the chat history (called by protocol handler)
        void addMessage(const std::string& sender, const std::string& message,
                        const std::string& channel = "");

        // Send the current input line (called by input handler)
        void sendMessage(const std::string& message);

        // Show/hide
        void setVisible(bool visible) { mVisible = visible; }
        bool isVisible() const        { return mVisible; }

        // Callback invoked when the user submits a message — wired to PlayerSync in Phase 3
        using SendCallback = std::function<void(const std::string& msg)>;
        void setSendCallback(SendCallback cb) { mSendCb = std::move(cb); }

    private:
        NetworkClient& mClient;
        bool           mVisible = false;

        struct Entry { std::string sender; std::string message; std::string channel; };
        std::vector<Entry> mHistory;

        SendCallback mSendCb;
    };

} // namespace mwmp
#endif // OPENMW_MWMP_GUI_CHATWINDOW_HPP
