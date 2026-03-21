#pragma once

#include <cstdint>
#include <string>

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_KeyCode.h>
#include <MyGUI_TextBox.h>
#include <MyGUI_Widget.h>

#include "../../mwgui/windowbase.hpp"

namespace mwmp
{
    /**
     * AccountDialog
     *
     * Modal shown after the player picks a server (from Direct Connect or the
     * Server Browser).  Collects username + password, hashes the password with
     * SHA-256, then calls Main::init() to start the connection.
     *
     * State machine:
     *   Idle  ──Login/Register──> Connecting ──worldReady──> [opens CharacterSelectDialog]
     *                                         └──rejected──> Rejected  (shows error, re-enables inputs)
     *
     * The dialog polls Main every frame while Connecting; it does NOT block.
     */
    class AccountDialog : public MWGui::WindowModal
    {
    public:
        /// Called when the handshake is accepted.  Receives the confirmed player
        /// name and server host so the caller can open CharacterSelectDialog.
        using WorldReadyCallback = std::function<void(const std::string& playerName,
                                                       const std::string& host)>;

        AccountDialog();

        void setWorldReadyCallback(WorldReadyCallback cb) { mWorldReadyCb = std::move(cb); }

        /// Set the target server.  Must be called before setVisible(true).
        void setServer(const std::string& host, uint16_t port);

        // WindowModal
        void onOpen()          override;
        void onFrame(float dt) override;

    private:
        enum class State { Idle, Connecting, Rejected };

        void doConnect(bool isRegister);
        void setState(State s, const std::string& msg = {});

        // Widget callbacks
        void onLoginClicked   (MyGUI::Widget* sender);
        void onRegisterClicked(MyGUI::Widget* sender);
        void onCancelClicked  (MyGUI::Widget* sender);
        void onKeyPress(MyGUI::Widget* sender, MyGUI::KeyCode key, MyGUI::Char ch);

        /// Compute SHA-256(input) and return the lowercase hex digest.
        /// Self-contained — no OpenSSL dependency in this translation unit.
        static std::string sha256hex(const std::string& input);

        MyGUI::EditBox* mUsername  = nullptr;
        MyGUI::EditBox* mPassword  = nullptr;
        MyGUI::Button*  mLoginBtn  = nullptr;
        MyGUI::Button*  mRegBtn    = nullptr;
        MyGUI::Button*  mCancelBtn = nullptr;
        MyGUI::TextBox* mStatus    = nullptr;

        std::string       mHost;
        uint16_t          mPort         = 25565;
        State             mState        = State::Idle;
        float             mConnectTimer = 0.f;   ///< seconds since connect attempt started
        WorldReadyCallback mWorldReadyCb;
    };

} // namespace mwmp
