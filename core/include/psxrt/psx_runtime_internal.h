#ifndef PSX_RUNTIME_INTERNAL_H
#define PSX_RUNTIME_INTERNAL_H

/**
 * @file psx_runtime_internal.h
 * @brief Functions and data the recompiled game code calls or reads in the engine, beyond ::CPUState.
 *
 * Every generated C file includes this header. The engine sources that
 * define these symbols or call into the generated code include it too.
 * It is staged with the first-boot module source kit and is not shipped
 * in the mod SDK, which carries psx_runtime.h alone. Like psx_runtime.h it
 * includes only `<stdint.h>`.
 */
#include "psx_runtime.h"

/**
 * @brief Storage-class prefix for an engine data symbol the recompiled module reads or writes.
 *
 * A Windows module build (`OPENPETE_RECOMP_MODULE_BUILD` defined) marks
 * the symbol `__declspec(dllimport)` so the module reaches it through the
 * executable's import table; every other build expands it to nothing.
 */
#if defined(_WIN32) && defined(OPENPETE_RECOMP_MODULE_BUILD)
#  define OPENPETE_ABI_DATA __declspec(dllimport)
#else
#  define OPENPETE_ABI_DATA
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Call the guest function at a computed address: a `jr`/`jalr` target or a BIOS vector.
 *
 * The generated code calls it where the target is a register value. The
 * address resolves through the loaded recompiled module's dispatch, then
 * the BIOS service tables at 0xA0, 0xB0 and 0xC0. An address that resolves
 * nowhere is logged, `v0` and `v1` are set to 0, and the game fiber yields
 * to the host without counting a VBL.
 * @param cpu The caller's CPU state.
 * @param addr The guest address called. */
void call_by_address(CPUState* cpu, uint32_t addr);

/** @brief Entry check the generated code runs at the start of every recompiled function.
 *
 * Runs the engine's per-entry hooks (hang watchdog, pad-read probe,
 * halt-at-PC dump, oracle anchors, the oracle's clock replay) and then the
 * registered game's override (psx_set_override_dispatch_game), when it has one.
 * @param cpu The CPU state at function entry.
 * @param addr The guest address of the function entered.
 * @return Nonzero when a hook handled the call and the recompiled body must not run. */
int psx_override_dispatch(CPUState* cpu, uint32_t addr);

/** @brief Install the per-game arm psx_override_dispatch runs after the engine's own hooks.
 *
 * openpete_game_runtime_register installs the registered descriptor's
 * override_dispatch; NULL runs no game arm.
 * @param fn Returns nonzero when the recompiled body must not run. */
void psx_set_override_dispatch_game(int (*fn)(CPUState* cpu, uint32_t addr));

/** @brief Return the registered game's override dispatch.
 *
 * @return The function psx_set_override_dispatch_game installed, or NULL. */
int (*psx_override_dispatch_game(void))(CPUState* cpu, uint32_t addr);

/** @brief Handle a recompiled `syscall` instruction.
 *
 * `a0` = 1 (EnterCriticalSection) masks interrupts and returns in `v0` 1
 * when they were enabled, else 0; `a0` = 2 (ExitCriticalSection) unmasks
 * them and delivers a VBLANK latched while they were masked. Any other
 * function is logged and changes no state.
 * @param cpu The caller's CPU state.
 * @param code Bits 6 to 25 of the instruction word. */
void psx_syscall(CPUState* cpu, uint32_t code);

