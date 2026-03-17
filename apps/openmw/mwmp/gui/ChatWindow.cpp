#include "ChatWindow.hpp"

#include <algorithm>
#include <MyGUI_RenderManager.h>
#include <components/settings/values.hpp>
#include <components/settings/settings.hpp>
#include "../../mwgui/windowbase.hpp"
#include <components/debug/debuglog.hpp>
#include <components/openmw-mp/Packets/Player/PacketChatMessage.hpp>
#include <components/openmw-mp/Base/BasePlayer.hpp>

#include "../network/Client.hpp"
#include "../Main.hpp"
#include "../sync/PlayerSync.hpp"

namespace mwmp
{

ChatWindow::ChatWindow(NetworkClient& client)
    : WindowBase("openmw_mp_chat.layout")
    , mClient(client)
    , mHistoryCurrent(mInputHistory.end())
{
    getWidget(mHistory, "chat_History");
    getWidget(mInput,   "chat_Input");

    mInput->eventEditSelectAccept
        += MyGUI::newDelegate(this, &ChatWindow::onInputAccept);
    mInput->eventKeyButtonPressed
        += MyGUI::newDelegate(this, &ChatWindow::onInputKeyPress);

    mHistory->setOverflowToTheLeft(true);

    // Input hidden by default
    mInput->setVisible(false);

    MyGUI::Window* win = mMainWidget->castType<MyGUI::Window>();
    win->eventWindowChangeCoord
        += MyGUI::newDelegate(this, &ChatWindow::onWindowChangeCoord);

    const MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
    const int x = static_cast<int>(Settings::windows().mMpChatX * viewSize.width);
    const int y = static_cast<int>(Settings::windows().mMpChatY * viewSize.height);
    const int w = static_cast<int>(Settings::windows().mMpChatW * viewSize.width);
    const int h = static_cast<int>(Settings::windows().mMpChatH * viewSize.height);
    mMainWidget->setCoord(x, y, w, h);
    MWGui::WindowBase::clampWindowCoordinates(win);
    onWindowChangeCoord(win);

    mDisplayMode = 1;
    mFadeTimer   = FADE_HOLD;
    applyDisplayMode();
}

// ---------------------------------------------------------------------------
void ChatWindow::onWindowChangeCoord(MyGUI::Window* window)
{
    MWGui::WindowBase::clampWindowCoordinates(window);
    const MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
    Settings::Manager::setFloat("mp chat x", "Windows", window->getPosition().left / static_cast<float>(viewSize.width));
    Settings::Manager::setFloat("mp chat y", "Windows", window->getPosition().top  / static_cast<float>(viewSize.height));
    Settings::Manager::setFloat("mp chat w", "Windows", window->getSize().width    / static_cast<float>(viewSize.width));
    Settings::Manager::setFloat("mp chat h", "Windows", window->getSize().height   / static_cast<float>(viewSize.height));
}

// ---------------------------------------------------------------------------
void ChatWindow::update(float dt)
{
    if (mDisplayMode != 2 || mInputOpen)
        return;

    mFadeTimer -= dt;

    const float t = std::max(0.f, std::min(1.f,
        (mFadeTimer + FADE_SPEED) / FADE_SPEED));
    mMainWidget->setAlpha(t);

    if (t <= 0.f)
        mMainWidget->setVisible(false);
}

// ---------------------------------------------------------------------------
void ChatWindow::addMessage(const std::string& sender,
                            const std::string& message)
{
    std::string line;
    if (!sender.empty())
        line = "#FFCC44" + sender + ": #FFFFFF" + message;
    else
        line = "#AAAAAA" + message;

    mHistory->addText(line + "\n");
    scrollToBottom();

    if (mDisplayMode == 2)
    {
        mFadeTimer = FADE_HOLD;
        mMainWidget->setAlpha(1.f);
        mMainWidget->setVisible(true);
    }
}

// ---------------------------------------------------------------------------
void ChatWindow::cycleDisplayMode()
{
    if (mDisplayMode == 2)
        mMainWidget->setAlpha(1.f);
    mDisplayMode = (mDisplayMode % 3) + 1;

    static const char* labels[] = { "", "Chat: Always visible", "Chat: Fade out", "Chat: Hidden" };
    MWBase::Environment::get().getWindowManager()->messageBox(labels[mDisplayMode]);

    Log(Debug::Info) << "[MP] Chat display mode: " << mDisplayMode;
    mFadeTimer = FADE_HOLD;
    applyDisplayMode();
}

// ---------------------------------------------------------------------------
void ChatWindow::applyDisplayMode()
{
    switch (mDisplayMode)
    {
        case 1:
            // Always visible — do NOT call setAlpha; breaks selection highlight.
            mMainWidget->setVisible(true);
            break;
        case 2:
            mMainWidget->setAlpha(1.f);
            mMainWidget->setVisible(true);
            break;
        case 3:
            if (!mInputOpen)
                mMainWidget->setVisible(false);
            break;
    }
}

// ---------------------------------------------------------------------------
void ChatWindow::openInput()
{
    if (mInputOpen) return;
    mInputOpen = true;
    mMainWidget->setVisible(true);
    if (mDisplayMode == 2)
        mMainWidget->setAlpha(1.f);
    mInput->setVisible(true);
    mInput->setCaption("");
    MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(mInput);
}

// ---------------------------------------------------------------------------
void ChatWindow::closeInput()
{
    mInputOpen = false;
    mInput->setCaption("");
    if (!mGuiInputActive)
        mInput->setVisible(false);
    MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(nullptr);
    MWBase::Environment::get().getWindowManager()->allowMouse();
    applyDisplayMode();
}

// ---------------------------------------------------------------------------
void ChatWindow::onGuiModeChanged(bool guiModeActive)
{
    if (mInputOpen) return;

    if (guiModeActive && !mGuiInputActive)
    {
        mGuiInputActive = true;
        mMainWidget->setVisible(true);
        if (mDisplayMode == 2)
            mMainWidget->setAlpha(1.f);
        mInput->setVisible(true);
    }
    else if (!guiModeActive && mGuiInputActive)
    {
        mGuiInputActive = false;
        mInput->setVisible(false);
        applyDisplayMode();
    }
}

// ---------------------------------------------------------------------------
void ChatWindow::onInputAccept(MyGUI::EditBox* /*sender*/)
{
    const std::string text = mInput->getOnlyText();
    if (text.empty())
    {
        closeInput();
        return;
    }

    BasePlayer& local = Main::get().getPlayerSync().localPlayer();
    PacketChatMessage pkt;
    pkt.setPlayer(&local);
    pkt.message = text;
    pkt.channel = "";
    mClient.sendReliable(pkt.encode(0));

    if (mInputHistory.empty() || mInputHistory.back() != text)
        mInputHistory.push_back(text);
    mHistoryCurrent = mInputHistory.end();
    mEditString.clear();

    closeInput();
}

// ---------------------------------------------------------------------------
void ChatWindow::onInputKeyPress(MyGUI::Widget* /*sender*/,
                                 MyGUI::KeyCode key, MyGUI::Char /*ch*/)
{
    if (key == MyGUI::KeyCode::Escape)
    {
        closeInput();
        return;
    }
    if (key == MyGUI::KeyCode::ArrowUp)
    {
        if (mInputHistory.empty()) return;
        if (mHistoryCurrent == mInputHistory.end())
            mEditString = mInput->getOnlyText();
        if (mHistoryCurrent != mInputHistory.begin())
        {
            --mHistoryCurrent;
            mInput->setOnlyText(*mHistoryCurrent);
            mInput->setTextCursor(mInput->getOnlyText().size());
        }
        return;
    }
    if (key == MyGUI::KeyCode::ArrowDown)
    {
        if (mHistoryCurrent == mInputHistory.end()) return;
        ++mHistoryCurrent;
        if (mHistoryCurrent != mInputHistory.end())
            mInput->setOnlyText(*mHistoryCurrent);
        else
            mInput->setOnlyText(mEditString);
        mInput->setTextCursor(mInput->getOnlyText().size());
        return;
    }
}

// ---------------------------------------------------------------------------
void ChatWindow::scrollToBottom()
{
    mHistory->setVScrollPosition(mHistory->getVScrollRange());
}

} // namespace mwmp
