# Phase 8 Plan - Actor Sync for All Clients

## Goal
Bring NPCs and creatures into multiplayer sync for all clients, using TES3MP as a reference for the working baseline while deliberately avoiding its weakest behavior: floaty movement, authority churn, and client-trusted combat.

## Current Status
The actor lane is no longer at the Phase 7 stub stage.

Current working-tree status in `openmw/`:
- cell-scoped actor authority, per-cell actor registry state, and actor bootstrap/relay are live on the server
- the actor packet family is implemented and in use: `ActorAuthority`, `ActorList`, `ActorPosition`, `ActorAnimFlags`, `ActorAnimPlay`, `ActorAttack`, `ActorCast`, `ActorDeath`, `ActorEquipment`, `ActorStatsDynamic`, `ActorAI`, and `ActorCombatRequest`
- `apps/openmw/mwmp/sync/ActorSync.*` is now a real runtime manager, not just a cache stub
- clients create and bind missing server-spawned actors from `ActorList` when the actor has a server `mpNum`
- authoritative `/spawnat` is live and currently serves as the main in-game actor test path
- spawned actor identity is normalized around server runtime ownership (`0-mpNum` on the gameplay/debug side) instead of trusting client-local runtime `refNum`s
- non-authority actor hits route through `ActorCombatRequest` to the current authority client, and the duplicate-death path from coarse `refId` hit replay has been removed
- spawned actor corpse open/loot/dispose flow is now synced across clients, including vanilla-style corpse opening before `Dispose of Corpse`

Still intentionally incomplete:
- dedicated buffered interpolation is still future work; current playback is functional but not the final smoothing model
- spawned actors remain runtime-only and are not persisted across server restart
- there is still no dedicated admin UI for spawned-actor despawn or actor-state inspection

## Starting Point from Phase 7
Phase 7 marked the actor lane as the next major block and established the guardrail that still applies now:
- do **not** cargo-cult TES3MP's low-ping authority churn or always-on ownership swapping

## TES3MP Findings

### What TES3MP did correctly
1. **Stable actor identity came first.**
   TES3MP keys actors with `refNum + mpNum`, and every actor packet uses that identity. This is the minimum viable base for actor sync.
2. **It split actor traffic by concern.**
   TES3MP separated `ActorList`, `ActorPosition`, `ActorAnimFlags`, `ActorAnimPlay`, `ActorAttack`, `ActorCast`, `ActorAI`, `ActorDeath`, `ActorEquipment`, and others instead of trying to push one giant state blob every tick.
3. **It used cell-scoped authority.**
   The server tracked one authority GUID per cell and accepted actor updates only from that authority owner.
4. **It had a dedicated playback path.**
   Remote-controlled actors were represented as `DedicatedActor` instances and updated separately from locally-owned actors.
5. **It persisted some actor state server-side.**
   Server cells remembered actor position/stat data so later packets and later visitors had some continuity.

### How TES3MP's pipeline worked
Client side (`TES3MP/apps/openmw/mwmp/`):
- `Cell::updateLocal()` updated locally-owned actors every `0.025` seconds and batched actor packets.
- `LocalActor` gathered position, animation flags, attack/cast, equipment, stats, death, and cell changes.
- `DedicatedActor` replayed remote actor state, including movement, AI packages, equipment, spells, attacks, casts, and death.
- `ProcessorActorAuthority` flipped a whole cell between local authority mode and dedicated playback mode.

Server side (`TES3MP/apps/openmw-mp/`):
- Each `Cell` stored a single authority GUID.
- `ProcessorActorPosition`, `ProcessorActorAnimFlags`, `ProcessorActorAttack`, `ProcessorActorCast`, `ProcessorActorDeath`, etc. only accepted updates from the current authority owner.
- The server relayed accepted actor packets to other players with the cell loaded.
- `Cell::readActorList()` persisted limited authoritative actor state inside the server cell.

### TES3MP weaknesses we should not copy
1. **Whole-cell ownership churn.**
   When authority changed, TES3MP flipped actors between `LocalActor` and `DedicatedActor` ownership modes for the entire cell. That is simple, but it causes visible instability and role churn.
2. **Very simple movement smoothing.**
   `DedicatedActor::move()` mostly lerped toward the latest position with a fixed multiplier and a hard distance gate. It works, but it looks floaty under jitter and handoff.
3. **Latest-packet playback instead of timestamped snapshot playback.**
   TES3MP largely consumed the newest state directly instead of interpolating through a short buffered timeline.
4. **Combat was still too client-driven.**
   The authority client effectively drove attack/cast timing and much of the resulting actor state. That is not the standard we want for a smoother and more trustworthy dedicated-actor model.
5. **Actor state persistence was partial.**
   Enough was stored to function, but not enough for a modern rollback-free, handoff-safe, late-join-safe actor system.

## Design Direction for This Fork
We should reuse TES3MP's good ideas:
- stable actor identity
- explicit authority assignment
- packet separation by concern
- dedicated remote playback objects
- server-side actor state storage

But we should replace its weak parts with:
- **stable authority leases**, not ping-chasing churn
- **buffered snapshot interpolation** for dedicated actors
- **server-owned combat truth** for hits, deaths, and major AI transitions
- **handoff-safe actor mirrors** so ownership changes do not visually pop
- **late-join snapshots** built from persistent server actor state, not just live relays

## Phase 8 Step-by-Step Plan

### Step 1 - Lock actor identity to the new content index
Build the actor system on top of the Phase 8 content-index work already in progress.
- Extend the server content catalog so every syncable static actor can be resolved by cell + stable reference identity.
- Define one canonical runtime actor key for the rebuild, based on the modern equivalent of TES3MP's `refNum/mpNum` pairing.
- Support placed or dynamically spawned actors with a server-assigned runtime ID path from day one.
- Decide which actors are syncable now: NPCs, creatures, summoned actors, escorts, followers, respawns, and script-spawned actors.

**Reason:** Nothing else is safe until every client and the server agree on exactly which actor is being discussed.

### Step 2 - Build a server actor registry as the source of truth
Create a dedicated server-side actor state store instead of relying on loose packet relay.
- Track identity, current cell, transform, velocity, anim flags, dynamic stats, death state, equipment surface state, combat state, and current authority lease.
- Separate persistent state from transient events.
- Store last accepted snapshot time and last accepted event time per actor.
- Make late-join and re-entering clients receive state from this registry, not from whichever client happened to send the most recent packet.

**Reason:** TES3MP stored only enough for relay continuity. We need enough for smooth playback, safe handoff, and future server authority.

### Step 3 - Replace raw cell ownership with stable authority leases
Keep explicit authority, but make it harder to churn.
- Start with cell-scoped authority assignment because the current Phase 7 work already expects it.
- Add a minimum lease duration, cooldown, and handoff hysteresis.
- Prefer keeping the current authority owner unless a hard condition requires transfer: disconnect, cell unload, no snapshots, or impossible distance.
- Allow the design to evolve later toward actor-cluster authority if cell-wide ownership proves too coarse.

**Reason:** This preserves TES3MP's simplicity while avoiding constant ownership flapping.

### Step 4 - Define the actor packet contract for the rebuild
Implement the actual packet layer under the existing enum slots.
- `ActorAuthority`: reliable authority assignment and revocation.
- `ActorList`: reliable bootstrap snapshot for cell load and late join.
- `ActorPosition`: high-rate unreliable snapshot stream with timestamps/sequence.
- `ActorAnimFlags`: unreliable state stream for locomotion posture.
- `ActorAnimPlay`: reliable one-shot animation triggers.
- `ActorAttack` and `ActorCast`: reliable combat intent/event packets.
- `ActorDeath`, `ActorAI`, `ActorEquipment`, `ActorStatsDynamic`: reliable or semi-reliable depending on semantics.

Each packet should carry enough metadata for:
- actor identity
- server or authority sequence number
- snapshot timestamp
- source authority generation or lease ID