/** @brief Count one iteration of a polling loop and, once a VBL's worth of guest instructions has accumulated, offer a VBL.
 *
 * The recompiler emits the call on the back-edge of a loop whose body
 * stores nothing, calls only pure leaves and loads, on the back-edge of
 * a device-status poll: a store-free body whose one call returns the v0 the
 * branch tests, into a call closure that reaches a device-status thunk and
 * never VSync, and on the back-edge of a call-carrying spin: a store-free
 * body that carries no register across iterations and calls only
 * functions whose call closure never reaches VSync (kernel calls allowed),
 * at least one of which makes a kernel call.
 * A call-carrying spin's iteration adds its count only when the previous
 * poll was its own and no VBL arrived since; any other iteration adds
 * nothing. Each call adds the body's static instruction count; at one
 * VBL of instructions at one instruction per cycle (33868800 x 37609 /
 * 2250000) a quantum is reached. Under the oracle's schedule the quantum
 * delivers only a VBLANK the reference shows taken before its next row; off
 * it, a pure spin and a device-status poll receive a VBL, a counted poll
 * none. Every register is preserved. The count restarts at every VBL.
 * @param cpu The looping function's CPU state.
 * @param header The loop's header address.
 * @param insns The body's instruction count, its pure leaves' included; a
 *        device-status poll counts its body alone.
 * @param pure The site kind: 1 for a pure spin (no register carries a value
 *        across iterations except through a load), 2 for a device-status
 *        poll, 3 for a call-carrying spin, 4 for a deferred call-carrying
 *        spin the resident overlay answered, 5 for one it did not (counted
 *        in the census, adds nothing), 0 for a counted poll. */
void psx_loop_poll(CPUState* cpu, uint32_t header, uint32_t insns, uint32_t pure);

/** @brief Poll a call-carrying spin whose callee lies in the overlay window, deciding its closure against the resident overlay.
 *
 * The main EXE is translated before any overlay, so a loop calling an
 * overlay function by constant address carries the callee here. The
 * resident overlay's quiet table (psx_quiet_<name>) answers whether the
 * callee's call closure is quiet, VSync-free and makes a kernel call, and
 * its cheapest return cost; an answered poll is psx_loop_poll kind 4 with that cost added, any
 * other kind 5.
 * @param cpu The looping function's CPU state.
 * @param header The loop's header address.
 * @param insns The body's instruction count without the deferred callee.
 * @param callee The overlay function the body calls. */
void psx_loop_poll_deferred(CPUState* cpu, uint32_t header, uint32_t insns, uint32_t callee);

/** @brief Execute one GTE command against the GTE register banks in @p cpu.
 *
 * Every recompiled `cop2` command instruction calls it; core/psx/gte.c
 * defines it.
 * @param cpu CPU state whose gte_data and gte_ctrl the command reads and writes.
 * @param cmd The instruction's low 25 bits: the command number and its sf, lm, mx, v and cv fields. */
void gte_execute(CPUState* cpu, uint32_t cmd);

/**
 * @brief Guest address of the `cop2` instruction that issued the latest gte_execute call; 0 when unknown.
 *
 * The generated code stores the address immediately before each
 * gte_execute call; the GTE instruments attribute commands with it.
 */
OPENPETE_ABI_DATA extern uint32_t openpete_gte_caller_pc;

/**
 * @brief Guest base address of Spyro 1's g_SonyImage scratch pool.
 *
 * Core defines it once; openpete_game_runtime_register sets it to the
 * registered descriptor's sony_image_base, the pool's address in that
 * game's executable, before any recompiled code runs. The recompiler
 * replaces every `lui`/`addiu` address pair that lands inside the window
 * the game's recomp.conf SONY_IMAGE_WINDOW names with this base plus the
 * pair's offset into it; a game with no window never reads it.
 */
OPENPETE_ABI_DATA extern uint32_t psx_runtime_g_sony_image_base;

/**
 * @name Moby cull hooks
 * Calls the recompiler inserts into the three moby render passes of
 * Spyro 1 (regular, specular, shaded). They record the 4:3 verdict of the
 * game's own cone and distance tests, so the `m_WasDrawn` byte the game
 * later reads takes the value it takes on hardware whatever the display
 * aspect.
 * @{ */

/** @brief Record a failed distance test when a box-cull lower-bound value is zero or negative.
 *
 * Called after the `add` of each lower-bound comparison; the game culls
 * the moby when the result is at most 0.
 * @param v The `add` instruction's destination register value. */
void psx_runtime_dist_gate_lower(int32_t v);

/** @brief Record a failed distance test when a box-cull upper-bound value is zero or positive.
 *
 * Called after the `sub` of each upper-bound comparison and after the
 * projected-Z comparison that follows MVMVA; the game culls the moby when
 * the result is at least 0.
 * @param v The `sub` instruction's destination register value. */
