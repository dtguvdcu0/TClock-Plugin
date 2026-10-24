[CmdletBinding()]
param([switch]$DownloadOnly)
$ErrorActionPreference = 'Stop'
$tcardRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$dependencyRoot = Join-Path $tcardRoot 'out\deps'
$utf8 = [Text.UTF8Encoding]::new($false)

function Assert-LocalPath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $boundary = $tcardRoot.TrimEnd('\')
    if ($full -ne $boundary -and -not $full.StartsWith($boundary + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Dependency path escaped the TCard workspace: $full"
    }
    # Inspect only this path's ancestry, including the workspace's own parents.
    # Reject junctions/symlinks instead of following them or repairing user folders.
    $cursor = $full
    while ($cursor) {
        $attributes = $null
        try { $attributes = [IO.File]::GetAttributes($cursor) }
        catch [IO.FileNotFoundException] { }
        catch [IO.DirectoryNotFoundException] { }
        if ($null -ne $attributes) {
            if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Dependency path contains a reparse point (preserved): $cursor"
            }
            if ($cursor -ne $full -and ($attributes -band [IO.FileAttributes]::Directory) -eq 0) {
                throw "Dependency ancestor is not a directory (preserved): $cursor"
            }
        }
        $cursor = [IO.Path]::GetDirectoryName($cursor.TrimEnd('\'))
    }
}
function Assert-Source([string]$Root, [string[]]$Required) {
    Assert-LocalPath $Root
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        throw "Dependency source directory is missing or incomplete (preserved): $Root"
    }
    foreach ($relative in $Required) {
        $file = Join-Path $Root $relative
        Assert-LocalPath $file
        if (-not (Test-Path -LiteralPath $file -PathType Leaf) -or ([IO.FileInfo]::new($file)).Length -le 0) {
            throw "Dependency source is incomplete (preserved): $Root; missing or empty $relative"
        }
    }
}
function Invoke-SourceGit([string[]]$Arguments) {
    $git = Get-Command git -ErrorAction Stop
    & $git.Source @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Dependency Git command failed: git $($Arguments -join ' ')" }
}
function Get-Revision([string]$Root) {
    Assert-LocalPath (Join-Path $Root '.git')
    if (-not (Test-Path -LiteralPath (Join-Path $Root '.git'))) { return $null }
    $git = Get-Command git -ErrorAction Stop
    $revision = & $git.Source -C $Root rev-parse --verify HEAD
    if ($LASTEXITCODE -ne 0 -or $revision -notmatch '^[0-9a-f]{40}$') {
        throw "Cannot identify the existing dependency checkout (preserved): $Root"
    }
    return [string]$revision
}
function Write-Receipt([string]$Name, [string]$Root, [object]$Manifest, [string]$Tag = '') {
    $directory = Join-Path $dependencyRoot $Name
    Assert-LocalPath $directory
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    $receipt = [ordered]@{
        sourceRoot = $Root
        upstream = $Manifest.git
        revision = (Get-Revision $Root)
        selection = $Manifest.selection
    }
    $previous = Join-Path $directory 'source.json'
    Assert-LocalPath $previous
    if ($Tag) { $receipt.tag = $Tag }
    elseif (Test-Path -LiteralPath $previous -PathType Leaf) {
        $old = [IO.File]::ReadAllText($previous, $utf8) | ConvertFrom-Json
        if ($old.revision -eq $receipt.revision -and $old.tag) { $receipt.tag = $old.tag }
    }
    Assert-LocalPath $previous
    [IO.File]::WriteAllText($previous, ($receipt | ConvertTo-Json), $utf8)
}
function Initialize-Source([string]$Name, [object]$Manifest, [string[]]$Required) {
    $vendor = Join-Path $tcardRoot ("third_party\" + $Name)
    $destination = Join-Path $vendor 'local'
    Assert-LocalPath $destination
    if (Test-Path -LiteralPath $destination) {
        Assert-Source $destination $Required
        Write-Receipt $Name $destination $Manifest
        return $destination
    }
    $tag = ''
    if ($Name -eq 'wolfssl') {
        # Only a missing source directory permits an online release lookup.
        $previousProtocol = [Net.ServicePointManager]::SecurityProtocol
        try {
            [Net.ServicePointManager]::SecurityProtocol = $previousProtocol -bor [Net.SecurityProtocolType]::Tls12
            $headers = @{ 'User-Agent' = 'TCard-build'; 'Accept' = 'application/vnd.github+json' }
            $response = Invoke-WebRequest -UseBasicParsing -Uri $Manifest.release_api -Headers $headers -TimeoutSec 120
            $release = $response.Content | ConvertFrom-Json
            if ($release.draft -or $release.prerelease -or $release.tag_name -notmatch '^v[0-9]+\.[0-9]+\.[0-9]+-stable$') {
                throw 'The official release response does not identify a stable wolfSSL tag.'
            }
            $tag = [string]$release.tag_name
        } finally { [Net.ServicePointManager]::SecurityProtocol = $previousProtocol }
    }
    $pending = Join-Path $vendor ('local.pending-' + [Guid]::NewGuid().ToString('N'))
    Assert-LocalPath $pending
    try {
        if ($tag) {
            Invoke-SourceGit -Arguments @('clone', '-c', 'core.autocrlf=false', '--depth', '1', '--single-branch', '--branch', $tag, '--', $Manifest.git, $pending)
        } else {
            if ($Manifest.revision -notmatch '^[0-9a-f]{40}$') { throw "Invalid fixed dependency revision: $Name" }
            Invoke-SourceGit -Arguments @('clone', '-c', 'core.autocrlf=false', '--depth', '1', '--no-checkout', '--', $Manifest.git, $pending)
            Invoke-SourceGit -Arguments @('-C', $pending, 'fetch', '--depth', '1', 'origin', $Manifest.revision)
            Invoke-SourceGit -Arguments @('-C', $pending, 'checkout', '--detach', $Manifest.revision)
            if ((Get-Revision $pending) -ne $Manifest.revision) { throw "Unexpected dependency revision: $Name" }
        }
        Assert-Source $pending $Required
        # Both resolved paths must remain siblings inside this dependency directory.
        $parent = [IO.Path]::GetFullPath($vendor).TrimEnd('\') + '\'
        foreach ($path in @($pending, $destination)) {
            Assert-LocalPath $path
            if (-not [IO.Path]::GetFullPath($path).StartsWith($parent, [StringComparison]::OrdinalIgnoreCase)) {
                throw 'Dependency publication escaped its vendor directory.'
            }
        }
        if (Test-Path -LiteralPath $destination) { throw "Dependency destination appeared during acquisition: $destination" }
        [IO.Directory]::Move($pending, $destination)
        Write-Receipt $Name $destination $Manifest $tag
        return $destination
    } catch {
        throw "Dependency acquisition failed; existing source and any pending checkout are preserved at $pending. $($_.Exception.Message)"
    }
}
function Get-SourceHash([string]$Path) {
    $hasher = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try { return [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $hasher.Dispose() }
}
function Get-WordHash([string[]]$Words) {
    if ($Words.Count -ne 2048 -or @($Words | Select-Object -Unique).Count -ne 2048 -or
        @($Words | Where-Object { $_ -cnotmatch '^[a-z]+$' }).Count -ne 0) {
        throw 'The recovery word list must contain exactly 2048 distinct lowercase ASCII words.'
    }
    $hasher = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::ASCII.GetBytes(($Words -join "`n") + "`n")
        return [BitConverter]::ToString($hasher.ComputeHash($bytes)).Replace('-', '').ToLowerInvariant()
    } finally { $hasher.Dispose() }
}

Assert-LocalPath $dependencyRoot
foreach ($name in @('md4c', 'mnemonic', 'wolfssl')) {
    Assert-LocalPath (Join-Path $tcardRoot ("third_party\" + $name + '\UPSTREAM.json'))
}
$md4cManifest = [IO.File]::ReadAllText((Join-Path $tcardRoot 'third_party\md4c\UPSTREAM.json'), $utf8) | ConvertFrom-Json
$md4cRoot = Initialize-Source 'md4c' $md4cManifest @('src\md4c.c', 'src\md4c.h', 'src\entity.c', 'src\entity.h')
$wordManifest = [IO.File]::ReadAllText((Join-Path $tcardRoot 'third_party\mnemonic\UPSTREAM.json'), $utf8) | ConvertFrom-Json
$wordHeader = Join-Path $tcardRoot 'third_party\mnemonic\generated\english.h'
Assert-LocalPath $wordHeader
if (Test-Path -LiteralPath $wordHeader -PathType Leaf) {
    $header = [IO.File]::ReadAllText($wordHeader, $utf8)
    $words = @([regex]::Matches($header, 'L"([a-z]+)"') | ForEach-Object { $_.Groups[1].Value })
    if ((Get-WordHash $words) -ne $wordManifest.wordlist_sha256) {
        throw "Existing recovery header has incompatible words (preserved): $wordHeader"
    }
} else {
    if (Test-Path -LiteralPath $wordHeader) { throw "Recovery header is not a regular file (preserved): $wordHeader" }
    $wordRoot = Initialize-Source 'mnemonic' $wordManifest @('src\mnemonic\wordlist\english.txt', 'LICENSE')
    $wordPath = Join-Path $wordRoot 'src\mnemonic\wordlist\english.txt'
    if ((Get-SourceHash $wordPath) -ne $wordManifest.wordlist_sha256) {
        throw "Recovery word list hash mismatch (preserved): $wordPath"
    }
    $words = [IO.File]::ReadAllLines($wordPath, $utf8)
    if ((Get-WordHash $words) -ne $wordManifest.wordlist_sha256) { throw 'Recovery word list is incompatible.' }
    $lines = @('#pragma once', '// MIT word list; see ../LICENSE and ../NOTICE.txt.',
        'namespace tcard_words {', 'inline constexpr const wchar_t* english[]={')
    $lines += @($words | ForEach-Object { '    L"' + $_ + '",' })
    $lines += @('};', 'static_assert(sizeof(english)/sizeof(english[0])==2048);', '}')
    Assert-LocalPath $wordHeader
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($wordHeader)) | Out-Null
    $stream = [IO.File]::Open($wordHeader, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $bytes = $utf8.GetBytes(($lines -join "`r`n") + "`r`n")
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } finally { $stream.Dispose() }
}
$wordReceiptRoot = Join-Path $dependencyRoot 'mnemonic'
Assert-LocalPath (Join-Path $wordReceiptRoot 'generated.json')
[IO.Directory]::CreateDirectory($wordReceiptRoot) | Out-Null
$wordReceipt = @{
    header = $wordHeader
    headerSha256 = (Get-SourceHash $wordHeader)
    wordlistSha256 = $wordManifest.wordlist_sha256
    expectedUpstreamRevision = $wordManifest.revision
}
[IO.File]::WriteAllText((Join-Path $wordReceiptRoot 'generated.json'), ($wordReceipt | ConvertTo-Json), $utf8)
$wolfManifest = [IO.File]::ReadAllText((Join-Path $tcardRoot 'third_party\wolfssl\UPSTREAM.json'), $utf8) | ConvertFrom-Json
$sourceRoot = Initialize-Source 'wolfssl' $wolfManifest @('CMakeLists.txt', 'COPYING', 'wolfssl\ssl.h', 'wolfssl\wolfcrypt\aes.h', 'src\ssl.c')
if ($DownloadOnly) { return }

$binaryRoot = Join-Path $dependencyRoot 'wolfssl\build'
$library = Join-Path $binaryRoot 'Release\wolfssl.lib'
Assert-LocalPath $binaryRoot
Assert-LocalPath $library
Assert-LocalPath (Join-Path $binaryRoot 'wolfssl\options.h')
Assert-LocalPath (Join-Path $binaryRoot 'tcard-config.json')
Assert-LocalPath (Join-Path $dependencyRoot 'wolfssl.props')
$cmake = Get-Command cmake -ErrorAction Stop
# Configure is local and cheap; CMake tracks its inputs and incrementally rebuilds developer edits.
# Never replace/reset a source checkout or delete an incompatible existing build cache.
& $cmake.Source -S $sourceRoot -B $binaryRoot -A x64 '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded' '-DBUILD_SHARED_LIBS=no' '-DWOLFSSL_TLS13=yes' '-DWOLFSSL_PSK=yes' '-DWOLFSSL_EARLYDATA=no' '-DWOLFSSL_KEYING_MATERIAL=yes' '-DKEEP_PEER_CERT=1' '-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS /DKEEP_PEER_CERT' '-DWOLFSSL_EXAMPLES=no' '-DWOLFSSL_CRYPT_TESTS=no' '-DWOLFSSL_CRYPT_TESTS_LIBS=no' '-DWOLFSSL_ASM=no'
if ($LASTEXITCODE -ne 0) { throw "wolfSSL configuration failed; source and cache are preserved: $binaryRoot" }
& $cmake.Source --build $binaryRoot --config Release --parallel 6
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $library -PathType Leaf) -or
    -not (Test-Path -LiteralPath (Join-Path $binaryRoot 'wolfssl\options.h') -PathType Leaf)) {
    throw 'wolfSSL static build failed.'
}
$config = @{ sourceRoot = $sourceRoot; configuration = 'tls13-exporter-static-mt-peer-cert-v3'; revision = (Get-Revision $sourceRoot) }
[IO.File]::WriteAllText((Join-Path $binaryRoot 'tcard-config.json'), ($config | ConvertTo-Json), $utf8)
$sourceValue = [Security.SecurityElement]::Escape($sourceRoot)
$buildValue = [Security.SecurityElement]::Escape($binaryRoot)
$props = '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003"><PropertyGroup><TCardWolfSslSourceRoot>' + $sourceValue + '</TCardWolfSslSourceRoot><TCardWolfSslBuildRoot>' + $buildValue + '</TCardWolfSslBuildRoot></PropertyGroup></Project>'
[IO.File]::WriteAllText((Join-Path $dependencyRoot 'wolfssl.props'), $props, $utf8)
