#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <MyGUI_Button.h>
#include <MyGUI_Window.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_KeyCode.h>
#include <MyGUI_ListBox.h>
#include <MyGUI_TextBox.h>
#include <MyGUI_Widget.h>

#include "../../mwgui/windowbase.hpp"
#include "KeyLinkDialog.hpp"
#include <components/openmw-mp/Packets/System/PacketHandshake.hpp>

namespace mwmp
{
    /**
     * CharacterSelectDialog
     *
     * Single dialog that handles the full connect-to-play flow.
     * Opens immediately after the player enters the server address.
     *
     * State machine:
     *
     *   [setServer called]
     *     ├── keypair found → ConnectingWithKey  (skip login panel)
     *     └── no keypair   → Login
     *
     *   Login ──Login/Register──> Connecting ──CharacterList──> CharSelect
     *          └──rejected──> Login (error)                          │       │
     *                                                     New Char   │ Enter │
     *                                                        ▼       ▼ World │
     *                                                    Naming   WaitingForData
     *                                                      └──confirm──> WaitingForData
     *                                                                        │
     *                                                                  [enterWorld]
     *
     *   ConnectingWithKey  ──  (same as Connecting) ──> CharSelect
     *                      └──rejected──> Login (error)
     */
    class CharacterSelectDialog : public MWGui::WindowModal
    {
    public:
        CharacterSelectDialog();

        /// Set the target server before calling setVisible(true).
        void setServer(const std::string& host, uint16_t port);

        // WindowModal
        void onOpen()  override;
        void onFrame(float dt) override;

    private:
        enum class State
        {
            Login,              ///< showing credentials form
            Connecting,         ///< password auth — waiting for CharacterList
            ConnectingWithKey,  ///< keypair auth  — waiting for CharacterList
            CharSelect,         ///< showing character list
            Naming,             ///< new-char name input visible
            WaitingForData,     ///< PacketCharacterSelect sent, waiting for response
        };

        // ── Login panel handlers ──────────────────────────────────────────
        void onLoginClicked      (MyGUI::Widget* sender);
        void onRegisterClicked   (MyGUI::Widget* sender);
        void onLoginCancelClicked(MyGUI::Widget* sender);
        void onLoginKeyPress(MyGUI::Widget* sender, MyGUI::KeyCode key, MyGUI::Char ch);
        void doConnect(bool isRegister);
        void doConnectWithKey(const std::string& storedUsername);

        // ── CharSelect panel handlers ─────────────────────────────────────
        void onEnterWorldClicked   (MyGUI::Widget* sender);
        void onNewCharClicked      (MyGUI::Widget* sender);
        void onNewCharNameConfirm  (MyGUI::Widget* sender);
        void onNewCharNameKeyPress (MyGUI::Widget* sender, MyGUI::KeyCode key, MyGUI::Char ch);
        void onCharCancelClicked   (MyGUI::Widget* sender);
        void onKeyLinkClicked      (MyGUI::Widget* sender);

        // ── Shared helpers ────────────────────────────────────────────────
        void showLoginPanel();
        void showCharPanel(bool resetNamingRow = true);
        void setLoginStatus(const std::string& msg);
        void setCharStatus (const std::string& msg);
        void updateKeyLinkButton();
        void populate(const std::vector<CharacterEntry>& characters);
        void sendCharacterSelect(const std::string& charName, bool isNew);
        void setCharPanelBusy(bool busy);
        void enterWorld();

        static std::string sha256hex(const std::string& input);

        // ── Login group widgets ───────────────────────────────────────────
        MyGUI::Widget*  mUsernameLabel   = nullptr;
        MyGUI::EditBox* mUsername        = nullptr;
        MyGUI::Widget*  mPasswordLabel   = nullptr;
        MyGUI::EditBox* mPassword        = nullptr;
        MyGUI::TextBox* mStatusLabel     = nullptr;
        MyGUI::Button*  mLoginBtn        = nullptr;
        MyGUI::Button*  mRegBtn          = nullptr;
        MyGUI::Button*  mLoginCancelBtn  = nullptr;

        // ── CharSelect group widgets ──────────────────────────────────────
        MyGUI::TextBox* mConnectedLabel     = nullptr;
        MyGUI::TextBox* mCharSelectHint     = nullptr;
        MyGUI::ListBox* mList               = nullptr;
        MyGUI::Widget*  mNewCharNameRow     = nullptr;
        MyGUI::EditBox* mNewCharNameEdit    = nullptr;
        MyGUI::Button*  mNewCharNameConfirm = nullptr;
        MyGUI::Button*  mKeyLinkBtn         = nullptr;   ///< "Link Machine" / "Machine Linked ✓"
        MyGUI::Button*  mEnterBtn           = nullptr;
        MyGUI::Button*  mNewCharBtn         = nullptr;
        MyGUI::Button*  mCharCancelBtn      = nullptr;

        // ── Sub-dialogs ───────────────────────────────────────────────────
        std::unique_ptr<KeyLinkDialog> mKeyLinkDialog;

        // ── State ─────────────────────────────────────────────────────────
        State       mState  = State::Login;
        float       mTimer  = 0.f;
        std::string mHost;
        uint16_t    mPort   = 25565;

        std::vector<CharacterEntry> mCharacters;
    };

} // namespace mwmp
