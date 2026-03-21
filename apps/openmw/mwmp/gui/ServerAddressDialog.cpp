#include "ServerAddressDialog.hpp"

#include <algorithm>
#include <stdexcept>

#include <MyGUI_InputManager.h>

#include <components/settings/values.hpp>

namespace mwmp
{

ServerAddressDialog::ServerAddressDialog()
    : WindowModal("openmw_mp_server_address.layout")
{
    getWidget(mAddress,       "Address");
    getWidget(mPort,          "Port");
    getWidget(mConnectButton, "ConnectButton");
    getWidget(mCancelButton,  "CancelButton");

    mConnectButton->eventMouseButtonClick +=
        MyGUI::newDelegate(this, &ServerAddressDialog::onConnectClicked);
    mCancelButton->eventMouseButtonClick +=
        MyGUI::newDelegate(this, &ServerAddressDialog::onCancelClicked);

    mAddress->eventKeyButtonPressed +=
        MyGUI::newDelegate(this, &ServerAddressDialog::onKeyPress);
    mPort->eventKeyButtonPressed +=
        MyGUI::newDelegate(this, &ServerAddressDialog::onKeyPress);
}

// ---------------------------------------------------------------------------
void ServerAddressDialog::onOpen()
{
    WindowModal::onOpen();
    const std::string& savedAddr = Settings::multiplayer().mLastServerAddress.get();
    const int savedPort = Settings::multiplayer().mLastServerPort.get();

    mAddress->setCaption(savedAddr);
    mPort->setCaption(std::to_string(savedPort > 0 ? savedPort : 25565));

    // Put focus where the user is most likely to start typing
    MyGUI::InputManager::getInstance().setKeyFocusWidget(
        savedAddr.empty() ? mAddress : mPort);
}

// ---------------------------------------------------------------------------
void ServerAddressDialog::onConnectClicked(MyGUI::Widget* /*sender*/)
{
    doConnect();
}

void ServerAddressDialog::onCancelClicked(MyGUI::Widget* /*sender*/)
{
    setVisible(false);
}

void ServerAddressDialog::onKeyPress(MyGUI::Widget* /*sender*/,
                                     MyGUI::KeyCode key, MyGUI::Char /*ch*/)
{
    if (key == MyGUI::KeyCode::Return || key == MyGUI::KeyCode::NumpadEnter)
        doConnect();
    else if (key == MyGUI::KeyCode::Escape)
        setVisible(false);
}

// ---------------------------------------------------------------------------
void ServerAddressDialog::doConnect()
{
    std::string addr = mAddress->getCaption();

    // Trim leading/trailing whitespace
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    addr.erase(addr.begin(), std::find_if(addr.begin(), addr.end(), notSpace));
    addr.erase(std::find_if(addr.rbegin(), addr.rend(), notSpace).base(), addr.end());

    if (addr.empty())
    {
        MyGUI::InputManager::getInstance().setKeyFocusWidget(mAddress);
        return;
    }

    int portVal = 25565;
    try
    {
        portVal = std::stoi(std::string(mPort->getCaption()));
    }
    catch (const std::exception&) {}

    if (portVal < 1 || portVal > 65535)
        portVal = 25565;

    // Persist for next open
    Settings::multiplayer().mLastServerAddress.set(addr);
    Settings::multiplayer().mLastServerPort.set(portVal);

    setVisible(false);

    if (mConnectCb)
        mConnectCb(addr, static_cast<uint16_t>(portVal));
}

} // namespace mwmp
