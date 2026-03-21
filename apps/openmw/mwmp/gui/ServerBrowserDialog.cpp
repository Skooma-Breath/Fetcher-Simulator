#include "ServerBrowserDialog.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <future>
#include <iomanip>
#include <sstream>

#include <MyGUI_InputManager.h>

#include <components/debug/debuglog.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/settings/values.hpp>

// cpp-httplib — vendored single header
#include "httplib.h"

namespace mwmp
{

// ============================================================================
//  Helpers
// ============================================================================

namespace
{
    /// Pad / truncate 'str' to exactly 'width' chars.
    std::string col(const std::string& str, int width)
    {
        if (static_cast<int>(str.size()) >= width)
            return str.substr(0, width);
        return str + std::string(width - static_cast<int>(str.size()), ' ');
    }

    /// Minimal JSON string extractor: find first occurrence of "key" in obj
    /// and return its raw value string (unquoted for strings, as-is for numbers/bools).
    std::string jsonString(const std::string& obj, const std::string& key)
    {
        // Look for  "key"  :
        const std::string needle = "\"" + key + "\"";
        auto pos = obj.find(needle);
        if (pos == std::string::npos) return {};
        pos += needle.size();

        // Skip whitespace + colon
        while (pos < obj.size() && (obj[pos] == ' ' || obj[pos] == '\t' ||
               obj[pos] == '\n' || obj[pos] == '\r' || obj[pos] == ':'))
            ++pos;
        if (pos >= obj.size()) return {};

        if (obj[pos] == '"')
        {
            // Quoted string — read until closing quote (naïve, no escape handling)
            ++pos;
            std::string result;
            while (pos < obj.size() && obj[pos] != '"')
                result += obj[pos++];
            return result;
        }
        else
        {
            // Bare value (number / bool / null) — read until delimiter
            std::string result;
            while (pos < obj.size() && obj[pos] != ',' && obj[pos] != '}' &&
                   obj[pos] != ']' && obj[pos] != ' ' && obj[pos] != '\n')
                result += obj[pos++];
            return result;
        }
    }

    int jsonInt(const std::string& obj, const std::string& key, int def = 0)
    {
        const std::string v = jsonString(obj, key);
        if (v.empty()) return def;
        try { return std::stoi(v); } catch (...) { return def; }
    }

    bool jsonBool(const std::string& obj, const std::string& key, bool def = false)
    {
        const std::string v = jsonString(obj, key);
        if (v == "true")  return true;
        if (v == "false") return false;
        return def;
    }

    /// Split a JSON array body (everything between the outer [ ]) into individual
    /// object strings.  Works for flat objects without nested arrays/objects.
    std::vector<std::string> splitJsonArray(const std::string& body)
    {
        std::vector<std::string> out;
        std::size_t pos = body.find('{');
        while (pos != std::string::npos)
        {
            // Find matching closing brace
            int depth = 0;
            std::size_t end = pos;
            while (end < body.size())
            {
                if      (body[end] == '{') ++depth;
                else if (body[end] == '}') { --depth; if (depth == 0) { ++end; break; } }
                ++end;
            }
            out.push_back(body.substr(pos, end - pos));
            pos = body.find('{', end);
        }
        return out;
    }

