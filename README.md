Fetcher Simulator
=================

This repository is a modified fork of [OpenMW](https://gitlab.com/OpenMW/openmw) 0.51.0 focused on adding a native multiplayer client and dedicated server.

OpenMW is an open-source open-world RPG engine for playing Morrowind by Bethesda Softworks. You need to own Morrowind to use this project with Morrowind data.

This fork includes multiplayer work based on and derived from [TES3MP](https://github.com/TES3MP/TES3MP), adapted for a newer OpenMW codebase and a GameNetworkingSockets-based transport instead of TES3MP's original RakNet layer.

TES3MP has not had a public release since 2022, and upstream OpenMW multiplayer is still set for 2090, so after years of playing and scripting in TES3MP on and off, I decided to:

![Shia LaBeouf JUST DO IT over Fetcher Simulator gameplay](docs/media/fetcher-simulator-just-do-it.gif)

AI-assisted coding tools were used during development of this fork. Meatbags are responsible for reviewing, integrating, and distributing the resulting changes.

You can join the project Discord to follow development or help test the public live test server: https://discord.gg/wXqQeSWRZF

Project Status
--------------

This is an experimental multiplayer fork, not upstream OpenMW and not upstream TES3MP.

The project currently has a working dedicated server, multiplayer client integration, player login and character persistence, server-side Lua scripting, and a large ActorSync implementation for NPC/player/world replication. The most active technical work is still around actor lifecycle edge cases, authority handoff, combat/death/spell visual parity, persistence correctness, and multiplayer-specific tooling.

Current Scope
-------------

The current project scope is to make a playable OpenMW 0.51-based multiplayer fork with:

- a dedicated `openmw-server` executable;
- a GameNetworkingSockets client/server transport;
- direct connect and server-browser-oriented client UI;
- account, character, and key-link login flows;
- player position, appearance, inventory, equipment, stats, spells, chat, speech, death, and cell-change sync;
- actor authority, NPC position, animation/presentation, attack, death, and corpse persistence sync;
- server-authoritative world state for containers, doors, placed objects, dynamic records, spawned actors, and dead vanilla actors;
- multiplayer compatibility work for client-side Lua scripts and mods where their state or behavior needs to be reflected across clients;
- server-side Lua scripts and bindings for commands, admin UI services, spawners, mark/recall, and persistence helpers;
- Windows tester packaging and release workflow support.

What Is Done
------------

- OpenMW 0.51.0 has been forked and extended with multiplayer client code under `apps/openmw/mwmp`.
- A standalone server lives under `apps/openmw-server`.
- Shared multiplayer protocol structures and packets live under `components/openmw-mp`.
- GameNetworkingSockets is integrated as the main multiplayer transport.
- Client UI exists for direct connect, account/login, character selection, key linking, chat, and server-list style flows.
- Server persistence exists for accounts, characters, player state, placed world objects, containers, doors, dynamic records, spawned actors, and dead vanilla actor records.
- ActorSync v2 uses deterministic actor identities, identity acknowledgement, compact position snapshots, presentation snapshots, attack events, and authority handoff logic.
- Server-side Lua can handle gameplay commands, admin UI service calls, destructible spawners, persistence helpers, and cell reset flows.
- Windows build/release workflows and tester package support exist in `.github/workflows` and `scripts`.

Source Style Surfing
--------------------

This fork includes Source-style surfing mechanics as a separate gameplay layer from the core OpenMW movement rules. The goal is to support surf maps and surf-like movement where players slide along steep ramp surfaces, preserve speed through ramp transitions, and avoid normal ground friction taking over while the player is still in a valid surf run.

![Fetcher Simulator surfing gameplay](docs/media/fetcher-simulator-surfing.gif)

The current implementation includes:

- surf surface detection and slope-state tracking in the movement/physics path;
- server-provided settings so maps or server scripts can enable and tune surf behavior;
- seam-preservation logic for ramp boundaries and mixed walkable/surf triangle transitions;
- protection against surf-controlled downward correction being counted as normal fall height;
- multiplayer correction handling intended to keep surf movement usable while the server remains authoritative.

Surfing is still an active tuning area. The remaining work is mostly around edge cases: server position corrections during ramp transitions, consistent classification near mixed triangle seams, and regression testing that confirms real falls still cause normal fall damage while valid surf motion does not.

Ongoing Work
------------

- ActorSync polish remains the main gameplay focus:
  - authority transitions between interiors/exteriors;
  - spawned actor movement after authority handoff;
  - animation and presentation smoothing;
  - NPC combat, knockout, death, spell, and hit-effect parity between authority and observer clients;
  - stale actor suppression, corpse bootstrap, and cross-cell actor recovery.
- Persistence is functional but still under active hardening for actor lifecycle and world-state edge cases.
- Destructible spawner support exists, but the spawner counter/state accounting still has known issues.
- Server browser support exists, but ping and public-list polish are incomplete.
- Admin/database tooling exists, but some UI and server-context actions are still being refined.
- Release packaging still needs a fuller third-party notice pass for bundled dependencies such as GameNetworkingSockets.

Future Work
-----------

- Broader multiplayer stress testing with more clients and longer sessions.
- Better automated regression tests for protocol, persistence, actor lifecycle, and Lua server scripts.
- More robust master-server/server-browser support.
- Build workflows for the dedicated server, plus client/server builds for other operating systems.
- Admin and moderation tooling suitable for public servers.
- Protocol compatibility and migration handling for saved server databases.
- Better compatibility paths for client-side Lua scripts and mods, including multiplayer-safe state sync and server mediation where needed.
- Quest sync for journal state, quest progression, dialogue outcomes, and related world-state changes.
- More complete docs for hosting, packaging, release, and gameplay configuration.
- Final third-party license/notice packaging for binary releases.

Project Layout
--------------

- `apps/openmw/mwmp` - multiplayer client integration, UI, networking, and sync systems.
- `apps/openmw-server` - dedicated server, Lua context, database, admin HTTP server, and server scripts.
- `components/openmw-mp` - shared multiplayer base structs, packet types, serialization, and protocol definitions.
- `apps/openmw-server/scripts` - server-side Lua gameplay/admin scripts.

Credits
-------

This project is based on [OpenMW](https://gitlab.com/OpenMW/openmw), an open-source engine for Morrowind.

Multiplayer work in this fork is based on and derived from [TES3MP](https://github.com/TES3MP/TES3MP), which added multiplayer functionality to OpenMW.

Copyright (c) 2016-2022, David Cernat & Stanislav Zhukov

GameNetworkingSockets is used for multiplayer transport:

- [ValveSoftware/GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets)

Thanks to Panzer and Arblarg for allowing their surf maps to be ported for this project.

See `AUTHORS.md` for OpenMW contributor credits.

License
-------

This project is distributed under GPLv3 with additional terms applicable to TES3MP-derived material. See `LICENSE`.

Font licenses and other bundled third-party notices remain in their existing files, including:

- `files/data/fonts/DejaVuFontLicense.txt`
- `files/data/fonts/DemonicLettersFontLicense.txt`
- `files/data/fonts/MysticCardsFontLicense.txt`
- `extern/GameNetworkingSockets/LICENSE`

Binary/package distributions should include the applicable license and notice files for OpenMW, TES3MP-derived material, GameNetworkingSockets, and other bundled dependencies.