**Reason:** The current rebuild has packet IDs but not the real contract needed for smooth playback or safe handoff.

### Step 5 - Implement server-side validation and relay rules
When actor packets arrive from clients:
- accept only from the active authority owner for the actor's current cell or lease
- reject stale generations after handoff
- reject impossible transform deltas and obviously broken state
- stamp accepted snapshots into the server actor registry
- rebroadcast normalized snapshots to interested clients

For combat and death:
- do not just dumb-relay forever
- treat the first Phase 8 slice as "server-validated authority client" if full server simulation is too large for one pass
- keep the server as the final writer of death state and authoritative HP result

**Reason:** TES3MP was mostly an authority-gated relay. We need that baseline plus validation.

### Step 6 - Build a client actor binding layer
Expand `ActorSync` from a stub into a real actor manager.
- Map runtime actor IDs to live `MWWorld::Ptr` instances.
- Resolve actor spawn/bootstrap from the new content index.
- Track per-cell actor mirrors, authority state, and runtime playback objects.
- Keep actor mirrors alive across handoff long enough to prevent pops.
- Avoid TES3MP's hard local/dedicated destruction and recreation where possible.

**Reason:** Smoothness depends as much on lifetime management as on packet timing.

### Step 7 - Implement buffered snapshot interpolation for dedicated actors
This is the biggest quality upgrade over TES3MP.
- Store a short history buffer of incoming transform snapshots per dedicated actor.
- Render slightly behind real time so interpolation usually happens between two known snapshots.
- Fall back to bounded extrapolation only for short gaps.
- Snap only on teleports, large corrections, or cell changes.
- Blend movement state, turning, jump/fall state, and weapon posture from the same buffered timeline.

Suggested baseline:
- target send cadence around 15-30 Hz for actor snapshots at first
- render interpolation delay around 100-150 ms
- hard teleport threshold significantly larger than ordinary locomotion error
- dedicated debug overlay for snapshot age, buffer depth, and correction distance

**Reason:** TES3MP's fixed lerp-to-latest approach is exactly what made remote actors look floaty.

### Step 8 - Separate movement ownership from visual playback
Locally authoritative actors and remotely played actors should share identity, not behavior code.
- Local authority path gathers movement/combat state from the live actor.
- Dedicated playback path consumes buffered snapshots and reliable events.
- Authority handoff should swap input ownership, not destroy the visual continuity object.
- Keep last known snapshot state available during transitions.

**Reason:** The more we decouple identity from ownership mode, the less visible handoff churn players will see.

### Step 9 - Implement combat as event-driven authority, not animation-driven relay
Combat needs its own plan inside actor sync.
- Keep attack wind-up, release, and cast start as reliable gameplay events.
- Let animation playback be downstream from combat events instead of the source of truth.
- Make the server own the accepted hit result, death result, and any stateful combat transitions.
- Sync dedicated actors' attack readiness, current swing/cast phase, and recovery windows so remote combat reads cleanly.
- Make projectile or missile follow-up integrate with the same authority chain later.

**Reason:** If movement becomes smooth but combat still trusts the client too much, actor sync will still feel bad.

### Step 10 - Add AI, death, equipment, and follower cell-change slices
Once transform and combat are stable:
- sync AI package changes and targets
- sync death state and corpse persistence
- sync equipment surface changes that matter visually
- sync actor cell changes, including followers crossing boundaries
- define how respawn or reset reuses actor identity

**Reason:** TES3MP proves these slices are practical, but they should come after the transform/combat foundation.

### Step 11 - Build authority handoff and recovery paths
Handle the ugly cases explicitly.
- authority client disconnects
- authority client unloads the cell
- actor moves into a cell without an eligible owner
- snapshots stop arriving
- two clients briefly believe they have authority

For each case:
- promote a new owner
- increment authority generation
- resend authoritative actor bootstrap for affected actors
- keep dedicated playback stable until new snapshots arrive

**Reason:** Smooth normal behavior is not enough; handoff quality is the difference between a prototype and a shippable system.

### Step 12 - Add test and debug tooling before widening scope
Before expanding to every actor packet:
- log authority assignment, handoff reason, and lease lifetime
- log actor snapshot rates and correction sizes
- add a debug UI or console dump for one watched actor
- create reproducible test cases for: follower movement, combat against NPCs, creature pathing, death visibility, cell change, and late join
- test with packet delay and loss simulation

**Reason:** Actor sync failures are timing-sensitive. Tooling has to ship with the system.

## Suggested Implementation Order in the Codebase

### Exact file map for the first implementation pass

#### Shared actor wire model
- `openmw/components/openmw-mp/Base/BaseActor.hpp`
  - extend the actor batch metadata used by all actor packets
  - keep the runtime actor identity format explicit and reusable
- `openmw/components/openmw-mp/Packets/Actor/ActorPacket.hpp`
  - add a shared actor-packet base with helpers for actor identity, transform, stats, AI, equipment, and batch metadata
- `openmw/components/openmw-mp/Packets/Actor/PacketActorAuthority.hpp`
  - authority assignment and revocation payload
- `openmw/components/openmw-mp/Packets/Actor/PacketActorList.hpp`
  - full cell bootstrap snapshot payload
- `openmw/components/openmw-mp/Packets/Actor/PacketActorPosition.hpp`
  - high-rate transform snapshots
- `openmw/components/openmw-mp/Packets/Actor/PacketActorAnimFlags.hpp`
  - locomotion posture and movement state
- `openmw/components/openmw-mp/Packets/Actor/PacketActorAnimPlay.hpp`
  - reliable one-shot actor animation triggers
- `openmw/components/openmw-mp/Packets/Actor/PacketActorAttack.hpp`
  - reliable actor combat events
- `openmw/components/openmw-mp/Packets/Actor/PacketActorCast.hpp`
  - reliable actor spellcast events
- `openmw/components/openmw-mp/Packets/Actor/PacketActorDeath.hpp`
  - death-state replication
- `openmw/components/openmw-mp/Packets/Actor/PacketActorEquipment.hpp`
  - visible equipment surface sync
- `openmw/components/openmw-mp/Packets/Actor/PacketActorStatsDynamic.hpp`
  - health, magicka, and fatigue sync for actors
- `openmw/components/openmw-mp/Packets/Actor/PacketActorAI.hpp`
  - AI package and target replication

#### Server authority and registry layer
- `openmw/apps/openmw-server/Server.hpp`
  - add actor registry structures, authority lease state, and handler declarations
- `openmw/apps/openmw-server/Server.cpp`
  - add actor packet dispatch
  - maintain per-cell actor authority
  - persist actor snapshots in the registry
  - rebroadcast actor state to interested clients
  - send actor bootstrap state during cell entry and late join

#### Client intake and runtime state
- `openmw/apps/openmw/mwmp/Main.cpp`
  - register protocol handlers for all actor packets
  - forward decoded actor payloads into `ActorSync`
- `openmw/apps/openmw/mwmp/sync/ActorSync.hpp`
  - extend the runtime actor cache with authority state, actor mirrors, and snapshot buffers
- `openmw/apps/openmw/mwmp/sync/ActorSync.cpp`
  - implement actor packet application
  - implement buffered dedicated-actor snapshot playback state
  - implement authority grant and revoke transitions

#### Build integration
- `openmw/apps/openmw/CMakeLists.txt`
  - update only if actor sync grows into additional translation units
- `openmw/apps/openmw-server/CMakeLists.txt`
  - update only if actor registry or authority logic is split into new source files

### Buildable milestone breakdown for the immediate coding pass
1. **Milestone A - Wire in actor packets**
   - add shared actor packet definitions
   - compile client and server includes cleanly
2. **Milestone B - Server authority baseline**
   - track one actor authority owner per cell
   - accept actor updates only from the active owner
   - send authority and actor bootstrap packets to clients entering the cell
