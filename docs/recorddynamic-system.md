# RecordDynamic System

## Purpose

`RecordDynamic` is the multiplayer path for server-authored custom records that do not exist in the base content files.

The current implementation is intended to support:

- Server-created custom records referenced by placed objects and spawned actors
- Stable explicit record IDs shared across all clients
- Persistence of shared records across server restarts
- A migration path toward TES3MP-style generated vs permanent record handling

## Current Architecture

### Transport

- Packet type: `RecordDynamic`
- Wire format: `recordType`, `action`, and a batch of `(recordId, payload)` entries
- Payload encoding: serialized plain Lua tables via `LuaUtil::serialize`

Relevant files:

- `components/openmw-mp/Packets/Worldstate/PacketRecordDynamic.hpp`
- `components/openmw-mp/Base/DynamicRecord.hpp`
- `apps/openmw/mwmp/Main.cpp`
- `apps/openmw/mwmp/sync/WorldStateSync.cpp`

### Server Runtime

The server keeps an in-memory registry of dynamic records in `MPServer::WorldState::dynamicRecords`.

Each record currently tracks:

- `recordType`
- `recordId`
- serialized `data`
- `recordScope` (`generated` or `permanent`)
- `persistent`
- `sequence`

Behavior:

- Upserts are authoritative on the server
- All connected clients receive live upsert/remove packets
- New joiners receive the current dynamic-record snapshot before cell state

Relevant files:

- `apps/openmw-server/Server.hpp`
- `apps/openmw-server/Server.cpp`
- `apps/openmw-server/LuaServerContext.cpp`

### Client Runtime

The client applies incoming records into `ESMStore` using `overrideRecord(...)`.

The apply path:

1. Decode `RecordDynamic`
2. Deserialize the Lua table payload
3. Convert the table into the correct ESM record type
4. Force `mId = recordId`
5. Insert or override in `ESMStore`

The client retries failed applications in `WorldStateSync` so records can survive join-time ordering issues, such as waiting for the world to be ready or for a `baseId` dependency to exist.

Dynamic record IDs are inserted with `ESM::RefId::stringRefId(...)` so they use the same identity path as `/placeat` and other object-placement code.

### Parser Support

The record parsers now support `baseId` in addition to the existing `template` userdata path.

That means server Lua can send plain tables like:

```lua
mp.upsertDynamicRecord("weapon", "$custom_weapon_1", {
  baseId = "iron shortsword",
  name = "Arena Test Sword",
  value = 250
})
```

Supported record types in the current system:

- `activator`
- `armor`
- `book`
- `clothing`
- `container`
- `creature`
- `door`
- `enchantment`
- `light`
- `miscellaneous` and `misc`
- `npc`
- `potion`
- `spell`
- `static`
- `weapon`

Not implemented yet:

- other magic-heavy record families

`potion` records still carry their effect list inline. They do not depend on separate `spell` records. The real shared-record dependency edges are:

- enchantable items using custom `enchantment` records
- actors or future spellbook/inventory sync using custom `spell` records

## Persistence Model

### Storage Decision

Dynamic records are stored in the existing server SQLite database, not a second database.

This is the preferred design for the current system because:

- Dynamic records are part of the same authoritative world state as placed objects, doors, and containers
- A single database keeps backup, deployment, and migration simpler
- Cross-system operations stay local to one persistence layer
- The expected data volume is still small enough that a second database would add more operational complexity than benefit

A separate database would only make sense later if:

- record churn becomes very high
- we want different retention or compaction rules
- we need independent replication or tooling for dynamic content

### Schema

Persistent records live in:

- `world_dynamic_records(record_type, record_id, record_scope, record_data, created_at, updated_at)`
- `world_dynamic_record_catalog(record_type, record_id, record_scope, is_persistent, created_at, updated_at)`
- `world_dynamic_record_links(record_id, link_kind, owner_a, owner_b, owner_c, owner_index)`

`world_dynamic_records` stores only persistent record payloads.

`world_dynamic_record_catalog` stores lifetime metadata for both persistent and session-only custom IDs.

`world_dynamic_record_links` stores persisted references to record IDs from:

- placed objects
- container parents
- container items
- door state
- saved player inventory
- saved player equipment
- dynamic records that depend on other dynamic records

Relevant files:

- `apps/openmw-server/PlayerDatabase.hpp`
- `apps/openmw-server/PlayerDatabase.cpp`

