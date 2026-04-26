# Exterior Actor Sync Plan

## Problem

Actor sync is currently cell-scoped, but the server treats a player as interested in only one cell: `ConnectedClient::player.cell`.

That works for interiors and for the player's current exterior grid cell. It breaks down for normal exterior play because OpenMW keeps a loaded exterior grid around the player. A client can see and simulate actors in adjacent active exterior cells, but the multiplayer server currently assigns authority, validates updates, sends actor bootstrap, and broadcasts actor packets against only the single current cell.

The result is that exterior actor sync is effectively one-cell wide.

## Current Code Shape

Important current paths:

- `apps/openmw-server/Server.cpp`
  - `cellMatches(...)` compares a packet cell against `client.player.cell`
  - `refreshActorAuthorityForCell(...)` uses `cellMatches(...)` for authority eligibility
  - `validateActorUpdate(...)` rejects actor packets whose `ActorList.cellId` does not match `client.player.cell`
  - `handlePlayerCellChange(...)` refreshes authority for only the old and new current cells, then sends state for only the new current cell
  - `broadcastToCell(...)` is used by actor and non-actor cell-scoped traffic and currently follows current-cell membership
- `apps/openmw/mwmp/sync/ActorSync.cpp`
  - `ActorSync::update(...)` can already iterate multiple `mCells`
  - `sendAuthoritativeActorUpdates(...)` can already find an active cell by id using `WorldScene::getActiveCells()`
  - the missing piece is that the server only creates authority/runtime entries for the one cell it knows the player occupies

## Target Model

Keep these two concepts separate:

- **active cell**: the player's canonical current cell, still sent through `PlayerCellChange`
- **loaded actor cells**: the set of cells the client currently has active and can observe/simulate for actor sync

For interiors, `loaded actor cells` is normally a single-cell set containing the active interior.

For exteriors, `loaded actor cells` should contain the active exterior grid cell plus the loaded neighboring exterior cells reported by the client.

## Recommended Implementation

### 1. Add Explicit Loaded-Cell Reporting

Add a small reliable packet, tentatively `PlayerLoadedCells`, instead of overloading `PlayerCellChange`.

Suggested payload:

- `activeCellId`
- `loadedCellIds[]`
- `sequence`

Client send rules:

- send after character select and after every `PlayerCellChange`
- send when the active exterior grid changes
- send when `WorldScene::getActiveCells()` changes even if the active cell string did not
- coalesce changes so this is a reliable state update, not a per-frame packet

Server fallback:

- if a client has not sent `PlayerLoadedCells`, treat `loaded actor cells = { current player cell }`
- this preserves current behavior for old clients and for initial handshakes

### 2. Track Per-Client Interest

Add to `ConnectedClient`:

```cpp
std::unordered_set<std::string> loadedActorCells;
uint32_t loadedActorCellsSequence = 0;
```

Normalize all cell ids to the existing server format:

- interiors: plain cell name
- exteriors: `EXT:x,y`

Reject empty ids and cap the set size. A sane initial cap is one interior cell or a modest exterior grid such as 25 cells. This prevents a malicious or broken client from claiming authority eligibility over the whole world.

### 3. Split Current-Cell Checks From Loaded-Cell Checks

Keep `cellMatches(...)` for systems that truly mean "player's active cell".

Add a separate helper:

```cpp
bool clientHasActorCellLoaded(const ConnectedClient& client, const std::string& cellId);
```

Use that helper in actor paths:

- `refreshActorAuthorityForCell(...)`
- `validateActorUpdate(...)`
- actor-specific broadcasts
- actor bootstrap sends

Do not blindly replace every `cellMatches(...)` call in the server. Doors, containers, placed objects, and settings may also need loaded-cell treatment later, but this slice should keep the behavior change focused on actors.

### 4. Refresh Authority For Added And Removed Cells

When the server receives a loaded-cell set:

1. compute added cells and removed cells
2. update the client's `loadedActorCells`
3. for each added cell:
   - call `refreshActorAuthorityForCell(cellId, client.guid)`
   - send `ActorAuthority`
   - send `ActorList`
4. for each removed cell:
   - call `refreshActorAuthorityForCell(cellId)`
   - send an authority revocation or rely on the next `ActorAuthority` packet for that cell

When the client changes active cell, continue handling `PlayerCellChange`, but let the loaded-cell update decide the complete exterior actor interest set.

### 5. Adjust Exterior Authority Selection

Current authority selection keeps the existing owner when possible, then uses the preferred entrant, then lowest GUID.

For multi-cell exterior interest, use this order:

1. keep the current authority if that client still has the actor cell loaded
2. prefer a client whose active cell exactly equals the actor cell
3. prefer the client whose active exterior grid is closest to the actor cell
4. prefer the explicit `preferredGuid` if still eligible
5. fall back to lowest GUID

This avoids unnecessary churn while still preferring the client most likely to simulate that cell accurately.

### 6. Keep Client ActorSync Mostly Intact First

`ActorSync::update(...)` already iterates all known actor cells. `sendAuthoritativeActorUpdates(...)` already resolves any active cell by id.

The first client-side slice should therefore be narrow:

- gather active cells from `WorldScene::getActiveCells()`
- serialize their ids using the same exterior `EXT:x,y` convention as `cellIdForPtr(...)`
- send reliable loaded-cell changes
- ensure actor authority/list packets for adjacent exterior cells populate `mCells`

Avoid rewriting playback or interpolation in this slice. Timestamped buffered interpolation remains deferred future work.

### 7. Add Actor-Specific Cell Broadcasts

Because `broadcastToCell(...)` is shared by several systems, introduce actor-focused helpers first:

- `broadcastActorToCell(cellId, payload, exceptConn, reliable)`
- `sendActorStateToInterestedClients(cellId)`

These should use `clientHasActorCellLoaded(...)`.

Later, if exterior placed objects, containers, doors, or settings show the same loaded-grid problem, promote the helper into a general cell-interest system.

## Acceptance Tests

Minimum live tests:

1. One client in an exterior cell receives `ActorAuthority` and `ActorList` for the active cell plus neighboring loaded exterior cells.
2. Spawn or place a creature in an adjacent loaded exterior cell and verify the client sees actor movement/combat sync without stepping into that cell.
3. Two clients in neighboring exterior cells both receive relevant actor packets for overlapping loaded cells.
4. Authority remains stable when both clients can see the same exterior cell.
5. Authority transfers when the current authority unloads the cell or disconnects.
6. Actor updates from a client are accepted for loaded exterior cells and rejected for cells outside its loaded set.
7. Interior behavior remains unchanged: one loaded actor cell, one authority path, no extra exterior-grid assumptions.

## Risks

- A client-reported loaded-cell set is client-provided data. The server should cap set size and still require current authority before accepting actor updates.
- Changing `broadcastToCell(...)` globally would widen unrelated systems. Keep the first slice actor-only.
- Exterior actors near cell boundaries may cross cells. The current actor identity/state path needs a later actor-cell-change slice so actors can move between cell registries cleanly.
- Loaded-cell set churn can create authority churn if the server immediately reacts to every tiny change. Coalesce client sends and preserve current authority whenever it remains eligible.

## Future Work

- Generalize cell interest for placed objects, doors, containers, weather/settings, and admin debug views if testing shows the same exterior-grid limitation there.
- Add actor cell-change handling for actors that physically cross exterior grid boundaries.
- Add timestamped buffered interpolation for dedicated actors after the loaded-grid authority model is stable.
- Add an admin/debug view showing each player's active cell, loaded actor cells, and current actor authority per cell.
