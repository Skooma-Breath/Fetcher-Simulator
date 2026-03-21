#pragma once

#include <string>

#include <MyGUI_Button.h>
#include <MyGUI_ListBox.h>
#include <MyGUI_TextBox.h>
#include <MyGUI_Widget.h>

#include "../../mwgui/windowbase.hpp"

namespace mwmp
{
    /**
     * CharacterSelectDialog  (Phase 6 stub)
     *
     * Shown after a successful handshake.  In Phase 8 the character list will
     * be populated from a server-sent CharacterList packet.  For Phase 6 the
     * dialog shows the connected player name and a single "Enter World" button
     * that drops the player straight into the game world.
     *
     * Usage:
     *   dlg->setConnectedInfo("Alice", "myserver.example.com");
     *   dlg->setVisible(true);
     */
    class CharacterSelectDialog : public MWGui::WindowModal
    {
    public:
        CharacterSelectDialog();

        /// Populate the status line before showing.
        void setConnectedInfo(const std::string& playerName,
                              const std::string& host);

        // WindowModal
        void onOpen() override;

    private:
        void onEnterWorldClicked(MyGUI::Widget* sender);
        void onCancelClicked    (MyGUI::Widget* sender);

        MyGUI::TextBox* mLabel       = nullptr;
        MyGUI::ListBox* mList        = nullptr;
        MyGUI::Button*  mEnterBtn    = nullptr;
        MyGUI::Button*  mCancelBtn   = nullptr;
    };

} // namespace mwmp
