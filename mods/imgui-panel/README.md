# imgui-panel

The Dear ImGui escape hatch (`openpete_imgui.h`) driving a real panel. The
eleven-widget vtable in `openpete_mod_ui.h` is the simple path; this mod is
what the other path looks like.

A tick hook — an override on `GamestateUpdate`, which runs every tick from
the title screen on — walks the level moby array, counts the live mobys
into a host-side ring and copies the ten nearest to Spyro. The UI section
then draws that copy with the full C API: `ImGui_PlotLines` over the ring,
an `ImGui_BeginTable` of the ten nearest, an `ImGui_Begin` window with an
`ImGui_ColorEdit3`, and a circle on the foreground draw list.

The split is the rule, not a style choice. Guest RAM is read in the tick
hook, on the sim thread, inside the tick. The section body runs on the
compose thread inside the engine's ImGui frame and reads only the host
copy: a guest read from there would sample the tick at an arbitrary point,
and a guest write would not survive a replay of the same inputs.

Opting in is one line, `OPENPETE_MOD_IMGUI()`, at file scope in one source
file. It defines the table pointer every `ImGui_*` call reads and exports
the descriptor the loader resolves at load. A mod without it references no
table and nothing is bound.

Two Containment rows, both off by default, make the engine's containment
paths visible:

- `break_end` omits an `ImGui_End`. Dear ImGui reports a recoverable error,
  the engine logs it against this mod and recovers the stack; three
  consecutive presents of that and the section is refused for the session.
- `hook_call` calls into the annex from a present hook and from the tick
  hook, both outside any section body. The engine drops each call, returns
  a zeroed value, and names the mod and the function once.

`scripts/check_mod_imgui.sh` drives both rows and the engagement counter in
a windowed run; `modtest.toml` covers compile, load and bind headless.
