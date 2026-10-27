# Save records

A mod that keeps state for a playthrough (a randomized route, a collection
count) needs it back when the player loads the save it belongs to. From api
13 the engine stores one record per mod per save beside the memory card, in
`<card>.modsave`, and hands it back at each load. The card image is never
modified.

Save records are served on Spyro the Dragon. On Spyro 2 and Spyro 3,
`save_register` returns -1 until their save descriptors land; branch on its
return value.

## Registering

```c
#ifdef OPENPETE_MOD_SAVE_API
static int my_write(openpete_mod_t* self, const openpete_mod_save_id_t* id,
                    const void** data, uint32_t* size) {
    my_encode(g_state->buf, g_state);   /* g_state lives in the arena */
    *data = g_state->buf;
    *size = sizeof g_state->buf;
    return 0;                           /* -1 keeps the previous record */
}

static void my_read(openpete_mod_t* self, const openpete_mod_save_read_t* r) {
    if (r->status == OP_MOD_SAVE_OK || r->status == OP_MOD_SAVE_GAP ||
        r->status == OP_MOD_SAVE_STALE)
        my_decode(g_state, r->data, r->size, r->schema);
    else
        my_reset(g_state);              /* a fresh playthrough */
}
#endif

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    /* ... guest_alloc g_state ... */
#ifdef OPENPETE_MOD_SAVE_API
    if (api->api_version >= OPENPETE_MOD_SAVE_API)
        api->save_register(self, 1, my_write, my_read);
#endif
    return 0;
}
```

`schema` is the record's format version. Raise it when the format changes;
a record written by a higher schema is never overwritten by a lower one.

## When the callbacks run

- `write` runs once per non-empty save the game writes, on the sim thread
  in tick context. The engine copies the bytes on return; any size is
  accepted. A mod that is turned off or not installed when the game saves
  gets no call, and its previous record is carried into the new save.
- `read` runs at the start of the game's next update after the game loads
  a save or starts a new playthrough, and when a mod is turned on after
  either. It replaces all of the mod's save-derived state.
- Neither runs on a savestate load, rewind, runahead rollback, process
  handoff or hot reload: those restore the arena with the rest of the
  timeline. Keep save-derived state in the arena (`guest_alloc`); host
  statics are not restored.
- A mod's state at entry is the fresh-playthrough state. No `read` arrives
  until the game loads a save or starts a new game.
- Both callbacks may read guest RAM and write the arena. `api->call` is not
  available in either.

On Spyro 1 the load runs inside the game's update, so the `read` arrives on
the update after the load; overrides that run later in the load's own
update see the state from before it.

## Statuses

| Status | Data | Meaning |
| --- | --- | --- |
| `OP_MOD_SAVE_OK` | the record | The save carries this mod's record. |
| `OP_MOD_SAVE_ABSENT` | none | The save has no record from this mod, or a new playthrough started. Reset. |
| `OP_MOD_SAVE_GAP` | the record | The record is intact, but the game saved at least once since while this mod was off or not installed. |
| `OP_MOD_SAVE_STALE` | the playthrough's newest record | The save was changed outside OpenPete (played on a PlayStation or another emulator, edited), or its record was lost. |
| `OP_MOD_SAVE_NEWER` | none | The record was written by a higher schema. The engine keeps it and does not call `write` for this playthrough. |
| `OP_MOD_SAVE_LEGACY_PAD` | the save's 112 spare bytes (`SaveFile+0x590`), schema 0 | No record, and the loaded save's spare bytes are non-zero: data a pad-writing mod stored. |

Handle a status value the header does not list as `OP_MOD_SAVE_ABSENT`.

`OP_MOD_SAVE_LEGACY_PAD` is how a mod moves from the spare bytes to save
records: import the bytes only when they carry the mod's own magic. Bytes
without it are another mod's data and are imported as nothing, never
reported to the player as damage.

## Mods that still use the spare bytes

A mod that overrides `SaveCreate` or `SaveLoad` and writes the 112 spare
bytes of Spyro 1's `SaveFile` without calling `save_register` runs as
before. Two such mods overwrite each other's data: the one loaded last
wins, and the other reads foreign bytes at the next load. The loader logs a
warning when two or more mods override the same save function without save
records.

## Files

`<card>.modsave` sits beside the card image. A copy of the card needs its
`.modsave`, or every mod reads its saves as `OP_MOD_SAVE_ABSENT` (or
`OP_MOD_SAVE_LEGACY_PAD`). An unreadable `.modsave` is renamed to
`<card>.modsave.bad-<n>` and treated as absent.
