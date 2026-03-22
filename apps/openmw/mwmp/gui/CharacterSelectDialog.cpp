#include "CharacterSelectDialog.hpp"

#include <MyGUI_InputManager.h>

#include <components/debug/debuglog.hpp>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/mechanicsmanager.hpp"
#include "../../mwbase/statemanager.hpp"
#include "../../mwbase/windowmanager.hpp"
#include "../../mwgui/inventorywindow.hpp"
#include "../../mwbase/world.hpp"
#include "../../mwgui/mode.hpp"
#include <components/esm/position.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadclas.hpp>
#include "../sync/PlayerSync.hpp"
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
    getWidget(mEnterBtn,           "EnterWorldButton");
    getWidget(mNewCharBtn,         "NewCharButton");
    getWidget(mCharCancelBtn,      "CharCancelButton");

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
}

// ---------------------------------------------------------------------------
void CharacterSelectDialog::setServer(const std::string& host, uint16_t port)
{
    mHost = host;
    mPort = port;
}

void CharacterSelectDialog::onOpen()
{
    WindowModal::onOpen();
    showLoginPanel();
    MyGUI::InputManager::getInstance().setKeyFocusWidget(
        mUsername->getCaption().empty() ? mUsername : mPassword);
}

// ---------------------------------------------------------------------------
// Show/hide helpers — toggle individual widgets directly (no sub-panels)
// ---------------------------------------------------------------------------
void CharacterSelectDialog::showLoginPanel()
{
    mState = State::Login;
    mTimer = 0.f;
    mMainWidget->castType<MyGUI::Window>()->setCaption("Multiplayer Login");

    // Login group — show
    mUsernameLabel->setVisible(true);
    mUsername->setVisible(true);
    mPasswordLabel->setVisible(true);
    mPassword->setVisible(true);
    mStatusLabel->setVisible(true);
    mLoginBtn->setVisible(true);
    mRegBtn->setVisible(true);
    mLoginCancelBtn->setVisible(true);

    // CharSelect group — hide
    mConnectedLabel->setVisible(false);
    mCharSelectHint->setVisible(false);
    mList->setVisible(false);
    mNewCharNameRow->setVisible(false);
    mEnterBtn->setVisible(false);
    mNewCharBtn->setVisible(false);
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

    // Login group — hide
    mUsernameLabel->setVisible(false);
    mUsername->setVisible(false);
    mPasswordLabel->setVisible(false);
    mPassword->setVisible(false);
    mStatusLabel->setVisible(false);
    mLoginBtn->setVisible(false);
    mRegBtn->setVisible(false);
    mLoginCancelBtn->setVisible(false);

    // CharSelect group — show
    mConnectedLabel->setVisible(true);
    mCharSelectHint->setVisible(true);
    mList->setVisible(true);
    mEnterBtn->setVisible(true);
    mNewCharBtn->setVisible(true);
    mCharCancelBtn->setVisible(true);

    if (resetNamingRow)
    {
        mNewCharNameRow->setVisible(false);
        mNewCharNameEdit->setCaption("");
        mState = State::CharSelect;
    }

    setCharStatus("");
    setCharPanelBusy(false);
}

// ---------------------------------------------------------------------------
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

void CharacterSelectDialog::setCharPanelBusy(bool busy)
{
    mEnterBtn->setEnabled(!busy);
    mNewCharBtn->setEnabled(!busy);
    mCharCancelBtn->setEnabled(!busy);
    mNewCharNameConfirm->setEnabled(!busy);
    mNewCharNameEdit->setEnabled(!busy);
}

// ---------------------------------------------------------------------------
// Login group handlers
// ---------------------------------------------------------------------------
void CharacterSelectDialog::onLoginClicked   (MyGUI::Widget*) { doConnect(false); }
void CharacterSelectDialog::onRegisterClicked(MyGUI::Widget*) { doConnect(true);  }

