#!/usr/bin/env python3
"""Emit the mod ImGui table: a function-pointer table over the dear_bindings C API.

Reads third_party/dear_bindings/dcimgui.json (the metadata published with the
dear_bindings release that matches the pinned ImGui) and writes:

  openpete_imgui_table.h   SDK + engine: `typedef struct openpete_imgui_table`,
                           one function pointer per non-denylisted function in
                           JSON order, each under the function's own `#if`
                           conditionals, plus the entry count and the table
                           digest as macros.
  openpete_imgui_macros.h  SDK: one function-like macro per entry routing the
                           call through the bound table, so a mod source calls
                           `ImGui_X(...)` as written in ImGui's documentation
                           and links no `ImGui_*` symbol.
  openpete_imgui_guard.cpp Engine: one static wrapper per entry that drops the
                           call when it is made outside a mod UI section body,
                           counts the calls that go through, and forwards to
                           dcimgui.cpp; the engine table initialised from the
                           wrappers; the digest and entry count as constants.

The digest covers the canonical entry text (the return type, name and
parameter list of every entry, with its conditionals), which is what defines
the table's layout. Both sides carry it, so an object built
against a different ImGui is refused at load instead of calling through a
table whose slots have moved.

Emission rules the JSON forces:

  - A function-pointer parameter carries its own name inside `declaration`
    (`const char* (*getter)(void*, int)`), so the name is not appended.
  - An array parameter's `declaration` carries the bounds at the end
    (`float[2]`); the name goes before the brackets.
  - A variadic function is an entry like any other (a pointer to a variadic
    function is legal C), but its guard wrapper cannot forward `...`, so it
    forwards through the `va_list` twin dear_bindings emits alongside it. The
    twin is found by signature, not by name: the names are irregular
    (`ImGui_TreeNodeStr` -> `ImGui_TreeNodeV`).
"""

import argparse
import hashlib
import json
import os
import re
import sys

# Lifecycle and input calls a mod must not make. The engine owns the context,
# the frame, the allocator, the settings file and the input queue; a mod that
# steers any of them breaks the overlay for every other mod. The `dcimgui.h`
# declaration stays visible, so a mod calling one fails at its own compile
# with an undefined reference rather than at runtime.
DENY_EXACT = {
    # Context lifecycle.
    "ImGui_CreateContext", "ImGui_DestroyContext",
    "ImGui_SetCurrentContext", "ImGui_GetCurrentContext",
    # Frame lifecycle: the engine runs NewFrame/Render and hands the draw
    # data to the backend.
    "ImGui_NewFrame", "ImGui_EndFrame", "ImGui_Render", "ImGui_GetDrawData",
    # Allocator, settings and state storage.
    "ImGui_SetAllocatorFunctions", "ImGui_GetAllocatorFunctions",
    "ImGui_SetStateStorage",
    # Capture forcing: the engine decides whether the game receives input
    # from io.WantCapture*.
    "ImGui_SetNextFrameWantCaptureMouse",
    "ImGui_SetNextFrameWantCaptureKeyboard",
    # Clipboard, shell-open and renderer callbacks.
    "ImGui_GetPlatformIO",
    # The engine runs IMGUI_CHECKVERSION() itself.
    "ImGui_DebugCheckVersionAndDataLayout",
    # Font atlas mutation. ImFontAtlas_AddFont* stays: a mod may add a font
    # and the engine's atlas rebuild handles it.
    "ImFontAtlas_RemoveFont", "ImFontAtlas_Build",
}

DENY_PREFIX = (
    "ImGui_CreateContext",          # ...Ex
    "ImGui_DestroyContext",
    "ImGui_LoadIniSettings",        # ...FromDisk / ...FromMemory
    "ImGui_SaveIniSettings",        # ...ToDisk / ...ToMemory
    "ImFontAtlas_Clear",            # ...InputData / ...TexData / ...Fonts
)

# Input injection: the whole ImGuiIO event queue.
DENY_RE = (
    re.compile(r"^ImGuiIO_Add\w*Event$"),
    re.compile(r"^ImGuiIO_AddInputCharacter\w*$"),
    re.compile(r"^ImGuiIO_ClearInputKeys$"),
    re.compile(r"^ImGuiIO_ClearEventsQueue$"),
)