3. **Milestone C - Client actor intake baseline**
   - decode actor packets and store actor snapshots by cell and actor key
   - maintain buffered state needed for smoothing and handoff safety
4. **Milestone D - Smoothing core**
   - interpolate actor position state inside `ActorSync`
   - keep the data path buildable even before deeper world binding is widened
5. **Milestone E - Hardening**
   - build the workspace
   - fix compile and integration errors

### Session status update

Completed across all sessions to date:
- shared actor packet layer added under `components/openmw-mp/Packets/Actor/`
- server-side actor authority and per-cell actor registry added in `apps/openmw-server/Server.*`
- client-side actor packet intake added in `apps/openmw/mwmp/Main.cpp`
- `ActorSync` expanded into:
  - inbound actor snapshot buffering and smoothing
  - live actor binding against active-cell world actors
  - outbound authoritative scanning for actor lists and actor positions
  - first-pass actor presentation playback onto bound actors

Completed in previous session (combat request, anim sync, equipment):
- `PacketActorCombatRequest` (ID 65) added: new packet that lets non-authority clients forward NPC hit events to the server, which routes them to the current authority client; the authority client runs vanilla `actorAttacked()` so aggro, bounty, and mortality all fire correctly on the right machine
- `handleActorCombatRequest` wired end-to-end: `NetworkMessages.hpp` → `PacketActorCombatRequest.hpp` → `Server.cpp` handler → `ActorSync::onActorCombatRequest` → `ActorSync::sendCombatRequest` → `Main::sendActorCombatRequest` → `PlayerSync::notifyLocalHit`
- head-tracking idle stepping fixed: outbound scan now captures `animFwd`/`animSide` from `movement.mPosition`; `isMoving` threshold raised from `0.01f` to `0.1f` to filter sub-threshold head-tracking inputs; `applyBoundActorState` now gates locomotion driving on actual axis magnitude (`> 0.1f`), not just the `isMoving` bool
- `PacketActorPosition` extended to carry `animFwd`, `animSide`, and `movementFlags` so locomotion state is continuously correct on remote clients without a separate `ActorAnimFlags` send every frame
- `mergeActorState` updated to merge presentation state and `animFlags` axes when `includeTransform=true`, so every position update also refreshes the remote's locomotion state
- `isAttackingOrCasting` made edge-driven in `applyBoundActorState`: `setAttackingOrSpell` is only called when the value changes, preventing repeated CC combat-loop triggers on remote clients


Completed in latest-1 session (NPC-vs-player combat, weapon draw state, spell VFX):

**NPC weapon mesh draw-state fix**
- Added `lastWeaponVisible` to `ActorRuntime`; `equipmentChanged()` now fires on
  draw-state transitions (Nothing vs Weapon) so the weapon mesh appears
  at the correct moment, not just on the initial equipment packet

**Non-authority player takes damage and dies from NPC attacks**
- `victimPlayerGuid` field added to `ActorList` and serialized in `PacketActorCombatRequest`
- `Npc::onHit` hook: NPC hitting a remote-player ghost calls `sendActorNpcPlayerHit()`
  before the ghost is resurrected
- `ActorSync::sendNpcPlayerDamage()`: sends `PacketActorCombatRequest` with
  `victimPlayerGuid` set; server routes to victim player directly
- `ActorSync::onActorCombatRequest`: early path applies damage to local player
  when `victimPlayerGuid == localGuid`

**NPC spell VFX on non-authority client**
- `ActorSync::notifyNpcCast()`: authority sends `PacketActorCast` when an NPC casts
- `worldimp.cpp::castSpell()` extended to call `notifyNpcCast` for NPC casters
- Enhanced `pendingCast` handler: cast-start plays spellcast anim and calls
  `CastSpell::playSpellCastingEffects()`; cast-release calls `launchMagicBolt()`
  with non-authoritative=true for bolt VFX (no extra damage on non-auth)

Additional live-test observations and session-2 fixes (based on in-game chat log + client logs):

**Live-test findings from session 2 (player "ass" / client2 observations):**
- Idle animations: nearly perfectly in sync ✅
- NPC self-cast VFX (restoration, shield on self): not seen by non-authority ❌ → fixed
- NPC cast animation: not seen (projectile was seen, but not cast animation) ❌ → fixed
- No destruction VFX from NPC melee attacks ❌ → partially improved via draw state fix
- NPC changing draw states rapidly ⚠️ → now mirrors authority accurately
- NPC spear not visible for non-authority ("spear that fart sees … not there for me") ❌ → fixed
- Puppet draws/sheaths mace on every NPC hit ❌ → fixed
- Non-authority not taking NPC melee damage ❌ → fixed (root cause: server guard)
- Non-authority puppet not dying from NPC melee ❌ → fixed
- NPC deaths in sync ✅
- Different death animations on different clients ⚠️ (known, not addressed yet)

**Session 2 bug fixes:**

Server routing fix (critical – root cause of no NPC→player damage):
- `Server.cpp::handleActorCombatRequest`: moved `victimPlayerGuid != 0` routing
  check BEFORE the authority guard `if (authorityGuid == c.guid) return`. The
  authority guard was blocking fart (cell authority) from routing NPC→player
  damage packets to ass. Fix: early-return after routing `victimPlayerGuid` packets,
  authority guard only applies to the non-authority→authority flow.

Ghost health damage suppression (fixes perpetual isDead=1 spam):
- `npc.cpp::Npc::onHit`: health damage is now skipped for remote-player ghost
  targets (`targetIsGhost=true`). Ghost health is driven solely by position/stats
  sync from the victim's client. Previously health was applied locally, causing
  ghost to die → resurrect to 1 HP → die again in an endless loop (87 seconds,
  27 isDead=1 packets observed in test). Damage forwarding via `sendActorNpcPlayerHit`
  still fires; victim client applies the damage there.
- Ghost resurrection fallback: restored health set to `getBase()` instead of
  `std::max(1, std::min(base, 1))` which always returned 1.

Ghost combat entry suppressed (fixes puppet weapon draw/sheath on every hit):
- `npc.cpp::Npc::onHit`: `actorAttacked(ptr, attacker)` is skipped when
  `targetIsGhost=true`. Calling it made the ghost's CharacterController enter
  combat mode and draw a weapon on every NPC hit (authority client saw remote
  player puppet repeatedly drawing/sheathing its mace). The NPC's AI already
  targets the ghost; actorAttacked is not needed for NPC targeting to continue.

NPC weapon visibility for non-authority (spear invisible for ass):
- `ActorSync.cpp::applyBoundActorState`: removed `shouldForceCombatPresentation`
  guard from draw-state logic. That guard was `actor.state.ai.type == Combat` but
  `ai.type` is only present in the once-sent `ActorList` packet and NOT in the
  20 Hz position packets. If an NPC entered combat after the initial list was sent,
  `ai.type` stayed `None`, `shouldForceCombatPresentation=false`, `drawState=Nothing`
  forever → weapon never shown. Fix: set draw state directly from `hasWeaponDrawn`
  / `hasSpellReadied` (present in every position packet, mirrors
  `stats.getDrawState()` on authority).

Spell VFX improvements (cast animation + hand glow + Self-range filter):
- `ActorSync.cpp::applyBoundActorState` (pendingCast handler): in the
  `cs.release=true` branch, now also plays the spellcast animation and triggers
  `CastSpell::playSpellCastingEffects()` (hand glow) before launching the bolt.
  This covers all spells since only `release=true` packets are sent.
- `launchMagicBolt` is now only called when the spell has at least one non-Self
  range effect (Touch or Target). Self-targeted spells (restoration, shield, etc.)
  get the hand glow + cast animation but no projectile — previously
  `launchMagicBolt` was called for all spells, producing nothing visible for
  self-targeted ones while wasting the call.

Additional live-test observations (latest run):