void psx_runtime_dist_gate_upper(int32_t v);

/** @brief Record the regular and specular passes' 4:3 cone verdict and return `mac1_abs - a2`.
 *
 * The moby is inside the 4:3 cone when 4 * (mac1_abs - a2) is less than
 * 3 * (mac3 + a3).
 * @param mac1_abs Absolute value of the GTE's MAC1 result.
 * @param a2 The moby's horizontal radius.
 * @param mac3 The GTE's MAC3 result.
 * @param a3 The bias added to @p mac3.
 * @return `mac1_abs - a2`, the value the game's next instruction consumes. */
int32_t psx_runtime_cone_widen_permoby(int32_t mac1_abs, int32_t a2,
                                       int32_t mac3, int32_t a3);

/** @brief Record the shaded pass's 4:3 cone verdict and return `mac1_abs - 102`.
 *
 * The shaded pass uses the constant radius 102 and bias 77; the moby is
 * inside the 4:3 cone when 4 * (mac1_abs - 102) is less than
 * 3 * (mac3 + 77).
 * @param mac1_abs Absolute value of the GTE's MAC1 result.
 * @param mac3 The GTE's MAC3 result.
 * @return `mac1_abs - 102`, the value the game's next instruction consumes. */
int32_t psx_runtime_cone_widen_shaded(int32_t mac1_abs, int32_t mac3);

/** @brief Store the `m_WasDrawn` byte at the start of a moby's iteration and mark both verdicts passing.
 *
 * Replaces the game's `sb` that clears the byte. Resetting the verdicts
 * here keeps a moby that skips the cone test (a billboard, an
 * extended-radius moby) from inheriting the previous moby's result.
 * @param cpu The guest CPU state.
 * @param addr Guest address of the `m_WasDrawn` byte.
 * @param val The register value the `sb` stores; its low byte is written. */
void psx_runtime_mwasdrawn_clear_store(CPUState* cpu, uint32_t addr, uint32_t val);

/** @brief Store the `m_WasDrawn` byte only when the moby passed both the 4:3 cone and the distance test.
 *
 * Replaces the game's `sb` that marks the moby drawn.
 * @param cpu The guest CPU state.
 * @param addr Guest address of the `m_WasDrawn` byte.
 * @param val The register value the `sb` stores; its low byte is written. */
void psx_runtime_mwasdrawn_store(CPUState* cpu, uint32_t addr, uint32_t val);
/** @} */

/**
 * @name Moby vertex-stream address hooks
 * Replace the final shift of the game's frame-word field decode: the `srl`
 * of `sll r,r,11 ; srl r,r,11|10`, whose source holds the frame word
 * shifted left by 11, or the `sll` of `and r,w,0x1FFFFF ; sll r,r,1`,
 * whose source holds the masked field. The hook recovers the 21-bit field
 * and maps it through the vertex-stream handle table.
 * A field below the handle bound that names no bound handle yields 0;
 * a field at or above the bound is a plain guest address and passes
 * through with the value the `srl` produces.
 * @{ */

/** @brief Return the vertex-stream address the game's `srl rd, rt, 11` decode yields.
 *
 * @param rt The `srl` source register value.
 * @return The bound handle's guest address, 0 for an unbound handle, or `rt >> 11` for a plain address. */
uint32_t psx_runtime_vstream_moby(uint32_t rt);

/** @brief Return the vertex-stream address the game's `srl rd, rt, 10` decode yields.
 *
 * @param rt The `srl` source register value.
 * @return The bound handle's guest address, 0 for an unbound handle, or `rt >> 10` for a plain address. */
uint32_t psx_runtime_vstream_halved(uint32_t rt);

/** @brief Return the vertex-stream address the game's `and rd, w, 0x1FFFFF ; sll rd, rd, 1` decode yields.
 *
 * Replaces the `sll` of the mask-and form of Spyro's halved field decode
 * (func_80048D10, which builds the mask with `lui`/`ori`).
 * @param rt The `sll` source register value: the frame word masked to 21 bits.
 * @return The bound handle's guest address, 0 for an unbound handle, or `rt << 1` for a plain address. */
