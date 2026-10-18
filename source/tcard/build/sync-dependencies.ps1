[CmdletBinding()]
param([switch]$DownloadOnly)
$ErrorActionPreference = 'Stop'
$tcardRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vendorRoot = Join-Path $tcardRoot 'third_party\wolfssl'
$manifest = Get-Content -LiteralPath (Join-Path $vendorRoot 'UPSTREAM.json') -Raw | ConvertFrom-Json
$dependencyRoot = Join-Path $tcardRoot 'out\deps'

function Get-ArchiveHash([string]$Path) {
    $hasher = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try { return [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $hasher.Dispose() }
}
# Resolve the latest official stable release on each build; do not silently use an older release.
$previousProtocol = [Net.ServicePointManager]::SecurityProtocol
try {
    [Net.ServicePointManager]::SecurityProtocol = $previousProtocol -bor [Net.SecurityProtocolType]::Tls12
    $headers = @{ 'User-Agent' = 'TCard-build'; 'Accept' = 'application/vnd.github+json' }
    $response = Invoke-WebRequest -UseBasicParsing -Uri $manifest.release_api -Headers $headers -TimeoutSec 120
    $release = $response.Content | ConvertFrom-Json
    if ($release.draft -or $release.prerelease -or -not $release.tag_name -or -not $release.zipball_url) {
        throw 'The official release response does not identify a stable source release.'
    }
    $releaseId = [long]$release.id
    if ($releaseId -le 0) { throw 'Invalid upstream release identifier.' }
    $generationRoot = Join-Path $dependencyRoot ("wolfssl\$releaseId")
    $archive = Join-Path $generationRoot 'source.zip'
    $receiptPath = Join-Path $generationRoot 'download.json'
    New-Item -ItemType Directory -Path $generationRoot -Force | Out-Null
    if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
        $pending = Join-Path $generationRoot ([Guid]::NewGuid().ToString('N') + '.partial')
        try {
            Invoke-WebRequest -UseBasicParsing -Uri $release.zipball_url -Headers @{ 'User-Agent' = 'TCard-build' } -OutFile $pending -TimeoutSec 120
            $receipt = @{ releaseId = $releaseId; tag = $release.tag_name; url = $release.zipball_url; sha256 = (Get-ArchiveHash $pending) }
            # Hashes here detect later cache corruption; they are not an upstream signature.
            Move-Item -LiteralPath $pending -Destination $archive
            [IO.File]::WriteAllText($receiptPath, ($receipt | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
        } finally {
            if (Test-Path -LiteralPath $pending) { Remove-Item -LiteralPath $pending }
        }
    }
} finally { [Net.ServicePointManager]::SecurityProtocol = $previousProtocol }
if (-not (Test-Path -LiteralPath $receiptPath -PathType Leaf)) { throw "Download metadata is missing: $receiptPath" }
$receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
if ($receipt.releaseId -ne $releaseId -or $receipt.tag -ne $release.tag_name -or (Get-ArchiveHash $archive) -ne $receipt.sha256) {
    throw "wolfSSL cache metadata/hash mismatch: $generationRoot"
}
if ($DownloadOnly) { return }

# Separate release directories avoid mixing an updated dependency with an older source/build tree.
$sourceParent = Join-Path $generationRoot 'source'
$binaryRoot = Join-Path $generationRoot 'build'
$stamp = Join-Path $binaryRoot 'tcard-config.json'
$library = Join-Path $binaryRoot 'Release\wolfssl.lib'
$cacheVersion = 'tls13-exporter-static-mt-peer-cert-v3'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$sourceZip = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    $roots = @($sourceZip.Entries | ForEach-Object { ($_.FullName -split '/')[0] } | Select-Object -Unique)
    if ($roots.Count -ne 1 -or [string]::IsNullOrWhiteSpace($roots[0]) -or $roots[0] -match '[\\:]' -or $roots[0] -in @('.', '..')) {
        throw 'Unexpected upstream source archive layout.'
    }
    $sourceRoot = Join-Path $sourceParent $roots[0]
    $extracted = Join-Path $generationRoot 'extracted.sha256'
    if (-not (Test-Path -LiteralPath $extracted) -or [IO.File]::ReadAllText($extracted) -ne $receipt.sha256) {
        foreach ($entry in $sourceZip.Entries) {
            $targetPath = [IO.Path]::GetFullPath((Join-Path $sourceParent $entry.FullName))
            if (-not $targetPath.StartsWith($sourceRoot + '\', [StringComparison]::OrdinalIgnoreCase) -and $targetPath.TrimEnd('\') -ne $sourceRoot) { throw 'Unsafe archive path.' }
            if ($entry.Name -eq '') { [IO.Directory]::CreateDirectory($targetPath) | Out-Null; continue }
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($targetPath)) | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $targetPath, $true)
        }
        [IO.File]::WriteAllText($extracted, $receipt.sha256, [Text.UTF8Encoding]::new($false))
    }
} finally { $sourceZip.Dispose() }
if (-not (Test-Path -LiteralPath (Join-Path $sourceRoot 'CMakeLists.txt'))) { throw 'Upstream CMake input is missing.' }
$cache = if (Test-Path -LiteralPath $stamp) { Get-Content -LiteralPath $stamp -Raw | ConvertFrom-Json } else { $null }
if (-not $cache -or $cache.sha256 -ne $receipt.sha256 -or $cache.configuration -ne $cacheVersion -or
    -not (Test-Path -LiteralPath $library) -or -not (Test-Path -LiteralPath (Join-Path $binaryRoot 'wolfssl\options.h'))) {
    $cmake = Get-Command cmake -ErrorAction Stop
    & $cmake.Source -S $sourceRoot -B $binaryRoot -A x64 '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded' '-DBUILD_SHARED_LIBS=no' '-DWOLFSSL_TLS13=yes' '-DWOLFSSL_PSK=yes' '-DWOLFSSL_EARLYDATA=no' '-DWOLFSSL_KEYING_MATERIAL=yes' '-DKEEP_PEER_CERT=1' '-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS /DKEEP_PEER_CERT' '-DWOLFSSL_EXAMPLES=no' '-DWOLFSSL_CRYPT_TESTS=no' '-DWOLFSSL_CRYPT_TESTS_LIBS=no' '-DWOLFSSL_ASM=no'
    if ($LASTEXITCODE -ne 0) { throw 'wolfSSL configuration failed.' }
    & $cmake.Source --build $binaryRoot --config Release --parallel 6
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $library)) { throw 'wolfSSL static build failed.' }
    $config = @{ sha256 = $receipt.sha256; configuration = $cacheVersion }
    [IO.File]::WriteAllText($stamp, ($config | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
}
$sourceValue = [Security.SecurityElement]::Escape($sourceRoot)
$buildValue = [Security.SecurityElement]::Escape($binaryRoot)
$props = '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003"><PropertyGroup><TCardWolfSslSourceRoot>' + $sourceValue + '</TCardWolfSslSourceRoot><TCardWolfSslBuildRoot>' + $buildValue + '</TCardWolfSslBuildRoot></PropertyGroup></Project>'
[IO.File]::WriteAllText((Join-Path $dependencyRoot 'wolfssl.props'), $props, [Text.UTF8Encoding]::new($false))