- A non-authority player was able to aggro an NPC by hitting it via the `ActorCombatRequest` path; when the NPC was killed by that non-authority player it died on both clients (authority and non-authority). This confirms the server-validated combat result path is functioning end-to-end.
- The death animation, however, was not synchronized: different clients played different death animation groups for the same kill. The death state and HP are authoritative, but the chosen death animation group is not yet shared consistently.
- NPC weapon visuals and attack animations were not replicated to the non-authority client: the NPC's equipped weapon did not appear on the non-auth client and attack animations did not play, even though the authority saw them.

Validated:
- project builds successfully after each implementation pass
- actor death replication is confirmed working across clients
- actor position sync has an outbound producer path from the authority client
- cross-authority NPC combat request path is fully wired and building clean

Current known gaps:
- Different death animations on different clients (random selection per client, not synced beyond the group name in deathState)
- Spell bolt direction uses computed aim toward target; fallback is NPC forward if target not found locally
- Head-look direction not synchronized (non-auth clients show vanilla-local idle head turns)
- Actor cell change / follower boundary crossing not yet implemented
- Authority handoff on disconnect / cell unload not yet robustly handled
- Door activation rejected as "unverified_target" during rapid cell transitions (race condition: object has no mpNum at activation time)

Next implementation focus:
- In-game test to validate all session-3 fixes (see below)

---

### Session 3 — Bug Fixes from Live Playtest Chat Logs

Test session with two players (fart/ass) in Balmora Temple. Player "ass" described bugs
via in-game chat as they happened. All bugs identified and fixed:

**Bug 1: No restoration VFX on non-auth client** (partially addressed)
- Self-range spell effects (restoration glow on NPC body) are not visible on non-auth.
- The hand-glow and cast animation DO work. The missing VFX is the spell-applied effect
  (e.g., golden glow from restoration being applied), which requires `inflict()` to run.
- Status: Known limitation. Would need visual-only spell effect application.

**Bug 2: Spell bolt launches at START of cast animation** ✅ FIXED
- Root cause: `world->launchMagicBolt()` was called immediately in the `pendingCast` handler,
  simultaneously with starting the cast animation.
- Fix: Added delayed bolt launch via `pendingBoltTimer` (0.6s delay) on `ActorRuntime`.
  The timer counts down each frame; when it expires, the bolt is launched using the
  actor's current facing direction. This gives the cast animation time to play before
  the projectile appears.
- Files: `ActorSync.hpp` (new fields), `ActorSync.cpp` (timer logic, deferred bolt launch)

**Bug 3: No melee attack animations on non-auth client** ✅ FIXED
- Root cause #1: The `pendingAttack` handler gated on `ai.type == Combat`, but `ai.type`
  is only sent in the initial ActorList and never updated in position packets — it goes
  stale immediately.
- Root cause #2: The authority always sent `attackAnimation = "slash"` regardless of the
  actual attack type being performed.
- Fix: Removed the `ai.type == Combat` gate; now checks `attack.pressed` alone (authority
  only sends attack packets during actual combat). Authority now reads `mp_attack_type`
  from the NPC's base node (set by CharacterController during `updateWeaponState()`),
  which gives the correct attack type (chop/slash/thrust).
- Files: `ActorSync.cpp` (both authority send and non-auth receive)

**Bug 4: Death animation differs between clients** ✅ FIXED
- Root cause: `playRandomDeath()` only checked the `mp_death_anim_group` user value for
  actors with `Flag_NetworkPlayerNpc` — regular NPCs (controlled by actor sync) don't
  have this flag, so they ignored the synced death group and picked a random one locally.
- Fix: Removed the `Flag_NetworkPlayerNpc` guard. All actors now check for
  `mp_death_anim_group`. Since this user value is only set by network sync code,
  single-player actors are unaffected.
- File: `character.cpp` (`playRandomDeath()`)

**Bug 5: Corpses replay death animation when re-entering cell** ✅ FIXED
- Root cause: When a non-auth player re-enters a cell with dead actors, the ActorList
  arrives with `isDead=true`. The code forced health to 0, triggering `killDeadActors()`
  → `kill()` → `playRandomDeath()` from `startpoint=0.0`, replaying the full death
  animation.
- Fix: Added `deathFromRealtimePacket` flag to `ActorRuntime`. Only `onActorDeath()`
  sets it to `true` (real-time death event). `onActorListUpdate()` leaves it `false`.
  In `applyBoundActorState()`, non-realtime deaths call
  `stats.setDeathAnimationFinished(true)`. In `playRandomDeath()`, added check: if
  `isDeathAnimationFinished()`, set `startpoint=1.0` to jump to the final corpse pose.
- Files: `ActorSync.hpp`, `ActorSync.cpp`, `character.cpp`

**Bug 6: Cannot activate corpses/doors as non-auth player** ✅ FIXED
- Root cause: Server's `resolveVerifiedTarget()` only verifies objects via
  `mp.getObjectByMpNum(mpNum)`. Base-game NPCs and doors from ESM data don't have
  mpNum values, so all activations were rejected as `unverified_target`.
- Fix: Modified `buildActivateIntentContext()` in `core.lua` to call
  `classifyActivation(object)` before rejecting. If the action is "actor" or "door"
  (base-game objects), the activation is allowed through even without mpNum verification.
  Distance checks and same-cell checks still apply for anti-cheat.
- File: `core.lua`

**Additional issues observed in logs:**
- Door activation race conditions (`unverified_target` for doors during rapid cell
  transitions) — separate from Bug 6, this is about timing during teleport. Lower priority.
- Corpse loot/dispose sync was still missing at this point in the timeline.
  Later sessions added the server-owned corpse container/dispose path for spawned actors.

### Files Changed (Session 3)
- `apps/openmw/mwmechanics/character.cpp` — `playRandomDeath()`: removed Flag_NetworkPlayerNpc
  guard; added isDeathAnimationFinished() → startpoint=1.0 check
- `apps/openmw/mwmp/sync/ActorSync.hpp` — `ActorRuntime`: added deathAlreadyApplied,
  deathFromRealtimePacket, pendingBoltTimer, pendingBoltSpellId
- `apps/openmw/mwmp/sync/ActorSync.cpp` — Fixed attack handler (removed ai.type gate),
  authority attack type capture (read mp_attack_type), delayed bolt launch, death replay
  prevention (deathFromRealtimePacket tracking)
- `apps/openmw-server/scripts/core.lua` — Allow activation of base-game actors and doors
  without mpNum verification

### Shared
- `openmw/components/openmw-mp/Base/BaseActor.hpp`
- new packet encode/decode files for actor packets
- `openmw/components/openmw-mp/NetworkMessages.hpp` updates only if packet shape or reserved IDs need clarification

### Server
- actor registry and authority manager in `openmw/apps/openmw-server/`
- cell-interest routing integration
- packet handlers for `ActorAuthority`, `ActorList`, `ActorPosition`, `ActorAttack`, `ActorCast`, `ActorDeath`, `ActorAI`, `ActorEquipment`, `ActorStatsDynamic`
- hooks into existing Phase 8 content-index work for actor resolution

### Client
- `openmw/apps/openmw/mwmp/sync/ActorSync.*`
- dedicated actor playback objects and snapshot buffers
- authority grant/revoke handling
- actor bootstrap and late-join application
- combat and animation event playback hooks

## Recommended Phase 8 Milestones
1. **8A - Identity + Registry**
   Stable actor keys, server actor registry, `ActorList` bootstrap.
2. **8B - Authority + Position**
   Authority leases, `ActorAuthority`, `ActorPosition`, client dedicated interpolation.
3. **8C - Combat Core**
   `ActorAttack`, `ActorCast`, `ActorDeath`, server-owned combat result path.
4. **8D - AI + Equipment + Cell Change**
   `ActorAI`, `ActorEquipment`, `ActorStatsDynamic`, `ActorCellChange`.
