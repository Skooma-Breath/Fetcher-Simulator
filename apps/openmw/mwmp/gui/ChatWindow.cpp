#include "ChatWindow.hpp"
#include <components/debug/debuglog.hpp>
#include "../network/Client.hpp"

namespace mwmp
{

ChatWindow::ChatWindow(NetworkClient& client) : mClient(client) {}

void ChatWindow::addMessage(const std::string& sender, const std::string& message,
                            const std::string& channel)
{
    mHistory.push_back({ sender, message, channel });
    Log(Debug::Info) << "[Chat][" << (channel.empty() ? "global" : channel)
                     << "] <" << sender << "> " << message;
    // Phase 3: push into MyGUI EditBox
}

void ChatWindow::sendMessage(const std::string& message)
{
    if (message.empty()) return;
    if (mSendCb) mSendCb(message);
}

} // namespace mwmp