### Load and Save Rules

- Persistent records are loaded during server startup
- Persistent records are replayed to joiners on character select
- Upserting a persistent record writes or updates the DB row
- Upserting a non-persistent record removes any previously persisted row for that `(type, id)`
- Removing a record deletes it from memory and from the DB

### Dangling Reference Cleanup

Dynamic records and the things that reference them do not currently share one transactional persistence model, so cleanup matters.

What is implemented now:

- Session-only dynamic records are not loaded from SQLite on server restart
- On startup, the server loads `world_dynamic_record_catalog`, finds explicit session-only record IDs, and scrubs references to those IDs from persisted world/player state
- That startup scrub removes dangling references from:
  - placed world objects
  - server-authoritative container parents and container items
  - door-state rows
  - saved player inventory
  - saved player equipment
- Startup cleanup also removes the stale session-only catalog rows and any stale persisted link rows for those IDs
- On live `mp.removeDynamicRecord(...)`, the server also scrubs exact matching references immediately from world state and connected players before broadcasting the record removal
- On live record removal, the server also clears outgoing `record_dependency` links owned by that record
- Cleanup saves repaired inventory/equipment snapshots back to SQLite without mutating character `last_seen`, so the repair pass does not look like fresh player activity

What this means in practice:

- Session-only records now clean up by explicit lifetime metadata, not by generated-prefix heuristics
- Generated persistent records are no longer treated as restart cleanup candidates just because they use `$custom_*`
- Session-generated records such as `$custom_weapon_1` still do not leave stale placed objects or saved inventory/equipment behind after a restart
- Permanent or otherwise explicit record IDs are also cleaned up correctly when they are removed through the server API during runtime

Current limitation:

- The link table currently tracks persisted references, but there is not yet a full higher-level GC policy deciding when an unreferenced generated record should be hard-deleted automatically
- Spawned actor links are runtime-only and are cleared on server restart because spawned actors are not yet persisted as world state

## Lua API

### ID Generation

Config:

- `Config.GENERATED_RECORD_ID_PREFIX`
- default: `"$custom"`

API:

```lua
local id = mp.generateDynamicRecordId("weapon")
```

Generated IDs follow:

```text
$custom_<recordType>_<number>
```

Current generated-number state is rebuilt from persisted generated records at startup and kept ahead during runtime.

### Upsert and Remove

```lua
mp.upsertDynamicRecord(recordType, recordId, data [, options])
mp.removeDynamicRecord(recordType, recordId)
```

`options` currently supports:

- `scope = "generated" | "permanent"`
- `persistent = true | false`

If `scope` is omitted, it is inferred from the generated-record prefix when possible; otherwise it defaults to `permanent`.

### Generated Convenience Helper

```lua
local id = mp.upsertGeneratedRecord(recordType, data [, options])
```

This:

1. allocates a generated ID
2. queues the upsert
3. returns the new ID

### Catalog Queries

The server Lua package now exposes the dynamic-record catalog directly:

```lua
local allRecords = mp.listDynamicRecords()
local info = mp.getDynamicRecordInfo("weapon", "$custom_weapon_1")
```

Each catalog entry currently includes:

- `recordType`
- `recordId`
- `scope`
- `persistent`
- `createdAt`
- `updatedAt`
- `linkCount`
- `loaded`

`linkCount` comes from the persisted link table, so Lua can make decisions against actual server-owned references instead of guessing from chat state.

## Lua Recordstore Layer

There is now a dedicated Lua wrapper module at:

- `apps/openmw-server/scripts/recordstore.lua`

This is the first real OpenMW-side equivalent to TES3MP's recordstore layer.

Current responsibilities:

- normalize create/reuse/remove flows for custom records
- keep lightweight dedupe metadata in Lua global storage
- reuse matching records by exact payload fingerprint when possible
- query the authoritative server catalog before reusing or removing anything
- expose manual GC for unlinked generated records
- expose admin/debug commands for listing and inspecting records
- synchronize dynamic record dependency links for known fields such as `enchant`

### Recordstore Helpers

The main helpers are:

```lua
local recordStore = require("recordstore")

local stored = recordStore.ensure("weapon", {
  baseId = "iron shortsword",
  name = "Arena Test Sword",
}, {
  scope = "generated",
  persistent = true,
})

local info = recordStore.getInfo("weapon", stored.recordId)
local removed, reason = recordStore.remove("weapon", stored.recordId, { force = true })
local gcResults = recordStore.gcGeneratedUnlinked()
```