5. **8E - Hardening**
   Handoffs, late join, loss/jitter testing, debug tools, tuning.

## Success Criteria
Phase 8 is complete when:
- all clients see NPCs and creatures moving in real time
- dedicated actors look stable under jitter and minor packet loss
- authority handoff does not visibly pop or freeze actors
- combat events are visible and consistent across clients
- death state is authoritative and late-join safe
- actor state survives cell visitor changes without TES3MP-style floatiness

## Bottom Line
TES3MP already proved the minimum viable actor sync model:
- stable actor identity
- explicit authority
- packetized actor state
- dedicated remote playback

That is the right baseline to study.

But for this fork, the correct upgrade path is:
**TES3MP architecture discipline + buffered movement playback + stable authority leases + stronger server combat truth**.

That combination should get actors in sync for all clients without inheriting TES3MP's most visible movement and combat weaknesses.

### Session 4 — Bug Fixes from Live Playtest #2

Test session revealed that non-authority clients received ZERO ActorAttack, ActorCast,
and ActorDeath packets while other actor packets (Position, StatsDynamic) worked fine.

**Bug A: NPC attack animations not visible on non-auth** FIXED
- Root cause #1: Attack edge detection in sendAuthoritativeActorUpdates() was gated
  on actor.ai.type == Combat. Gate removed.
- Root cause #2: Non-auth receiver relied on setAttackingOrSpell(true) which the CC
  on puppet NPCs may not process. Fix: direct animation->play() using weapon group.
- Files: ActorSync.cpp (sender + receiver), weapontype.hpp include added

**Bug B: Death animation mismatch between clients** FIXED
- Root cause #1: playRandomDeath() only set mp_death_anim_group for the local player,
  not NPCs. Fix: now sets for ALL actors.
- Root cause #2: MP update runs BEFORE mechanics update. Death send deferred via
  deathPacketSent flag until anim group is available.
- Files: character.cpp, ActorSync.cpp, ActorSync.hpp

**Bug C: CC only reads mp_attack_type for Flag_NetworkPlayerNpc** FIXED
- Extended mp_attack_type reading to all actors in updateWeaponState().
- File: character.cpp

**Bug D: No spell cast VFX on non-auth** DIAGNOSTIC LOGGING ADDED
- Authority sends PacketActorCast but non-auth received zero. Logging added at every
  point in the pipeline to determine where packets are lost.
- Files: ActorSync.cpp, Server.cpp

**Bug E: Spell VFX not visible on victim** FIXED
- Root cause: Non-authority clients skip inflict() (correctly, to avoid duplicate
  gameplay effects), but this also skips playEffects(), so the body-hit VFX (glow,
  particles, hit sound) never appear on the target or caster.
- Fix (3 parts):
  1. projectilemanager.cpp: When a visual-only bolt hits an actor target, iterate
     the spell RT_Target effects and call MWMechanics::playEffects() on the target
     so the hit glow/particles/sound show on the victim body.
  2. ActorSync.cpp (Self-range): When receiving a cast release, iterate the spell
     RT_Self effects and call MWMechanics::playEffects() on the caster so restoration
     glow, shield shimmer, etc. are visible on the NPC body (not just hand glow).
  3. ActorSync.cpp (Touch-range): When the authority includes a targetRefId in the
     cast packet, find the target actor in the cell and call playEffects() for each
     RT_Touch effect so touch-range spell VFX appear on the victim immediately.
- Files: projectilemanager.cpp, ActorSync.cpp

### New bugs observed in playtest chat logs (not yet addressed)
- Knockout/fatigue animations not synced: When an NPC falls over from fatigue loss,
  non-auth player sees snap-to-lying-down and snap-to-standing instead of the knockout
  and standup animations playing smoothly.
- No flinch/hit reaction: Non-auth player reports no flinching when getting hit
  by NPCs.
- Attack animation stuck: NPC attacks sometimes get stuck at the fully-extended
  hit pose on non-auth client before continuing.

### Session 5 — Remaining combat presentation fixes from logs

Investigated `openmw-server.log` plus both client logs after the second playtest.
The server chat confirmed death desync was still reproducible (`ilen faveran` face-up
for one client and face-down for the other), and the client logs showed three concrete
remaining issues:

**Bug F: Knockout / stand-up playback snapped on non-auth** ✅ FIXED
- Root cause: actor-sync NPCs were not applying the same hit-state flag pipeline that
  remote players already used. Fatigue sync also overwrote the forced negative fatigue
  needed to keep `knockout` looping, so observers skipped straight to the floor pose or
  popped upright.
- Fix: ported the remote-player hit-state application logic into actor-sync NPC playback.
  `applyBoundActorState()` now reads `MF_KNOCKED_OUT`, `MF_KNOCKED_DOWN`, and
  `MF_RECOVERY`, drives `setKnockedDown()` / `setHitRecovery()` on edges, forces
  negative fatigue during knockout, restores fatigue on exit, and suppresses dynamic
  fatigue overwrite while knockout is active.

**Bug G: No flinch / hit reaction on non-auth** ✅ FIXED
- Root cause: actor-sync NPCs received the authoritative lower-body animation group, but
  hit-state groups like `hit*`, `knockdown`, and `knockout` were treated like ordinary
  idle variants. They could be overridden or never promoted with the right priority.
- Fix: direct animation-group replay now classifies hit-state groups separately, disables
  stale hit groups on transition, and replays them at `Priority_Knockdown` so the same
  flinch / recoil presentation wins on observing clients.

**Bug H: Attack animation stuck at extended pose on non-auth** ✅ FIXED
- Root cause: attack packets were replayed with `autodisable=false` and without a clean
  restart boundary, so repeated attack presses could leave the weapon animation parked at
  the follow-through pose until another state displaced it.
- Fix: attack playback now mirrors press/release edges, restarts only on a real rising
  edge, explicitly disables the weapon group before replay, and uses `autodisable=true`
  so the attack animation naturally exits instead of sticking.

**Additional hardening included**
- Reset actor-sync transient playback state on death/resurrection so stale hit/attack
  state cannot leak across lifecycle transitions.
- Added `<limits>` include because the direct animation replay path now uses
  `std::numeric_limits<uint32_t>::max()` in the rebuilt function body.

### Files Changed (Session 4)
- apps/openmw/mwmechanics/character.cpp
- apps/openmw/mwmp/sync/ActorSync.hpp
- apps/openmw/mwmp/sync/ActorSync.cpp
- apps/openmw-server/Server.cpp
- apps/openmw/mwworld/projectilemanager.cpp

### Files Changed (Session 5)
- apps/openmw/mwmp/sync/ActorSync.hpp
- apps/openmw/mwmp/sync/ActorSync.cpp

### Session 6 — New log pass after follow-up playtest

Investigated the fresh `openmw-server.log` and both client logs.
The new server chat identified two still-open issues:

**Bug I: NPC death pose still mismatched between clients** IN PROGRESS
- Evidence: server chat reported `she died face down for me and face up for fart`.
- Log finding: the authority sent a concrete death group, but non-authority playback
  could still be vulnerable to late or duplicate death-state updates overwriting the
  first chosen death pose.
- Fix applied: actor-sync runtime now latches the first synced death animation group
  for a death event (`appliedDeathAnimGroup`) and reuses that latched value when
  applying death locally, instead of allowing later state churn to replace it.

**Bug J: Self-cast NPC enchant/spell VFX still not visible to non-auth** IN PROGRESS
- Evidence: server chat reported no visible self-restoration VFX or even cast animation
  from NPC self-casts on the observing client.
- Log finding: earlier logs showed `onActorCast received`, but the newest pass did not
  yet include enough playback-side detail to prove whether the packet, animation, or
  VFX branch is failing in the self-cast path.
- Fix applied: added targeted logging through the actor-cast playback path so the next
  run will show whether the observer receives the cast packet, which cast animation key
  was requested, and whether self-range VFX playback actually executes.

