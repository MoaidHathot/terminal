# OverShell's fork of Windows Terminal

This branch (`overshell`) is [microsoft/terminal](https://github.com/microsoft/terminal) plus a
short series of commits, kept rebaseable, that exists for one reason: to build the **Windows
Terminal WPF control** from source as the NuGet package **`OverShell.Terminal.Wpf`**, with the
patches [OverShell](https://github.com/MoaidHathot/OverShell) needs and upstream does not ship.
Everything the fork adds lives under `build/overshell/` and `.github/workflows/overshell-*.yml`;
upstream files are touched only where a patch needs them, so a sync is a rebase, not a merge.

The consumer treats the result as a *distributable*, not as source: OverShell references the
package by version and upgrades by bumping it. Upstream moves on, the fork rebases, a new version
comes out, the consumer bumps. No submodule, no vendored binaries, no build of this tree on the
consumer's machine.

## What is in the package

Exactly what upstream's own `WpfTerminalControl` pack target produces (minus the 80 MB native PDBs,
which go to the GitHub release instead):

| | |
|---|---|
| `lib/net472/`, `lib/net8.0-windows7.0/` | `Microsoft.Terminal.Wpf.dll` - the managed `HwndHost` control (`src/cascadia/WpfTerminalControl`) |
| `runtimes/win-x64/native/`, `runtimes/win-arm64/native/` | `Microsoft.Terminal.Control.dll` - parser, buffer, AtlasEngine renderer, the `HwndTerminal` C API (`src/cascadia/TerminalControl`) |

Assembly and file names stay upstream's so the patch surface stays minimal; only the package id is
ours (`Microsoft.*` ids are reserved on nuget.org anyway).

## Versions

`A.B.YYMMDD.N`: upstream release branch `A.B`, the date of the upstream base commit, and the fork's
revision on that base. `build/overshell/UPSTREAM` records the base (`commit=`, `version=A.B.YYMMDD`, `runner=`);
a release tag is `wpf-v<version>.<N>`. So `1.25.260302.1` is the first fork build of upstream commit
`9ae724a` (2026-03-02, the 1.25 line - the same commit the third-party `CI.Microsoft.Terminal.Wpf
1.25.260303002` was built from; unchanged upstream bits, tagged on `rel/1.25.260302.1`), and
`1.25.260302.2` the second, with patches 2 and 3.

## The patch series

Each commit on top of upstream, oldest first. A sync rebases exactly these.

| # | What | Upstream files touched |
|---|---|---|
| 1 | **Build + publish infrastructure**: `build/overshell/` (this README, `Build.ps1`, `Pack.ps1`, the nuspec, `UPSTREAM`), `.github/workflows/overshell-{ci,release,sync}.yml`, `.github/dependabot.yml`, `CODEOWNERS` | none |
| 2 | **Composed rendering mode** (`TERMINAL_CREATE_COMPOSED`, `CreateTerminalEx`, `TerminalSetBackgroundOpacity`, `TerminalUpdateComposition`; `TerminalControl.UseComposition` / `BackgroundOpacity`): the terminal renders into a composition surface that the library wraps in a DirectComposition visual on the host's top-level window, over an input-only child HWND (`WS_EX_NOREDIRECTIONBITMAP`), so the swap chain keeps its alpha and the default background can be translucent over the window's backdrop. One composition device per process, one target per top-level window; the visual follows the child's position, size, visibility and re-parenting. Existing callers see no change. | `src/cascadia/TerminalControl/HwndTerminal.{cpp,hpp}`, `dll/Microsoft.Terminal.Control.def`, `src/cascadia/WpfTerminalControl/{NativeMethods,TerminalContainer,TerminalControl.xaml}.cs`, and in the engine: `TargetSettings.undoXamlScale` + `AtlasEngine::SetUndoXamlScale` (the SwapChainPanel scale compensation gets an off switch: `common.h`, `AtlasEngine.{h,api.cpp,r.cpp}`) |
| 3 | **dcomp.dll for the composition path**: `AtlasEngine::_createSwapChain` loads `dcomp.dll` when no XAML host has (under WPF or plain Win32 `GetModuleHandle` returned null and the first frame threw, silently). A one-line fix worth sending upstream. | `src/renderer/atlas/AtlasEngine.r.cpp` |

Why a child HWND stays: it keeps focus, keyboard, mouse (hit-testing is rect-based, so a window with
no pixels still gets the mouse), text services (IME) and UI Automation exactly as upstream has them;
only the pixels moved. Spike 8 / 8b in the OverShell repository (`spikes/dcomp-transparency`) are the
measurements behind the shape: a composition swap chain blends with the backdrop only as a visual on
the **top-level** window (a child-HWND target is non-deterministic, an HWND swap chain ignores alpha),
and a non-redirected child under a `topmost` target still hit-tests. A host must switch
`WS_CLIPCHILDREN` off on that window: WPF cannot repaint under a child HWND, so anything it painted
there before the child covered it would stay as a ghost under the translucent terminal.

## How a release happens

1. CI is green on `overshell` (`overshell-ci`: build x64 and pack; ARM64 on request).
2. A maintainer pushes a tag: `git tag wpf-v1.25.260302.1 && git push origin wpf-v1.25.260302.1`.
3. `overshell-release` builds x64 and ARM64 on GitHub-hosted runners, packs, creates the GitHub
   release with the package, the native PDBs and `SHA256SUMS.txt`, then pushes the package to
   nuget.org through **Trusted Publishing**: `NuGet/login` exchanges the job's OIDC token for a
   one-hour API key against a policy on nuget.org that names this repository and this workflow
   file (`overshell-release.yml`). The only secret is `NUGET_USER`, the nuget.org profile name,
   which is public anyway. A dispatched run of the same workflow is a dry run: build and pack
   with a given version, package left as a workflow artifact, nothing tagged or pushed.
4. OverShell bumps `OverShell.Terminal.Wpf` in its `Directory.Packages.props`.

## How a sync happens

The first real run (base `9ae724a` -> `v1.26.2734.0`) ended in
[issue #2](https://github.com/MoaidHathot/terminal/issues/2), as it should: upstream added a
`.github/dependabot.yml` since the base, as the fork did, so the rebase stops on that file. The
resolution is the human's - merge the two Dependabot files - and a 1.26 base will also want
`vars.OVERSHELL_RUNNER` moved to a VS 2026 image if upstream's toolset floor moved with it.

`overshell-sync` runs weekly (and on demand, optionally with an explicit upstream ref): it finds
upstream's newest `vA.B.C.D` tag, rebases the patch series from the recorded base onto it on a
`sync/<tag>` branch, updates `UPSTREAM`, pushes, opens a pull request and starts `overshell-ci` for
it. Conflicts leave an issue with the file list instead. A human reviews the PR, checks CI, merges,
then tags. Nothing is merged or published without a person.

Two things to expect from upstream over time:

- **Toolset moves.** This base wants Visual Studio 2022 (v143) and the 10.0.22621 SDK, which only
  the `windows-2022` runner has; 1.26 and `main` want VS 2026 (v145) and the 10.0.26100 SDK, which
  `windows-latest` has. The runner is part of the tree (`runner=` in `UPSTREAM`): the sync picks
  `windows-latest` when the target's `common.build.pre.props` names v145, so each branch builds on
  the image it needs; the repository variable `OVERSHELL_RUNNER` overrides for a one-off.
- **The pack target changes.** Upstream's `WpfTerminalControl.csproj` collects the native DLLs
  itself; we pack with our own nuspec from the built files, so that target's shape does not matter.

## Building locally

Visual Studio 2022 with *Desktop development with C++*, *Universal Windows Platform development*
and *C++ (v143) Universal Windows Platform tools* (the repository's `.vsconfig`), the 10.0.22621
SDK, PowerShell 7:

```powershell
./build/overshell/Build.ps1 -Platform x64 -Wpf       # bin\x64\Release\Microsoft.Terminal.Control\, bin\...\WpfTerminalControl\
./build/overshell/Build.ps1 -Platform ARM64
./build/overshell/Pack.ps1 -Version 0.0.1-local       # artifacts\packages\OverShell.Terminal.Wpf.0.0.1-local.nupkg
```

`Build.ps1` is a port of the `buildWPF` path of upstream's Azure Pipelines job
(`build/pipelines/templates-v2/job-build-project.yml`): vcpkg from Visual Studio, `VCToolsVersion`
pinned to the newest installed toolset, the three restores, then
`msbuild OpenConsole.slnx /t:Terminal\Control\TerminalControl;Terminal\wpf\WpfTerminalControl`.

## Safety

- Workflows run with a read-only `GITHUB_TOKEN`; the release job alone gets `contents: write` and
  `id-token: write`, the sync job `contents`/`pull-requests`/`issues`/`actions: write`. The
  repository setting *Allow GitHub Actions to create and approve pull requests* is on for the
  sync job's pull request (it is the only workflow here that opens one; a fork's pull request still
  runs with a read-only token); issues are enabled so a conflict has somewhere to go. The sync job
  uses the REST endpoints for both: the GraphQL `createIssue` mutation refuses a forked
  repository's `GITHUB_TOKEN`.
- CI uses `pull_request`, never `pull_request_target`: a fork's PR runs with no secrets and cannot
  publish. Release and sync refuse to run outside `MoaidHathot/terminal`.
- Third-party actions are pinned to commit SHAs; Dependabot keeps them current.
- Publishing cannot happen from a laptop: there is no API key to leak, and the nuget.org policy is
  bound to this repository and workflow.

Security issues in the fork's own code (the patches, the scripts, the workflows): see the
[OverShell security policy](https://github.com/MoaidHathot/OverShell/blob/main/SECURITY.md).
Issues in Windows Terminal itself belong to Microsoft (`SECURITY.md` at the repository root).

## Licence

MIT, as upstream (`LICENSE`). Not affiliated with Microsoft; Windows Terminal is a trademark of
Microsoft Corporation.