Important behavior:

- `ensure(...)` will reuse an existing record only if:
  - the payload fingerprint matches exactly
  - the record type matches
  - `scope` matches
  - `persistent` matches
  - the record still exists in the authoritative server catalog
- generated records still get server-assigned IDs
- permanent records default to a deterministic `recordstore_<type>_<hash>` id when no explicit id is provided

### Recordstore Commands

Admin-only chat commands:

- `/recordstore list [type|all]`
- `/recordstore info <type> <recordId>`
- `/recordstore info [type|all]`
- `/recordstore sync`
- `/recordstore gc [session|persistent|all] [type|all]`

`/recordstore gc` currently removes only generated records with `linkCount == 0`.
This is an explicit/manual GC step for now, not a fully automatic policy.

Implementation note:

- the recordstore list/sync/gc path now flattens the authoritative server catalog into a normal Lua array before iterating it
- this avoids the earlier bug where `mp.listDynamicRecords()` was exposed as a read-only userdata wrapper and Lua-side `ipairs`/`#` checks treated the catalog as empty
- manual GC now re-resolves each candidate through `getDynamicRecordInfo(...)` before removing it
- this avoids trusting potentially stale per-entry fields from the list response during the same session as an object delete
- manual GC now also enumerates recordstore-owned ids from Lua metadata first, then de-duplicates against the live catalog
- this makes GC robust even if the list binding shape changes again, as long as `getDynamicRecordInfo(...)` still resolves the record id
- the preferred GC path is now a direct server binding that filters the authoritative dynamic-record catalog by `scope == generated` and `linkCount == 0`, then queues removals from that server-side candidate set
- if that authoritative path returns zero candidates, `recordstore.lua` now falls back to the Lua-side metadata/catalog sweep instead of exiting early
- `/recordstore gc all` now normalizes the `all` persistence filter to `nil` before candidate filtering; previously the literal string `"all"` caused every candidate to be rejected during the Lua fallback pass

## Server Test Harness

There is now a dedicated server Lua test module at:

- `apps/openmw-server/scripts/recorddynamic_test.lua`

This is intentionally narrower than TES3MP's `/storerecord` plus `/createrecord` workflow in `CoreScripts/scripts/commandHandler.lua`.
The current OpenMW path is optimized around:

- cloning from a known `baseId`
- choosing `generated` vs `permanent`
- choosing `session` vs `persistent`
- routing creation through the Lua recordstore layer
- quickly creating records from in-game chat for sync testing

### Commands

Admin-only slash commands:

- `/recordtest types`
- `/recordtest info <type|all>`
- `/recordtest create <type|all> [generated|permanent] [persistent|session] [baseId|"base id"]`
- `/recordtest enchant <weapon|armor|book|clothing> [generated|permanent] [persistent|session] [itemBaseId|"item base"] [enchantBaseId|"enchant base"]`
- `/recordtest remove <type|all>`

Defaults:

- scope defaults to `generated`
- lifetime defaults to `session`

That default is deliberate so test records do not accumulate in the SQLite DB unless requested.

`/recordtest enchant ...` creates the enchantment first and then the owning item record, which keeps initial client replay ordering safe for that pair and immediately pins the enchantment through a `record_dependency` link.

### Supported Test Types

The test harness currently covers the same record families supported by `RecordDynamic`:

- `activator`
- `armor`
- `book`
- `clothing`
- `container`
- `creature`
- `door`
- `light`
- `miscellaneous`
- `npc`
- `potion`
- `static`
- `weapon`

The script ships with vanilla-oriented default `baseId` values for each type. The current defaults were validated against stock `Morrowind.esm`, including:

- `activator`: `active_de_p_bed_28`
- `book`: `bk_guide_to_vvardenfell`
- `light`: `light_com_torch_01`
- `potion`: `p_restore_health_s`
- `weapon`: `iron shortsword`

If a specific content setup does not have one of those record IDs, override it directly in chat by quoting the `baseId` when needed.

### Test-State Tracking

The test harness stores generated test IDs in Lua global storage:

- section: `RecordDynamicTest`
- key: `generatedIds`

Behavior:

- generated `persistent` test IDs are remembered across server restarts
- generated `session` test IDs are cleared from tracking on server init
- permanent test IDs are deterministic (`recordtest_<type>`) and do not need tracking