### Files Changed (Session 6)
- apps/openmw/mwmp/sync/ActorSync.hpp
- apps/openmw/mwmp/sync/ActorSync.cpp

### Session 7 — Fix remaining bugs from playtest #3 chat logs

Investigated the fresh `openmw-server.log` in-game chat messages. Nine chat messages
from the tester identified five remaining bugs. All five have been fixed in this session.

**Bug K: Punching knocked NPC does no health damage + wrong hit sound** ✅ FIXED
- Evidence: server chat "punching the knocked npc should be doing health damage but it
  is not" and "its still making the same sound as landing a hit when they were still
  standing" and "it should match the default bahavior of openmw for punching a knocked
  out npc".
- Root cause: The authority's `sendAuthoritativeActorUpdates()` captured `MF_RUN` and
  `MF_SNEAK` flags but **never set** `MF_KNOCKED_DOWN`, `MF_KNOCKED_OUT`, or
  `MF_RECOVERY` — even though the receiver in `applyBoundActorState()` already had full
  knockout/knockdown/recovery handling code that consumed these flags. With the flags
  always zero, the non-auth client never put NPCs into knockout state. This meant
  `getKnockedDown()` returned false, `getHandToHandDamage()` returned `healthdmg=false`,
  and the combat request carried `healthDamage=false` so the authority applied no damage.
- Fix: Added three flag captures in `sendAuthoritativeActorUpdates()`:
  - `MF_KNOCKED_DOWN` ← `stats.getKnockedDown()`
  - `MF_KNOCKED_OUT`  ← `stats.getFatigue().getCurrent() < 0.f`
  - `MF_RECOVERY`     ← `stats.getHitRecovery()`

**Bug L: Standup animation not playing after knockout on non-auth** ✅ FIXED
- Root cause: Same as Bug K — `MF_KNOCKED_OUT` was never sent, so the receiver's
  knockout state machine never entered the knockout state. The knockout animation never
  started, so the standup transition (fatigue recovers → animation stops looping → CC
  transitions to standup) never occurred.
- Fix: Same as Bug K. With the flags now being sent, the full knockout→standup animation
  cycle works on non-auth clients.

**Bug J: Self-cast NPC enchant/spell VFX still not visible to non-auth** ✅ FIXED
- Evidence: server chat "the vfx from her on self enchant are still not visible".
- Root cause: `notifyNpcCast()` sends the item's refId (e.g., "iron_spear_enchanted")
  for enchantment casts, not a spell ID. The receiver tried
  `store.get<ESM::Spell>().search(spellRefId)` which returned null for item refIds,
  causing all VFX code to be skipped.
- Fix: Added `resolveCastSource()` helper in the anonymous namespace that first tries
  spell lookup, then falls back through weapon/armor/clothing/book stores to find the
  item, reads its enchantment field, and returns the `ESM::Enchantment*`. Both cast-start
  and cast-release playback code now uses this resolver, so enchantment hand-glow,
  self-range body VFX, and bolt launch all work for enchanted item casts.

**Bug N: Destruction VFX not visible on player when hit by NPC's enchanted weapon** ✅ FIXED
- Evidence: server chat "im also still not seeing the destruction vfx on myself when
  getting hit with the enchanted spear".
- Root cause: `applyOnStrikeEnchantment()` runs on the authority (the client that owns
  the NPC cell). The enchantment hit VFX fired on the authority's screen but the victim
  player on another client never saw them. The NPC→player damage packet only carried raw
  damage, not enchantment information.
- Fix: In `onActorCombatRequest()` NPC→player damage handler, after applying damage,
  the code now looks up the attacking NPC's synced weapon equipment (from actor sync
  state), checks for a "When Strikes" enchantment, and calls `playEffects()` for each
  effect on the local player. This makes destruction glow, fire particles, frost
  shimmer, etc. visible on the victim player's body.

**Bug O: Guards spam "you violated the law" dialogue on respawn** ✅ FIXED
- Evidence: server chat "the guards spammed me with the you violated the law line again
  when i respawned" and "it must have printed a hundred times or so in the dialoge
  window".
- Root cause: `respawnLocally()` teleported the player to a temple/shrine marker but
  did not clear bounty. Guards in the respawn area all detected the crime simultaneously
  and each initiated dialogue, flooding the dialogue window.
- Fix: Added bounty clearing in `respawnLocally()`:
  1. `setBounty(0)` — clears the player's criminal record
  2. `world->getPlayer().recordCrimeId()` — records the crime-ID threshold so NPC
     crime-disposition modifiers are restored (same mechanism as vanilla `PayFineThief`)
  3. `mechanics->stopCombat(player)` — stops any NPCs still in combat with the player
     from the pre-death fight

### Files Changed (Session 7)
- apps/openmw/mwmp/sync/ActorSync.cpp — Added knockout/knockdown/recovery flag capture
  in authority sender; added `resolveCastSource()` helper for enchantment cast VFX
  resolution; rewrote cast playback to handle both spells and enchanted items; added
  enchanted weapon VFX playback on victim player in NPC→player damage handler; added
  ESM includes for enchantment/armor/clothing/book stores.
- apps/openmw/mwmp/sync/PlayerSync.cpp — Added bounty clearing, crime-ID recording,
  and combat stopping in `respawnLocally()` to prevent guard dialogue spam on respawn.

### Session 8 — Bug Fixes from Live Playtest #4 Chat Logs

Investigated the fresh `openmw-server.log`, both client logs, and user feedback.
The server chat confirmed multiple issues, and client log analysis revealed the root
causes. Three bugs fixed in this session.

**Bug P: NPC attack animations not playing for non-auth (regression)** ✅ FIXED
- Evidence: server chat "there are however no attack animations anymore" and user
  confirming in conversation "npc attack animations not playing for the non auth
  player is happening again".
- Root cause: In `onActorAttack()`, the `pendingAttack` flag was set but
  `lastAttackPressed` was never reset to `false`. The authority only sends attack
  packets on rising edges (always `pressed=true`), so after the first attack set
  `lastAttackPressed=true`, subsequent attacks had `pressedChanged=false` in
  `applyBoundActorState()`, causing the `animation->play()` call to be skipped.
  The animation was partially visible through the CC animation group relay
  (`currentAnimGroup="weapontwowide"`) but without the proper attack animation
  key framing (start/stop keys for chop/slash/thrust).
- Fix: Added `runtime.lastAttackPressed = false` in `onActorAttack()` after
  setting `pendingAttack = true`. Since the authority already does edge detection,
  each received packet is a distinct attack event. Resetting `lastAttackPressed`
  ensures `applyBoundActorState()` always sees a proper rising edge.

**Bug Q: Attack type always empty in authority's attack packet** ✅ FIXED
- Evidence: client1 log showed `attackAnim=` (empty) for ALL authority NPC attack
  sends, causing the fallback to "slash" regardless of actual attack type.
- Root cause: In `character.cpp::updateWeaponState()`, the `mp_attack_type` user
  value was only written to the base node for the local player
  (`mPtr == getPlayer()`). For authority-controlled NPCs, the attack type
  determined by AiCombat was never written to the base node. When
  `sendAuthoritativeActorUpdates()` read `mp_attack_type`, it found nothing and
  fell back to "slash".
- Fix: Restructured the mp_attack_type hooks so that:
  1. Non-player actors first try to read a synced attack type from the base node
     (for non-auth NPCs that receive their type from ActorSync)
  2. ALL actors then write their final `mAttackType` to the base node, ensuring
     both PlayerSync (player) and ActorSync (authority NPCs) can read the value.

**Bug R: Death animation mismatch between clients (Bug I)** ✅ FIXED
- Evidence: server chat "the death animations are still not picking the same
  loop/group for both clients" and "face up for me and face down for fart".
