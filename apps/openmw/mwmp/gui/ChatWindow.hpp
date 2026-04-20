#pragma once
#include <list>
#include <string>
#include <MyGUI_EditBox.h>
#include <MyGUI_Widget.h>
#include <MyGUI_Window.h>
#include "../../mwgui/windowbase.hpp"
#include "../../mwbase/environment.hpp"
#include "../../mwbase/windowmanager.hpp"

namespace mwmp
{
    class NetworkClient;

    // In-game multiplayer chat window.
    //
    // Display modes (cycled with F6 / A_ChatMode):
    //   1 — Always visible, full opacity.  setAlpha is NEVER called in this
    //       mode — calling setAlpha() even at 1.0 breaks MyGUI selection
    //       highlight rendering.
    //   2 — Visible on new message, fades out after FADE_HOLD seconds.
    //   3 — Always hidden (messages still received silently).
    //
    // Input: press Y (A_ChatOpen) or \ to open the input box, Enter to send,
    // Escape to cancel.
    class ChatWindow : public MWGui::WindowBase
    {
    public:
        explicit ChatWindow(NetworkClient& client);

        // Per-frame update — drives fade timer in mode 2.
        void update(float dt);

        // Append a received or sent message.
        void addMessage(const std::string& sender, const std::string& message);

        // Cycle display mode 1→2→3→1 (bound to F6 / A_ChatMode).
        void cycleDisplayMode();

        // Open the input box and grab focus (Y key / A_ChatOpen).
        void openInput();

        // Called by WindowManager on GUI mode changes (pause menu open/close).
        void onGuiModeChanged(bool guiModeActive);

        bool isInputOpen() const { return mInputOpen; }

    private:
        void onInputAccept  (MyGUI::EditBox* sender);
        void onInputKeyPress(MyGUI::Widget* sender, MyGUI::KeyCode key, MyGUI::Char ch);

        void onWindowChangeCoord(MyGUI::Window* window);
        void closeInput();
        void applyDisplayMode();
        void scrollToBottom();

        // Returns true if the text was handled locally and should not be sent
        // over the network. Adds any output directly to the local history.
        bool handleCommand(const std::string& text);

        NetworkClient& mClient;

        MyGUI::EditBox* mHistory = nullptr;
        MyGUI::EditBox* mInput   = nullptr;

        bool mInputOpen      = false;
        bool mGuiInputActive = false;

        // Display mode: 1=always on, 2=fade, 3=hidden
        int   mDisplayMode = 1;
        float mFadeTimer   = 0.f;

        static constexpr float FADE_HOLD  = 6.f;
        static constexpr float FADE_SPEED = 1.5f;

        // Input history (↑/↓ to cycle)
        std::list<std::string>           mInputHistory;
        std::list<std::string>::iterator mHistoryCurrent;
        std::string                      mEditString;
    };
}