This gives the chat commands enough memory to clean up previously-created generated test records without adding another SQL table.

## Generated vs Permanent

The current implementation now goes beyond the original TES3MP-style split and has a stronger explicit lifetime model.

What exists now:

- explicit `generated` vs `permanent` scope
- generated ID allocation
- persistent storage for either scope
- scope replay to the server runtime
- explicit session-only lifetime metadata in the record catalog
- persisted link metadata for placed objects, containers, door state, inventory, equipment, and dynamic-record dependencies
- startup cleanup for session-only record IDs
- live cleanup when an existing dynamic record is explicitly removed
- dependency tracking between dynamic records through persisted `record_dependency` links
- a Lua recordstore wrapper for dedupe/reuse/remove flows
- admin/debug commands for listing, inspecting, syncing, and manually GC'ing records
- authoritative server-side GC filtering for generated records with `linkCount == 0`

What is still missing for the full intended lifecycle:

- automatic generated-record garbage collection when links disappear during normal gameplay
- actor/runtime link tracking for spawned NPCs and creatures
- richer dependency discovery beyond currently known fields such as `enchant`
- validation rules for parent/child lifetime mismatches
- actor spawning flows that consume synced dynamic actor records directly

Compared to TES3MP:

- TES3MP provides dynamic-record transport and record authoring helpers, but it does not provide this same explicit persisted catalog/link-count model
- OpenMW should therefore keep the stronger catalog/link policy instead of trying to preserve TES3MP's looser lifecycle behavior

## Relationship to `/placeat` and `/spawnat`

`/placeat` can now rely on shared dynamic records because clients receive the custom record definition before placed-object replay.

`/spawnat` is now the live actor-spawn path for synced static and dynamic actor records.

Current flow:

1. Create or reuse a dynamic `npc` or `creature` record
2. Sync that record through `RecordDynamic`
3. Spawn the actor instance using server authority
4. Insert the actor into the authoritative server actor registry and broadcast it through `ActorList`
5. Route non-authority hits through `ActorCombatRequest` using the exact spawned-actor `mpNum`
6. Keep runtime link tracking so the actor instance keeps its generated dependencies alive until despawn, removal, or restart

Current corpse behavior for spawned actors:

- dead spawned actors can be activated by non-authority players
- first activate opens the corpse container, even when it is empty
- corpse inventory snapshots and container deltas flow through the server-owned container sync path
- `Dispose of Corpse` sends an explicit multiplayer dispose request instead of relying on local object deletion side effects
- the server removes the corpse authoritatively only after the corpse container is empty

This avoids relying on client-local `world.createRecord` IDs, which are not multiplayer-safe.

## Persistence Policy Decision

Chosen policy:

1. Generated records should remain alive only while they still have links
2. Persistent generated records may survive a restart, but they are not immortal; once their authoritative link count reaches zero, they become GC candidates
3. Permanent records are script-owned/shared records and should not be auto-removed just because they are currently unlinked
4. Manual record removal should soft-fail when links still exist, and require an explicit `force` flag to proceed
5. Forced removal should continue to scrub exact matching references from persisted world/player state before broadcasting the record removal
6. Future cell-reset logic should participate in unlinking generated placed objects by removing the same persisted/world links that normal object deletion removes; once the last link disappears, normal generated-record GC rules should apply

Why this is the most appropriate policy:

- it matches the current catalog/link-count architecture already implemented in the server
- it avoids leaking generated persistent records forever just because they were once saved
- it keeps permanent records available for hand-authored shared content
- it preserves a safe admin workflow by making destructive removal explicit when links still exist
- it composes cleanly with future actor spawning and cell reset work

Operational interpretation:

- `generated + linkCount > 0` means keep the record
- `generated + linkCount == 0` means the record is eligible for GC
- `permanent + linkCount == 0` means keep the record unless explicitly removed
- `force` removal is an override for admin/debug cleanup, not the normal lifecycle

## Next Steps

Recommended next slices, updated to reflect current progress:

