[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $ArchivePath,

    [Parameter(Mandatory)]
    [string] $ExpectedExecutable,

    [Parameter(Mandatory)]
    [ValidateSet('ko', 'en')]
    [string] $Language,

    [Parameter(Mandatory)]
    [string] $Version
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Historical bilingual archives remain verifiable. Starting with 0.4.2, only
# the English product is built and the README documents full settings files.
$coreVersionMatch = [regex]::Match($Version, '^(\d+\.\d+\.\d+)(?:-|$)')
if (-not $coreVersionMatch.Success) { throw "Invalid numeric release version: $Version" }
$usesFullSettings = [version] $coreVersionMatch.Groups[1].Value -ge [version] '0.4.2'
if ($usesFullSettings -and $Language -ne 'en') {
    throw 'Releases starting with 0.4.2 are English-only.'
}

function Get-LowerSha256 {
    param([Parameter(Mandatory)] [string] $Path)
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-EntrySha256 {
    param([Parameter(Mandatory)] $Entry)

    $stream = $Entry.Open()
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = $sha256.ComputeHash($stream)
        ([BitConverter]::ToString($bytes)).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $sha256.Dispose()
        $stream.Dispose()
    }
}

function Read-ZipText {
    param([Parameter(Mandatory)] $Entry)

    $stream = $Entry.Open()
    $reader = [IO.StreamReader]::new(
        $stream,
        [Text.UTF8Encoding]::new($false, $true),
        $true)
    try {
        $reader.ReadToEnd()
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

function Get-CanonicalArchivePath {
    param([Parameter(Mandatory)] [string] $Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw 'Archive path is empty.'
    }
    $normalisedPath = $Path.Replace('\', '/')
    $isDirectory = $normalisedPath.EndsWith('/')
    $pathWithoutTrailingSlash = if ($isDirectory) {
        $normalisedPath.Substring(0, $normalisedPath.Length - 1)
    }
    else {
        $normalisedPath
    }

    if ([string]::IsNullOrWhiteSpace($pathWithoutTrailingSlash) -or
        $pathWithoutTrailingSlash.StartsWith('/') -or
        $pathWithoutTrailingSlash -match '^[A-Za-z]:') {
        throw "Unsafe archive path: $Path"
    }

    $segments = @($pathWithoutTrailingSlash -split '/')
    if (@($segments | Where-Object { $_ -in @('', '.', '..') }).Count -gt 0) {
        throw "Non-canonical archive path: $Path"
    }
    foreach ($segment in $segments) {
        if ($segment -match '[\x00-\x1f<>:"|?*]' -or
            $segment.EndsWith('.') -or $segment.EndsWith(' ')) {
            throw "Archive path is unsafe on Windows: $Path"
        }
        $baseName = ($segment -split '\.', 2)[0]
        if ($baseName -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$') {
            throw "Archive path uses a reserved Windows name: $Path"
        }
    }

    $canonicalPath = $segments -join '/'
    if (-not $canonicalPath.Equals(
        $pathWithoutTrailingSlash,
        [StringComparison]::Ordinal)) {
        throw "Non-canonical archive path: $Path"
    }

    [pscustomobject]@{
        Path = $canonicalPath
        IsDirectory = $isDirectory
    }
}

function Resolve-ArchiveLink {
    param(
        [Parameter(Mandatory)] [string] $SourcePath,
        [Parameter(Mandatory)] [string] $Target
    )

    $pathOnly = ($Target -split '[?#]', 2)[0]
    if ([string]::IsNullOrWhiteSpace($pathOnly)) {
        return $null
    }

    try {
        $pathOnly = [Uri]::UnescapeDataString($pathOnly)
    }
    catch {
        throw "Invalid escaped Markdown link '$Target' in $SourcePath."
    }

    $pathOnly = $pathOnly.Replace('\', '/')
    if ($pathOnly.StartsWith('/') -or $pathOnly -match '^[A-Za-z]:') {
        throw "Absolute Markdown link '$Target' is not allowed in $SourcePath."
    }

    $sourceDirectory = [IO.Path]::GetDirectoryName($SourcePath.Replace('/', '\'))
    $combined = if ([string]::IsNullOrWhiteSpace($sourceDirectory)) {
        $pathOnly
    }
    else {
        ($sourceDirectory.Replace('\', '/') + '/' + $pathOnly)
    }

    $segments = [Collections.Generic.List[string]]::new()
    foreach ($segment in ($combined -split '/')) {
        if ([string]::IsNullOrEmpty($segment) -or $segment -eq '.') {
            continue
        }
        if ($segment -eq '..') {
            if ($segments.Count -eq 0) {
                throw "Markdown link '$Target' escapes the package root in $SourcePath."
            }
            $segments.RemoveAt($segments.Count - 1)
            continue
        }
        $segments.Add($segment)
    }

    $segments -join '/'
}

function Get-ReferenceLabel {
    param([Parameter(Mandatory)] [string] $Label)
    ([regex]::Replace($Label.Trim(), '\s+', ' ')).ToLowerInvariant()
}

function Assert-MarkdownTarget {
    param(
        [Parameter(Mandatory)] [string] $SourcePath,
        [Parameter(Mandatory)] [string] $Target,
        [Parameter(Mandatory)] $EntriesByPath
    )

    $targetValue = $Target.Trim().Trim('<', '>')
    if ([string]::IsNullOrWhiteSpace($targetValue)) {
        throw "Empty Markdown link in $SourcePath."
    }
    if ($targetValue.StartsWith('#')) {
        return
    }
    if ($targetValue -match '^[A-Za-z]:') {
        throw "Windows drive path is not allowed in ${SourcePath}: $targetValue"
    }
    if ($targetValue.StartsWith('//')) {
        throw "Protocol-relative Markdown link is not allowed in ${SourcePath}: $targetValue"
    }
    $schemeMatch = [regex]::Match($targetValue, '^(?<scheme>[A-Za-z][A-Za-z0-9+.-]*):')
    if ($schemeMatch.Success) {
        $scheme = $schemeMatch.Groups['scheme'].Value.ToLowerInvariant()
        if ($scheme -notin @('http', 'https', 'mailto')) {
            throw "Unsafe Markdown link scheme in ${SourcePath}: $targetValue"
        }
        return
    }

    $resolvedTarget = Resolve-ArchiveLink -SourcePath $SourcePath -Target $targetValue
    if ($null -eq $resolvedTarget) {
        return
    }
    if ($resolvedTarget -ieq $SourcePath -and -not $targetValue.Contains('#')) {
        throw "Markdown link points back to the same file: $SourcePath -> $targetValue"
    }
    if (-not $EntriesByPath.ContainsKey($resolvedTarget)) {
        throw "Broken local Markdown link: $SourcePath -> $targetValue"
    }
}

function Assert-MarkdownLinks {
    param(
        [Parameter(Mandatory)] [string] $SourcePath,
        [Parameter(Mandatory)] [string] $Markdown,
        [Parameter(Mandatory)] $EntriesByPath
    )

    $referenceDefinitions = [Collections.Generic.Dictionary[string, string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    $referenceDefinitionPattern = [regex]::new(
        '(?m)^[ \t]{0,3}\[(?<label>[^\]\r\n]+)\]:[ \t]*(?:<(?<angle>[^>\r\n]+)>|(?<plain>\S+))')
    foreach ($definition in $referenceDefinitionPattern.Matches($Markdown)) {
        $label = Get-ReferenceLabel -Label $definition.Groups['label'].Value
        $target = if ($definition.Groups['angle'].Success) {
            $definition.Groups['angle'].Value
        }
        else {
            $definition.Groups['plain'].Value
        }
        if ($referenceDefinitions.ContainsKey($label)) {
            throw "Duplicate Markdown reference '$label' in $SourcePath."
        }
        $referenceDefinitions.Add($label, $target)
        Assert-MarkdownTarget `
            -SourcePath $SourcePath `
            -Target $target `
            -EntriesByPath $EntriesByPath
    }

    $inlineLinkPattern = [regex]::new(
        '\]\(\s*(?:<(?<angle>[^>\r\n]+)>|(?<plain>[^)\s]+))')
    foreach ($linkMatch in $inlineLinkPattern.Matches($Markdown)) {
        $target = if ($linkMatch.Groups['angle'].Success) {
            $linkMatch.Groups['angle'].Value
        }
        else {
            $linkMatch.Groups['plain'].Value
        }
        Assert-MarkdownTarget `
            -SourcePath $SourcePath `
            -Target $target `
            -EntriesByPath $EntriesByPath
    }

    $referenceLinkPattern = [regex]::new(
        '!?\[(?<text>[^\]\r\n]+)\]\[(?<label>[^\]\r\n]*)\]')
    foreach ($linkMatch in $referenceLinkPattern.Matches($Markdown)) {
        $rawLabel = $linkMatch.Groups['label'].Value
        if ([string]::IsNullOrWhiteSpace($rawLabel)) {
            $rawLabel = $linkMatch.Groups['text'].Value
        }
        $label = Get-ReferenceLabel -Label $rawLabel
        if (-not $referenceDefinitions.ContainsKey($label)) {
            throw "Undefined Markdown reference '$label' in $SourcePath."
        }
    }
}

$resolvedArchive = (Resolve-Path -LiteralPath $ArchivePath).Path
$resolvedExecutable = (Resolve-Path -LiteralPath $ExpectedExecutable).Path
$expectedArchiveName = "Mic-FX-Bridge-$Version-windows-x64-$Language.zip"
if ([IO.Path]::GetFileName($resolvedArchive) -cne $expectedArchiveName) {
    throw "Unexpected archive name. Expected $expectedArchiveName."
}

$sidecarPath = $resolvedArchive + '.sha256'
if (-not (Test-Path -LiteralPath $sidecarPath -PathType Leaf)) {
    throw "Missing archive checksum: $sidecarPath"
}
$sidecarText = (Get-Content -Raw -LiteralPath $sidecarPath).Trim()
$sidecarMatch = [regex]::Match($sidecarText, '^([0-9A-Fa-f]{64})  ([^\r\n]+)$')
if (-not $sidecarMatch.Success) {
    throw "Invalid archive checksum format: $sidecarPath"
}
if ($sidecarMatch.Groups[2].Value -cne $expectedArchiveName) {
    throw "Archive checksum names the wrong file: $($sidecarMatch.Groups[2].Value)"
}
$archiveHash = Get-LowerSha256 -Path $resolvedArchive
if ($sidecarMatch.Groups[1].Value.ToLowerInvariant() -cne $archiveHash) {
    throw "Archive checksum mismatch: $expectedArchiveName"
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($resolvedArchive)
try {
    $entriesByPath = [Collections.Generic.Dictionary[string, object]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    $directoryPaths = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)

    foreach ($entry in $archive.Entries) {
        $externalAttributes = [BitConverter]::ToUInt32(
            [BitConverter]::GetBytes([int] $entry.ExternalAttributes),
            0)
        if (($externalAttributes -band 0x400) -ne 0) {
            throw "Archive entry is a Windows reparse point: $($entry.FullName)"
        }
        $unixFileType = (($externalAttributes -shr 16) -band 0xf000)
        if ($unixFileType -notin @(0, 0x4000, 0x8000)) {
            throw "Archive entry is not a regular file or directory: $($entry.FullName)"
        }

        $canonicalEntry = Get-CanonicalArchivePath -Path $entry.FullName
        $entryPath = $canonicalEntry.Path
        if ($unixFileType -ne 0 -and
            (($unixFileType -eq 0x4000) -ne $canonicalEntry.IsDirectory)) {
            throw "Archive entry type does not match its path: $($entry.FullName)"
        }
        if ($canonicalEntry.IsDirectory) {
            if (-not $directoryPaths.Add($entryPath)) {
                throw "Duplicate archive directory: $entryPath"
            }
            continue
        }
        if ($entriesByPath.ContainsKey($entryPath)) {
            throw "Duplicate archive path: $entryPath"
        }
        $entriesByPath.Add($entryPath, $entry)
    }

    foreach ($entryPath in $entriesByPath.Keys) {
        if ($directoryPaths.Contains($entryPath)) {
            throw "Archive path is both a file and a directory: $entryPath"
        }
        $segments = @($entryPath -split '/')
        for ($index = 1; $index -lt $segments.Count; ++$index) {
            $parentPath = ($segments[0..($index - 1)] -join '/')
            if ($entriesByPath.ContainsKey($parentPath)) {
                throw "Archive file is also used as a directory: $parentPath"
            }
        }
    }
    foreach ($directoryPath in $directoryPaths) {
        $segments = @($directoryPath -split '/')
        for ($index = 1; $index -le $segments.Count; ++$index) {
            $parentPath = ($segments[0..($index - 1)] -join '/')
            if ($entriesByPath.ContainsKey($parentPath)) {
                throw "Archive file is also used as a directory: $parentPath"
            }
        }
    }

    $requiredPaths = @(
        'Mic FX Bridge.exe',
        'README.md',
        'PRIVACY.md',
        'SECURITY.md',
        'THIRD_PARTY_NOTICES.md',
        'LICENSE',
        'SHA256SUMS.txt',
        'THIRD_PARTY_LICENSES/JUCE-LICENSE.md',
        'THIRD_PARTY_LICENSES/VST3-SDK-LICENSE.txt',
        'THIRD_PARTY_LICENSES/FLAC-LICENSE.txt',
        'THIRD_PARTY_LICENSES/OGG-VORBIS-LICENSE.txt',
        'THIRD_PARTY_LICENSES/PNGLIB-LICENSE.txt',
        'THIRD_PARTY_LICENSES/ZLIB-LICENSE.txt',
        'THIRD_PARTY_LICENSES/IJG-JPEG-README.txt',
        'THIRD_PARTY_LICENSES/HARFBUZZ-LICENSE.txt',
        'THIRD_PARTY_LICENSES/SHEENBIDI-LICENSE.txt',
        'THIRD_PARTY_LICENSES/LUNASVG-LICENSE.txt',
        'THIRD_PARTY_LICENSES/PLUTOVG-LICENSE.txt'
    )
    foreach ($requiredPath in $requiredPaths) {
        if (-not $entriesByPath.ContainsKey($requiredPath)) {
            throw "Required package file is missing: $requiredPath"
        }
    }

    $expectedExecutableHash = Get-LowerSha256 -Path $resolvedExecutable
    $packagedExecutableHash = Get-EntrySha256 -Entry $entriesByPath['Mic FX Bridge.exe']
    if ($packagedExecutableHash -cne $expectedExecutableHash) {
        throw 'Packaged executable does not match the executable from this build.'
    }

    $manifestText = Read-ZipText -Entry $entriesByPath['SHA256SUMS.txt']
    $manifest = [Collections.Generic.Dictionary[string, string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($line in ($manifestText -split '\r?\n')) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        $match = [regex]::Match($line, '^([0-9A-Fa-f]{64})  (.+)$')
        if (-not $match.Success) {
            throw "Invalid SHA256SUMS entry: $line"
        }
        $manifestPath = $match.Groups[2].Value.Replace('\', '/')
        if ($manifest.ContainsKey($manifestPath)) {
            throw "Duplicate SHA256SUMS entry: $manifestPath"
        }
        $manifest.Add($manifestPath, $match.Groups[1].Value.ToLowerInvariant())
    }

    $hashedPaths = @($entriesByPath.Keys | Where-Object { $_ -cne 'SHA256SUMS.txt' })
    if ($manifest.Count -ne $hashedPaths.Count) {
        throw "SHA256SUMS covers $($manifest.Count) files; expected $($hashedPaths.Count)."
    }
    foreach ($entryPath in $hashedPaths) {
        if (-not $manifest.ContainsKey($entryPath)) {
            throw "SHA256SUMS is missing: $entryPath"
        }
        $actualHash = Get-EntrySha256 -Entry $entriesByPath[$entryPath]
        if ($manifest[$entryPath] -cne $actualHash) {
            throw "SHA256SUMS mismatch: $entryPath"
        }
    }

    $topLevelMarkdown = @($entriesByPath.Keys | Where-Object {
        $_ -notmatch '/' -and $_.EndsWith('.md', [StringComparison]::OrdinalIgnoreCase)
    })
    foreach ($markdownPath in $topLevelMarkdown) {
        $markdown = Read-ZipText -Entry $entriesByPath[$markdownPath]
        Assert-MarkdownLinks `
            -SourcePath $markdownPath `
            -Markdown $markdown `
            -EntriesByPath $entriesByPath
    }

    $packageReadme = Read-ZipText -Entry $entriesByPath['README.md']
    $packagePrivacy = Read-ZipText -Entry $entriesByPath['PRIVACY.md']
    $packageHeadingCount = [regex]::Matches($packageReadme, '(?m)^## ').Count
    if ($packageHeadingCount -ne 8) {
        throw "Package README has $packageHeadingCount sections; expected eight including persistence guidance, after removing source-only sections."
    }

    $persistenceFragments = if ($usesFullSettings) {
        @('Save settings', 'Load settings', 'MicVstBridge.settings', 'five seconds')
    }
    else {
        @('.micfxchain', 'chain-recovery', '.startup-backup')
    }
    foreach ($fragment in $persistenceFragments) {
        if (-not $packageReadme.Contains($fragment)) {
            throw "Package README is missing persistence guidance: $fragment"
        }
    }

    if ([version] $coreVersionMatch.Groups[1].Value -ge [version] '0.4.4') {
        $requiredReadmeFragments = @('Built-in microphone measurement and correction were removed in 0.4.4.',
            'CABLE Input', 'CABLE Output', 'startup-backup', 'VST3', '[MIT License](LICENSE)', '[PRIVACY.md](PRIVACY.md)')
        $forbiddenReadmeFragments = @('## Measure and flatten', 'Measure / correct', 'Apply profile',
            '100% correction', '## Build from source', '## English edition')
        $requiredPrivacyFragments = @('MicVstBridge.settings', 'retired fields', 'startup-backup')
        $forbiddenPrivacyFragments = @('During a guided microphone measurement', 'two raw captures')
    }
    elseif ($Language -eq 'ko') {
        $requiredReadmeFragments = @(
            '1/2',
            '2/2',
            '5 cycles',
            '100%',
            'pre-trim',
            'limiter',
            '[MIT License](LICENSE)',
            '[PRIVACY.md](PRIVACY.md)'
        )
        $forbiddenReadmeFragments = @(
            '1/3',
            '2/3',
            '3/3',
            '15 cycles',
            '70%',
            '+4 dB boost',
            'powershell -ExecutionPolicy',
            'scripts\build.ps1',
            'dist\',
            'PRIVACY.ko.md'
        )
        $requiredPrivacyFragments = @(
            '%APPDATA%\MicVstBridge\MicVstBridge.settings',
            '100%',
            'SHA-256'
        )
        $forbiddenPrivacyFragments = @(
            '70%',
            'positive-gain',
            'headroom pre-trim'
        )
    }
    else {
        $requiredReadmeFragments = @(
            'reference 1/2',
            'vocal microphone 2/2',
            '5 cycles',
            '100% correction',
            'attenuation-only',
            'final limiter',
            'Mic FX Bridge original code and artwork are covered by the [MIT License](LICENSE).'
        )
        $forbiddenReadmeFragments = @(
            'reference repeat',
            '15 cycles',
            '70% correction strength',
            '+4 dB boost',
            'headroom pre-trim',
            '## Korean and English editions',
            '## Build from source',
            "The repository's original code and artwork are"
        )
        $requiredPrivacyFragments = @('two raw captures')
        $forbiddenPrivacyFragments = @(
            'three raw captures',
            'reference repeatability difference',
            'positive-gain sum',
            'headroom pre-trim'
        )
    }

    foreach ($fragment in $requiredReadmeFragments) {
        if (-not $packageReadme.Contains($fragment)) {
            throw "Package README is missing required $Language text: $fragment"
        }
    }
    foreach ($fragment in $forbiddenReadmeFragments) {
        if ($packageReadme.Contains($fragment)) {
            throw "Package README contains obsolete or source-only text: $fragment"
        }
    }
    foreach ($fragment in $requiredPrivacyFragments) {
        if (-not $packagePrivacy.Contains($fragment)) {
            throw "Package privacy notice is missing required $Language text: $fragment"
        }
    }
    foreach ($fragment in $forbiddenPrivacyFragments) {
        if ($packagePrivacy.Contains($fragment)) {
            throw "Package privacy notice contains obsolete text: $fragment"
        }
    }
}
finally {
    $archive.Dispose()
}

Write-Host "Verified release package: $expectedArchiveName" -ForegroundColor Green
