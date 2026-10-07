# Upstreaming the patch series

microsoft/terminal takes contributions in a fixed order (`CONTRIBUTING.md`): **file an issue first**,
wait for triage (the team triages several times a week; small things get `Issue-Bug` / `Issue-Task`,
larger things `Issue-Feature` and sometimes a request for a spec), then open the pull request and
reference the issue. A pull request without an issue, or for an `Issue-Feature` before the team has
said yes, tends to sit. Keep an eye on notifications: their bot closes issues and pull requests that
go unanswered.

Two separate submissions, because they have different odds. The branches are ready on this fork,
based on upstream `main`, containing nothing of the fork's own infrastructure:

| Branch | Content | Odds |
|---|---|---|
| `upstream/atlas-dcomp-load` | one hunk in `AtlasEngine.r.cpp` | good: a robustness fix with no behaviour change for them |
| `upstream/wpf-composed-rendering` | the composed mode (includes the hunk above; rebase after the first merges) | uncertain: the WPF control is low priority for the team, and in #15989 they pointed at a converged control they were planning |

Everything below is text to paste. The maintainer's own `gh` token is SAML-bound for the `microsoft`
organisation, so the issues and pull requests are opened in the browser.

---

## Issue 1 (bug / task) - paste at https://github.com/microsoft/terminal/issues/new

**Title:** `AtlasEngine's composition swap chain path assumes dcomp.dll is already loaded`

**Body:**

### Description

`AtlasEngine::_createSwapChain` creates a composition swap chain when the engine has no HWND. It
looks `DCompositionCreateSurfaceHandle` up with

```cpp
const auto module = GetModuleHandleW(L"dcomp.dll");
const auto DCompositionCreateSurfaceHandle = GetProcAddressByFunctionDeclaration(module, DCompositionCreateSurfaceHandle);
THROW_LAST_ERROR_IF(!DCompositionCreateSurfaceHandle);
```

`GetModuleHandleW` only finds a library that something else has already loaded. Under XAML that is
always the case (the framework loads dcomp.dll long before any swap chain exists). A host that
drives the engine without a HWND and composes the swap chain itself - a DirectComposition visual on
a Win32 or WPF window - can reach this code first. Then `GetModuleHandleW` returns null,
`GetProcAddress(nullptr, ...)` fails, and `Present()` throws `ERROR_MOD_NOT_FOUND` on the first frame
and on every frame after it, with nothing drawn and nothing logged a host would see.

### Steps to reproduce

Build the WPF control from source with `HwndTerminal::Initialize` not calling `engine->SetHwnd(...)`
(so the engine takes the composition path) and host it in a plain WPF application, which does not
load dcomp.dll on its own. Nothing renders; the render thread fails at `_createSwapChain` every frame.

### Expected behavior

The engine loads `dcomp.dll` itself when it is not loaded yet
(`LoadLibraryExW(L"dcomp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)`), the way other lazily
resolved system APIs in the code base are handled.

### Actual behavior

`ERROR_MOD_NOT_FOUND` from `_createSwapChain` whenever no other component loaded dcomp.dll first.

I have a one-hunk fix ready (`MoaidHathot/terminal`, branch `upstream/atlas-dcomp-load`) and will
open the pull request once this is triaged.

---

## Pull request 1 - from `MoaidHathot:upstream/atlas-dcomp-load` to `microsoft:main`

Open it at https://github.com/microsoft/terminal/compare/main...MoaidHathot:terminal:upstream/atlas-dcomp-load

**Title:** `AtlasEngine: load dcomp.dll for the composition swap chain when it is not loaded yet`