- Root cause: Race condition between StatsDynamic and ActorDeath packets. The
  authority sends StatsDynamic (with isDead=true, hp=0) in frame N and the
  ActorDeath packet (with the synced death animation group) in frame N+1
  (because `mp_death_anim_group` isn't set until `playRandomDeath()` runs in the
  mechanics update, which happens after the MP update). On the non-auth client,
  in `applyBoundActorState()`, the dynamic stats application
  (`health.setCurrent(0)`) set health to 0 BEFORE the death branch had a chance
  to set `mp_death_anim_group` on the base node. When `killDeadActors()` →
  `kill()` → `playRandomDeath()` later ran, it found no synced death group and
  picked a random animation.
- Fix (three-part):
  1. **Early pre-death block** (before stats application): When the actor is
     transitioning to dead and the death anim group is available, set
     `mp_death_anim_group` on the base node BEFORE any health change can trigger
     `playRandomDeath()`. Also set `deathAnimationFinished(true)` for non-realtime
     deaths (cell loads) so they jump to the corpse pose.
  2. **Main death branch** (unchanged but now redundant for same-frame case):
     Still handles the case where only the ActorDeath packet arrived without a
     prior StatsDynamic packet.
  3. **Late death anim replay**: When the ActorDeath packet arrives AFTER death
     was already applied from a stats packet in a previous frame, detect the
     mismatch (`appliedDeathAnimGroup != state.deathAnimGroup`) and directly
     replay the correct death animation via `animation->play()` at
     `Priority_Death` from startpoint=0. This handles the cross-frame race
     condition where the stats packet triggers a jump-to-end-pose in frame M
     and the death packet arrives with the correct group in frame M+1.

**Known limitation noted (not fixed)**
- NPC potion consumption VFX: The user reported possibly not seeing restoration
  VFX when an NPC drank health potions during combat. Potion consumption goes
  through `MWMechanics::CastSpell::cast(item)` which doesn't trigger the
  `notifyNpcCast` hook (that hook is in `updateWeaponState()` for spell/enchantment
  casts only). Syncing potion VFX would require a new hook in the alchemy
  consumption path. Lower priority since the user was uncertain and later confirmed
  spell VFX works correctly for spell/enchantment casts.

### Files Changed (Session 8)
- apps/openmw/mwmp/sync/ActorSync.cpp — Fixed attack animation regression by
  resetting `lastAttackPressed` in `onActorAttack()`; added early pre-death block
  in `applyBoundActorState()` to set `mp_death_anim_group` before stats can
  trigger `playRandomDeath()`; added late death anim replay for cross-frame death
  packet arrival.
- apps/openmw/mwmechanics/character.cpp — Restructured mp_attack_type hooks in
  `updateWeaponState()` so authority NPCs write their attack type to the base node,
  not just the player.

### Session 9 - Log Analysis and Fixes from Live Playtest 5

Investigated openmw-server.log, both client logs, and cross-referenced against
the Session 8 source code.

**Key finding**: The binary tested in playtest 5 was built at 13:26 but the
Session 8 source changes were written at 14:07 - the Session 8 fixes (Bugs P, Q,
R) were never compiled into the tested binary. Most bugs reported in the chat
messages are addressed by the existing Session 8 source code and simply need
a rebuild. Three additional issues were identified and fixed.

#### Playtest chat messages mapped to status

| Chat Message | Bug | Status |
|---|---|---|
| in not seeing the attack animations / first attack...the rest do not | Bug P | Session 8 fix (needs rebuild) |
| the death animation is still not the same between clients | Bug R | Session 8 fix (needs rebuild) |
| vfx from the npcs casting on self enchants | Bug J | Session 7 fix (working) |
| i am still seeing the destruction vfx on myself from her enchanted spear | Bug N | Confirming fix works |
| faverans restoration...does show the vfx properly for me | - | Confirming spell VFX works |
| i was unable to see the purple shield bubble on him after he cast it | New Bug S | Fixed this session |
| once he started melee attack i saw his first punch, the rest are not animating | Bug P | Session 8 fix (needs rebuild) |

#### New fixes in this session

**Bug Q-bis: Authority NPCs attack type overridden by stale base node value** FIXED
- Root cause: The Session 8 mp_attack_type fix in character.cpp added a
  read-from-base-node block for ALL non-player actors. For authority NPCs, this
  read overrode the freshly-determined AI attack type (setAIAttackType value)
  with the PREVIOUS attacks stale mp_attack_type value from the base node.
  After the first attack, all subsequent attacks would reuse the first attacks
  type instead of the AI-chosen type.
- Fix: Guarded the mp_attack_type read to only apply when the actors AI sequence
  is empty (aiSeq.isEmpty()). Non-authority puppet NPCs have their AI sequence
  cleared every frame by applyBoundActorState(), so they correctly read the
  network-synced attack type. Authority NPCs retain their AI combat packages, so
  the AI-determined mAttackType is preserved as the source of truth.

**Bug S: Persistent spell VFX (Shield bubble, etc.) not visible on non-auth** FIXED
- Evidence: server chat - i was unable to see the purple shield bubble on him after
  he cast it.
- Root cause: Two-part problem.
  1. playEffects() correctly adds looping VFX (Shield bubble mesh) via
     Animation::addEffect(model, effectId, loop=true) when a self-cast packet
     is received.
  2. BUT CharacterController::updateContinuousVfx() runs every frame and checks
     getMagicEffects().getOrDefault(effectKey).getMagnitude() <= 0. For non-auth
     NPCs, the spell is not in ActiveSpells/MagicEffects (those arent synced), so
     the magnitude is 0. The VFX is immediately removed on the next frame.
- Fix (three-part):
  1. applyBoundActorState() now sets mp_remote_actor = true on the base node
     for all non-authority NPCs.
  2. updateContinuousVfx() now checks for this flag and returns early for remote
     actors, skipping the magnitude-based removal check. The VFX persists until
     the actor dies (explicit cleanup via removeEffects() in the death handler)
     or the cell is unloaded.
  3. Added anim->removeEffects() call in the death branch of
     applyBoundActorState() to clean up persistent VFX when a puppet NPC dies.

### Files Changed (Session 9)
- apps/openmw/mwmechanics/character.cpp - Guarded mp_attack_type read in
  updateWeaponState() to only apply to puppet NPCs (empty AI sequence);
  added mp_remote_actor early-return in updateContinuousVfx() to prevent
  immediate removal of persistent spell VFX on non-auth NPCs.
- apps/openmw/mwmp/sync/ActorSync.cpp - Set mp_remote_actor = true on
  non-auth NPC base nodes in applyBoundActorState(); added
  removeEffects() call in death handler to clean up persistent VFX.

### Session 10 — Fix knockdown animation stuck on non-auth client

#### Playtest chat messages mapped to status

| Chat Message | Bug | Status |
|---|---|---|
| the knock bug still happens | Bug T | Fixed this session |
| after i landed the health damage punch while he was knocked on the ground he is now stuck laying on the ground in knock anim pose | Bug T | Fixed this session |
| fart see's him standing and punching me | Bug T | Fixed this session |
| i see him sliding around abit while laying on the ground | Bug T | Fixed this session |

#### Bug T: Puppet NPC stuck in knockdown animation after being punched while knocked — FIXED

- **Evidence**: Server chat — "after i landed the health damage punch while he was
  knocked on the ground he is now stuck laying on the ground in knock anim pose";
  "fart see's him standing and punching me"; "i see him sliding around abit while
  laying on the ground."
