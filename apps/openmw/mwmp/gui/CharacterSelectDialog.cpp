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
#include <components/misc/strings/lower.hpp>
#include <components/esm3/loadclas.hpp>
#include "../sync/PlayerSync.hpp"
#include "../Main.hpp"

namespace mwmp
{

CharacterSelectDialog::CharacterSelectDialog()
    : WindowModal("openmw_mp_char_select.layout")
{
    getWidget(mLabel,     "ConnectedLabel");
    getWidget(mList,      "CharacterList");
    getWidget(mEnterBtn,  "EnterWorldButton");
    getWidget(mCancelBtn, "CancelButton");

    mEnterBtn->eventMouseButtonClick  +=
        MyGUI::newDelegate(this, &CharacterSelectDialog::onEnterWorldClicked);
    mCancelBtn->eventMouseButtonClick +=
        MyGUI::newDelegate(this, &CharacterSelectDialog::onCancelClicked);

    // Phase 6 stub: single placeholder row so the list is not empty
    mList->addItem("[ character list — Phase 8 ]");
}

// ---------------------------------------------------------------------------
void CharacterSelectDialog::setConnectedInfo(const std::string& playerName,
                                              const std::string& host)
{
    mLabel->setCaption("Connected as " + playerName + " on " + host);
}

void CharacterSelectDialog::onOpen()
{
    WindowModal::onOpen();
    MyGUI::InputManager::getInstance().setKeyFocusWidget(mEnterBtn);
}

// ---------------------------------------------------------------------------
void CharacterSelectDialog::onEnterWorldClicked(MyGUI::Widget* /*sender*/)
{
    setVisible(false);
    MWBase::Environment::get().getWindowManager()->removeGuiMode(MWGui::GM_MainMenu);

    const bool isNew   = Main::isInitialised() && Main::get().isNewCharacter();
    const std::string spawnCell = Main::isInitialised() ? Main::get().getSpawnCell() : "";
    const std::string loginName = Main::isInitialised() ? Main::get().getPlayerName() : "";

    if (isNew)
    {
        // New character: bypass the Morrowind intro/Census script entirely.
        // bypass=true sets sCharGenState=-1 (skips vanilla chargen script) and
        // drops the player in the world without the intro movie.
        Log(Debug::Info) << "[MP] CharacterSelectDialog: new character — bypass intro, spawn in toddtest";
        MWBase::Environment::get().getStateManager()->newGame(true);

        // Set the player display name from the login username.
        if (!loginName.empty())
            MWBase::Environment::get().getMechanicsManager()->setPlayerName(loginName);

        // Teleport to the spawn cell (default: toddtest).
        // Use the same interior/exterior-aware helper as the returning player path.
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

        // Kick off in-world chargen: Race → Class → Birth → Review.
        // We skip the Name dialog entirely — the player's name already comes from
        // the login username (set above via setPlayerName).
        //
        // setNewGame(true) creates a fresh CharacterCreation object.
        // startCharGen() primes mCreationStage = CSE_BirthSignChosen so every
        // dialog (Race, Class, Birth, Review) self-advances without needing the
        // vanilla script hooks (EnableRaceMenu etc.) that bypass=true suppressed.
        MWBase::Environment::get().getWindowManager()->setNewGame(true);
        MWBase::Environment::get().getWindowManager()->startCharGen();

        // Arm the watcher ONLY after the player clicks "Done" on the Review dialog —
        // not right now, because isInCell() is already true and the watcher would
        // fire on the very next frame before the player even sees Race.
        if (Main::isInitialised())
        {
            MWBase::Environment::get().getWindowManager()->setCharGenCompleteCallback(
                []() {
                    if (Main::isInitialised())
                    {
                        Log(Debug::Info) << "[MP] Chargen dialogs complete — arming watcher";
                        Main::get().startWatchingCharGen();
                    }
                    // Re-enable all GUI windows (inventory, map, etc.).
                    // setNewGame(true) called disallowAll(); in vanilla the chargen
                    // script calls setNewGame(false) once sCharGenState==-1, but we
                    // bypass that script, so we must do it here ourselves.
                    MWBase::Environment::get().getWindowManager()->setNewGame(false);
                });
        }

        // Open the Race dialog — first step of chargen.
        MWBase::Environment::get().getWindowManager()->pushGuiMode(MWGui::GM_Race);
    }
    else
    {
        // Returning player — bypass chargen, set player name from login,
        // then teleport to the server-supplied cell (or "toddtest" as default).
        Log(Debug::Info) << "[MP] CharacterSelectDialog: returning player — bypass chargen";

        // bypass=true: skips intro video + chargen, sets sCharGenState=-1.
        MWBase::Environment::get().getStateManager()->newGame(true);

        // Refresh the inventory window's player Ptr to point at the NEW game's player.
        // Without this, InventoryPreview::mPtr still references the previous session's
        // player NPC (which may be a completely different race), causing the paper doll
        // to show the wrong head/hands after rebuildAvatar(). updatePlayer() calls
        // mPreview->updatePtr() + rebuild() internally, clearing the stale reference.
        MWBase::Environment::get().getWindowManager()->updatePlayer();

        // Set the player's display name to the login username.
        if (!loginName.empty())
            MWBase::Environment::get().getMechanicsManager()->setPlayerName(loginName);

        // Restore race/class/birthsign saved from the player's first chargen session.
        // The server sent these back in the HandshakeResponse; Main stores them.
        if (Main::isInitialised())
        {
            auto mm = MWBase::Environment::get().getMechanicsManager();
            const std::string race   = Main::get().getRestoredRace();
            const std::string head   = Main::get().getRestoredHeadMesh();
            const std::string hair   = Main::get().getRestoredHairMesh();
            const bool        isMale = Main::get().getRestoredIsMale();
            const std::string clsName = Main::get().getRestoredClassName();
            const std::string birth   = Main::get().getRestoredBirthSign();

            try
            {
                if (!race.empty())
                {
                    Log(Debug::Info) << "[MP] Restoring race=" << race
                                     << " head=" << head << " hair=" << hair
                                     << " male=" << isMale;
                    mm->setPlayerRace(ESM::RefId::deserializeText(race), isMale,
                                      ESM::RefId::deserializeText(head),
                                      ESM::RefId::deserializeText(hair));
                    // Rebuild the inventory paper doll — setPlayerRace updates the
                    // in-world NPC but the inventory avatar is a separate render scene
                    // that only refreshes when explicitly told to, same as chargen does.
                    MWBase::Environment::get().getWindowManager()
                        ->getInventoryWindow()->rebuildAvatar();
                }
                // Restore class using the full CLDTstruct decoded from the server
                // (already decoded into localPlayer().charClass.mData by Main.cpp).
                // Use setPlayerClass(const ESM::Class&) so custom/generated classes
                // get inserted into the dynamic store and are fully functional.
                if (!clsName.empty())
                {
                    Log(Debug::Info) << "[MP] Restoring class=" << clsName;
                    ESM::Class cls;
                    cls.mName        = clsName;
                    cls.mData        = Main::get().getPlayerSync().localPlayer().charClass.mData;
                    cls.mRecordFlags = 0;
                    mm->setPlayerClass(cls);
                }
                if (!birth.empty())
                {
                    Log(Debug::Info) << "[MP] Restoring birthsign=" << birth;
                    mm->setPlayerBirthsign(ESM::RefId::deserializeText(birth));
                }
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "[MP] Chargen restore error: " << e.what();
            }
        }

        // Teleport to the saved/spawn cell at the saved XYZ + rotation.
        const std::string targetCell = spawnCell.empty() ? "toddtest" : spawnCell;

        // Stash saved coords BEFORE any world lookup — both findInteriorPosition and
        // findExteriorPosition zero out the entire ESM::Position at entry, so we must
        // not pass our saved values into them.
        const float sx = Main::isInitialised() ? Main::get().getSpawnX()    : 0.f;
        const float sy = Main::isInitialised() ? Main::get().getSpawnY()    : 0.f;
        const float sz = Main::isInitialised() ? Main::get().getSpawnZ()    : 0.f;
        const float rx = Main::isInitialised() ? Main::get().getSpawnRotX() : 0.f;
        const float ry = Main::isInitialised() ? Main::get().getSpawnRotY() : 0.f;
        const float rz = Main::isInitialised() ? Main::get().getSpawnRotZ() : 0.f;
        // Non-zero saved position means we have real data to restore.
        const bool hasSavedPos = (sx != 0.f || sy != 0.f || sz != 0.f);

        MWBase::World* world = MWBase::Environment::get().getWorld();

        // findInteriorPosition / findExteriorPosition fill 'dest' with the cell's
        // default COC spawn point — we overwrite with saved coords afterwards.
        ESM::Position dest{};

        const auto interiorId = world->findInteriorPosition(targetCell, dest);
        if (!interiorId.empty())
        {
            if (hasSavedPos)
            {
                dest.pos[0] = sx; dest.pos[1] = sy; dest.pos[2] = sz;
                dest.rot[0] = rx; dest.rot[1] = ry; dest.rot[2] = rz;
            }
            Log(Debug::Info) << "[MP] Teleporting to interior: " << targetCell
                             << " pos=(" << dest.pos[0] << "," << dest.pos[1] << "," << dest.pos[2]
                             << ") rot=" << dest.rot[2];
            world->changeToCell(interiorId, dest, true);
        }
        else
        {
            const auto exteriorId = world->findExteriorPosition(targetCell, dest);
            if (!exteriorId.empty())
            {
                if (hasSavedPos)
                {
                    dest.pos[0] = sx; dest.pos[1] = sy; dest.pos[2] = sz;
                    dest.rot[0] = rx; dest.rot[1] = ry; dest.rot[2] = rz;
                }
                Log(Debug::Info) << "[MP] Teleporting to exterior: " << targetCell
                                 << " pos=(" << dest.pos[0] << "," << dest.pos[1] << "," << dest.pos[2]
                                 << ") rot=" << dest.rot[2];
                world->changeToCell(exteriorId, dest, true);
            }
            else
            {
                Log(Debug::Warning) << "[MP] Cell '" << targetCell << "' not found, trying interior fallback";
                world->changeToInteriorCell(targetCell, dest, true);
            }
        }
    }
}

void CharacterSelectDialog::onCancelClicked(MyGUI::Widget* /*sender*/)
{
    setVisible(false);
    if (Main::isInitialised())
        Main::destroy();
}

} // namespace mwmp