**Body** (the repository's pull request template asks for these headings):

## Summary of the Pull Request

`AtlasEngine::_createSwapChain` resolves `DCompositionCreateSurfaceHandle` from `dcomp.dll` with
`GetModuleHandleW`, which assumes the library is already in the process. Under XAML it always is; a
host that composes the swap chain itself may reach the code before anything loaded it, and the first
frame then throws `ERROR_MOD_NOT_FOUND`, silently, every frame. This loads the library when it is
missing. No change for XAML hosts: `GetModuleHandleW` finds it and nothing else runs.

## References and Relevant Issues

Closes #<issue 1>. Related: #15989 (a WPF-control user ended up at exactly this path).

## Detailed Description of the Pull Request / Additional comments

`LOAD_LIBRARY_SEARCH_SYSTEM32`, since dcomp.dll is a system component. The handle is deliberately
not freed: the engine keeps using the function for its lifetime, and dcomp.dll stays loaded in any
process that composes.

## Validation Steps Performed

Built the WPF control with the engine on the composition path (no `SetHwnd`) and hosted it in a WPF
application: before the change the render thread failed at `_createSwapChain` on every frame
(`ERROR_MOD_NOT_FOUND`), after it the swap chain is created and the terminal renders. XAML app:
unaffected (the library was already loaded; the new branch is not taken).

## PR Checklist

- [x] Closes #<issue 1>
- [x] Tests added/passed (manual validation above; no unit test covers the swap chain path)
- [ ] Documentation updated (none needed)
- [ ] Schema updated (none)

---

## Issue 2 (feature) - paste at https://github.com/microsoft/terminal/issues/new

**Title:** `WPF control: a composed rendering mode so the terminal background can be translucent`

**Body:**

### Description of the new feature

The WPF control (`Microsoft.Terminal.Wpf` / `HwndTerminal`) cannot have a translucent background.
Its terminal renders into a swap chain created for the control's child HWND
(`CreateSwapChainForHwnd`), and DXGI ignores the alpha channel of HWND swap chains: whatever the
host paints behind the control, the terminal's background is opaque. `TerminalSetTheme` takes
`COLORREF`s, so there is no alpha to pass anyway, and `AtlasEngine::EnableTransparentBackground`
is never called on this path. #15989 asked for this in 2023 and ended with the asker rewriting the
renderer locally around DirectComposition without sharing the code; a `WS_EX_NOREDIRECTIONBITMAP`
child was tried there and did not help on its own (it is in `main` now, for memory).

I have implemented this as an **opt-in mode** and would like to contribute it. Shape:

- `CreateTerminalEx(parentHwnd, flags, ...)` with `TERMINAL_CREATE_COMPOSED`. `CreateTerminal` stays
  as it is (it calls `CreateTerminalEx` with no flags); the default path is byte-for-byte unchanged.
- A composed terminal keeps its child HWND for everything a HWND gives - focus, keyboard, mouse,
  TSF, UI Automation - but `AtlasEngine` renders into a composition surface (the engine's existing
  XAML path) instead of the HWND. `HwndTerminal` wraps the surface handle in a DirectComposition
  visual on the child's **top-level** window (one device per process, one target per top-level
  window, `topmost`), kept at the child's position and size, attached while the child is shown,
  moved when the child moves or is re-parented (`TerminalUpdateComposition` for hosts that
  `SetParent`).
- `TerminalSetBackgroundOpacity(terminal, float)`: cells with the default background get that alpha;
  text and coloured cells stay opaque, as in the Terminal app. It survives `TerminalSetTheme`.
- The engine gains `SetUndoXamlScale(bool)`: without a HWND it assumes a XAML `SwapChainPanel` and
  applies an inverse-DPI matrix (`_updateMatrixTransform`), which a host composing the swap chain
  itself has to switch off. Default unchanged.
- Managed side: `TerminalControl.UseComposition` (read once, when the HWND is created) and
  `TerminalControl.BackgroundOpacity`; the container falls back to the HWND terminal when a composed
  one cannot be created.

Why the visual goes on the top-level window and not the child: measured on Windows 11 24H2, a
composition swap chain as a DirectComposition visual on the **top-level** window blends with the
window's backdrop (acrylic/Mica) every time; the same visual bound to a `CreateTargetForHwnd` on the
*child* HWND blended with white in three runs out of four (non-deterministic); an HWND swap chain
ignores alpha regardless. A `WS_EX_NOREDIRECTIONBITMAP` child under a `topmost` target still
hit-tests (`WindowFromPoint` returns it), so input needs no change.

Two things a host has to do, which the documentation of the mode would say: paint or extend a
backdrop behind the control, and switch `WS_CLIPCHILDREN` off on its top-level window - WPF, for
one, cannot repaint under a child HWND, so anything it painted there before the child covered it
stays as a ghost under a translucent terminal.

### Proposed technical implementation details

The implementation is complete and in use (MoaidHathot/terminal, branch
`upstream/wpf-composed-rendering`, one commit on `main`, ~500 lines: `HwndTerminal.{cpp,hpp}`,
`Microsoft.Terminal.Control.def`, `WpfTerminalControl/{NativeMethods,TerminalContainer,TerminalControl.xaml}.cs`,
and `SetUndoXamlScale` in `AtlasEngine.{h,api.cpp,r.cpp}` + `common.h`). It depends on the one-line
dcomp.dll load fix in #<issue 1>. Validated end to end in a WPF host: real screen pixels through
acrylic over a known colour behind the window at opacity 0.5 / 1.0 / 0.5, the visual following the
control across windows (re-parenting) and through hide/show, with the control's keyboard, mouse
selection, scrolling and UIA unchanged; the opaque default path verified unchanged by the host's
existing test suite.

