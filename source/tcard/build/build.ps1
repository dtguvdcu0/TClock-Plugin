[CmdletBinding()]
param(
    [ValidateSet('Release')]
    [string]$Configuration = 'Release',
    [ValidateSet('x64')]
    [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'
$buildDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$tcardRoot = Resolve-Path (Join-Path $buildDir '..')
$solution = Join-Path $tcardRoot 'TCard.sln'
$buildRoot = Join-Path $tcardRoot ("out\x64\$Configuration")
# Resolve tools before downloads or interacting with build-output processes.
$msbuild = $env:MSBUILD
if ([string]::IsNullOrWhiteSpace($msbuild)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $msbuild = & $vswhere -latest -products '*' -version '[18.0,19.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\Current\Bin\MSBuild.exe' | Select-Object -First 1
    }
    if ([string]::IsNullOrWhiteSpace($msbuild)) { $msbuild = 'MSBuild.exe' }
}
$msbuildCommand = Get-Command $msbuild -CommandType Application -ErrorAction SilentlyContinue
if (-not $msbuildCommand) { throw 'MSBuild was not found. Install Visual Studio 2026 C++ build tools (v145), or set MSBUILD.' }
$msbuild = $msbuildCommand.Source
$versionOutput = & $msbuild -nologo -version
if ($LASTEXITCODE -ne 0 -or [version]($versionOutput | Select-Object -Last 1) -lt [version]'18.0') {
    throw 'TCard requires Visual Studio 2026 MSBuild and the v145 C++ toolset.'
}
$null = Get-Command cmake -CommandType Application -ErrorAction Stop

function Stop-TCardForBuild {
    $allowedPaths = @(
        [IO.Path]::GetFullPath((Join-Path $buildRoot 'plugins\TCard.exe'))
    )
    $processes = @(Get-Process -Name 'TCard' -ErrorAction SilentlyContinue)
    foreach ($process in $processes) {
        if ($process.Threads.Count -eq 0) {
            Write-Output "Ignoring an exited TCard process entry $($process.Id)"
            continue
        }
        $path = $null
        try { $path = $process.MainModule.FileName } catch { }
        if (-not $path) { throw "Cannot verify TCard executable path for PID $($process.Id)." }
        if ($allowedPaths -notcontains [IO.Path]::GetFullPath($path)) { continue }
        if ($path) {
            Write-Output "Stopping TCard process $($process.Id): $path"
        } else {
            Write-Output "Stopping TCard process $($process.Id)"
        }
        if (-not $process.HasExited -and $process.MainWindowHandle -ne 0) {
            $process.CloseMainWindow() | Out-Null
            $process.WaitForExit(3000) | Out-Null
        }
        if (-not $process.HasExited) {
            throw "TCard PID $($process.Id) is still running. Close it before rebuilding this artifact."
        }
    }
    $remaining = @(Get-Process -Name 'TCard' -ErrorAction SilentlyContinue | Where-Object { $_.Threads.Count -gt 0 -and $allowedPaths -contains $_.Path })
    if ($remaining.Count -gt 0) {
        $ids = ($remaining | ForEach-Object Id) -join ', '
        throw "Unable to stop TCard process(es): $ids. Close TCard or run the build with sufficient rights."
    }
}

function Assert-SyncArtifactIdle {
    $syncPaths = @([IO.Path]::GetFullPath((Join-Path $buildRoot 'plugins\TCardSync.exe')))
    foreach ($syncProcess in @(Get-Process -Name TCardSync -ErrorAction SilentlyContinue)) {
        if ($syncProcess.Threads.Count -eq 0) { continue }
        $syncPath = $syncProcess.MainModule.FileName
        if (-not $syncPath) { throw "Cannot verify sync worker path for PID $($syncProcess.Id)." }
        if ($syncPaths -contains [IO.Path]::GetFullPath($syncPath)) {
            throw "TCardSync PID $($syncProcess.Id) owns this artifact. Request normal --stop with its profile before building/deploying."
        }
    }
}

# A running optional node must close normally; builds never terminate it.
foreach ($signalProcess in @(Get-Process -Name TCardSignal -ErrorAction SilentlyContinue)) {
    if ($signalProcess.Threads.Count -eq 0) { continue }
    $signalPath = $signalProcess.MainModule.FileName
    if (-not $signalPath) { throw "Cannot verify Signal executable path for PID $($signalProcess.Id)." }
    if ([IO.Path]::GetFullPath($signalPath) -eq [IO.Path]::GetFullPath((Join-Path $buildRoot 'plugins\TCardSignal.exe'))) {
        throw "TCardSignal PID $($signalProcess.Id) owns this build artifact. Stop its owned launcher normally first."
    }
}
Assert-SyncArtifactIdle
Stop-TCardForBuild

& (Join-Path $buildDir 'sync-dependencies.ps1')
& $msbuild $solution /m /nr:false /p:Configuration=$Configuration /p:Platform=$Platform /v:minimal
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$languageSource = Join-Path $tcardRoot 'lang'
$languageBuild = Join-Path $buildRoot 'plugins\tcard\lang'
if (Test-Path -LiteralPath $languageSource) {
    New-Item -ItemType Directory -Path $languageBuild -Force | Out-Null
    foreach ($languageFile in @('en.ini', 'ja.ini')) {
        Copy-Item -LiteralPath (Join-Path $languageSource $languageFile) -Destination $languageBuild -Force
    }
}

$signalArtifact = Join-Path $buildRoot 'plugins\TCardSignal.exe'
if (-not (Test-Path -LiteralPath $signalArtifact)) { throw 'Optional-distribution TCardSignal executable was not built.' }
$syncArtifact = Join-Path $buildRoot 'plugins\TCardSync.exe'
if (-not (Test-Path -LiteralPath $syncArtifact)) { throw 'TCardSync executable was not built.' }
$icon = Join-Path $tcardRoot 'icon3.ico'
if (-not (Test-Path -LiteralPath $icon)) { throw "Icon asset not found: $icon" }
$buildIcon = Join-Path $buildRoot 'plugins\icon3.ico'
New-Item -ItemType Directory -Path (Split-Path -Parent $buildIcon) -Force | Out-Null
Copy-Item -LiteralPath $icon -Destination $buildIcon -Force
