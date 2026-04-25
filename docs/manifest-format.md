# Manifest format (deferred to Phase 2)

The function-authority manifest (`games/<game>/manifest/symbols.toml`) is
introduced in Phase 2, when the recompiler gains thunk codegen. Its design
is specified in detail at `notes/plans/openpete-pc-port.md` §6.

Phase 0b ships an empty `manifest/` directory under `games/spyro-1/` so
the layout is in place. The actual schema and an end-to-end worked example
will land here when Phase 2 begins.