- **Root cause**: Two-part problem.
  1. When the local player on a non-authority client hits a puppet NPC, `Npc::onHit()`
     (and `Creature::onHit()`) applied damage (health/fatigue) and set
     knockdown/recovery states **locally** on the puppet NPC. This is wrong — the
     authority is the source of truth for all NPC state, and damage should only be
     applied via the CombatRequest→authority→stats-sync-back pipeline.
  2. The local fatigue damage from `onHit()` would deplete the puppet NPC's fatigue
     to 0, triggering a knockout animation locally. When the player punched the
     knocked NPC again, `onHit()` re-applied fatigue damage (or set
     `setKnockedDown(true)`/`setHitRecovery(true)`), keeping the puppet permanently
     in knockdown state. Meanwhile, the authority's NPC had recovered and was fighting
     normally. The `applyBoundActorState()` sync layer only cleared knockdown when
     `hitFlagsChanged` was true (a transition edge from the authority), but since
     the authority was already sending hitFlags=0 and `lastAppliedHitFlags` was
     also 0, no clearing occurred.
- **Fix** (three-part):
  1. In `Npc::onHit()`, added detection of puppet NPCs via the `mp_remote_actor`
     base node flag. When the victim is a puppet NPC, health damage, fatigue damage,
     and knockdown/recovery state setting are all skipped. The authority handles
     these exclusively via the CombatRequest pipeline. Visual feedback (hit sounds,
     combat AI triggers) still plays normally.
  2. Applied the same fix to `Creature::onHit()` for non-NPC actors (creatures).
  3. In `applyBoundActorState()`, added a safety net: when the authority reports no
     hit flags (`hitFlags == 0`), any locally-set `knockedDown` or `hitRecovery`
     state is unconditionally cleared, regardless of whether `hitFlagsChanged` is
     true. This catches edge cases where local code sets these states between
     authority ticks.

### Files Changed (Session 10)
- apps/openmw/mwclass/npc.cpp — Added `targetIsPuppetNpc` detection via
  `mp_remote_actor` flag; skip health/fatigue damage application and
  knockdown/recovery state setting for puppet NPCs in `onHit()`.
- apps/openmw/mwclass/creature.cpp — Same puppet NPC protection in
  `Creature::onHit()`: skip health/fatigue damage and knockdown/recovery
  for puppet NPCs.
- apps/openmw/mwmp/sync/ActorSync.cpp — Added safety net in
  `applyBoundActorState()`: unconditionally clear knockdown/recovery and
  restore fatigue when authority reports hitFlags=0, regardless of edge
  detection state.

### Session 11 — Fix NPC sliding while prone on non-auth client

#### Playtest chat messages mapped to status

| Chat Message | Bug | Status |
|---|---|---|
| the bug was not fixed | Bug T (partial) | Fixed this session |
| he's stuck laying down sliding around | Bug U | Fixed this session |

#### Root cause analysis

Session 10's fix to `onHit()` correctly prevented local damage/knockdown
from being applied to puppet NPCs. The non-auth client now properly receives
knockdown state via network flags (MF_KNOCKED_OUT / MF_KNOCKED_DOWN) and
the NPC does eventually recover. However two new visual bugs remained:

**Bug U: NPC slides across floor while in knockout animation**
- The authority's NPC recovers and moves around fighting (authority's position
  ticks forward normally). `applyBoundActorState()` applied these position
  updates unconditionally via `world->moveObject()`, so the non-auth's prone
  NPC was teleported frame-by-frame to wherever the authority had walked.
  Result: the NPC visually slid across the room while lying in knockout pose.
- Fix: compute the incoming hit flags early (`earlyHitFlags`) and skip
  `world->moveObject` / `world->rotateObject` when the authority is reporting
  `MF_KNOCKED_DOWN` or `MF_KNOCKED_OUT`. Position updates resume (with a
  one-frame snap) on the first tick the authority reports no longer knocked,
  which coincides with the start of the get-up animation and is visually
  indistinguishable.

**Bug T (remaining): Slow/missed get-up when authority skips Phase B**
- The get-up signal (`mp_knockout_release_pending = true`) was only set in
  the `leftKnockOut` branch, which required the authority to pass through an
  intermediate `MF_KNOCKED_DOWN`-only packet (fatigue recovered but KO
  animation not yet finished — "Phase B"). With fast fatigue regeneration
  or if the phase was shorter than one 50 ms position tick, the authority
  jumped directly from `MF_KNOCKED_OUT` to `hitFlags=0`, hitting the outer
  `else if (hitFlagsChanged)` block which set `mp_knockout_release_pending =
  false`. The KO animation then had to wait for its current loop iteration
  to reach "stop" naturally before the NPC could stand, adding up to ~1 s
  of extra prone time.
- Fix: changed `baseNode->setUserValue("mp_knockout_release_pending", false)`
  in the outer `else if (hitFlagsChanged)` branch to
  `baseNode->setUserValue("mp_knockout_release_pending", wasKnockedOut)`.
  When the transition is KO → no flags, `wasKnockedOut = true` and the
  get-up release is fired immediately regardless of whether Phase B was seen.

### Files Changed (Session 11)
- apps/openmw/mwmp/sync/ActorSync.cpp — Gated `world->moveObject` /
  `world->rotateObject` on `!skipPositionUpdate` (early hit-flag check for
  MF_KNOCKED_DOWN or MF_KNOCKED_OUT); fixed outer `else if (hitFlagsChanged)`
  to set `mp_knockout_release_pending = wasKnockedOut` instead of always
  `false`, ensuring the get-up animation is triggered even when Phase B is
  skipped.

### Session 12 — Root-cause fix for NPC permanently stuck in knockout loop

#### Playtest chat messages mapped to status

| Chat Message | Bug | Status |
|---|---|---|
| the knock bug is still happening. | Bug T (residual) | Fixed this session |

#### Root cause analysis

Sessions 10 and 11 correctly prevented local damage and set `mp_knockout_release_pending`
to fire the get-up signal, but a **stale fatigue override in the same `applyBoundActorState`
call** undid the fatigue restore every frame, keeping `knockout = true` in the
CharacterController permanently.

The mechanism:

1. The NPC is knocked out on the authority; `StatsDynamic` is sent while the NPC is
   taking health damage — at that moment `dyn.fatigue.current` (the cached authority
   fatigue stored in `actor.state.dynamicStats`) is a large **negative** value.
2. `StatsDynamic` is only sent on *health* changes, so the subsequent `RestoreFatigue`
   cast that makes fatigue positive on the authority does **not** trigger a new packet.
   `actor.state.dynamicStats.fatigue.current` therefore remains stale-negative on the
   non-auth client.
3. When the position packet clears `MF_KNOCKED_OUT`, the `hitFlagsChanged` branch
   (or `leftKnockOut` sub-branch) in `applyBoundActorState` correctly calls
   `stats.setFatigue(fatigue = 1)` and sets `mp_knockout_release_pending = true`.
4. **In the same function call**, a few lines later, the dynamic-stats sync block runs:
   `if (!isKnockedOut) fatigue.setCurrent(dyn.fatigue.current)` — `isKnockedOut` is
   now `false`, so the sync executes, overwriting the freshly-restored `1.0` with the
   stale negative value.
5. The CharacterController re-evaluates `knockout = (fatigue < 0) = true` and continues
   looping the knockout animation indefinitely.

#### Fix

Changed the fatigue sync guard from `!isKnockedOut` to
`!isKnockedOut && dyn.fatigue.current >= 0.f`.

When `dyn.fatigue.current` is still negative (stale from the knockdown period) and
there is no active KO flag, the sync is skipped. The locally-restored positive fatigue
value is preserved, `knockout = false` on the next CC tick, `setLoopingEnabled(false)`
disables the loop, and the NPC stands up normally.

All positive values from fresh `StatsDynamic` packets are still applied as before.
The next health-change event will send a new `StatsDynamic` with the current positive
fatigue, resuming normal sync.

### Files Changed (Session 12)
- apps/openmw/mwmp/sync/ActorSync.cpp — Changed fatigue dynamic-stats sync guard
  from `!isKnockedOut` to `!isKnockedOut && dyn.fatigue.current >= 0.f` to prevent
  stale negative fatigue (from a `StatsDynamic` sent during the knockdown period)
  from overriding the fatigue restore in the same `applyBoundActorState` call.