# Rendered as a comment at the top of the macro header so a mod author who
# hits an undefined reference sees why.
DENY_DOC = (
    "Context lifecycle: ImGui_CreateContext*, ImGui_DestroyContext*, "
    "ImGui_SetCurrentContext, ImGui_GetCurrentContext.",
    "Frame lifecycle: ImGui_NewFrame, ImGui_EndFrame, ImGui_Render, "
    "ImGui_GetDrawData.",
    "Allocator, settings and state storage: ImGui_SetAllocatorFunctions, "
    "ImGui_GetAllocatorFunctions, ImGui_LoadIniSettings*, "
    "ImGui_SaveIniSettings*, ImGui_SetStateStorage.",
    "Input injection and capture forcing: ImGuiIO_Add*Event, "
    "ImGuiIO_AddInputCharacter*, ImGuiIO_ClearInputKeys, "
    "ImGuiIO_ClearEventsQueue, ImGui_SetNextFrameWantCaptureMouse, "
    "ImGui_SetNextFrameWantCaptureKeyboard.",
    "Platform IO (clipboard, shell-open, renderer callbacks): "
    "ImGui_GetPlatformIO.",
    "Font atlas mutation: ImFontAtlas_Clear*, ImFontAtlas_RemoveFont, "
    "ImFontAtlas_Build. ImFontAtlas_AddFont* stays.",
    "Version check: ImGui_DebugCheckVersionAndDataLayout.",
)


def denied(name):
    return (name in DENY_EXACT
            or name.startswith(DENY_PREFIX)
            or any(rx.match(name) for rx in DENY_RE))


def cond_open(conds):
    out = []
    for c in conds:
        kind = c["condition"]
        expr = c["expression"]
        if kind == "ifdef":
            out.append("#ifdef " + expr)
        elif kind == "ifndef":
            out.append("#ifndef " + expr)
        else:
            out.append("#if " + expr)
    return out


def is_varargs(fn):
    return any(a.get("is_varargs") for a in fn["arguments"])


def param_decl(arg):
    """One parameter as it appears in a declaration."""
    if arg.get("is_varargs"):
        return "..."
    t = arg["type"]
    decl = t["declaration"]
    name = arg["name"]
    if t["description"]["kind"] == "Type":
        # Function pointer: the declaration already spells the name.
        return decl
    if arg.get("is_array"):
        i = decl.rindex("[")
        return decl[:i] + " " + name + decl[i:]
    return decl + " " + name


def param_list(fn):
    args = fn["arguments"]
    if not args:
        return "void"
    return ", ".join(param_decl(a) for a in args)


def arg_names(fn):
    """Names to forward, in order, excluding a trailing `...`."""
    out = []
    for a in fn["arguments"]:
        if a.get("is_varargs"):
            continue
        out.append(a["name"])
    return out


def ret_decl(fn):
    return fn["return_type"]["declaration"]


def find_va_twins(functions):
    """Map each variadic function to its va_list counterpart.

    The C names are irregular (`ImGui_TreeNodeStr` -> `ImGui_TreeNodeV`,
    `ImGuiTextBuffer_appendf` -> `ImGuiTextBuffer_appendfv`), so the match runs
    on the C++ name the binding was generated from: the twin is the same class
    member with a `V`/`v` suffix. The signature is compared as well, since two
    members can share a suffix stem (`TreeNodeV` covers both the string and the
    pointer overload) and two unrelated functions can share a signature
    (`ImGui_Text` and `ImGui_LogText`)."""
    candidates = {}
    for fn in functions:
        args = fn["arguments"]
        if not args:
            continue
        last = args[-1]
        if last.get("is_varargs") or "type" not in last:
            continue
        if last["type"]["declaration"] != "va_list":
            continue
        key = (fn.get("original_class"), fn["original_fully_qualified_name"])
        sig = (ret_decl(fn), tuple(param_decl(a) for a in args[:-1]))
        candidates.setdefault(key, []).append((sig, fn["name"]))

    twins = {}
    for fn in functions:
        if not is_varargs(fn):
            continue
        sig = (ret_decl(fn),
               tuple(param_decl(a) for a in fn["arguments"][:-1]))
        orig = fn["original_fully_qualified_name"]
        found = None
        for suffix in ("V", "v"):
            key = (fn.get("original_class"), orig + suffix)
            for cand_sig, cand_name in candidates.get(key, []):
                if cand_sig == sig:
                    found = cand_name
                    break
            if found:
                break
        if found is None:
            raise SystemExit(
                "gen_imgui_table: no va_list twin for variadic %s" % fn["name"])
        twins[fn["name"]] = found
    return twins


def entry_text(fn):
    """The canonical one-line form of a table entry; the digest covers these."""
    return "%s (*%s)(%s);" % (ret_decl(fn), fn["name"], param_list(fn))


