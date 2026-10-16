# imgui-panel

The ImGui escape hatch (`openpete_imgui.h`) driving a real panel. The
eleven-widget vtable in `openpete_mod_ui.h` is the simple path; this mod is
what the other path looks like.

A tick hook walks the level moby array, counts the live mobys
into a host-side ring and copies the ten nearest to Spyro, whose position
it reads through `api->player_pose`. On Spyro 1 the hook is an override
on `GamestateUpdate`, which runs every tick from the title screen on. Spyro
2 and 3 name no tick function yet, so their arms bind the per-tick camera
update by address (`func_8001D87C`, `func_8001204C`), which runs on
gameplay ticks only. The moby array layout comes from the typed `Moby`
view on Spyro 1 and 3 and from the offsets the game's own draw queue reads
on Spyro 2. The UI section
then draws that copy with the full C API: `ImGui_PlotLines` over the ring,
an `ImGui_BeginTable` of the ten nearest, an `ImGui_ColorEdit3`, and a
circle on the foreground draw list.

The section is registered with `OPENPETE_MOD_UI_SECTION_FLAGS(ui_draw,
OPENPETE_MOD_UI_ALWAYS)`, so the body runs on every present rather than
only inside the mod's node of the Mods panel. It opens two windows of its
own, which is what an always body has to do: at top level there is no
containing node, and a widget outside an `ImGui_Begin` would land in
ImGui's default debug window. The stats window and the marker draw
whatever the overlay is doing; the detail window with the plot, the table
and the colour editor opens only while `ui->overlay_open()` reports the
overlay is up, since with it closed the engine puts
`ImGuiWindowFlags_NoInputs` on the mod's windows and nothing in them can
be clicked.

The stats window carries a button and the mod logs, once a second, the
presents in which that button was hovered:

```
imgui-panel: hovered N/M presents, L live mobys
```

N is 0 for every present drawn with the overlay closed, which is the
measurement of the no-input rule.

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

- `break_end` omits an `ImGui_End`. ImGui reports a recoverable error,
  the engine logs it against this mod and recovers the stack; three
  consecutive presents of that and the section is refused for the session.
- `hook_call` calls into ImGui from a present hook and from the tick
  hook, both outside any section body. The engine drops each call, returns
  a zeroed value, and names the mod and the function once.

`tests/harness/check_mod_imgui.sh` drives both rows, the engagement counter and
the hover tally in a windowed run; `modtest.toml` covers compile, load and
bind headless.
