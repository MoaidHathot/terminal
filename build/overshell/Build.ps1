#Requires -Version 7.0
<#
.SYNOPSIS
    Builds the Windows Terminal WPF control from this tree: the native Microsoft.Terminal.Control.dll
    for one platform and, when asked, the managed Microsoft.Terminal.Wpf.dll.

.DESCRIPTION
    A port of what microsoft/terminal's own Azure Pipelines job does for `buildWPF` /
    `buildWPFDotNetComponents` (build/pipelines/templates-v2/job-build-project.yml), as a script a
    GitHub Actions runner or a developer box with Visual Studio can run:

      1. find Visual Studio (vswhere) and its MSBuild;
      2. point vcpkg at the Visual Studio copy (VCPKG_ROOT), or bootstrap one under dep/vcpkg;
      3. pin VCToolsVersion to the newest installed toolset (build/scripts/Set-LatestVCToolsVersion.ps1);
      4. restore: build/packages.config, the solution, dep/nuget/packages.config - into .\packages,
         as NuGet.Config says;
      5. msbuild OpenConsole.slnx for the solution targets Terminal\Control\TerminalControl and,
         with -Wpf, Terminal\wpf\WpfTerminalControl.

    Outputs land where upstream puts them: bin\<Platform>\<Configuration>\Microsoft.Terminal.Control\
    and bin\<Platform>\<Configuration>\WpfTerminalControl\<tfm>\. Nothing here is specific to the
    OverShell patches; the same script builds an unpatched tree.

.PARAMETER Platform
    x64 or ARM64 (upstream's solution platform names).

.PARAMETER Wpf
    Also build the managed WPF assembly (platform-neutral; one build is enough per package).

.PARAMETER BinLog
    Where to write the MSBuild binary log (default: msbuild-<Platform>.binlog in the repo root).

.PARAMETER Version
    The package version (A.B.YYMMDD.N). Stamped on the managed assembly as its informational version;
    AssemblyVersion stays A.B.0.0 and FileVersion becomes A.B.YYMM.DDNNN, since those fields hold 16 bits
    each. The native DLL carries no version resource: upstream stamps it with internal tooling (XES).
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'ARM64')] [string] $Platform = 'x64',
    [ValidateSet('Release', 'Debug')] [string] $Configuration = 'Release',
    [switch] $Wpf,
    [switch] $SkipRestore,
    [string] $BinLog,
    [string] $Version
)

$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $repo
if (-not $BinLog) { $BinLog = Join-Path $repo "msbuild-$Platform.binlog" }

function Step([string] $text) { Write-Host "`n==> $text" -ForegroundColor Cyan }