void CharacterSelectDialog::onLoginCancelClicked(MyGUI::Widget*)
{
    if (Main::isInitialised())
        Main::destroy();
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
    if (mState == State::Connecting) return;

    const std::string user = std::string(mUsername->getCaption());
    const std::string pass = std::string(mPassword->getCaption());

    if (user.empty()) { setLoginStatus("Username cannot be empty."); return; }
    if (pass.empty()) { setLoginStatus("Password cannot be empty."); return; }
    if (mHost.empty()){ setLoginStatus("No server address set.");    return; }

    if (Main::isInitialised())
        Main::destroy();

    mState = State::Connecting;
    mTimer = 0.f;
    setLoginStatus("Connecting...");
    mLoginBtn->setEnabled(false);
    mRegBtn->setEnabled(false);
    mUsername->setEnabled(false);
    mPassword->setEnabled(false);

    if (!Main::init(mHost, mPort, user, sha256hex(pass), isRegister))
    {
        showLoginPanel();
        setLoginStatus("Could not initiate connection to " + mHost + ".");
    }
}

// ---------------------------------------------------------------------------
// CharSelect group handlers
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
    if (name.empty())
    {
        MyGUI::InputManager::getInstance().setKeyFocusWidget(mNewCharNameEdit);
        return;
    }
    sendCharacterSelect(name, /*isNew=*/true);
}

void CharacterSelectDialog::onCharCancelClicked(MyGUI::Widget*)
{
    if (Main::isInitialised())
        Main::destroy();
    setVisible(false);
    mState = State::Login;
}

// ---------------------------------------------------------------------------
void CharacterSelectDialog::sendCharacterSelect(const std::string& charName, bool isNew)
{
    if (!Main::isInitialised()) return;

    Main::get().clearCharSelectError();

    PacketCharacterSelect pkt;
    pkt.charName = charName;
    pkt.isNew    = isNew;
    Main::get().getNetworking().sendReliable(pkt.encode());

    mState = State::WaitingForData;
    mTimer = 0.f;
    setCharStatus("Loading...");
    setCharPanelBusy(true);

    Log(Debug::Info) << "[MP] Sent CharacterSelect: '"
                     << charName << "' isNew=" << isNew;
}

