# OverShell.Terminal.Wpf

The Windows Terminal WPF control, built from source by [OverShell's fork of
microsoft/terminal](https://github.com/MoaidHathot/terminal/tree/overshell) for
[OverShell](https://github.com/MoaidHathot/OverShell). Two files, the same two the official
`WpfTerminalControl` pack target produces:

| File | What |
|---|---|
| `lib/net472/Microsoft.Terminal.Wpf.dll`, `lib/net8.0-windows7.0/Microsoft.Terminal.Wpf.dll` | the managed `HwndHost` control (`TerminalControl`, `TerminalContainer`, `ITerminalConnection`) |
| `runtimes/win-x64/native/Microsoft.Terminal.Control.dll`, `runtimes/win-arm64/native/...` | the native engine: the VT parser, the text buffer, the AtlasEngine renderer, the `HwndTerminal` C API |

Version `A.B.YYMMDD.N` means: upstream branch `A.B`, upstream base commit dated `YYMMDD`, fork
revision `N` on that base. The exact upstream commit and the list of patches applied on top of it are
in the repository's [`build/overshell/README.md`](https://github.com/MoaidHathot/terminal/blob/overshell/build/overshell/README.md)
and in the package's repository metadata (`commit`).

Native symbols (`Microsoft.Terminal.Control.pdb`, ~80 MB per platform) are not in the package;
they are attached to the matching GitHub release at
https://github.com/MoaidHathot/terminal/releases.

Licence: MIT, as upstream. Not affiliated with Microsoft; Windows Terminal is a trademark of
Microsoft Corporation.