# ---- 1. Visual Studio + MSBuild -----------------------------------------------------------------
# The TerminalControl path is C++/WinRT built as a Windows Store (AppContainer) component
# (OpenConsoleUniversalApp in src/cppwinrt.build.pre.props), so the Visual Studio that builds it needs
# the Windows Store platform toolset for the tree's toolset version on top of Desktop C++ - which is
# what MSB8020 ("build tools for 'v145' application Type Windows Store cannot be found") and MSB8036
# complain about, two minutes into a build, when it is missing. Checked here by the file MSBuild
# itself looks for, not by installer component ids: the GitHub runner image and a desktop install
# carry the same toolset under different component names. Of several instances, the newest that can
# build the tree wins; when none can, the message names the installer component that adds it.
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found at $vswhere - is Visual Studio installed?" }
$sdkVersion = ([regex]::Match((Get-Content (Join-Path $repo 'src\common.build.pre.props') -Raw), '<WindowsTargetPlatformVersion[^>]*>([\d.]+)<')).Groups[1].Value
$sdkInclude = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Include\$sdkVersion"
$instances = @(& $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json | Sort-Object { [Version] $_.installationVersion } -Descending)
if ($instances.Count -eq 0) { throw 'No Visual Studio with the C++ x64 toolset was found (workload "Desktop development with C++").' }
function StoreToolset([object] $instance) {
    # v145 under VS 2026 (MSBuild\Microsoft\VC\v180), v143 under VS 2022 (v170): src/common.build.pre.props.
    $major = ([Version] $instance.installationVersion).Major
    $toolset = if ($major -ge 18) { 'v145' } else { 'v143' }
    Join-Path $instance.installationPath "MSBuild\Microsoft\VC\v$($major)0\Application Type\Windows Store\10.0\Platforms\$Platform\PlatformToolsets\$toolset\Toolset.props"
}
$usable = $instances | Where-Object { Test-Path (StoreToolset $_) } | Select-Object -First 1
if (-not $usable) {
    $closest = $instances[0]
    $setup = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\setup.exe'
    throw ("$($closest.displayName) at $($closest.installationPath) has no Windows Store platform toolset for $Platform " +
           "(expected $(StoreToolset $closest)). The tree's C++/WinRT projects build as Windows Store components, which needs the " +
           "'C++ Universal Windows Platform tools' component (Microsoft.VisualStudio.ComponentGroup.UWP.VC) on top of Desktop C++:`n" +
           "  & '$setup' modify --installPath '$($closest.installationPath)' --add Microsoft.VisualStudio.ComponentGroup.UWP.VC --passive --norestart")
}
if (-not (Test-Path $sdkInclude)) {
    throw "The tree wants the Windows SDK $sdkVersion (src/common.build.pre.props) and $sdkInclude is not there; install it with the Visual Studio installer (Microsoft.VisualStudio.Component.Windows11SDK.$(($sdkVersion -split '\.')[2])) or from the Windows SDK page."
}
$vsRoot = $usable.installationPath
$msbuild = Join-Path $vsRoot 'MSBuild\Current\Bin\amd64\MSBuild.exe'
if (-not (Test-Path $msbuild)) { $msbuild = Join-Path $vsRoot 'MSBuild\Current\Bin\MSBuild.exe' }
if (-not (Test-Path $msbuild)) { throw "MSBuild not found under $vsRoot" }
Step "Visual Studio: $vsRoot ($($usable.displayName) $($usable.installationVersion); Windows Store toolset for $Platform present; SDK $sdkVersion)"
Write-Host "MSBuild: $msbuild"
& $msbuild -version | Select-Object -Last 1 | ForEach-Object { Write-Host "MSBuild version: $_" }

# ---- 2. vcpkg (steps-install-vcpkg.yml) ----------------------------------------------------------
$vcpkgVs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.Vcpkg -property installationPath
if ($vcpkgVs) {
    $env:VCPKG_ROOT = Join-Path $vcpkgVs 'VC\vcpkg'
    Step "vcpkg: Visual Studio's ($env:VCPKG_ROOT)"
} else {
    $local = Join-Path $repo 'dep\vcpkg'
    if (-not (Test-Path (Join-Path $local 'vcpkg.exe'))) {
        Step 'vcpkg: bootstrapping a local copy under dep/vcpkg'
        if (Test-Path $local) { Remove-Item -Recurse -Force $local }
        git clone --depth 1 https://github.com/microsoft/vcpkg $local
        & (Join-Path $local 'bootstrap-vcpkg.bat') -disableMetrics
    }
    $env:VCPKG_ROOT = $local
    Step "vcpkg: local ($env:VCPKG_ROOT)"
}
# No telemetry from a CI box.
$env:VCPKG_DISABLE_METRICS = '1'

# ---- 3. VCToolsVersion (Set-LatestVCToolsVersion.ps1, minus the ##vso) --------------------------
$instances = [xml](& $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -include packages -format xml)
$toolsCore = $instances.instances.instance.packages.package | Where-Object { $_.id -eq 'Microsoft.VisualCpp.Tools.Core' } | Select-Object -First 1
$vcToolsRoot = Join-Path $vsRoot 'VC\Tools\MSVC'
$vcToolsVersion = $toolsCore.version
if (-not $vcToolsVersion -or -not (Test-Path (Join-Path $vcToolsRoot $vcToolsVersion))) {
    $vcToolsVersion = (Get-ChildItem $vcToolsRoot -Directory | ForEach-Object { [Version] $_.Name } | Sort-Object -Descending | Select-Object -First 1).ToString(3)
}
$env:VCToolsVersion = $vcToolsVersion
Step "VCToolsVersion: $vcToolsVersion"

