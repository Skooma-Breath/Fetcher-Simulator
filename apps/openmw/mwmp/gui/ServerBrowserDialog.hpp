#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <string>
#include <vector>

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_KeyCode.h>
#include <MyGUI_ListBox.h>
#include <MyGUI_TextBox.h>
#include <MyGUI_Widget.h>

#include "../../mwgui/windowbase.hpp"

namespace mwmp
{
    /// Flat description of one public game server, as returned by the master server.
    struct ServerEntry
    {
        std::string host;
        uint16_t    port             = 25565;
        std::string name;
        std::string version;
        std::string gameMode;
        std::string country;
        int         currentPlayers   = 0;
        int         maxPlayers       = 0;
        int         ping             = -1;   ///< -1 = not yet measured
        bool        passwordProtected = false;
    };

    /**
     * ServerBrowserDialog
     *
     * The "Server Browser" modal shown from the main menu.
     * Fetches the public server list from the master server on a background
     * thread (std::async), then populates a ListBox.  Supports live name
     * filtering and column-header sort.
     *
     * State machine:
     *   Idle ──refresh()──> Loading ──success──> Loaded
     *                                └──failure──> Error ──refresh()──> Loading
     *
     * Connect path:
     *   double-click row  ─┐
     *   Connect button     ├──> ConnectCallback(host, port) → caller handles the rest
     *   Enter key          ┘
     */
    class ServerBrowserDialog : public MWGui::WindowModal
    {
    public:
        using ConnectCallback = std::function<void(const std::string& address, uint16_t port)>;

        ServerBrowserDialog();

        /// Called when the user selects a server and confirms.
        void setConnectCallback(ConnectCallback cb) { mConnectCb = std::move(cb); }

        /// Begin an async fetch of the server list (non-blocking).
        /// Safe to call while already loading — previous future is abandoned.
        void refresh();

        // WindowModal
        void onFrame(float dt) override;
        void onOpen()  override;

    private:
        // ── Internal state ──────────────────────────────────────────────────
        enum class State { Idle, Loading, Loaded, Error };

        void setState(State s);

        // ── Data ─────────────────────────────────────────────────────────────
        /// Parse the JSON body returned by GET /servers and populate mServers.
        /// Hand-rolled to avoid introducing a JSON library dependency.
        void parseAndPopulate(const std::string& json);

        /// Rebuild mFiltered from mServers applying the current search text,
        /// then repopulate mList.
        void applyFilter();

        /// Format one ServerEntry as a fixed-width display string for the ListBox.
        std::string formatRow(const ServerEntry& e) const;

        /// Connect to the server at index mSelected (in mFiltered).
        void doConnect();

        // ── Widget callbacks ─────────────────────────────────────────────────
        void onRefreshClicked  (MyGUI::Widget*  sender);
        void onConnectClicked  (MyGUI::Widget*  sender);
        void onCancelClicked   (MyGUI::Widget*  sender);
        void onListDoubleClick (MyGUI::ListBox* sender, size_t index);
        void onListSelectChange(MyGUI::ListBox* sender, size_t index);
        void onSearchChanged   (MyGUI::EditBox* sender);
        void onKeyPress        (MyGUI::Widget*  sender, MyGUI::KeyCode key, MyGUI::Char ch);
        void onColumnClicked   (MyGUI::Widget*  sender);

        // ── Widgets ──────────────────────────────────────────────────────────
        MyGUI::EditBox* mSearch      = nullptr;
        MyGUI::ListBox* mList        = nullptr;
        MyGUI::TextBox* mStatus      = nullptr;
        MyGUI::Button*  mRefreshBtn  = nullptr;
        MyGUI::Button*  mConnectBtn  = nullptr;
        MyGUI::Button*  mCancelBtn   = nullptr;

        // Column header TextBoxes (made clickable for sorting)
        MyGUI::TextBox* mColLock    = nullptr;
        MyGUI::TextBox* mColName    = nullptr;
        MyGUI::TextBox* mColPlayers = nullptr;
        MyGUI::TextBox* mColPing    = nullptr;
        MyGUI::TextBox* mColVersion = nullptr;
        MyGUI::TextBox* mColMode    = nullptr;
        MyGUI::TextBox* mColCountry = nullptr;

        // ── State ────────────────────────────────────────────────────────────
        State  mState = State::Idle;
        std::future<std::string> mFuture;

        std::vector<ServerEntry> mServers;   ///< full unfiltered result
        std::vector<size_t>      mFiltered;  ///< indices into mServers after filter+sort

        std::string mMasterUrl;

        // Sort state: column index (0=lock,1=name,2=players,3=ping,4=ver,5=mode,6=cc),
        // default players descending.
        int  mSortCol = 2;
        bool mSortAsc = false;

        size_t mSelected = MyGUI::ITEM_NONE; ///< index into mFiltered

        ConnectCallback mConnectCb;
    };

} // namespace mwmp
