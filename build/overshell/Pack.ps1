#Requires -Version 7.0
<#
.SYNOPSIS
    Packs OverShell.Terminal.Wpf: the managed Microsoft.Terminal.Wpf.dll for net472 and
    net8.0-windows, the native Microsoft.Terminal.Control.dll per platform, a README naming the
    upstream base and the patches - the same shape as microsoft/terminal's own WpfTerminalControl
    pack target, with the native PDBs left out (80 MB each; they go to the GitHub release instead).

.PARAMETER Version
    The package version, e.g. 1.25.260302.1 (see README.md for the scheme).

.PARAMETER InputRoot
    Where to look for the built files. Scanned recursively, so it can be this repository's bin\
    after Build.ps1, or a folder of downloaded CI artifacts.

.PARAMETER Platforms
    The native platforms the package must carry; a missing one fails the pack.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Version,
    [string] $InputRoot,
    [string[]] $Platforms = @('x64', 'arm64'),
    [string] $OutputDirectory
)

$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
if (-not $InputRoot) { $InputRoot = Join-Path $repo 'bin' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo 'artifacts\packages' }
if ($Version -notmatch '^\d+\.\d+\.\d+(\.\d+)?(-[0-9A-Za-z.-]+)?$') { throw "'$Version' is not a NuGet version" }

$staging = Join-Path $repo 'obj\overshell-pack'
if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Force $staging, $OutputDirectory | Out-Null

function Find-One([string] $name, [scriptblock] $filter, [string] $what) {
    $hits = @(Get-ChildItem -Path $InputRoot -Recurse -Filter $name -File | Where-Object $filter)
    if ($hits.Count -eq 0) { throw "no $what ($name) under $InputRoot" }
    if ($hits.Count -gt 1) { Write-Warning "$($hits.Count) candidates for $what; taking the newest:`n  $($hits.FullName -join "`n  ")" }
    return ($hits | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1)
}

function Stage([System.IO.FileInfo] $file, [string] $relativeDir) {
    $dir = Join-Path $staging $relativeDir
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item $file.FullName (Join-Path $dir $file.Name)
    Write-Host ("{0,10:N0} KB  {1}\{2}" -f ($file.Length / 1KB), $relativeDir, $file.Name)
}

# The managed assembly, once per target framework. Upstream's OutputPath is
# bin\<Platform>\<Configuration>\WpfTerminalControl\<tfm>\; a CI artifact keeps the tail.
foreach ($tfm in @(@{ Dir = 'net472'; Lib = 'net472' }, @{ Dir = 'net8.0-windows'; Lib = 'net8.0-windows7.0' })) {
    $dll = Find-One 'Microsoft.Terminal.Wpf.dll' { $_.DirectoryName -match "[\\/]$([regex]::Escape($tfm.Dir))$" } "managed assembly for $($tfm.Dir)"
    Stage $dll "lib\$($tfm.Lib)"
    foreach ($side in 'Microsoft.Terminal.Wpf.xml', 'Microsoft.Terminal.Wpf.pdb') {
        $sideFile = Get-Item (Join-Path $dll.DirectoryName $side) -ErrorAction SilentlyContinue
        if ($sideFile) { Stage $sideFile "lib\$($tfm.Lib)" }
    }
}

# The native DLL per platform. Upstream's layout is bin\<Platform>\...; a CI artifact is named by
# platform instead, so either the path or an ancestor folder may carry the name.
foreach ($platform in $Platforms) {
    $dll = Find-One 'Microsoft.Terminal.Control.dll' { $_.FullName -match "[\\/]$platform[\\/]" } "native DLL for $platform"
    Stage $dll "runtimes\win-$($platform.ToLowerInvariant())\native"
}

Copy-Item (Join-Path $PSScriptRoot 'package-readme.md') (Join-Path $staging 'README.md')
Copy-Item (Join-Path $repo 'LICENSE') (Join-Path $staging 'LICENSE.txt')

$nuget = Get-Command nuget.exe -ErrorAction SilentlyContinue
if (-not $nuget) {
    $nugetExe = Join-Path $repo 'obj\tools\nuget.exe'
    if (-not (Test-Path $nugetExe)) {
        New-Item -ItemType Directory -Force (Split-Path $nugetExe) | Out-Null
        Invoke-WebRequest -Uri 'https://dist.nuget.org/win-x86-commandline/latest/nuget.exe' -OutFile $nugetExe
    }
    $nuget = Get-Command $nugetExe
}

$commit = (git -C $repo rev-parse HEAD).Trim()
& $nuget.Source pack (Join-Path $PSScriptRoot 'OverShell.Terminal.Wpf.nuspec') -Version $Version -BasePath $staging -OutputDirectory $OutputDirectory -NonInteractive -NoDefaultExcludes -Properties "commit=$commit"
if ($LASTEXITCODE -ne 0) { throw "nuget pack failed ($LASTEXITCODE)" }
Get-ChildItem $OutputDirectory -Filter '*.nupkg' | ForEach-Object { Write-Host ("{0,10:N0} KB  {1}" -f ($_.Length / 1KB), $_.FullName) }
