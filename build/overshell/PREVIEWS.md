# Terminal preview ABI (1.26.260930.4)

This extension adds read-only previews to the WPF/HwndTerminal host. It does
not resize the terminal, create another parser/session, or forward preview input.
All entrypoints run on the terminal window's UI thread.

- `TerminalCreatePreview(sourceHwnd, destinationHwnd, out token)` creates a composition
  visual sharing the source's surface. Both windows belong to the calling UI thread;
  the destination is top-level. Opaque sources return `E_NOTIMPL`; an unready composed
  surface returns `E_PENDING`.
- `TerminalUpdatePreview(token, destinationRect, visibleClip, radius)` fits the rendered
  surface into the destination, preserving aspect ratio. Coordinates are physical
  pixels in the destination client area. Empty intersections detach the visual.
- `TerminalDestroyPreview(token)` removes it. Repeated destruction is harmless.
  Source teardown invalidates its tokens and removes their visuals automatically.
- `TerminalGetPreviewSnapshot(sourceHwnd, out block)` returns a coherent styled viewport
  for composed **or opaque** sources, under the buffer lock. Free it with
  `TerminalFreePreviewSnapshot`. No UI Automation round trips per character are needed.

The snapshot starts with eight little-endian uint32 fields: total bytes, ABI version
(1), columns, rows, run count, UTF-16-unit count, rendered pixel width and height.
Then come 32-byte runs: column, row, cell width, foreground COLORREF, background COLORREF,
flags, UTF-16 offset and length. Flags: bold=1, italic=2, underline=4, strikethrough=8.
UTF-16 text follows the runs. Wide glyphs retain their cell width and trailing cells
are omitted from text; coloured spaces remain. Limits bound the allocation to 262144
viewport cells and 1048576 UTF-16 units. Consumers validate the header and every range.

Mirrors fit the renderer's last requested size rather than a hidden HWND's rectangle:
hosts can arrange hidden HWNDs independently of the rendered buffer. New swap chains
replace all associated preview contents. Tokens are monotonically allocated and UI
thread checked before registry access, so late managed disposal cannot dereference a
destroyed source.

The OverShell consumer has `previewnative` checks for the ABI, colours/styles, wide
Unicode, opaque fallback, unchanged grids, clipping and disposal. Input-desktop pixel
checks verify red-to-green updates from a hidden source. `previewbench` supplies the
baseline and per-stage performance protocol. The consumer's final validation passed
518 C# tests, 43 on-screen preview checks, 39 opaque-mode checks and the existing UI
regression suite. Performance evidence is recorded in OverShell's
`docs/DASHBOARD-PREVIEWS.md`.

## AI assistance disclosure

The preview implementation and automated validation used AI assistance. This fork's
maintainer explicitly authorized public publication on 2026-10-28 under the
maintainer-permission provision in `AGENTS.md`. The reported checks are automated
validation; they are not a claim of independent human verification.
