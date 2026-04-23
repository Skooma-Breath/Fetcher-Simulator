# Admin UI System

## Purpose

The admin UI is the first in-game graphical control surface for multiplayer server scripts.

It exists to replace command memorization for the common admin workflows:

- browsing the shared slash-command catalog
- browsing the live dynamic-record catalog
- creating simple custom records
- deleting unlinked records
- running recordstore metadata sync and generated-record GC

This is intentionally the first slice, not the full server-management UI.

## Architecture

### Server Side

The server entry point remains `core.lua`.

This slice adds two supporting modules:

- `apps/openmw-server/scripts/command_registry.lua`
- `apps/openmw-server/scripts/admin_ui_service.lua`

`command_registry.lua` is the shared source of truth for:

- `/help` chat output
- the command/help data shown in the GUI

`admin_ui_service.lua` is the server RPC/service layer for the GUI. It currently serves:

- initial snapshots for `/helpmenu`
- dynamic-record browsing data
- simple record creation requests
- record deletion requests
- recordstore sync requests
- generated-record GC requests

The GUI uses the already-existing multiplayer Lua event bridge:

- client -> server: `mp.sendToServer("AdminUi_Request", payload)`
- server -> client:
  - `AdminUi_Open`
  - `AdminUi_Snapshot`
  - `AdminUi_Toast`

The server remains authoritative. The UI does not talk to SQLite directly and does not mutate state outside the existing server APIs.

### Client Side

The client test package now includes:

- `mp-clients/lua-test-data/scripts/mp_admin_ui/global.lua`
- `mp-clients/lua-test-data/scripts/mp_admin_ui/player.lua`

The global script is only a bridge:

- receives multiplayer Lua events from the server
- forwards them to the local player script with `player:sendEvent(...)`

The player script owns the UI:

- creates the custom interactive layer `MpAdminUi`
- opens `Interface` mode with built-in windows hidden
- renders the window with `openmw.ui`
- sends GUI actions back to the server

This keeps the UI implementation in standard OpenMW client Lua instead of reintroducing TES3MP-style limited modal dialogs.

## Current Scope

`/helpmenu` currently opens a two-tab window.

Opening the menu itself no longer requires admin login, but record mutation actions still do.

### Commands Tab

Shows:

- categorized slash commands from the shared command registry, filtered to the requesting player's current admin visibility
- current player list
- dynamic-record counts by type

The intent is to provide a real GUI equivalent of `/help` instead of another chat dump.

### Records Tab

Shows:

- live dynamic-record rows from the authoritative server catalog
- local paging and type filtering
- create form for schema-driven record creation
- delete button for unlinked records
- buttons for:
  - refresh snapshot
  - recordstore metadata sync
  - generated-record GC

The create form currently supports:

- base-id clone records for placeable/runtime record types
- blank-default support for `spell` and `enchantment`
- schema-driven advanced fields for:
  - `weapon`
  - `armor`
  - `clothing`
  - `light`
  - `potion`
  - `spell`
  - `enchantment`
- generated/permanent scope
- session/persistent lifetime

## Relationship To RecordDynamic

The admin UI is a frontend over the existing `RecordDynamic` / `recordstore` system.

It does not add a second custom-record implementation.

The GUI create/delete/sync/GC actions all route through:

- `recordstore.ensure(...)`
- `recordstore.remove(...)`
- `recordstore.syncState()`
- `recordstore.gcGeneratedUnlinked(...)`

That means chat tools and GUI tools operate on the same authoritative state and should remain interchangeable.

## Current Limitations

This slice is intentionally constrained.

Not implemented yet:

- actor spawn/despawn UI
- server-memory inspection beyond the current player/record snapshot
- a web write/admin frontend beyond the current read-only database browser

There is also no special container/object delete helper in the UI yet, so container-linked cleanup still follows the same server limitations documented in `recorddynamic-system.md`.

## Database Browser Backend

The server now has a read-only database browsing backend intended for both the future in-game database tab and the future web browser.

Current backend surface:

- `mp.listDatabaseTables()` returns the approved table catalog plus row counts
- `mp.browseDatabaseTable(tableName, offset, limit)` returns a paged row slice for one approved table
- `admin_ui_service.lua` includes the table catalog in the normal snapshot and supports a `database` / `database_browse` action that returns one page

Current in-game frontend coverage:

- a `Database` tab in `/helpmenu`
- read-only table selection and paged row browsing
- no direct SQL entry and no write path

This keeps database access explicit and server-owned instead of encouraging arbitrary SQL or direct SQLite writes from tools.

Current web frontend coverage:

- a loopback-only HTTP listener enabled by `Config.ADMIN_HTTP_ENABLED`
- host/port configured by `Config.ADMIN_HTTP_HOST` / `Config.ADMIN_HTTP_PORT`
- host is forced to loopback (`127.0.0.1` or `localhost`) even if misconfigured
- browser entry point at `/admin/`
- JSON endpoints:
  - `/api/admin/health`
  - `/api/admin/snapshot`
  - `/api/admin/database/tables`
  - `/api/admin/database/page?table=...&offset=...&limit=...`
- current scope is intentionally read-only and focused on the approved database browser
- `/admin/help` explains the database purpose, stored data, technical flow, and planned live-edit model
- character account ownership is displayed and filtered by account username where possible, while the underlying database still stores the stable numeric account id
- known ID cells are clickable for read-only navigation between related tables, including character inventory/equipment rows and dynamic-record link ownership

The HTTP layer reuses the same Lua-side admin service shaping used by the in-game help menu instead of inventing a second database access path.

## Recommended Next Steps

The next practical order is:

1. refine the advanced record editor with richer effect editing and type-specific validation feedback
2. add a live state tab for players, loaded cells, placed objects, and actor authority
3. extend the `Database` tab with better row formatting and record-linked navigation
4. add admin actions for placed-object cleanup and future actor spawn/despawn
5. extend the loopback HTTP API beyond read-only browsing with explicit auth and mutation endpoints when we are ready to expose record actions in the browser

## Testing

Use this minimal validation pass after restarting the server and client:

1. `/helpmenu`
2. confirm the Commands tab loads categories and summaries
3. `/login <admin password>` if you want to test record creation, deletion, sync, or GC
4. switch to the Records tab
5. create a generated/session weapon
6. set one or two advanced fields such as `value`, `health`, or `isSilver`
7. confirm it appears in the list immediately
8. delete an unlinked record from the list
9. create another record, place it with `/placeat`, then confirm Delete is disabled once `links > 0`
10. run Sync Meta and GC Unlinked from the UI and confirm the toast/status footer updates

If the UI opens but does not populate, check:

- `openmw-server.log`
- client `openmw.log`
- whether the client actually loaded `mp_phase7_test.omwscripts`