uint32_t psx_runtime_vstream_halved_field(uint32_t rt);
/** @} */

/**
 * @name Particle cull hooks
 * Calls the recompiler inserts into the Spyro 1 particle and sparkle
 * renderers. They test the screen X the GTE would have produced at 4:3
 * (the canonical-SX values below), so the particle drawn flag and the
 * sparkle kill decision match hardware whatever the display aspect.
 * @{ */

/** @brief Store the particle drawn byte (`m_03`) only when the particle passed the 4:3 screen-X test.
 *
 * Replaces the game's `sb` that marks the particle drawn.
 * @param cpu The guest CPU state.
 * @param addr Guest address of the flag byte.
 * @param val The register value the `sb` stores; its low byte is written. */
void psx_runtime_part_drawn_store(CPUState* cpu, uint32_t addr, uint32_t val);

/** @brief Record whether the second-latest projected vertex lies inside 0 < SX < 512 and return `a - b`.
 *
 * Used by the particle kinds that read the screen X between two RTPS
 * commands.
 * @param a The `sub` instruction's first operand.
 * @param b The `sub` instruction's second operand.
 * @return `a - b`. */
int32_t psx_runtime_part_cull_sx1(int32_t a, int32_t b);

/** @brief Record whether the latest projected vertex lies inside 0 < SX < 512 and return `a - b`.
 *
 * Used by the particle kinds that project one vertex.
 * @param a The `sub` instruction's first operand.
 * @param b The `sub` instruction's second operand.
 * @return `a - b`. */
int32_t psx_runtime_part_cull_sx2(int32_t a, int32_t b);

/** @brief Record whether any of the last three projected vertices lies inside 0 < SX < 512 and return `v << 16`.
 *
 * Used by the four-vertex strip particles.
 * @param v The register value the game shifts left by 16.
 * @return `v << 16`. */
int32_t psx_runtime_part_cull_4vert(int32_t v);

/** @brief Return the latest vertex's 4:3 screen X, shifted left by 16, for the sparkle clip test.
 *
 * Replaces the value the game shifts into `a1`, so the sparkle kill
 * decision and the g_Sparkles slot use do not depend on the display
 * aspect.
 * @return `(openpete_canonical_sx2_q16 >> 16) << 16`. */
int32_t psx_runtime_sparkle_canon_sx(void);
/** @} */

/**
 * @name Canonical screen X
 * The screen X of the last three vertices RTPS and RTPT projected, as the
 * GTE computes it at 4:3, in 16.16 fixed point. sx2 is the latest and sx0
 * the oldest, in the order of the GTE's SXY FIFO. Written by the GTE on
 * every projected vertex; the particle cull hooks and the widescreen
 * layer read them. A value is inside the 4:3 screen when it lies in
 * (0, 0x02000000).
 * @{ */
extern int64_t openpete_canonical_sx0_q16;
extern int64_t openpete_canonical_sx1_q16;
extern int64_t openpete_canonical_sx2_q16;
/** @} */

/** @brief Append one terrain vertex's world position and the camera to the file named by OPENPETE_ENVVERT_LOG.
 *
 * The recompiler inserts the call into the terrain vertex transform of
 * Spyro 1 (after the `lw $a0, 4($s1)` that loads a vertex's second
 * world word in func_800258F0). Each call writes one 48-byte record: the
 * host yield counter, the vertex pointer, the two packed world words, the
 * camera position and the nine shorts of g_Camera's view matrix. OPENPETE_ENVVERT_LOG_SKIP sets the first counter
 * value logged and OPENPETE_ENVVERT_LOG_FRAMES the count of counter values
 * (default 80) before the file closes. Reads guest RAM, writes none, and
 * returns at once when OPENPETE_ENVVERT_LOG is unset.
 * @param cpu The guest CPU state.
 * @param w0 First packed world word (`v0`).
 * @param w1 Second packed world word (`a0`).
 * @param s1 Guest address of the vertex.
 * @param s7 Camera X from +0x28 of the camera block, shifted right by 4.
 * @param t8 Camera Y from +0x2C, shifted right by 4.
 * @param t9 Camera Z from +0x30, shifted right by 4. */
