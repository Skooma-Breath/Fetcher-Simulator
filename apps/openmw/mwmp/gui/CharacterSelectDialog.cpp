#include "CharacterSelectDialog.hpp"

#include <MyGUI_InputManager.h>

#include <algorithm>
#include <components/debug/debuglog.hpp>

#include "../Identity.hpp"
#include "../Main.hpp"
#include "../network/Client.hpp"
#include "../sha256.hpp"

namespace mwmp
{

// ---------------------------------------------------------------------------
CharacterSelectDialog::CharacterSelectDialog()
    : WindowModal("openmw_mp_char_select.layout")
{
    // ── Login group ──────────────────────────────────────────────────────
    getWidget(mUsernameLabel,  "UsernameLabel");
    getWidget(mUsername,       "Username");
    getWidget(mPasswordLabel,  "PasswordLabel");
    getWidget(mPassword,       "Password");
    getWidget(mStatusLabel,    "StatusLabel");
    getWidget(mLoginBtn,       "LoginButton");
    getWidget(mRegBtn,         "RegisterButton");
    getWidget(mLoginCancelBtn, "LoginCancelButton");

    mLoginBtn->eventMouseButtonClick  +=
        MyGUI::newDelegate(this, &CharacterSelectDialog::onLoginClicked);
    mRegBtn->eventMouseButtonClick    +=
        MyGUI::newDelegate(this, &CharacterSelectDialog::onRegisterClicked);
    mLoginCancelBtn->eventMouseButtonClick +=
        MyGUI::newDelegate(this, &CharacterSelectDialog::onLoginCancelClicked);
    mUsername->eventKeyButtonPressed +=
        MyGUI::newDelegate(this, &CharacterSelectDialog::onLoginKeyPress);
    mPassword->eventKeyButtonPressed +=
        MyGUI::newDelegate(this, &CharacterSelectDialog::onLoginKeyPress);

    // ── CharSelect group ─────────────────────────────────────────────────
    getWidget(mConnectedLabel,     "ConnectedLabel");
    getWidget(mCharSelectHint,     "CharSelectHint");
    getWidget(mList,               "CharacterList");
    getWidget(mNewCharNameRow,     "NewCharNameRow");
    getWidget(mNewCharNameEdit,    "NewCharNameEdit");
    getWidget(mNewCharNameConfirm, "NewCharNameConfirm");
    getWidget(mKeyLinkBtn,         "KeyLinkButton");
    getWidget(mEnterBtn,           "EnterWorldButton");
    getWidget(mNewCharBtn,         "NewCharButton");
    getWidget(mCharCancelBtn,      "CharCancelButton");
    getWidget(mDeleteCharBtn,      "DeleteCharButton");

    mEnterBtn->eventMouseButtonClick  +=
        MyGUI::newDelegate(this, &CharacterSelectDialog::onEnterWorldClicked);
    mNewCharBtn->eventMouseButtonClick +=
        MyGUI::newDelegate(this, &CharacterSelectDialog::onNewCharClicked);
    mNewCharNameConfirm->eventMouseButtonClick +=
        MyGUI::newDelegate(this, &CharacterSelectDialog::onNewCharNameConfirm);
    mNewCharNameEdit->eventKeyButtonPressed +=
        MyGUI::newDelegate(this, &CharacterSelectDialog::onNewCharNameKeyPress);
    mCharCancelBtn->eventMouseButtonClick +=
        MyGUI::newDelegate(this, &CharacterSelectDialog::onCharCancelClicked);
    mDeleteCharBtn->eventMouseButtonClick +=
        MyGUI::newDelegate(this, &CharacterSelectDialog::onDeleteCharClicked);
    mKeyLinkBtn->eventMouseButtonClick +=
        MyGUI::newDelegate(this, &CharacterSelectDialog::onKeyLinkClicked);
}

// ---------------------------------------------------------------------------
void CharacterSelectDialog::setServer(const std::string& host, uint16_t port)
{
    mHost = host;
    mPort = port;
    // setKeysDir in Main so Identity knows where to store keys.
    // Use a relative "mp-keys" directory next to the executable for now.
    Main::setStaticKeysDir(std::filesystem::current_path() / "mp-keys");
}

void CharacterSelectDialog::onOpen()
{
    WindowModal::onOpen();

    // If we have a keypair for this server, skip the login form entirely.
    if (!mHost.empty() && Identity::hasKeypair(mHost, mPort))
    {
        const std::string storedUser = Identity::getStoredUsername(mHost, mPort);
        if (!storedUser.empty())
        {
            doConnectWithKey(storedUser);
            return;
        }
    }

    showLoginPanel();
    MyGUI::InputManager::getInstance().setKeyFocusWidget(
        mUsername->getCaption().empty() ? mUsername : mPassword);
}

// ---------------------------------------------------------------------------
// Panel show/hide
// ---------------------------------------------------------------------------
void CharacterSelectDialog::showLoginPanel()
{
    mState = State::Login;
    mTimer = 0.f;
    mMainWidget->castType<MyGUI::Window>()->setCaption("Multiplayer Login");

    mUsernameLabel->setVisible(true);
    mUsername->setVisible(true);
    mPasswordLabel->setVisible(true);
    mPassword->setVisible(true);
    mStatusLabel->setVisible(true);
    mLoginBtn->setVisible(true);
    mRegBtn->setVisible(true);
    mLoginCancelBtn->setVisible(true);

    mConnectedLabel->setVisible(false);
    mCharSelectHint->setVisible(false);
    mList->setVisible(false);
    mNewCharNameRow->setVisible(false);
    mKeyLinkBtn->setVisible(false);
    mEnterBtn->setVisible(false);
    mNewCharBtn->setVisible(false);
    mDeleteCharBtn->setVisible(false);
    mCharCancelBtn->setVisible(false);

    setLoginStatus("");
    mLoginBtn->setEnabled(true);
    mRegBtn->setEnabled(true);
    mUsername->setEnabled(true);
    mPassword->setEnabled(true);
}

void CharacterSelectDialog::showCharPanel(bool resetNamingRow)
{
    mMainWidget->castType<MyGUI::Window>()->setCaption("Select Character");

    mUsernameLabel->setVisible(false);
    mUsername->setVisible(false);
    mPasswordLabel->setVisible(false);
    mPassword->setVisible(false);
    mStatusLabel->setVisible(false);
    mLoginBtn->setVisible(false);
    mRegBtn->setVisible(false);
    mLoginCancelBtn->setVisible(false);

    mConnectedLabel->setVisible(true);
    mCharSelectHint->setVisible(true);
    mList->setVisible(true);
    mKeyLinkBtn->setVisible(true);
    mEnterBtn->setVisible(true);
    mNewCharBtn->setVisible(true);
    mCharCancelBtn->setVisible(true);
    mDeleteCharBtn->setVisible(true);

    if (resetNamingRow)
    {
        mNewCharNameRow->setVisible(false);
        mNewCharNameEdit->setCaption("");
        mState = State::CharSelect;
    }

    mDeletePending = false;
    mDeletePendingName.clear();
    mDeleteCharBtn->setCaption("Delete");
    updateKeyLinkButton();
    setCharStatus("");
    setCharPanelBusy(false);
}

void CharacterSelectDialog::setLoginStatus(const std::string& msg)
{
    mStatusLabel->setCaption(msg);
}

void CharacterSelectDialog::setCharStatus(const std::string& msg)
{
    if (msg.empty())
    {
        mConnectedLabel->setCaption(
            "Connected as " + (Main::isInitialised() ? Main::get().getPlayerName() : "") + "  |  " + mHost);
        mCharSelectHint->setCaption(
            "Select a character and click Enter World, or create a new one.");
    }
    else
    {
        mCharSelectHint->setCaption(msg);
    }
}

void CharacterSelectDialog::updateKeyLinkButton()
{
    if (Identity::hasKeypair(mHost, mPort))
        mKeyLinkBtn->setCaption("Machine Linked \xE2\x9C\x93"); // UTF-8 check mark
    else
        mKeyLinkBtn->setCaption("Link Machine");
}

void CharacterSelectDialog::setCharPanelBusy(bool busy)
{
    mEnterBtn->setEnabled(!busy);
    mNewCharBtn->setEnabled(!busy);
    mCharCancelBtn->setEnabled(!busy);
    mKeyLinkBtn->setEnabled(!busy);
    mNewCharNameConfirm->setEnabled(!busy);
    mNewCharNameEdit->setEnabled(!busy);
    mDeleteCharBtn->setEnabled(!busy);
}

// ---------------------------------------------------------------------------
// Login group
// ---------------------------------------------------------------------------
void CharacterSelectDialog::onLoginClicked   (MyGUI::Widget*) { doConnect(false); }
void CharacterSelectDialog::onRegisterClicked(MyGUI::Widget*) { doConnect(true);  }

void CharacterSelectDialog::onLoginCancelClicked(MyGUI::Widget*)
{
    if (Main::isInitialised()) Main::destroy();
    setVisible(false);
}

void CharacterSelectDialog::onLoginKeyPress(MyGUI::Widget*,
                                             MyGUI::KeyCode key, MyGUI::Char)
{
    if (key == MyGUI::KeyCode::Return || key == MyGUI::KeyCode::NumpadEnter)
        doConnect(false);
    else if (key == MyGUI::KeyCode::Escape)
        onLoginCancelClicked(nullptr);
}

void CharacterSelectDialog::doConnect(bool isRegister)
{
    if (mState == State::Connecting || mState == State::ConnectingWithKey) return;

    const std::string user = std::string(mUsername->getCaption());
    const std::string pass = std::string(mPassword->getCaption());

    if (user.empty()) { setLoginStatus("Username cannot be empty."); return; }
    if (pass.empty()) { setLoginStatus("Password cannot be empty."); return; }
    if (mHost.empty()){ setLoginStatus("No server address set.");    return; }

    if (Main::isInitialised()) Main::destroy();

    mState = State::Connecting;
    mTimer = 0.f;
    setLoginStatus("Connecting...");
    mLoginBtn->setEnabled(false);
    mRegBtn->setEnabled(false);
    mUsername->setEnabled(false);
    mPassword->setEnabled(false);

    if (!Main::init(mHost, mPort, user, sha256hex(pass), isRegister, /*useKeypair=*/false))
    {
        showLoginPanel();
        setLoginStatus("Could not initiate connection to " + mHost + ".");
    }
}

void CharacterSelectDialog::doConnectWithKey(const std::string& storedUsername)
{
    if (Main::isInitialised()) Main::destroy();

    // Show a minimal "connecting" state on the login panel momentarily,
    // then switch to char panel once connected.
    mMainWidget->castType<MyGUI::Window>()->setCaption("Connecting...");
    // Hide all groups — a clean blank window while challenge-response happens.
    showLoginPanel();
    // Set state AFTER showLoginPanel() — showLoginPanel() resets mState to Login,
    // so we must override it here to enter the ConnectingWithKey polling path in onFrame.
    mState = State::ConnectingWithKey;
    mTimer = 0.f;
    setLoginStatus("Connecting with linked key...");
    mLoginBtn->setVisible(false);
    mRegBtn->setVisible(false);
    mLoginCancelBtn->setVisible(true);
    mLoginCancelBtn->setEnabled(true);

    Log(Debug::Info) << "[MP] Attempting keypair auth for " << storedUsername
                     << " on " << mHost << ":" << mPort;

    // Init with empty password — publicKey will be set in onConnected().
    if (!Main::init(mHost, mPort, storedUsername, "", false))
    {
        showLoginPanel();
        setLoginStatus("Could not initiate connection to " + mHost + ".");
    }
}

// ---------------------------------------------------------------------------
// CharSelect group
// ---------------------------------------------------------------------------
void CharacterSelectDialog::populate(const std::vector<CharacterEntry>& characters)
{
    mCharacters = characters;
    mList->removeAllItems();

    if (mCharacters.empty())
    {
        mEnterBtn->setVisible(false);
        mNewCharBtn->setCaption("Create Character");
        mList->addItem("No characters yet — click Create Character to begin.");
    }
    else
    {
        mEnterBtn->setVisible(true);
        mNewCharBtn->setCaption("New Character");
        for (const auto& c : mCharacters)
        {
            std::string label = c.name;
            if (!c.className.empty()) label += "  (" + c.className + ")";
            if (!c.race.empty())      label += "  —  " + c.race;
            if (c.isNew)              label += "  [incomplete]";
            mList->addItem(label);
        }
        mList->setIndexSelected(0);
    }
}

void CharacterSelectDialog::onEnterWorldClicked(MyGUI::Widget*)
{
    if (mState != State::CharSelect) return;
    const size_t sel = mList->getIndexSelected();
    if (sel == MyGUI::ITEM_NONE || sel >= mCharacters.size()) return;
    sendCharacterSelect(mCharacters[sel].name, /*isNew=*/false);
}

void CharacterSelectDialog::onNewCharClicked(MyGUI::Widget*)
{
    if (mState != State::CharSelect) return;
    mState = State::Naming;
    mNewCharNameRow->setVisible(true);
    mNewCharNameEdit->setCaption("");
    mCharSelectHint->setCaption("Enter a name for your new character, then click Create.");
    MyGUI::InputManager::getInstance().setKeyFocusWidget(mNewCharNameEdit);
    mEnterBtn->setEnabled(false);
    mNewCharBtn->setEnabled(false);
}

void CharacterSelectDialog::onNewCharNameKeyPress(MyGUI::Widget*,
                                                   MyGUI::KeyCode key, MyGUI::Char)
{
    if (key == MyGUI::KeyCode::Return || key == MyGUI::KeyCode::NumpadEnter)
        onNewCharNameConfirm(nullptr);
    else if (key == MyGUI::KeyCode::Escape)
    {
        mNewCharNameRow->setVisible(false);
        mNewCharNameEdit->setCaption("");
        mState = State::CharSelect;
        mEnterBtn->setEnabled(!mCharacters.empty());
        mNewCharBtn->setEnabled(true);
        setCharStatus("");
    }
}

void CharacterSelectDialog::onNewCharNameConfirm(MyGUI::Widget*)
{
    if (mState != State::Naming) return;
    const std::string name = std::string(mNewCharNameEdit->getCaption());
    if (name.empty()) { MyGUI::InputManager::getInstance().setKeyFocusWidget(mNewCharNameEdit); return; }
    sendCharacterSelect(name, /*isNew=*/true);
}

void CharacterSelectDialog::onCharCancelClicked(MyGUI::Widget*)
{
    if (Main::isInitialised()) Main::destroy();
    setVisible(false);
    mState = State::Login;
}

void CharacterSelectDialog::onDeleteCharClicked(MyGUI::Widget*)
{
    if (mState != State::CharSelect && mState != State::Naming) return;
    if (!Main::isInitialised()) return;

    const size_t sel = mList->getIndexSelected();
    if (sel == MyGUI::ITEM_NONE || sel >= mCharacters.size()) return;

    const std::string& charName = mCharacters[sel].name;

    if (!mDeletePending || mDeletePendingName != charName)
    {
        // First click — arm confirmation
        mDeletePending     = true;
        mDeletePendingName = charName;
        mDeleteCharBtn->setCaption("Confirm Delete");
        setCharStatus("Delete '" + charName + "'? Click Confirm Delete to proceed.");
        return;
    }

    // Second click — send the request
    mDeletePending = false;
    mDeletePendingName.clear();
    mDeleteCharBtn->setCaption("Delete");

    PacketDeleteCharRequest pkt;
    pkt.charName = charName;
    Main::get().getNetworking().sendReliable(pkt.encode());

    setCharPanelBusy(true);
    setCharStatus("Deleting '" + charName + "'...");
    mState = State::WaitingForData;
}

void CharacterSelectDialog::onKeyLinkClicked(MyGUI::Widget*)
{
    if (mState != State::CharSelect) return;
    if (!Main::isInitialised()) return;

    const std::string username = Main::get().getPlayerName();
    const bool linked = Identity::hasKeypair(mHost, mPort);

    if (!mKeyLinkDialog)
        mKeyLinkDialog = std::make_unique<KeyLinkDialog>();

    mKeyLinkDialog->open(mHost, mPort, username, linked,
        [this](bool isNowLinked) {
            // Refresh button text after dialog closes.
            updateKeyLinkButton();
            (void)isNowLinked;
        });
}

// ---------------------------------------------------------------------------
void CharacterSelectDialog::sendCharacterSelect(const std::string& charName, bool isNew)
{
    if (!Main::isInitialised()) return;
    Main::get().sendCharacterSelect(charName, isNew);

    mState = State::WaitingForData;
    mTimer = 0.f;
    setCharStatus("Loading...");
    setCharPanelBusy(true);
}

// ---------------------------------------------------------------------------
// onFrame
// ---------------------------------------------------------------------------
void CharacterSelectDialog::onFrame(float dt)
{
    // Both Connecting and ConnectingWithKey wait for the same thing: isWorldReady()
    if (mState == State::Connecting || mState == State::ConnectingWithKey)
    {
        mTimer += dt;
        if (mTimer > 15.f)
        {
            if (Main::isInitialised()) Main::destroy();
            showLoginPanel();
            setLoginStatus(mState == State::ConnectingWithKey
                ? "Keypair auth timed out. Try logging in with your password."
                : "Connection timed out.");
            return;
        }
        if (!Main::isInitialised())
        {
            showLoginPanel();
            setLoginStatus("Connection failed.");
            return;
        }

        Main& mp = Main::get();

        if (mp.isNetworkDisconnected())
        {
            const std::string reason = mp.getRejectReason();

            // If keypair auth was rejected because the server doesn't recognise
            // the key, the local file is orphaned — delete it so onOpen() stops
            // looping and pre-fill the username so the user just needs their password.
            if (mState == State::ConnectingWithKey
                && reason.find("Key not recognised") != std::string::npos)
            {
                const std::string storedUser = Identity::getStoredUsername(mHost, mPort);
                Identity::removeKeypair(mHost, mPort);
                showLoginPanel();
                if (!storedUser.empty())
                    mUsername->setCaption(storedUser);
                setLoginStatus("Key not registered on this server — enter your password to log in.");
            }
            else
            {
                showLoginPanel();
                setLoginStatus(reason.empty() ? "Rejected by server." : reason);
            }
            Main::destroy();
            return;
        }

        if (mp.isWorldReady())
        {
            mConnectedLabel->setCaption(
                "Connected as " + mp.getPlayerName() + "  |  " + mHost);
            showCharPanel();
            populate(mp.getCharacterList());
        }
        return;
    }

    if (mState == State::WaitingForData)
    {
        mTimer += dt;

        if (!Main::isInitialised()) { showLoginPanel(); return; }
        Main& mp = Main::get();

        // Poll for delete-character response (state is WaitingForData during delete).
        if (mp.isDeleteCharResponseReady())
        {
            const auto& rsp = mp.getDeleteCharResponse();
            mp.clearDeleteCharResponse();
            mState = State::CharSelect;
            setCharPanelBusy(false);
            mEnterBtn->setEnabled(!mCharacters.empty());
            mNewCharBtn->setEnabled(true);
            if (rsp.success)
            {
                // Remove the deleted entry from local list and repopulate.
                mCharacters.erase(
                    std::remove_if(mCharacters.begin(), mCharacters.end(),
                        [&](const CharacterEntry& c){ return c.name == rsp.charName; }),
                    mCharacters.end());
                populate(mCharacters);
                setCharStatus("Character '" + rsp.charName + "' deleted.");
            }
            else
            {
                setCharStatus(rsp.error.empty()
                    ? "Delete failed." : rsp.error);
            }
            return;
        }

                const std::string& err = mp.getCharSelectError();
        if (!err.empty())
        {
            setCharStatus(err);
            mp.clearCharSelectError();
            mState = State::CharSelect;
            mNewCharNameRow->setVisible(false);
            mNewCharNameEdit->setCaption("");
            setCharPanelBusy(false);
            mEnterBtn->setEnabled(!mCharacters.empty());
            mNewCharBtn->setEnabled(true);
            return;
        }

        if (mTimer > 15.f)
        {
            setCharStatus("Server did not respond. Please try again.");
            mState = State::CharSelect;
            mNewCharNameRow->setVisible(false);
            setCharPanelBusy(false);
            mEnterBtn->setEnabled(!mCharacters.empty());
            mNewCharBtn->setEnabled(true);
            return;
        }

        if (mp.isCharacterDataReady())
            enterWorld();
    }
}

// ---------------------------------------------------------------------------
// enterWorld
// ---------------------------------------------------------------------------
void CharacterSelectDialog::enterWorld()
{
    setVisible(false);
    if (Main::isInitialised())
        Main::get().enterSelectedCharacterWorld(true);
}

// ---------------------------------------------------------------------------
/*static*/
std::string CharacterSelectDialog::sha256hex(const std::string& input)
{
    return mwmp::crypto::sha256hex(input);
}

} // namespace mwmp
