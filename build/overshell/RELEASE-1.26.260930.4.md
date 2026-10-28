# OverShell.Terminal.Wpf 1.26.260930.4

- Add `TerminalCreatePreview`, `TerminalUpdatePreview` and `TerminalDestroyPreview`
  for read-only, aspect-fit mirrors of composed terminal surfaces.
- Add `TerminalGetPreviewSnapshot` and `TerminalFreePreviewSnapshot` for bounded,
  versioned full-viewport snapshots with colours, styles and wide-glyph cell widths.
- Preserve source terminal dimensions, update mirrors on swap-chain replacement,
  invalidate tokens during source teardown, and prune expired composition targets.
- Ship x64 and ARM64 native binaries, managed WPF assemblies, symbols and checksums
  through the signed release workflow.

The OverShell consumer's automated validation includes 518 C# tests, 43 input-desktop
preview checks, 39 opaque-mode checks, existing UI regressions and repeated profiling
against the original dashboard baseline. The native proof additionally exercised
hidden-source pixel updates, clipping and unchanged terminal dimensions.

## AI assistance disclosure

The preview implementation and automated validation used AI assistance. This fork's
maintainer explicitly authorized public publication on 2026-10-28 under the
maintainer-permission provision in `AGENTS.md`. Automated results are not a claim of
independent human verification.
