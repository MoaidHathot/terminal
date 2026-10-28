# OverShell.Terminal.Wpf 1.26.260930.5

- Fix the native crash triggered by Sixel images, including the portrait displayed
  by `dnx MoaidHathot` in OverShell 0.3.1 and 0.4.0.
- Use a checked palette lookup to avoid MSVC 19.51 optimizing the hardened
  byte-indexed lookup into an unconditional fail-fast. All 256 palette entries,
  native image rendering and bounds checking are preserved.
- Add built-DLL regression checks to CI and release packaging: 16 scenarios
  across opaque and composed hosts, covering full palettes, transparency,
  fragmented input, palette redefinition, scrolling, alternate screens and resizing.
- Ship signed x64 and ARM64 native binaries and managed WPF assemblies, with
  native symbols and checksums attached to the GitHub release.

The OverShell consumer passed 518 C# tests and 24 on-screen checks, including
the real `dnx MoaidHathot -- --duration 6` command in both rendering modes and
successful shell/image output after the command exits.

[Diagnosis and regression details](https://github.com/MoaidHathot/terminal/blob/wpf-v1.26.260930.5/build/overshell/SIXEL.md).