1. Automate generated-record GC from existing link updates
   - status: partial
   - link rows already update for placed objects, containers, door state, inventory, equipment, and record dependencies
   - manual `/recordstore gc` already removes generated records with `linkCount == 0`
   - implemented in this session:
     - server-side automatic generated-record GC candidate collection based on the authoritative catalog
     - automatic GC passes after trusted unlink-producing paths:
       - object delete
       - player inventory updates
       - player equipment updates
       - container set/delta persistence updates
       - authoritative placed-object removal
       - authoritative inventory grants
     - warning logs when a generated persistent record becomes unlinked and is about to be auto-GC'd
     - shared server-side GC candidate filtering reused by both the Lua-facing GC path and the new automatic GC path
   - remaining work:
     - verify whether every current container path is truly safe for automatic unlink/GC in live multiplayer sessions
     - add future despawn and actor-runtime unlink hooks
     - decide whether any additional rate-limiting or batching is needed if many records become unlinked at once

2. Extend dependency tracking beyond current `enchant` support
   - status: partial
   - implemented in this session:
     - `recordstore.ensure(...)` now discovers dependency ids from:
       - `data.enchant`
       - `data.enchantmentId`
       - explicit `options.dependencies`
       - `data.spell`
       - `data.spellId`
       - `data.trap`
       - `data.trapSpell`
       - `data.spells`
     - persisted `record_dependency` links continue to be stored server-side
     - lifetime validation now rejects:
       - permanent records depending on generated records
       - persistent records depending on session-only records
       - missing dependency ids that are not present in the authoritative catalog
   - remaining work:
     - support future actor inventory/spell dependency extraction
     - decide whether dependency validation should also enforce stricter type compatibility rules

3. Add richer magic-record authoring helpers
   - status: partial
   - implemented in this session:
     - plain server Lua helpers now build non-empty `effects` tables from named presets
     - `/recordtest spell ...` creates custom spell records with effect payloads
     - `/recordtest trap <book|spell> ...` creates trap/spell-carrier test records backed by generated spell dependencies
   - current presets:
     - `firebite`
     - `hearth`
     - `spark`
   - remaining work:
     - expose a cleaner general-purpose helper path than the current test-harness presets
     - expand effect authoring beyond the current preset-based workflow

4. Extend actor spawning so `/spawnat` can use synced dynamic `npc` and `creature` records directly
   - status: partial
   - implemented in this session:
     - added a new authoritative server/Lua actor spawn path
     - added Lua bindings for spawning and removing authoritative actors
     - added admin `/spawnat <refId> [distance] [direction] [refNum] [mpNum]`
     - spawned actors are inserted into the existing server `actorCells` registry and broadcast through `ActorList`
     - clients now create missing server-spawned actors from `ActorList` when the actor has a server `mpNum`
     - `/spawnat` now server-assigns actor `mpNum` when omitted or passed as `0`
     - dynamic `npc` and `creature` records are re-sent to the cell immediately before the spawn `ActorList`
     - spawned dynamic actors create `spawned_actor` links so generated actor records stay alive until despawn, disappearance from authoritative actor lists, or server restart
     - non-authority combat now identifies spawned actors by exact `mpNum` and no longer relies on coarse `refId` hit replay
     - spawned actor corpse open/loot/dispose flow is now synced across clients through the server container/dispose path
   - current limitation:
     - spawned actors are still runtime-only; they are not persisted across server restart
   - remaining work:
     - add an admin despawn command/UI flow around the existing remove binding
     - persist spawned actors only if/when we decide they should become durable world state

5. Keep the persistence policy aligned with implementation
   - status: decided and partially encoded
   - follow-through work:
     - keep the chosen generated/permanent GC behavior in future actor-runtime GC paths
     - make future cell-reset unlinking reuse the same link-removal semantics as normal deletion
     - keep forced removal as an explicit override only

6. Extend the Lua recordstore to understand cross-record dependencies directly
   - status: partial
   - current support:
     - explicit `options.dependencies`
     - enchantment-backed item dependencies
     - spell/trap-style dependency discovery
     - lifetime validation against the authoritative catalog
   - remaining work:
     - add future actor inventory/spell dependency helpers
     - surface clearer validation errors when a dependency id is missing or incompatible
     - decide whether some dependency extraction should move from Lua into server-side validation

7. Decide which manual admin/debug actions should become automatic
   - status: partial
   - current manual actions:
     - `/recordstore sync`
     - `/recordstore gc`
   - recommendation:
     - keep `/recordstore sync` as a manual repair/debug command
     - keep `/recordstore gc` as a fallback/debug command even though several unlink paths now auto-GC
     - add more warning/observability around suspicious zero-link generated persistent records and future actor unlink paths

## Practical Guidance

For now:

