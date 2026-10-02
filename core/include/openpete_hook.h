#pragma once
/* openpete_hook.h - the OPENPETE_HOOK decomp seam.
 *
 * Once a Spyro function is decompiled to matching C, its render->gameplay
 * interception can no longer be an address-keyed recompiler substitution
 * (there is no recompiled instruction to key on). Instead the hook lives
 * as a macro at the matching source location:
 *
 *     int func_80022A2C(...) {
 *         ...
 *         OPENPETE_HOOK(cone_gate, cpu, mac1, mac3);   // <- seam
 *         ...
 *     }
 *
 * The macro has two expansions, selected by OPENPETE_BUILD:
 *
 *   - MATCHING / decomp build (OPENPETE_BUILD undefined): expands to a
 *     no-op expression statement, so the compiled object code is
 *     byte-identical to the un-hooked source - the spyro-1 match is
 *     preserved. (Same conditional shape as the existing
 *     -DOPENPETE_SKIP_DECOMP gating.)
 *
 *   - OPENPETE build (OPENPETE_BUILD defined): expands to a call to the
 *     named host hook `openpete_hook_<name>(args...)`, a normal C function
 *     a feature module declares + defines. This is the decomp-side analogue
 *     of the recompiler's AppendCall/ReplaceCall primitives: the same
 *     verdict_fn the manifest names, now reachable from matched C.
 *
 * One mechanism, three payoffs: preserves the byte-match, lets us patch a
 * decomped function, and carries hooks into custom-compiled-from-source
 * builds (the compiler may move addresses; the macro stays in the right
 * semantic place). */

#ifdef OPENPETE_BUILD
#  define OPENPETE_HOOK(name, ...) openpete_hook_##name(__VA_ARGS__)
#else
#  define OPENPETE_HOOK(name, ...) ((void)0)
#endif
