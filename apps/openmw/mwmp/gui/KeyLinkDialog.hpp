#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_TextBox.h>
#include <MyGUI_Widget.h>

#include "../../mwgui/windowbase.hpp"

namespace mwmp
{
    /**
     * KeyLinkDialog
     *
     * Explains machine linking and lets the player link or unlink this machine.
     *
     * When opened after the charselect panel:
     *   - If NOT linked: shows explanation + "Link" button.
     *     Clicking Link: generates keypair, sends PacketLinkKeyRequest.
     *   - If ALREADY linked: shows link status + "Unlink" button.
     *     Clicking Unlink: sends PacketUnlinkKeyRequest, deletes key file.
     *
     * Callback fires (with updated linked status) when the dialog closes
     * so CharacterSelectDialog can refresh its KeyLinkButton text.
     */
    class KeyLinkDialog : public MWGui::WindowModal
    {
    public:
        using CloseCallback = std::function<void(bool isNowLinked)>;

        KeyLinkDialog();

        /// Open the dialog.  isLinked = current link state for this server.
        void open(const std::string& host, uint16_t port,
                  const std::string& username, bool isLinked,
                  CloseCallback cb);

        // WindowModal
        void onOpen() override;

    private:
        void onActionClicked (MyGUI::Widget* sender);
        void onCancelClicked (MyGUI::Widget* sender);

        void doLink();
        void doUnlink();

        MyGUI::TextBox* mTitleLabel   = nullptr;
        MyGUI::EditBox* mBodyText     = nullptr;
        MyGUI::TextBox* mStatusLabel  = nullptr;
        MyGUI::Button*  mActionBtn    = nullptr;
        MyGUI::Button*  mCancelBtn    = nullptr;

        std::string   mHost;
        uint16_t      mPort     = 25565;
        std::string   mUsername;
        bool          mIsLinked = false;
        CloseCallback mCallback;
    };

} // namespace mwmp
