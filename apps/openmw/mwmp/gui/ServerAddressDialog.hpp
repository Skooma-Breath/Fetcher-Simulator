#pragma once

#include <functional>
#include <string>

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_KeyCode.h>
#include <MyGUI_Widget.h>

#include "../../mwgui/windowbase.hpp"

namespace mwmp
{
    /**
     * ServerAddressDialog
     *
     * The "Direct Connect" modal shown from the main menu.
     * Presents an address field, a port field, and Connect/Cancel buttons.
     * Persists the last-used address+port in [Multiplayer] settings so
     * they are pre-populated on next open.
     *
     * Usage:
     *   auto dlg = std::make_unique<mwmp::ServerAddressDialog>();
     *   dlg->setConnectCallback([](const std::string& addr, uint16_t port) {
     *       // initiate connection
     *   });
     *   dlg->setVisible(true);
     */
    class ServerAddressDialog : public MWGui::WindowModal
    {
    public:
        using ConnectCallback = std::function<void(const std::string& address, uint16_t port)>;

        ServerAddressDialog();

        /// Called when the user confirms.
        void setConnectCallback(ConnectCallback cb) { mConnectCb = std::move(cb); }

        // WindowModal
        void onOpen() override;

    private:
        void onConnectClicked(MyGUI::Widget* sender);
        void onCancelClicked (MyGUI::Widget* sender);
        void onKeyPress(MyGUI::Widget* sender, MyGUI::KeyCode key, MyGUI::Char ch);
        void doConnect();

        MyGUI::EditBox* mAddress       = nullptr;
        MyGUI::EditBox* mPort          = nullptr;
        MyGUI::Button*  mConnectButton = nullptr;
        MyGUI::Button*  mCancelButton  = nullptr;

        ConnectCallback mConnectCb;
    };

} // namespace mwmp