void openpete_envvert_probe(void* cpu, uint32_t w0, uint32_t w1,
                            uint32_t s1, uint32_t s7, uint32_t t8,
                            uint32_t t9);

/**
 * @name Registered guest memory regions
 * A region maps a guest address range above the 2 MB of main RAM onto a
 * host buffer. The CPUState memory callbacks, the guest-pointer resolvers
 * and the savestate scans consult the table; a load or store the
 * generated code performs inline on main RAM does not. Gameplay code never
 * addresses a region; a build defining `OPENPETE_DEBUG_REGION_GUARD`
 * aborts when a canonical-RAM path receives a region address.
 * @{ */

/**
 * @brief Map a guest address range onto a host buffer.
 * @param psx_base Guest base address in kuseg, kseg0 or kseg1, with a physical address (`& 0x1FFFFFFF`) of at least 0x200000.
 * @param size Length in bytes.
 * @param host_buffer The host memory; the caller keeps ownership.
 * @return 0 on success; nonzero when @p size is 0, @p host_buffer is NULL,
 *         the range overlaps main RAM, the scratchpad or a registered
 *         region, or all 16 table slots are in use.
 */
int      psx_runtime_register_region(uint32_t psx_base,
                                     uint32_t size,
                                     void*    host_buffer);

/** @brief Extend a registered region in place to a larger size over the same host buffer.
 *
 * The caller has already backed the new bytes. The size is published with
 * a release store, so a lookup on another thread sees either extent and
 * never reads past backed memory.
 * @param psx_base The base the region was registered at, in any segment.
 * @param new_size The new length in bytes, at least the current one.
 * @return 0 on success; 3, 4 or 5 when the grown range would overlap main
 *         RAM, the scratchpad or another region; 6 for a smaller size; 7
 *         when no region is registered at @p psx_base. */
int      psx_runtime_grow_region(uint32_t psx_base, uint32_t new_size);

/** @brief Remove the region registered at a base address; does nothing when none is.
 *
 * Bases are compared by physical address, so any segment's spelling of
 * the base matches.
 * @param psx_base The base the region was registered at. */
void     psx_runtime_unregister_region(uint32_t psx_base);

/**
 * @brief Find the host pointer for a guest address inside a registered region.
 *
 * Only a KUSEG address below 0x20000000, a KSEG0 or a KSEG1 address
 * resolves; a KSEG2 address never does.
 * @param psx_addr The guest address.
 * @param out_host Receives the host pointer when not NULL.
 * @param out_remaining Receives the byte count to the region's end when not NULL.
 * @return 1 when @p psx_addr lies in a registered region, else 0.
 */
int      psx_runtime_resolve_region(uint32_t psx_addr,
                                    void**   out_host,
                                    uint32_t* out_remaining);

/**
 * @brief Find the guest address of a host pointer inside a registered region's buffer.
 * @param host The host pointer.
 * @param out_vaddr Receives the kseg0 guest address when not NULL.
 * @return 1 when @p host lies in a registered region's buffer, else 0.
 */
int      psx_runtime_region_host_to_vaddr(const void* host,
                                          uint32_t* out_vaddr);

/**
 * @brief Return the main-RAM offset of a guest address, `addr & 0x1FFFFF`.
 *
 * A build defining `OPENPETE_DEBUG_REGION_GUARD` prints the address and
 * aborts when it lies in a registered region; the call marks a site that
 * only handles main RAM.
 * @param addr The guest address.
 * @return The offset into main RAM.
 */
uint32_t psx_runtime_canonicalize(uint32_t addr);

/** @brief Return the number of registered regions, so a resolver can skip the table lookup when it is 0.
 *
 * @return The registered region count. */
uint32_t psx_runtime_region_count(void);
/** @} */

#ifdef __cplusplus
}
#endif

#endif /* PSX_RUNTIME_INTERNAL_H */
