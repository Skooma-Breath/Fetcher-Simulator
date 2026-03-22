#include "AccountDialog.hpp"
#include "../sha256.hpp"

#include <iomanip>
#include <sstream>

#include <MyGUI_InputManager.h>

#include <components/debug/debuglog.hpp>

#include "../Main.hpp"

namespace mwmp
{

// ============================================================================
//  AccountDialog
// ============================================================================

AccountDialog::AccountDialog()
    : WindowModal("openmw_mp_account.layout")
{
    getWidget(mUsername,  "Username");
    getWidget(mPassword,  "Password");
    getWidget(mLoginBtn,  "LoginButton");
    getWidget(mRegBtn,    "RegisterButton");
    getWidget(mCancelBtn, "CancelButton");
    getWidget(mStatus,    "StatusLabel");

    mLoginBtn->eventMouseButtonClick  +=
        MyGUI::newDelegate(this, &AccountDialog::onLoginClicked);
    mRegBtn->eventMouseButtonClick    +=
        MyGUI::newDelegate(this, &AccountDialog::onRegisterClicked);
    mCancelBtn->eventMouseButtonClick +=
        MyGUI::newDelegate(this, &AccountDialog::onCancelClicked);

    mUsername->eventKeyButtonPressed +=
        MyGUI::newDelegate(this, &AccountDialog::onKeyPress);
    mPassword->eventKeyButtonPressed +=
        MyGUI::newDelegate(this, &AccountDialog::onKeyPress);
}

// ---------------------------------------------------------------------------
void AccountDialog::setServer(const std::string& host, uint16_t port)
{
    mHost = host;
    mPort = port;
}

void AccountDialog::onOpen()
{
    WindowModal::onOpen();
    setState(State::Idle);
    MyGUI::InputManager::getInstance().setKeyFocusWidget(
        mUsername->getCaption().empty() ? mUsername : mPassword);
}

// ---------------------------------------------------------------------------
void AccountDialog::onFrame(float dt)
{
    if (mState != State::Connecting) return;

    mConnectTimer += dt;

    // Fallback timeout — GNS can take several seconds to time out on its own
    if (mConnectTimer > 15.f)
    {
        if (Main::isInitialised())
            Main::destroy();
        setState(State::Rejected, "Connection timed out.");
        return;
    }

    if (!Main::isInitialised())
    {
        setState(State::Rejected, "Connection failed.");
        return;
    }

    Main& mp = Main::get();

    // Success: handshake accepted.
    // Set mState BEFORE firing the callback so that even if onFrame is called
    // again before setVisible takes full effect, the early-return guard fires.
    if (mp.isWorldReady())
    {
        const std::string name = mp.getPlayerName();
        mState = State::Idle;   // ← prevent re-entry on subsequent frames
        mConnectTimer = 0.f;
        setVisible(false);
        if (mWorldReadyCb)
            mWorldReadyCb(name, mHost);
        return;
    }

    // Rejection: GNS has fully closed the connection
    // Note: isNetworkDisconnected() is false while still in Connecting state,
    // so this branch only fires after the server has explicitly rejected us.
    if (mp.isNetworkDisconnected())
    {
        const std::string reason = mp.getRejectReason();
        setState(State::Rejected, reason.empty() ? "Rejected by server." : reason);
        Main::destroy();
    }
}

// ---------------------------------------------------------------------------
void AccountDialog::onLoginClicked   (MyGUI::Widget*) { doConnect(false); }
void AccountDialog::onRegisterClicked(MyGUI::Widget*) { doConnect(true);  }

void AccountDialog::onCancelClicked(MyGUI::Widget*)
{
    if (Main::isInitialised())
        Main::destroy();
    setVisible(false);
}

void AccountDialog::onKeyPress(MyGUI::Widget* /*sender*/,
                                MyGUI::KeyCode key, MyGUI::Char /*ch*/)
{
    if (key == MyGUI::KeyCode::Return || key == MyGUI::KeyCode::NumpadEnter)
        doConnect(false);
    else if (key == MyGUI::KeyCode::Escape)
        onCancelClicked(nullptr);
}

// ---------------------------------------------------------------------------
void AccountDialog::doConnect(bool isRegister)
{
    if (mState == State::Connecting) return;

    const std::string user = std::string(mUsername->getCaption());
    const std::string pass = std::string(mPassword->getCaption());

    if (user.empty())
    {
        setState(State::Rejected, "Username cannot be empty.");
        return;
    }
    if (pass.empty())
    {
        setState(State::Rejected, "Password cannot be empty.");
        return;
    }
    if (mHost.empty())
    {
        setState(State::Rejected, "No server address set.");
        return;
    }

    if (Main::isInitialised())
        Main::destroy();

    const std::string hash = sha256hex(pass);

    mConnectTimer = 0.f;
    setState(State::Connecting);

    if (!Main::init(mHost, mPort, user, hash, isRegister))
    {
        setState(State::Rejected, "Could not initiate connection to " + mHost + ".");
        return;
    }
}

// ---------------------------------------------------------------------------
void AccountDialog::setState(State s, const std::string& msg)
{
    mState = s;
    const bool busy = (s == State::Connecting);
    mLoginBtn->setEnabled(!busy);
    mRegBtn->setEnabled(!busy);
    mUsername->setEnabled(!busy);
    mPassword->setEnabled(!busy);

    switch (s)
    {
        case State::Idle:
            mStatus->setCaption("");
            break;
        case State::Connecting:
            mStatus->setCaption("Connecting...");
            break;
        case State::Rejected:
            mStatus->setCaption(msg.empty() ? "Failed." : msg);
            break;
    }
}

// ---------------------------------------------------------------------------
/*static*/
std::string AccountDialog::sha256hex(const std::string& input)
{
    return mwmp::crypto::sha256hex(input);
}

} // namespace mwmp