def banner(path):
    return ("/* Generated by tools/gen_imgui_table.py from\n"
            " * third_party/dear_bindings/dcimgui.json. Do not edit.\n"
            " * %s\n */\n" % os.path.basename(path))


def emit_table(entries, digest, out_path):
    n = len(entries)
    lines = [banner(out_path),
             "#ifndef OPENPETE_IMGUI_TABLE_H",
             "#define OPENPETE_IMGUI_TABLE_H",
             "",
             "#include <stdint.h>",
             "",
             "/* One slot per callable ImGui C function, in dcimgui.json",
             " * order. Entries under a conditional exist only when both sides",
             " * of the table are compiled with the same defines; the digest",
             " * and sizeof() checks at load catch any mismatch. */",
             "typedef struct openpete_imgui_table {"]
    for fn in entries:
        conds = cond_open(fn.get("conditionals", []))
        lines.extend(conds)
        lines.append("    " + entry_text(fn))
        lines.extend(["#endif"] * len(conds))
    lines.append("} openpete_imgui_table_t;")
    lines.append("")
    lines.append("/* Entries emitted by the generator, conditionals included. */")
    lines.append("#define OPENPETE_IMGUI_TABLE_ENTRIES %du" % n)
    lines.append("")
    lines.append("/* SHA-256 of the canonical entry text (return type, name and")
    lines.append(" * parameter list of every entry, with its conditionals). */")
    body = ", ".join("0x%02x" % b for b in digest)
    lines.append("#define OPENPETE_IMGUI_TABLE_SHA256_INIT { %s }" % body)
    lines.append("")
    lines.append("#endif /* OPENPETE_IMGUI_TABLE_H */")
    return "\n".join(lines) + "\n"


def emit_macros(entries, out_path):
    lines = [banner(out_path),
             "#ifndef OPENPETE_IMGUI_MACROS_H",
             "#define OPENPETE_IMGUI_MACROS_H",
             "",
             "/* Every callable ImGui_* name routes through the table the",
             " * loader binds, so a mod links no ImGui symbol. Cost: a mod",
             " * cannot take the address of an ImGui_* function; it reads the",
             " * table slot instead.",
             " *",
             " * Not in the table, and so not redefined here — a call to one",
             " * fails at the mod's own compile with an undefined reference:"]
    for line in DENY_DOC:
        lines.append(" *   - " + line)
    lines.extend([" */", ""])
    for fn in entries:
        conds = cond_open(fn.get("conditionals", []))
        lines.extend(conds)
        name = fn["name"]
        if not fn["arguments"]:
            lines.append("#define %s() (openpete_imgui_tbl->%s())" % (name, name))
        else:
            lines.append("#define %s(...) (openpete_imgui_tbl->%s(__VA_ARGS__))"
                         % (name, name))
        lines.extend(["#endif"] * len(conds))
    lines.extend(["", "#endif /* OPENPETE_IMGUI_MACROS_H */"])
    return "\n".join(lines) + "\n"


# Per-function prologue for the guard wrapper: the named window-flags
# parameter gains ImGuiWindowFlags_NoInputs while an always-section body is
# drawing with the dev overlay closed. NoInputs is per window (no mouse, no
# nav, no nav focus, and hovered tests skip the window), so it belongs on the
# call that creates the window; a global config flag would also silence the
# engine's own panels, which stay interactive with the overlay closed.
NO_INPUT_FLAG_ARG = {
    "ImGui_Begin":        "flags",
    "ImGui_BeginChild":   "window_flags",
    "ImGui_BeginChildID": "window_flags",
}