# ---- 4. restore (steps-restore-nuget.yml) --------------------------------------------------------
# Every restore reads the repository's NuGet.Config and nothing else: a developer box whose global
# NuGet configuration maps package patterns to its own feeds would otherwise map StyleCop.Analyzers
# (WpfTerminalControl.csproj) to a source this tree does not list, and the solution restore fails
# with NU1100. nuget.exe gets it through -ConfigFile; MSBuild's restore through RestoreConfigFile,
# which makes that file the whole configuration (the same as `dotnet restore --configfile`).
$nugetConfig = Join-Path $repo 'NuGet.Config'
$packages = Join-Path $repo 'packages'
if ($SkipRestore -and -not (Test-Path (Join-Path $packages 'Microsoft.Windows.ImplementationLibrary.*'))) {
    throw "-SkipRestore, but $packages has no restored packages yet; run once without it."
}
if (-not $SkipRestore) {
    $nuget = Get-Command nuget.exe -ErrorAction SilentlyContinue
    if (-not $nuget) {
        $nugetDir = Join-Path $repo 'obj\tools'
        New-Item -ItemType Directory -Force $nugetDir | Out-Null
        $nugetExe = Join-Path $nugetDir 'nuget.exe'
        if (-not (Test-Path $nugetExe)) {
            Step 'nuget.exe: downloading the latest release'
            Invoke-WebRequest -Uri 'https://dist.nuget.org/win-x86-commandline/latest/nuget.exe' -OutFile $nugetExe
        }
        $nuget = Get-Command $nugetExe
    }
    Step "NuGet: $($nuget.Source)"
    & $nuget.Source restore build/packages.config -ConfigFile $nugetConfig -PackagesDirectory $packages -NonInteractive
    if ($LASTEXITCODE -ne 0) { throw "nuget restore build/packages.config failed ($LASTEXITCODE)" }
    & $nuget.Source restore dep/nuget/packages.config -ConfigFile $nugetConfig -PackagesDirectory $packages -NonInteractive
    if ($LASTEXITCODE -ne 0) { throw "nuget restore dep/nuget/packages.config failed ($LASTEXITCODE)" }
    Step "msbuild /t:Restore ($Platform $Configuration)"
    & $msbuild OpenConsole.slnx /t:Restore /p:Configuration=$Configuration /p:Platform=$Platform "/p:RestoreConfigFile=$nugetConfig" /m /nologo /v:m /nr:false
    if ($LASTEXITCODE -ne 0) { throw "solution restore failed ($LASTEXITCODE)" }
}

# ---- 5. build ------------------------------------------------------------------------------------
$targets = @('Terminal\Control\TerminalControl')
if ($Wpf) { $targets += 'Terminal\wpf\WpfTerminalControl' }
$versionProps = @()
if ($Version) {
    if ($Version -notmatch '^(\d+)\.(\d+)\.(\d\d)(\d\d)(\d\d)\.(\d+)$') { throw "'$Version' is not A.B.YYMMDD.N" }
    $versionProps = @(
        "/p:Version=$Version",
        "/p:AssemblyVersion=$($Matches[1]).$($Matches[2]).0.0",
        "/p:FileVersion=$($Matches[1]).$($Matches[2]).$($Matches[3])$($Matches[4]).$([int]$Matches[5] * 1000 + [int]$Matches[6])",
        '/p:ContinuousIntegrationBuild=true'
    )
}
Step "msbuild /t:$($targets -join ';') ($Platform $Configuration$(if ($Version) { ", $Version" }))"
& $msbuild OpenConsole.slnx "/t:$($targets -join ';')" /p:Configuration=$Configuration /p:Platform=$Platform /p:WindowsTerminalBranding=Dev /p:PGOBuildMode=None @versionProps /m /nologo /v:m /nr:false "/bl:$BinLog"
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE); see $BinLog" }

# ---- outputs -------------------------------------------------------------------------------------
Step 'Outputs'
$native = Join-Path $repo "bin\$Platform\$Configuration\Microsoft.Terminal.Control\Microsoft.Terminal.Control.dll"
if (-not (Test-Path $native)) { throw "expected $native" }
Get-Item $native | ForEach-Object { Write-Host ("{0,10:N0} KB  {1}" -f ($_.Length / 1KB), $_.FullName) }
if ($Wpf) {
    $managed = @(Get-ChildItem (Join-Path $repo 'bin') -Recurse -Filter Microsoft.Terminal.Wpf.dll)
    if ($managed.Count -eq 0) { throw 'expected Microsoft.Terminal.Wpf.dll under bin\' }
    $managed | ForEach-Object { Write-Host ("{0,10:N0} KB  {1}" -f ($_.Length / 1KB), $_.FullName) }
}