- Use `baseId` whenever possible to clone from an existing static record
- Use `generated` scope for throwaway, derived, or test-created records whose lifetime should follow actual references
- Use `permanent` scope for hand-authored shared records you want to keep even when currently unlinked
- Set `persistent = false` for temporary session-only records
- Use `persistent = true` for any custom record that should survive a server restart because world state or saved player state still references it
- Do not treat `persistent = true` as "keep forever"; for generated records it only means the record and its links may survive restart while references still exist
- Keep custom record IDs stable if objects or future actors will reference them long-term
- Prefer `recordstore.ensure(...)` from Lua instead of calling `mp.upsertDynamicRecord(...)` directly for normal script-owned custom records
- For enchantable item records, set `data.enchant = "<recordId>"`; the recordstore now persists that as a dependency link
- For other cross-record relationships, prefer explicit `options.dependencies = { ... }` until more first-class helpers exist
- Use `/recordstore list` and `/recordstore info ...` when debugging before looking at raw logs
- Use `/recordtest create ...` for chat-driven sync checks instead of hand-editing serialized Lua tables
- Use `/recordtest enchant ...` when you want a generated enchantment-backed item pair without manually stitching the ids together
- If a record creates successfully but `/placeat` does not show anything, check the client log first for `Unknown baseId` or `failed to create manual cell ref` messages
- `setdelete1` is only a reliable GC test for placed refs that actually flow through multiplayer `ObjectDelete`
- placed containers do not currently unlink through that route because local `World::deleteObject()` skips refs with container stores, so a deleted placed container can still keep a `container_parent` link until we add a dedicated admin/server delete path
- if a delete and `/recordstore gc` happen back-to-back in the same session, run `/recordstore info <type> <id>` once before GC when debugging; that confirms whether the server has already observed the unlink
- if a generated persistent record shows `links=0`, treat it as a GC candidate or a warning condition, not as a permanently retained asset

## Admin UI Integration

There is now an in-game GUI entry point at `/helpmenu`.

Opening the menu itself is public; record mutation actions inside it still require admin login.

It is a frontend over the same `recordstore` and `RecordDynamic` APIs documented above, not a separate system.

Current GUI coverage:

- shared command catalog from `command_registry.lua`
- authoritative dynamic-record list
- schema-driven create/delete flows for custom records
- advanced create fields for supported `weapon`, `armor`, `clothing`, `light`, `potion`, `spell`, and `enchantment` payloads
- recordstore metadata sync
- generated-record GC

Relevant files:

- `apps/openmw-server/scripts/admin_ui_service.lua`
- `apps/openmw-server/scripts/command_registry.lua`
- `mp-clients/lua-test-data/scripts/mp_admin_ui/global.lua`
- `mp-clients/lua-test-data/scripts/mp_admin_ui/player.lua`

The deeper admin-UI architecture and the future web-admin direction are tracked in:

- `docs/admin-ui-system.md`

## Next Session Test Checklist

Use this exact order in the next multiplayer test session.

1. Restart the server so the new Lua scripts and `openmw-server.exe` are loaded.
2. Join with a clean client and run `/login <admin password>`.
3. Verify the catalog is initially sane:
   - `/recordstore list`
   - `/recordstore sync`
4. Create test records through the recordstore-backed test harness:
   - `/recordtest create weapon generated session`
   - `/recordtest create potion generated session`
   - `/recordtest create spell generated session`
   - `/recordtest create enchantment generated session`
   - `/recordtest enchant weapon generated session`
   - `/recordtest create misc generated persistent`
   - `/recordtest create all`
5. Inspect the created records:
   - `/recordtest info all`
   - `/recordstore list weapon`
   - `/recordstore info weapon <recordId>`
   - `/recordstore info spell <recordId>`
   - `/recordstore info enchantment <recordId>`
   - after `/recordtest enchant ...`, confirm the enchantment already has a nonzero link count before the item is placed
6. Place the placeable records:
   - `/placeat <recordId>`
   - repeat for at least `weapon`, `potion`, `miscellaneous`, `container`, `door`, and `light`
7. Verify link counts after placement:
   - `/recordstore info weapon <recordId>`
   - link counts should be greater than zero for anything now placed or saved in inventory
   - for enchanted test items, the enchantment should stay linked through the parent item record