    /// Perform the blocking HTTP GET on a background thread.
    /// Returns the response body on success, or an empty string on failure.
    std::string fetchServerList(const std::string& masterUrl)
    {
        try
        {
            httplib::Client cli(masterUrl);
            cli.set_connection_timeout(5);
            cli.set_read_timeout(8);
            auto res = cli.Get("/servers");
            if (res && res->status == 200)
                return res->body;
            Log(Debug::Warning) << "[ServerBrowser] master server returned HTTP "
                                 << (res ? res->status : -1);
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "[ServerBrowser] HTTP error: " << e.what();
        }
        return {};
    }
} // anonymous namespace

// ============================================================================
//  Constructor
// ============================================================================

ServerBrowserDialog::ServerBrowserDialog()
    : WindowModal("openmw_mp_server_browser.layout")
{
    getWidget(mSearch,     "SearchBox");
    getWidget(mList,       "ServerList");
    getWidget(mStatus,     "StatusLabel");
    getWidget(mRefreshBtn, "RefreshButton");
    getWidget(mConnectBtn, "ConnectButton");
    getWidget(mCancelBtn,  "CancelButton");

    getWidget(mColLock,    "ColLock");
    getWidget(mColName,    "ColName");
    getWidget(mColPlayers, "ColPlayers");
    getWidget(mColPing,    "ColPing");
    getWidget(mColVersion, "ColVersion");
    getWidget(mColMode,    "ColMode");
    getWidget(mColCountry, "ColCountry");

    // Column header click → sort
    for (MyGUI::TextBox* hdr : { mColLock, mColName, mColPlayers,
                                  mColPing, mColVersion, mColMode, mColCountry })
    {
        hdr->setNeedMouseFocus(true);
        hdr->eventMouseButtonClick +=
            MyGUI::newDelegate(this, &ServerBrowserDialog::onColumnClicked);
    }

    mList->eventListChangePosition +=
        MyGUI::newDelegate(this, &ServerBrowserDialog::onListSelectChange);
    mList->eventListMouseItemActivate +=
        MyGUI::newDelegate(this, &ServerBrowserDialog::onListDoubleClick);

    mSearch->eventEditTextChange +=
        MyGUI::newDelegate(this, &ServerBrowserDialog::onSearchChanged);
    mSearch->eventKeyButtonPressed +=
        MyGUI::newDelegate(this, &ServerBrowserDialog::onKeyPress);
    mList->eventKeyButtonPressed +=
        MyGUI::newDelegate(this, &ServerBrowserDialog::onKeyPress);

    mRefreshBtn->eventMouseButtonClick +=
        MyGUI::newDelegate(this, &ServerBrowserDialog::onRefreshClicked);
    mConnectBtn->eventMouseButtonClick +=
        MyGUI::newDelegate(this, &ServerBrowserDialog::onConnectClicked);
    mCancelBtn->eventMouseButtonClick +=
        MyGUI::newDelegate(this, &ServerBrowserDialog::onCancelClicked);

    mConnectBtn->setEnabled(false);
}

// ============================================================================
//  WindowModal overrides
// ============================================================================

void ServerBrowserDialog::onOpen()
{
    WindowModal::onOpen();
    mMasterUrl = Settings::multiplayer().mMasterServerUrl.get();
    mSelected  = MyGUI::ITEM_NONE;
    mConnectBtn->setEnabled(false);
    MyGUI::InputManager::getInstance().setKeyFocusWidget(mList);
}

void ServerBrowserDialog::onFrame(float /*dt*/)
{
    if (mState != State::Loading) return;
    if (!mFuture.valid())         return;

    if (mFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;   // still in-flight

    const std::string body = mFuture.get();
    if (body.empty())
    {
        setState(State::Error);
        mStatus->setCaption("Could not reach master server.");
    }
    else
    {
        parseAndPopulate(body);
        setState(State::Loaded);
    }
}

// ============================================================================
//  Public API
// ============================================================================

void ServerBrowserDialog::refresh()
{
    if (mMasterUrl.empty())
    {
        mStatus->setCaption("No master server URL configured.");
        setState(State::Error);
        return;
    }

    setState(State::Loading);
    mSelected = MyGUI::ITEM_NONE;
    mConnectBtn->setEnabled(false);

    const std::string url = mMasterUrl; // capture by value for the lambda
    mFuture = std::async(std::launch::async, fetchServerList, url);
}

// ============================================================================
//  State machine
// ============================================================================

void ServerBrowserDialog::setState(State s)
{
    mState = s;
    switch (s)
    {
        case State::Idle:
            mStatus->setCaption("");
            break;
        case State::Loading:
            mList->removeAllItems();
            mStatus->setCaption("Fetching server list...");
            mRefreshBtn->setEnabled(false);
            break;
        case State::Loaded:
            mRefreshBtn->setEnabled(true);
            break;
        case State::Error:
            mRefreshBtn->setEnabled(true);
            break;
    }
}

// ============================================================================
//  JSON parsing
// ============================================================================

void ServerBrowserDialog::parseAndPopulate(const std::string& json)
{
    mServers.clear();

    const std::vector<std::string> objects = splitJsonArray(json);
    mServers.reserve(objects.size());

    for (const std::string& obj : objects)
    {
        ServerEntry e;
        e.host             = jsonString(obj, "host");
        e.port             = static_cast<uint16_t>(jsonInt(obj, "port", 25565));
        e.name             = jsonString(obj, "name");
        e.version          = jsonString(obj, "version");
        e.gameMode         = jsonString(obj, "game_mode");
        e.country          = jsonString(obj, "country");
        e.currentPlayers   = jsonInt(obj, "current_players");
        e.maxPlayers       = jsonInt(obj, "max_players");
        e.passwordProtected = jsonBool(obj, "password_protected");
        e.ping             = -1;  // async ping not implemented yet

        if (!e.host.empty())
            mServers.push_back(std::move(e));
    }

    applyFilter();
}

// ============================================================================
//  Filter + sort → list population
// ============================================================================

void ServerBrowserDialog::applyFilter()
{
    const std::string search = Misc::StringUtils::lowerCase(
        std::string(mSearch->getCaption()));

    // Build filtered index list
    mFiltered.clear();
    mFiltered.reserve(mServers.size());
    for (std::size_t i = 0; i < mServers.size(); ++i)
    {
        if (!search.empty())
        {
            const std::string nameLow = Misc::StringUtils::lowerCase(mServers[i].name);
            if (nameLow.find(search) == std::string::npos)
                continue;
        }
        mFiltered.push_back(i);
    }

    // Sort
    const int sc = mSortCol;
    const bool asc = mSortAsc;
    std::stable_sort(mFiltered.begin(), mFiltered.end(),
        [this, sc, asc](std::size_t a, std::size_t b)
        {
            const ServerEntry& A = mServers[a];
            const ServerEntry& B = mServers[b];
            bool less = false;
            switch (sc)
            {
                case 0: less = A.passwordProtected < B.passwordProtected; break;
                case 1: less = A.name              < B.name;              break;
                case 2: less = A.currentPlayers    < B.currentPlayers;    break;
                case 3: less = A.ping              < B.ping;              break;
                case 4: less = A.version           < B.version;           break;
                case 5: less = A.gameMode          < B.gameMode;          break;
                case 6: less = A.country           < B.country;           break;
                default: break;
            }
            return asc ? less : !less;
        });

    // Populate ListBox
    mList->removeAllItems();
    for (std::size_t fi : mFiltered)
        mList->addItem(formatRow(mServers[fi]));

    // Update status
    if (mState == State::Loaded || mState == State::Loading)
    {
        std::ostringstream ss;
        ss << mFiltered.size() << " / " << mServers.size() << " servers";
        mStatus->setCaption(ss.str());
    }

    mSelected = MyGUI::ITEM_NONE;
    mConnectBtn->setEnabled(false);
}

std::string ServerBrowserDialog::formatRow(const ServerEntry& e) const
{
    // Fixed-width columns matching the layout header positions:
    // lock(2) | name(38) | players(9) | ping(7) | version(9) | mode(9) | cc(3)
    std::ostringstream ss;
    ss << col(e.passwordProtected ? "[P]" : "   ", 3);
    ss << " ";
    ss << col(e.name, 38);
    ss << " ";
    {
        std::string p = std::to_string(e.currentPlayers) + "/" + std::to_string(e.maxPlayers);
        ss << col(p, 9);
    }
    ss << " ";
    {
        std::string ping = (e.ping >= 0) ? (std::to_string(e.ping) + "ms") : "---";
        ss << col(ping, 7);
    }
    ss << " ";
    ss << col(e.version, 9);
    ss << " ";
    ss << col(e.gameMode, 9);
    ss << " ";
    ss << col(e.country, 3);
    return ss.str();
}

// ============================================================================
//  Widget callbacks
// ============================================================================

void ServerBrowserDialog::onRefreshClicked(MyGUI::Widget* /*sender*/)
{
    refresh();
}

void ServerBrowserDialog::onConnectClicked(MyGUI::Widget* /*sender*/)
{
    doConnect();
}

void ServerBrowserDialog::onCancelClicked(MyGUI::Widget* /*sender*/)
{
    setVisible(false);
}

void ServerBrowserDialog::onListSelectChange(MyGUI::ListBox* /*sender*/, size_t index)
{
    mSelected = index;
    mConnectBtn->setEnabled(index != MyGUI::ITEM_NONE && index < mFiltered.size());
}

void ServerBrowserDialog::onListDoubleClick(MyGUI::ListBox* /*sender*/, size_t index)
{
    mSelected = index;
    doConnect();
}

void ServerBrowserDialog::onSearchChanged(MyGUI::EditBox* /*sender*/)
{
    applyFilter();
}

void ServerBrowserDialog::onKeyPress(MyGUI::Widget* /*sender*/,
                                     MyGUI::KeyCode key, MyGUI::Char /*ch*/)
{
    if (key == MyGUI::KeyCode::Return || key == MyGUI::KeyCode::NumpadEnter)
        doConnect();
    else if (key == MyGUI::KeyCode::Escape)
        setVisible(false);
}

void ServerBrowserDialog::onColumnClicked(MyGUI::Widget* sender)
{
    int col = -1;
    if      (sender == mColLock)    col = 0;
    else if (sender == mColName)    col = 1;
    else if (sender == mColPlayers) col = 2;
    else if (sender == mColPing)    col = 3;
    else if (sender == mColVersion) col = 4;
    else if (sender == mColMode)    col = 5;
    else if (sender == mColCountry) col = 6;
    if (col < 0) return;

    if (mSortCol == col)
        mSortAsc = !mSortAsc;
    else
    {
        mSortCol = col;
        // Default direction: descending for numeric cols, ascending for text
        mSortAsc = (col == 1 || col == 4 || col == 5 || col == 6);
    }

    // Refresh sort indicator on headers (append ▲/▼ to active header caption)
    const std::string indicator = mSortAsc ? " ^" : " v";
    const std::string baseNames[] = { "", "Server Name", "Players", "Ping",
                                       "Version", "Mode", "CC" };
    MyGUI::TextBox* hdrs[] = { mColLock, mColName, mColPlayers, mColPing,
                                 mColVersion, mColMode, mColCountry };
    for (int i = 0; i < 7; ++i)
        hdrs[i]->setCaption(baseNames[i] + (i == mSortCol ? indicator : ""));

    applyFilter();
}

// ============================================================================
//  Connect
// ============================================================================

void ServerBrowserDialog::doConnect()
{
    if (mSelected == MyGUI::ITEM_NONE || mSelected >= mFiltered.size())
        return;

    const ServerEntry& e = mServers[mFiltered[mSelected]];
    if (e.host.empty()) return;

    setVisible(false);

    if (mConnectCb)
        mConnectCb(e.host, e.port);
}

} // namespace mwmp