// ---------------------------------------------------------------------------
// onFrame
// ---------------------------------------------------------------------------
void CharacterSelectDialog::onFrame(float dt)
{
    if (mState == State::Connecting)
    {
        mTimer += dt;
        if (mTimer > 15.f)
        {
            if (Main::isInitialised()) Main::destroy();
            showLoginPanel();
            setLoginStatus("Connection timed out.");
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
            showLoginPanel();
            setLoginStatus(reason.empty() ? "Rejected by server." : reason);
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
    MWBase::Environment::get().getWindowManager()->removeGuiMode(MWGui::GM_MainMenu);

    const bool        isNew     = Main::get().isNewCharacter();
    const std::string spawnCell = Main::get().getSpawnCell();
    const std::string charName  = Main::get().getCharacterName();
    const std::string loginName = Main::get().getPlayerName();
    const std::string worldName = charName.empty() ? loginName : charName;

    if (isNew)
    {
        Log(Debug::Info) << "[MP] New character — spawning in: " << spawnCell;
        MWBase::Environment::get().getStateManager()->newGame(true);

        if (!worldName.empty())
            MWBase::Environment::get().getMechanicsManager()->setPlayerName(worldName);

        const std::string targetCell = spawnCell.empty() ? "toddtest" : spawnCell;
        MWBase::World* world = MWBase::Environment::get().getWorld();
        ESM::Position pos{};
        const auto intId = world->findInteriorPosition(targetCell, pos);
        if (!intId.empty())
            world->changeToCell(intId, pos, true);
        else
        {
            ESM::Position extPos{};
            const auto extId = world->findExteriorPosition(targetCell, extPos);
            if (!extId.empty())
                world->changeToCell(extId, extPos, true);
            else
                world->changeToInteriorCell(targetCell, pos, true);
        }

        MWBase::Environment::get().getWindowManager()->setNewGame(true);
        MWBase::Environment::get().getWindowManager()->startCharGen();
        MWBase::Environment::get().getWindowManager()->setCharGenCompleteCallback(
            []() {
                if (Main::isInitialised())
                {
                    Log(Debug::Info) << "[MP] Chargen complete — arming watcher";
                    Main::get().startWatchingCharGen();
                }
                MWBase::Environment::get().getWindowManager()->setNewGame(false);
            });
        MWBase::Environment::get().getWindowManager()->pushGuiMode(MWGui::GM_Race);
    }
    else
    {
        Log(Debug::Info) << "[MP] Returning player — restoring in: " << spawnCell;
        MWBase::Environment::get().getStateManager()->newGame(true);
        MWBase::Environment::get().getWindowManager()->updatePlayer();

        if (!worldName.empty())
            MWBase::Environment::get().getMechanicsManager()->setPlayerName(worldName);

        auto mm = MWBase::Environment::get().getMechanicsManager();
        try
        {
            const std::string race    = Main::get().getRestoredRace();
            const std::string head    = Main::get().getRestoredHeadMesh();
            const std::string hair    = Main::get().getRestoredHairMesh();
            const bool        isMale  = Main::get().getRestoredIsMale();
            const std::string clsName = Main::get().getRestoredClassName();
            const std::string birth   = Main::get().getRestoredBirthSign();

            if (!race.empty())
            {
                mm->setPlayerRace(ESM::RefId::deserializeText(race), isMale,
                                  ESM::RefId::deserializeText(head),
                                  ESM::RefId::deserializeText(hair));
                MWBase::Environment::get().getWindowManager()
                    ->getInventoryWindow()->rebuildAvatar();
            }
            if (!clsName.empty())
            {
                ESM::Class cls;
                cls.mName        = clsName;
                cls.mData        = Main::get().getPlayerSync().localPlayer().charClass.mData;
                cls.mRecordFlags = 0;
                mm->setPlayerClass(cls);
            }
            if (!birth.empty())
                mm->setPlayerBirthsign(ESM::RefId::deserializeText(birth));
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "[MP] Chargen restore error: " << e.what();
        }

        const std::string targetCell = spawnCell.empty() ? "toddtest" : spawnCell;
        const float sx = Main::get().getSpawnX(),    sy = Main::get().getSpawnY(),
                    sz = Main::get().getSpawnZ();
        const float rx = Main::get().getSpawnRotX(), ry = Main::get().getSpawnRotY(),
                    rz = Main::get().getSpawnRotZ();
        const bool hasSavedPos = (sx != 0.f || sy != 0.f || sz != 0.f);

        MWBase::World* world = MWBase::Environment::get().getWorld();
        ESM::Position dest{};

        const auto intId = world->findInteriorPosition(targetCell, dest);
        if (!intId.empty())
        {
            if (hasSavedPos) { dest.pos[0]=sx; dest.pos[1]=sy; dest.pos[2]=sz;
                               dest.rot[0]=rx; dest.rot[1]=ry; dest.rot[2]=rz; }
            world->changeToCell(intId, dest, true);
        }
        else
        {
            const auto extId = world->findExteriorPosition(targetCell, dest);
            if (!extId.empty())
            {
                if (hasSavedPos) { dest.pos[0]=sx; dest.pos[1]=sy; dest.pos[2]=sz;
                                   dest.rot[0]=rx; dest.rot[1]=ry; dest.rot[2]=rz; }
                world->changeToCell(extId, dest, true);
            }
            else
                world->changeToInteriorCell(targetCell, dest, true);
        }
    }
}

// ---------------------------------------------------------------------------
/*static*/
std::string CharacterSelectDialog::sha256hex(const std::string& input)
{
    return mwmp::crypto::sha256hex(input);
}

} // namespace mwmp