8. Verify automatic GC behavior:
   - use `setdelete1` on a placed non-container custom record such as a weapon, door, or light
   - confirm the client log shows `[MP] WorldObjectSync: deleted mpNum=...`
   - confirm the server log shows the unlink path followed by automatic generated-record GC
   - run `/recordstore info <type> <recordId>` and verify the record is already gone, or reached `links=0` immediately before auto-GC removed it
   - confirm that still-linked generated records, especially placed containers, are not removed unexpectedly
9. Verify current manual GC behavior still works as a fallback:
   - create or keep an unlinked generated record that survives long enough to inspect manually
   - run `/recordstore gc`
   - confirm the command still removes any remaining generated records with `linkCount == 0`
10. Verify dependency and magic helper behavior:
   - run `/recordtest spell generated session firebite`
   - run `/recordtest trap book generated session spark`
   - run `/recordtest trap spell generated session hearth`
   - inspect the created records with `/recordtest info all`
   - confirm the carrier record shows a nonzero dependency-driven link count for the generated spell record
   - confirm invalid lifetime combinations are rejected, for example a persistent record depending on a session-only dependency
11. Verify `/spawnat` foundation behavior:
   - create or reuse a dynamic `npc` or `creature` record id if desired
   - run `/spawnat <recordId>` or `/spawnat <baseActorId>`
   - confirm the server logs the authoritative actor spawn
   - confirm clients in the cell receive and visibly create the actor through `ActorList`
   - for dynamic actor records, confirm `world_dynamic_record_links` shows a `spawned_actor` row while the actor exists
   - kill at least one spawned actor from the non-authority client and confirm:
     - one death event is accepted for the killed actor
     - the corpse container opens correctly
     - looting and `Dispose of Corpse` remove the corpse for both clients
12. Verify forced-removal policy:
   - create or keep one generated record that still has links
   - run the normal remove path and confirm it soft-fails while links remain
   - run the forced remove path and confirm the server scrubs matching references before broadcasting removal
13. Restart the server again.
14. Reconnect and verify restart behavior:
   - session-only records should be gone
   - persistent records should still exist
   - placed objects or saved inventory referencing persistent records should still resolve
   - generated persistent records with no surviving links should be treated as GC candidates, not permanent keep-forever records
   - `/recordstore list` should not show the wiped session-only ids
15. If anything fails, collect:
   - `openmw-server.log`
   - client `openmw.log`
   - the output of `/recordstore list`
   - the output of `/recordtest info all`

## Session Summary / Remaining Work

Implemented or effectively present in the current working tree:

- dynamic-record catalog queries from Lua
- persisted link counting for placed objects, containers, door state, inventory, equipment, and record dependencies
- persisted dependency links for dynamic records
- startup cleanup for session-only record ids
- live cleanup when a dynamic record is explicitly removed
- recordstore-backed create/reuse/remove flows
- authoritative/manual GC for generated records with zero links
- automatic generated-record GC after several trusted unlink-producing server paths
- warning logs for generated persistent records that become unlinked and are about to be auto-GC'd
- shared authoritative GC candidate filtering used by both Lua/manual GC and server-side automatic GC
- spell/trap-style dependency discovery in the Lua recordstore layer
- lifetime validation for cross-record dependencies
- richer magic test helpers with non-empty effect presets
- admin/test commands for spell and trap authoring flows
- authoritative `/spawnat` support backed by the server actor registry, client-side actor creation from `ActorList`, dynamic actor record replay, server-assigned actor mpNums, and runtime `spawned_actor` links
- exact spawned-actor combat routing via `ActorCombatRequest` and actor `mpNum`
- synced corpse open/loot/dispose handling for spawned actors, including vanilla-style `Dispose of Corpse` gating on an empty container
- `openmw-server` and `openmw` now build successfully in the existing `MSVC2022_64` build tree for the `RelWithDebInfo` configuration after these changes
- test/admin commands for record creation, inspection, sync, and GC

Still remaining after this session:

1. admin despawn command/UI flow around spawned actor removal
2. future actor inventory/spell dependency extraction and validation
3. validation that container-related automatic GC behavior is safe in all live multiplayer cases
4. any batching/rate-limiting or observability polish needed for larger auto-GC waves
5. deciding whether spawned actors should remain runtime-only or become persisted world state
6. broader cleanup/reset participation for future despawn and cell-reset flows
7. broader full-project build/test coverage beyond the successfully built `openmw-server` and `openmw` targets

Recommended next implementation order:

1. admin despawn command/UI flow
2. future actor inventory/spell dependency helpers and validation
3. cell-reset/runtime unlink integration and auto-GC polish
