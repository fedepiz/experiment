# CLAUDE.md

A tile-based simulation game ("Imperium") written in Odin. World generation
is deterministic: the same parameters and the same seed give the same world,
always (see Determinism below).

## Commands

- Build: `odin build src` (writes `src.bin` at the root)
- Run: `odin run src` — run from the repo root; the app reads `assets/` and
  `data/` relative to the cwd
- Tests: `odin test src/rng`, `odin test src/tabula`, `odin test src/ui`
- Headless worldgen report: `./src.bin worlds N [first_seed]` writes one CSV
  row of measurements per world to stdout.

## Layout

- `src/` is package main: `main.odin` (entry, frame loop), `report.odin`
- Base: `geo`, `rng`, `chunk` (chunked list), `color`, `tabula` (data format)
- Platform: `window` (GLFW), `render` (GL 4.1 instanced quads), `draw`
- Game: `src/game` with subpackages `defs`, `thing` (the database),
  `board`, `tiling`, `worldgen`
- Client: `src/client` with subpackages `hud`, `map_view`; UI core in `ui`

## Comments: ASD-STE100

Write every comment in ASD-STE100 Simplified Technical English:

- One topic per sentence. Short sentences. Active voice, present tense.
- Use simple approved words. Do not use a difficult word where a simple
  word is correct.
- Say what the code does. Never write what the code does not do.
- A comment uses the vocabulary of its own layer, never the caller's domain.
- Keep a comment to 1-3 lines.

An example of the voice, from `worldgen`: "A band of 0 to 0 accepts each
value (ZII), so a row with no band accepts each tile. Make the last row of
the file such a row."

Section headers are `//~ fp: Title` over a line of slashes; sub-sections are
`//- fp: ...`.

## Conventions

- ZII everywhere: the zero value is the nil value. No `_nil()` or `is_nil()`
  helpers for struct types. A table's row 0 is the nil row.
- No array+count pairs. A fixed array with a sibling count field becomes
  `[dynamic;N]T`. A partially-filled scratch slice is resliced (`s = s[:n]`).
  Deliberate exceptions: the SoA pools of `thing.Db`, and stacks or caches
  whose capacity is set at runtime.
- Plain `int` wherever width is not load-bearing. Sized types only for
  pixels, GL, rng state, and file formats.
- `tabula` getters return `(value, ok)` with `#optional_ok`. The single-value
  form reads as zero where the key is absent. `or_else` names a fallback at
  the call site. The ok tells presence apart from a value.
- Allocators: a procedure that allocates for the caller takes
  `allocator := context.allocator` last. One arena per lifetime (frame,
  persist, game-world, module table). An arena init failure panics at the
  site and names the arena.
- Data files load with zero defaults: the file value goes onto `{0}`
  verbatim, a broken file gives a visibly broken result and a stderr report,
  and the program does not stop. Contracts assert loudly; nothing absorbs an
  impossible state in silence.
- Tests go at the end of the module file they test, under a
  `//~ fp: Tests` section. Never in a separate `*_test.odin` file.
- Interfaces carry the minimal currency: opaque ids in, facts out. A module
  never exports its ontology.

## Determinism

The same seed must give the same world, on every build and every machine.
These rules protect that property:

- `worldgen` calls `libc.cosf/sinf/powf`, not `core:math`. A difference of
  one ulp in a noise value can move a tile across a band limit, which
  changes the world.
- `rng` must stay bit-identical. Golden values sit in the tests at the end
  of `src/rng/rng.odin`. Run `odin test src/rng` after any touch.
- The A* heap in `board` is hand-rolled on purpose: its sift order decides
  the tie-breaks between equal-cost paths, and those tie-breaks decide
  roads and movement. Do not swap it for `core:container/priority_queue`.
- `report.odin`'s `csv_f4` rounds decimal ties away from zero, so the CSV
  of a seed never changes between runs or toolchains.