def emit_guard(entries, twins, digest, out_path):
    hooked = set()
    lines = [banner(out_path),
             "",
             "#include <stdarg.h>",
             "#include <stdint.h>",
             "",
             "#include \"dcimgui.h\"",
             "#include \"openpete_imgui_table.h\"",
             "#include \"openpete_imgui_guard.h\"",
             "",
             "/* Value-initialised return for a dropped call. The wrappers name",
             " * the return type as a type-id, which covers pointers and the",
             " * by-value struct returns (ImVec2, ImVec4) alike. */",
             "template <typename T> static inline T opig_zero(void)",
             "{",
             "    T v{};",
             "    return v;",
             "}",
             ""]

    for fn in entries:
        conds = cond_open(fn.get("conditionals", []))
        lines.extend(conds)
        name = fn["name"]
        ret = ret_decl(fn)
        params = param_list(fn)
        is_void = (ret.strip() == "void")
        drop = "return;" if is_void else "return opig_zero<%s>();" % ret
        lines.append("static %s opig_%s(%s)" % (ret, name, params))
        lines.append("{")
        lines.append("    if (!openpete_imgui_body_active()) {")
        lines.append("        openpete_imgui_guard_refused(\"%s\");" % name)
        lines.append("        " + drop)
        lines.append("    }")
        lines.append("    openpete_imgui_count_call();")
        flag_arg = NO_INPUT_FLAG_ARG.get(name)
        if flag_arg:
            if flag_arg not in arg_names(fn):
                raise SystemExit(
                    "gen_imgui_table: %s has no parameter %r for the "
                    "NoInputs hook (dear_bindings renamed it)"
                    % (name, flag_arg))
            lines.append("    if (openpete_imgui_always_closed())")
            lines.append("        %s |= ImGuiWindowFlags_NoInputs;" % flag_arg)
            hooked.add(name)
        if is_varargs(fn):
            names = arg_names(fn)
            last = names[-1]
            twin = twins[name]
            lines.append("    va_list opig_args;")
            lines.append("    va_start(opig_args, %s);" % last)
            call = "%s(%s)" % (twin, ", ".join(names + ["opig_args"]))
            if is_void:
                lines.append("    %s;" % call)
                lines.append("    va_end(opig_args);")
            else:
                lines.append("    %s opig_r = %s;" % (ret, call))
                lines.append("    va_end(opig_args);")
                lines.append("    return opig_r;")
        else:
            call = "%s(%s)" % (name, ", ".join(arg_names(fn)))
            lines.append("    %s%s;" % ("" if is_void else "return ", call))
        lines.append("}")
        lines.extend(["#endif"] * len(conds))
        lines.append("")

    # Every window-opening entry must have taken the hook. A denylist entry
    # or an upstream rename would otherwise drop the no-input rule silently,
    # and a mod's always window would start eating the game's mouse.
    missing = sorted(set(NO_INPUT_FLAG_ARG) - hooked)
    if missing:
        raise SystemExit(
            "gen_imgui_table: no table entry for %s, so the NoInputs hook "
            "was not emitted; update NO_INPUT_FLAG_ARG to the current "
            "dear_bindings names" % ", ".join(missing))

    lines.append("extern \"C\" const openpete_imgui_table_t "
                 "openpete_imgui_engine_table;")
    lines.append("const openpete_imgui_table_t openpete_imgui_engine_table = {")
    for fn in entries:
        conds = cond_open(fn.get("conditionals", []))
        lines.extend(conds)
        lines.append("    .%s = opig_%s," % (fn["name"], fn["name"]))
        lines.extend(["#endif"] * len(conds))
    lines.append("};")
    lines.append("")
    lines.append("extern \"C\" const uint32_t openpete_imgui_table_entries;")
    lines.append("const uint32_t openpete_imgui_table_entries = %du;"
                 % len(entries))
    lines.append("")
    lines.append("extern \"C\" const uint8_t openpete_imgui_table_sha256[32];")
    lines.append("const uint8_t openpete_imgui_table_sha256[32] =")
    lines.append("    OPENPETE_IMGUI_TABLE_SHA256_INIT;")
    return "\n".join(lines) + "\n"


def write_if_changed(path, text):
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            if f.read() == text:
                return False
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", required=True,
                    help="third_party/dear_bindings/dcimgui.json")
    ap.add_argument("--out-dir", required=True)
    args = ap.parse_args()

    with open(args.json, "r", encoding="utf-8") as f:
        meta = json.load(f)
    functions = meta["functions"]

    entries = [fn for fn in functions if not denied(fn["name"])]
    twins = find_va_twins(functions)

    h = hashlib.sha256()
    for fn in entries:
        for c in fn.get("conditionals", []):
            h.update(("%s %s\n" % (c["condition"], c["expression"])).encode())
        h.update((entry_text(fn) + "\n").encode())
    digest = h.digest()

    table_h = os.path.join(args.out_dir, "openpete_imgui_table.h")
    macros_h = os.path.join(args.out_dir, "openpete_imgui_macros.h")
    guard_cpp = os.path.join(args.out_dir, "openpete_imgui_guard.cpp")

    write_if_changed(table_h, emit_table(entries, digest, table_h))
    write_if_changed(macros_h, emit_macros(entries, macros_h))
    write_if_changed(guard_cpp, emit_guard(entries, twins, digest, guard_cpp))

    sys.stderr.write("[imgui-table] %d entries (%d denied), digest %s\n"
                     % (len(entries), len(functions) - len(entries),
                        digest.hex()[:16]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
