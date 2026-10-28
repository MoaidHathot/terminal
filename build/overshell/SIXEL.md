# Sixel crash: checked palette lookup

## Reproduction and diagnosis

`dnx MoaidHathot` renders a native Sixel portrait. OverShell 0.3.1 and 0.4.0
terminated while decoding it; Windows Terminal and Windows Terminal Preview did
not. The 0.4.0 crash dump, decoded with the release's matching native PDB, showed:

```text
0xC0000409 / FAST_FAIL_INVALID_ARG (5)
Microsoft.Terminal.Control.dll + 0x5de09
std::array<unsigned long, 256>::operator[]
til::at
SixelParser::_colorFromIndex
SixelParser::_maybeFlushImageBuffer
StateMachine::ProcessString
TerminalSendOutput
```

The smallest reproduction is a one-column red image:

```text
ESC P 0;1q#1;2;100;0;0~ ESC \
```

The release disassembly branches straight from the first nontransparent pixel to
`__fastfail(5)`, without reading its color index. A standalone C++ palette/vector
reproduction with MSVC **19.51.36260.0** confirmed the build-specific behavior:

| Build | Result for all 256 entries |
|---|---|
| `/O2`, STL hardening on, `operator[]` | `0xC0000409` |
| `/Od`, STL hardening on, `operator[]` | Correct sum (32640) |
| `/O2`, STL hardening off, `operator[]` | Correct sum (32640) |
| `/O2`, hardening on, destructor tombstones off | `0xC0000409` |
| `/O2`, hardening on, checked `at` lookup | Correct sum (32640) |

The fork builds with `WindowsTerminalBranding=Dev`, which enables STL hardening
in `src/common.build.post.props`; upstream's normal release/preview branding
does not enable that opt-in. This accounts for the different terminal behavior.

## Fix and regression protection

`_colorFromIndex` now uses `_colorTable.at(static_cast<size_t>(tableIndex))`.
It retains bounds checking, all palette entries, image support and the rest of
the build's hardening. The modified source file uses LF line endings.

`build/overshell/Test-Sixel.ps1` loads the actual compiled DLL in isolated child
processes, so an uncatchable native termination fails the test runner rather than
being mistaken for successful exception handling. It checks eight scenarios in
each of the opaque and composed hosts: a colored image, opaque fill, transparency,
all 256 colors, fragmented input and palette redefinition, aspect ratio/scrolling,
alternate screen, and output after resizing. A device-status reply after every
scenario confirms that the terminal parser is still working.

The test reproduces the failure against `1.26.260930.4` and passes all **16**
checks against the corrected optimized DLL. Both the CI and release workflows
run it before collecting x64 build outputs.

OverShell also has a `sixel` self-test mode that sends images through a real ConPTY
shell, checks actual red/blue pixels on the input desktop, and optionally runs a
bounded application via `OVERSHELL_SIXEL_APP`.

Diagnosis, implementation and automated verification used AI assistance. These
results describe automated tests, not independent human verification.
