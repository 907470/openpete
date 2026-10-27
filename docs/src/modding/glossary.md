# Glossary

| Term | Meaning |
|---|---|
| moby | The game's own name for a movable world object: Spyro (sometimes), a gem, an enemy, a chest, a 3D HUD element. |
| class | A moby's type, `m_Class`. It selects the model the moby draws with and the code that runs it, and it is the key most overrides and material selectors are written against. |
| tick | One simulation step, 29.913 per second (NTSC 1125000/37609). Gameplay state advances only on a tick, so a mod that changes what the game does works in tick context. |
| VBL | The PSX vertical blank, 59.826 Hz, two per tick. The CLI word "frame" means VBL: `--frame-limit` counts these. |
| present | One drawn frame. How many presents a tick gets depends on the machine and on `--render-fps`, so a present count is never a contract. |
| canonical, sub-tick | The scene extracted at the tick itself is canonical. Scenes drawn between two ticks are sub-tick and carry `alpha`, how far past the tick they sit, from 0 up to 1. |
| extraction | The walk that turns guest state into draw data, once per scene. Refine callbacks run here. |
| guest RAM | The game's 2 MB of PSX memory, held as a byte array in the process. Writing it changes gameplay; everything else a mod does cannot. |
| vaddr | A PSX virtual address, the `0x80...` form the game uses internally. `api->guest(vaddr)` turns one into a host pointer. Guest structs store vaddrs, never host pointers. |
| arena | Guest memory a mod owns, from `api->guest_alloc`. It is timeline state: savestates, rewind and runahead carry and restore it like any other guest bytes, which host statics are not. |
| DMA-reachable pool | The part of the arena below physical 16 MiB, guest 0x80200000 to 0x80FFFFFF, 14 MiB. The GPU follows packet links through 24-bit addresses, so packets, primitives and ordering tables live here. Every `guest_alloc` call without `OP_GALLOC_NO_DMA` takes a block from it. |
| general pool | The part of the arena above physical 16 MiB, guest 0x81000000 to 0x9EFFFFFF, 480 MiB. A `guest_alloc` call with `OP_GALLOC_NO_DMA` (api 13) takes a block from it; nothing may truncate an address inside such a block to 24 bits. |
| tick context, present context | Where a call runs. Guest RAM writes, `api->call`, `guest_alloc` and registration are legal in tick context and refused in present context, which covers present hooks and UI section bodies. |
| override, `base()` | A mod's function that replaces a game function, and the call that runs the next override in the chain and then the original. Where `base()` sits in the body makes the override a pre-hook, a post-hook or a replacement. |
| mutative | A mod that writes guest RAM, declared as `sim_mutative` in the manifest. Each save (Spyro 1) or memory card (Spyro 2 and 3) is tagged with the mutative set that wrote it. |
| save record | A mod's own data for one game save, kept by the engine in `<card>.modsave` beside the card and delivered back when that save is loaded (`api->save_register`, api 13, Spyro 1). See [Save records](save-records.md). |
| savestate, rewind, runahead | Timeline features: capture and restore a moment, step backwards, speculatively run ticks ahead. Each needs the mod to declare how its own state behaves (`state`, `runahead`), and each is refused for an undeclared mod. |
