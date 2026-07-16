# Multiplayer quest journal persistence

The multiplayer server persists quest journal entries and quest indices against
the character that originally produced each change. `Config.JOURNAL_SHARING`
controls which persisted character records are combined and sent to a player:

- `"player"` (default) loads only the selected character's journal.
- `"group"` combines journals for members of the selected character's named
  `Config.JOURNAL_GROUPS` group. A character not assigned to a group remains
  player-specific.
- `"server"` combines journals produced by every character in the database.

Because records retain their originating character, changing the sharing mode
does not delete, duplicate, or reassign stored journal data.

## Configuring groups

Groups are keyed by an operator-chosen name. Members use the authenticated
account name and can optionally restrict membership to one character slot:

```lua
Config.JOURNAL_SHARING = "group"
Config.JOURNAL_GROUPS = {
    fellowship = {
        { account = "alice", character = "Nerevar" },
        { account = "bob" }, -- every character on bob's account
    },
}
```

A member should appear in no more than one group. Group names are sorted before
membership is resolved, making accidental duplicate assignments deterministic,
but the configuration should still be corrected.

## MWScript behavior

Only the journal result is synchronized. The receiving client directly applies
the resolved entry text, timestamp, and quest index; it does not execute the
originating `Journal` or `SetJournalIndex` MWScript instruction. Arbitrary
MWScript global and local variables remain local unless another multiplayer
system explicitly synchronizes them.

Consequently, `"player"` mode never advances another player's quest journal.
`"group"` and `"server"` modes intentionally make `GetJournalIndex` observe the
shared quest state on recipients after a journal update arrives.

## Restore ordering

The server sends the authoritative journal snapshot before `CharacterData`, so
the client queues it while still outside the world. The snapshot is applied on
the first running-world update before new journal deltas are detected. Large
snapshots are chunked without displaying journal-entry notifications during
login; live shared additions display the normal notification once.

The current packet and database implementation covers quest entries and quest
indices. Dialogue topic-response history and read-book tracking use separate
multiplayer message IDs and are not part of this journal persistence policy.
