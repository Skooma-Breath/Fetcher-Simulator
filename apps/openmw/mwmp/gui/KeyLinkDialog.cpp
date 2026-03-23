#include "KeyLinkDialog.hpp"
#include <MyGUI_EditBox.h>
#include "../Identity.hpp"
#include "../Main.hpp"
#include "../network/Client.hpp"

#include <components/debug/debuglog.hpp>
#include <components/openmw-mp/Packets/System/PacketHandshake.hpp>

namespace mwmp
{

static const char* kBodyUnlinked =
    "Linking this machine stores a small key file on your computer. "
    "The next time you connect to this server your identity will be confirmed "
    "automatically without a password.\n\n"
    "You can unlink at any time from this screen. "
    "Each machine has its own independent key — revoking one does not affect "
    "any other machine.";

static const char* kBodyLinked =
    "This machine is linked to your account on this server. "
    "You connect automatically without a password.\n\n"
    "Click Unlink to remove the key from this machine and this server. "
    "You will need your password to log in again afterwards.";

// ---------------------------------------------------------------------------
KeyLinkDialog::KeyLinkDialog()
    : WindowModal("openmw_mp_key_link.layout")
{
    getWidget(mTitleLabel,  "TitleLabel");
    getWidget(mBodyText,    "BodyText");
    getWidget(mStatusLabel, "StatusLabel");
    getWidget(mActionBtn,   "ActionButton");
    getWidget(mCancelBtn,   "CancelButton");

    mActionBtn->eventMouseButtonClick +=
        MyGUI::newDelegate(this, &KeyLinkDialog::onActionClicked);
    mCancelBtn->eventMouseButtonClick +=
        MyGUI::newDelegate(this, &KeyLinkDialog::onCancelClicked);
}

// ---------------------------------------------------------------------------
void KeyLinkDialog::open(const std::string& host, uint16_t port,
                          const std::string& username, bool isLinked,
                          CloseCallback cb)
{
    mHost      = host;
    mPort      = port;
    mUsername  = username;
    mIsLinked  = isLinked;
    mCallback  = std::move(cb);
    setVisible(true);
}

void KeyLinkDialog::onOpen()
{
    WindowModal::onOpen();
    mStatusLabel->setCaption("");

    if (mIsLinked)
    {
        mTitleLabel->setCaption("Machine Linked");
        mBodyText->setOnlyText(kBodyLinked);
        mActionBtn->setCaption("Unlink");
    }
    else
    {
        mTitleLabel->setCaption("Link This Machine");
        mBodyText->setOnlyText(kBodyUnlinked);
        mActionBtn->setCaption("Link");
    }
    mActionBtn->setEnabled(true);
}

// ---------------------------------------------------------------------------
void KeyLinkDialog::onActionClicked(MyGUI::Widget*)
{
    if (mIsLinked) doUnlink();
    else           doLink();
}

void KeyLinkDialog::onCancelClicked(MyGUI::Widget*)
{
    setVisible(false);
    if (mCallback) mCallback(mIsLinked);
}

// ---------------------------------------------------------------------------
void KeyLinkDialog::doLink()
{
    if (!Main::isInitialised())
    {
        mStatusLabel->setCaption("Not connected — connect to a server first.");
        return;
    }

    // Generate keypair on disk (includes username for future auto-login).
    if (!Identity::generateKeypair(mHost, mPort, mUsername))
    {
        mStatusLabel->setCaption("Failed to generate key file. Check disk permissions.");
        return;
    }

    // Send the public key to the server so it associates it with our account.
    const std::string pubKey = Identity::getPublicKeyBase64(mHost, mPort);
    if (pubKey.empty())
    {
        mStatusLabel->setCaption("Key file missing after generation — unexpected error.");
        return;
    }

    PacketLinkKeyRequest pkt;
    pkt.publicKey = pubKey;
    pkt.label     = "linked machine";
    Main::get().getNetworking().sendReliable(pkt.encode());

    mIsLinked = true;
    mStatusLabel->setCaption("Linked! Future logins will skip the password.");
    mActionBtn->setCaption("Unlink");
    mActionBtn->setEnabled(true);

    Log(Debug::Info) << "[Identity] Machine linked for " << mUsername
                     << " on " << mHost << ":" << mPort;
}

void KeyLinkDialog::doUnlink()
{
    if (Main::isInitialised())
    {
        const std::string pubKey = Identity::getPublicKeyBase64(mHost, mPort);
        if (!pubKey.empty())
        {
            PacketUnlinkKeyRequest pkt;
            pkt.publicKey = pubKey;
            Main::get().getNetworking().sendReliable(pkt.encode());
        }
    }

    Identity::removeKeypair(mHost, mPort);

    mIsLinked = false;
    mStatusLabel->setCaption("Unlinked. You will need your password next time.");
    mActionBtn->setCaption("Link");
    mActionBtn->setEnabled(true);

    Log(Debug::Info) << "[Identity] Machine unlinked for " << mHost << ":" << mPort;
}

} // namespace mwmp