I know the WPF control is not a priority and that a converged control was discussed in #15989; if
that is still the plan and this is unwelcome, say so and I will keep it in the fork. If it is
welcome, I will open the pull request against `main` once this is triaged.

---

## Pull request 2 - from `MoaidHathot:upstream/wpf-composed-rendering` to `microsoft:main`

Open it at https://github.com/microsoft/terminal/compare/main...MoaidHathot:terminal:upstream/wpf-composed-rendering
**after pull request 1 has merged**, and rebase the branch first (`git rebase upstream/main`; the
dcomp hunk disappears as already present).

**Title:** `HwndTerminal: an opt-in composed rendering mode, for translucent backgrounds in the WPF control`

**Body:**

## Summary of the Pull Request

An opt-in `TERMINAL_CREATE_COMPOSED` flag (`CreateTerminalEx`) under which the WPF control's
terminal renders into a composition surface that `HwndTerminal` wraps in a DirectComposition visual
on the host's top-level window, over its input-only child HWND - so the swap chain keeps its alpha
and `TerminalSetBackgroundOpacity` can make the default background translucent over the host's
backdrop. `CreateTerminal` and the default path are unchanged. Managed side:
`TerminalControl.UseComposition` and `BackgroundOpacity`.

## References and Relevant Issues

Closes #<issue 2>. Related: #15989. Depends on #<pull request 1> (merged).

## Detailed Description of the Pull Request / Additional comments

The commit message carries the design; in short: the child HWND stays (focus, keyboard, mouse, TSF,
UIA untouched; `WS_EX_NOREDIRECTIONBITMAP`, which `main` already sets); the engine takes its XAML
path minus the SwapChainPanel scale compensation (`SetUndoXamlScale(false)`); the render thread hands
the surface handle to the window thread through a posted message, which creates the visual, places it
at the child's rectangle and attaches it to a per-top-level-window target (`topmost`), re-placed on
`WM_WINDOWPOSCHANGED`, re-targeted when the child is re-parented, detached on `WM_DESTROY`. One
composition device per process, created on first use so hosts that never compose never load
dcomp.dll. The opacity is applied as the alpha of `TextColor::DEFAULT_BACKGROUND`, which the
renderer already honours when `EnableTransparentBackground` is on, and re-applied after
`TerminalSetTheme` resets the entry.

Hosts: paint or extend a backdrop behind the control, and clear `WS_CLIPCHILDREN` on the top-level
window (WPF cannot repaint under a child HWND; with clipping on, whatever it painted there before
the child covered it stays as a ghost under the translucent terminal).

## Validation Steps Performed

- Default path: the host's full test suite unchanged with and without the patch (a WPF application
  with the control in `HwndHost`: typing, selection, scrolling, find, UIA text reads, tear-off
  re-parenting, DPI, session restore).
- Composed path, real screen pixels: a lime window placed behind the host; the terminal's empty
  area reads `#0C6106` at opacity 0.5 (half of the theme's `#0C0C0C` over acrylic-blurred lime),
  exactly `#0C0C0C` at opacity 1.0 (set live), `#0C6106` again at 0.5; the visual follows the
  control into another top-level window (`SetParent`) and back, and through hide/show when tabs
  switch; a second composed terminal in the same window shares the target.
- Keyboard, mouse selection, scrolling and UIA (TextPattern reads of the buffer) work the same in
  the composed mode, since the child HWND is unchanged.
- No DirectComposition (or an older native library): `CreateTerminalEx` fails, the managed control
  falls back to `CreateTerminal`.

## PR Checklist

- [x] Closes #<issue 2>
- [x] Tests added/passed (manual validation above; the WPF control has no automated tests upstream)
- [ ] Documentation updated: the XML doc comments on `TerminalControl.UseComposition` /
      `BackgroundOpacity`; no docs.microsoft.com page covers the WPF control
- [ ] Schema updated (none)

---

## After either merges

- Pull request 1 merged: the fork's series keeps its own copy of the hunk until the next sync
  rebases onto a base that has it (the rebase drops the now-empty commit). Nothing to do.
- Pull request 2 merged: same, and the fork then carries only its build infrastructure - the goal.
  The package keeps shipping either way; OverShell notices nothing.
