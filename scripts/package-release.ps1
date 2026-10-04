[CmdletBinding()]
param(
    [string] $Version = '',
    [string] $BuildDirectory = '',
    [string] $OutputDirectory = '',
    [switch] $LocalCandidate
)

$ErrorActionPreference = 'Stop'

$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$versionLine = Select-String -LiteralPath (Join-Path $projectRoot 'CMakeLists.txt') `
    -Pattern '^project\(MicVstBridge VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES CXX\)$' |
    Select-Object -First 1
if ($null -eq $versionLine) {
    throw 'Unable to read the project version from CMakeLists.txt.'
}
$projectVersion = $versionLine.Matches[0].Groups[1].Value

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = $projectVersion
}

# Release names may carry a SemVer prerelease suffix while Windows/CMake build
# outputs continue to use the numeric core version. Build metadata is omitted
# deliberately so every accepted version is also an unambiguous file name.
$numericIdentifier = '(?:0|[1-9][0-9]*)'
$nonNumericIdentifier = '(?:[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)'
$prereleaseIdentifier = "(?:$numericIdentifier|$nonNumericIdentifier)"
$releaseVersionPattern = "^(?<core>$numericIdentifier\.$numericIdentifier\.$numericIdentifier)(?:-(?:$prereleaseIdentifier)(?:\.$prereleaseIdentifier)*)?$"
$releaseVersionMatch = [regex]::Match($Version, $releaseVersionPattern)
if (-not $releaseVersionMatch.Success) {
    throw "Version must be SemVer in the form <major>.<minor>.<patch>[-prerelease]: $Version"
}
$releaseCoreVersion = $releaseVersionMatch.Groups['core'].Value
if ($releaseCoreVersion -ne $projectVersion) {
    throw "Release core version $releaseCoreVersion does not match project version $projectVersion."
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path (Join-Path $projectRoot 'dist\packages') $Version
}
else {
    $OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$resolvedOutput = (Resolve-Path -LiteralPath $OutputDirectory).Path
foreach ($language in @('en')) {
    $existingArchive = Join-Path $resolvedOutput "Mic-FX-Bridge-$Version-windows-x64-$language.zip"
    if ((Test-Path -LiteralPath $existingArchive) -or (Test-Path -LiteralPath ($existingArchive + '.sha256'))) {
        throw "Package $Version already exists. Use a new version; published local archives are immutable."
    }
}
$manifestPath = Join-Path $projectRoot "dist/$projectVersion/build-manifest.json"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw 'Versioned build manifest missing. Run the full build first.'
}
$buildManifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($buildManifest.version -ne $projectVersion -or $buildManifest.configuration -ne 'Release' -or
    $buildManifest.sourceCommit -notmatch '^[0-9a-f]{40}$') { throw 'Invalid release build provenance.' }
$candidateManifest = $buildManifest.PSObject.Properties.Name -contains 'sourceState' -and
    $buildManifest.sourceState -eq 'uncommitted-local-candidate'
if ($candidateManifest -ne [bool]$LocalCandidate) {
    throw 'Local candidate provenance requires explicit -LocalCandidate; ordinary release mode does not accept it.'
}
if ($LocalCandidate) {
    if ($Version -notmatch '-local\.') { throw 'Local candidate packages require a -local.<number> suffix.' }
    . (Join-Path $PSScriptRoot 'SourceSnapshot.ps1')
    $currentSnapshot = Get-MicSourceSnapshot -ProjectRoot $projectRoot
    $currentCommit = & git -C $projectRoot rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $currentCommit -ne $buildManifest.sourceCommit -or
        $currentSnapshot.sha256 -ne $buildManifest.sourceSnapshotSha256) {
        throw 'Current source inputs differ from the verified local candidate build.'
    }
}
if (@($buildManifest.editions).Count -ne 1 -or $buildManifest.editions[0].language -ne 'en') {
    throw 'New releases require exactly one English edition in the build manifest.'
}
foreach ($language in @('en')) {
    $entry = @($buildManifest.editions | Where-Object language -eq $language)
    $exePath = Join-Path $projectRoot "dist/$projectVersion/$language/Mic FX Bridge.exe"
    if ($entry.Count -ne 1 -or (Get-FileHash -LiteralPath $exePath).Hash -ne $entry[0].sha256) {
        throw "The $language executable differs from its verified build manifest."
    }
}

$projectLicense = Join-Path $projectRoot 'LICENSE'
if (-not (Test-Path -LiteralPath $projectLicense -PathType Leaf)) {
    throw 'Project LICENSE is required before creating a distributable archive.'
}
$projectLicenseText = Get-Content -Raw -LiteralPath $projectLicense
if ($projectLicenseText -notmatch '(?m)^MIT License\s*$' -or
    $projectLicenseText -match '(?i)TODO|<copyright|\[copyright') {
    throw 'Project LICENSE must contain the completed MIT licence without placeholders.'
}

$juceCandidates = @((Join-Path $projectRoot '.deps\JUCE'))
if (-not [string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $juceCandidates += (Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) '_deps\juce-src')
}
$juceSource = $juceCandidates |
    Where-Object { Test-Path -LiteralPath (Join-Path $_ 'LICENSE.md') -PathType Leaf } |
    Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($juceSource)) {
    throw 'Pinned JUCE source was not found. Build first, or pass -BuildDirectory so dependency licences can be packaged.'
}

$thirdPartyLicenses = @(
    @{ Source = 'LICENSE.md'; Destination = 'JUCE-LICENSE.md' },
    @{ Source = 'modules\juce_audio_processors_headless\format_types\VST3_SDK\LICENSE.txt'; Destination = 'VST3-SDK-LICENSE.txt' },
    @{ Source = 'modules\juce_audio_formats\codecs\flac\Flac Licence.txt'; Destination = 'FLAC-LICENSE.txt' },
    @{ Source = 'modules\juce_audio_formats\codecs\oggvorbis\Ogg Vorbis Licence.txt'; Destination = 'OGG-VORBIS-LICENSE.txt' },
    @{ Source = 'modules\juce_graphics\image_formats\pnglib\LICENSE'; Destination = 'PNGLIB-LICENSE.txt' },
    @{ Source = 'modules\juce_core\zip\zlib\README'; Destination = 'ZLIB-LICENSE.txt' },
    @{ Source = 'modules\juce_graphics\image_formats\jpglib\README'; Destination = 'IJG-JPEG-README.txt' },
    @{ Source = 'modules\juce_graphics\fonts\harfbuzz\COPYING'; Destination = 'HARFBUZZ-LICENSE.txt' },
    @{ Source = 'modules\juce_graphics\unicode\sheenbidi\LICENSE'; Destination = 'SHEENBIDI-LICENSE.txt' },
    @{ Source = 'modules\juce_graphics\drawables\lunasvg\LICENSE'; Destination = 'LUNASVG-LICENSE.txt' },
    @{ Source = 'modules\juce_graphics\drawables\lunasvg\plutovg\LICENSE'; Destination = 'PLUTOVG-LICENSE.txt' }
)
foreach ($notice in $thirdPartyLicenses) {
    $noticeSource = Join-Path $juceSource $notice.Source
    if (-not (Test-Path -LiteralPath $noticeSource -PathType Leaf)) {
        throw "Required third-party licence file was not found: $noticeSource"
    }
}

$editions = @(
    @{ Language = 'en'; Readme = 'README.en.md'; Privacy = 'PRIVACY.en.md' }
)

$utf8NoBom = [Text.UTF8Encoding]::new($false)
$repositoryNavigationPattern = [regex]::new(
    '(?m)^(?:\[[^\]\r\n]+\]\(README\.(?:ko|en)\.md\)\s*\|\s*)?\[[^\]\r\n]+\]\(README\.md\)\r?\n\r?\n')

function Write-PackageReadme {
    param(
        [Parameter(Mandatory)]
        [string] $Source,

        [Parameter(Mandatory)]
        [string] $SourcePrivacyName,

        [Parameter(Mandatory)]
        [string] $Destination
    )

    $sourceText = [IO.File]::ReadAllText($Source, [Text.Encoding]::UTF8)
    $packageText = $repositoryNavigationPattern.Replace($sourceText, '', 1)
    if ($packageText -eq $sourceText) {
        throw "Expected repository navigation was not found in $Source."
    }

    $sourcePrivacyLink = "[$SourcePrivacyName]($SourcePrivacyName)"
    if (-not $packageText.Contains($sourcePrivacyLink)) {
        throw "Expected privacy link $sourcePrivacyLink was not found in $Source."
    }
    $packageText = $packageText.Replace($sourcePrivacyLink, '[PRIVACY.md](PRIVACY.md)')

    # Release archives are binary distributions. Keep development instructions
    # in the source-tree README rather than directing binary-package users to
    # build scripts that are not part of their ZIP.
    # Select the English guide's source-only sections explicitly; changes to
    # product guidance must not accidentally remove a neighbouring section.
    $sectionHeadings = [regex]::Matches($packageText, '(?m)^## [^\r\n]+\r?$')
    $buildSections = @($sectionHeadings | Where-Object { $_.Value.Trim() -eq '## Build from source' })
    if ($buildSections.Count -ne 1) {
        throw "Expected one Build from source section in $Source."
    }
    for ($index = $sectionHeadings.Count - 1; $index -ge 0; --$index) {
        $heading = $sectionHeadings[$index]
        if ($heading.Value.Trim() -notin @('## English edition', '## Korean and English editions', '## Build from source')) {
            continue
        }
        $sectionEnd = if ($index + 1 -lt $sectionHeadings.Count) {
            $sectionHeadings[$index + 1].Index
        }
        else {
            $packageText.Length
        }
        $packageText = $packageText.Remove($heading.Index, $sectionEnd - $heading.Index)
    }

    [IO.File]::WriteAllText($Destination, $packageText, $utf8NoBom)
}

$stageRoot = Join-Path $resolvedOutput ('.stage-' + [Guid]::NewGuid().ToString('N'))
$resolvedStage = [IO.Path]::GetFullPath($stageRoot)
$requiredPrefix = $resolvedOutput.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
if (-not $resolvedStage.StartsWith($requiredPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Unsafe staging path: $resolvedStage"
}
New-Item -ItemType Directory -Path $resolvedStage | Out-Null
$preserveStage = $false

try {
    foreach ($edition in $editions) {
        $language = $edition.Language
        $builtExe = Join-Path (Join-Path (Join-Path $projectRoot 'dist') $projectVersion) `
            "$language\Mic FX Bridge.exe"
        if (-not (Test-Path -LiteralPath $builtExe -PathType Leaf)) {
            throw "Built $language executable was not found: $builtExe"
        }

        $editionStage = Join-Path $resolvedStage $language
        $licenceStage = Join-Path $editionStage 'THIRD_PARTY_LICENSES'
        New-Item -ItemType Directory -Path $licenceStage -Force | Out-Null

        Copy-Item -LiteralPath $builtExe -Destination (Join-Path $editionStage 'Mic FX Bridge.exe')
        Write-PackageReadme `
            -Source (Join-Path $projectRoot $edition.Readme) `
            -SourcePrivacyName $edition.Privacy `
            -Destination (Join-Path $editionStage 'README.md')
        Copy-Item -LiteralPath (Join-Path $projectRoot $edition.Privacy) -Destination (Join-Path $editionStage 'PRIVACY.md')
        Copy-Item -LiteralPath (Join-Path $projectRoot 'SECURITY.md') -Destination $editionStage
        Copy-Item -LiteralPath (Join-Path $projectRoot 'THIRD_PARTY_NOTICES.md') -Destination $editionStage

        Copy-Item -LiteralPath $projectLicense -Destination $editionStage

        foreach ($notice in $thirdPartyLicenses) {
            Copy-Item -LiteralPath (Join-Path $juceSource $notice.Source) `
                -Destination (Join-Path $licenceStage $notice.Destination)
        }

        $checksums = Get-ChildItem -LiteralPath $editionStage -File -Recurse |
            Sort-Object FullName |
            ForEach-Object {
                $relativePath = $_.FullName.Substring($editionStage.Length).TrimStart(
                    [IO.Path]::DirectorySeparatorChar,
                    [IO.Path]::AltDirectorySeparatorChar).Replace('\', '/')
                $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
                "$hash  $relativePath"
            }
        Set-Content -LiteralPath (Join-Path $editionStage 'SHA256SUMS.txt') `
            -Value $checksums -Encoding ascii

        $archiveName = "Mic-FX-Bridge-$Version-windows-x64-$language.zip"
        $archivePath = Join-Path $resolvedOutput $archiveName
        $archiveStage = Join-Path $resolvedStage ("archive-$language")
        New-Item -ItemType Directory -Path $archiveStage | Out-Null
        $stagedArchivePath = Join-Path $archiveStage $archiveName
        Compress-Archive -Path (Join-Path $editionStage '*') `
            -DestinationPath $stagedArchivePath -CompressionLevel Optimal

        $archiveHash = (Get-FileHash -LiteralPath $stagedArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()
        Set-Content -LiteralPath ($stagedArchivePath + '.sha256') `
            -Value "$archiveHash  $archiveName" -Encoding ascii

        & (Join-Path $projectRoot 'tests\VerifyReleasePackage.ps1') `
            -ArchivePath $stagedArchivePath `
            -ExpectedExecutable $builtExe `
            -Language $language `
            -Version $Version

        $stagedSidecarPath = $stagedArchivePath + '.sha256'
        $sidecarPath = $archivePath + '.sha256'
        $archiveBackupPath = Join-Path $archiveStage 'previous.zip'
        $sidecarBackupPath = Join-Path $archiveStage 'previous.zip.sha256'
        $archiveBackedUp = $false
        $sidecarBackedUp = $false
        $newArchivePublished = $false
        $newSidecarPublished = $false
        try {
            if (Test-Path -LiteralPath $archivePath) {
                throw "Archive appeared during packaging; refusing to replace $archivePath"
            }
            if (Test-Path -LiteralPath $sidecarPath) {
                throw "Checksum appeared during packaging; refusing to replace $sidecarPath"
            }

            Move-Item -LiteralPath $stagedArchivePath -Destination $archivePath
            $newArchivePublished = $true
            Move-Item -LiteralPath $stagedSidecarPath -Destination $sidecarPath
            $newSidecarPublished = $true
        }
        catch {
            $publishFailure = $_
            $rollbackFailures = [Collections.Generic.List[string]]::new()

            if ($newSidecarPublished) {
                try { Remove-Item -LiteralPath $sidecarPath -Force }
                catch { $rollbackFailures.Add($_.Exception.Message) }
            }
            if ($newArchivePublished) {
                try { Remove-Item -LiteralPath $archivePath -Force }
                catch { $rollbackFailures.Add($_.Exception.Message) }
            }
            if ($sidecarBackedUp) {
                try { Move-Item -LiteralPath $sidecarBackupPath -Destination $sidecarPath }
                catch { $rollbackFailures.Add($_.Exception.Message) }
            }
            if ($archiveBackedUp) {
                try { Move-Item -LiteralPath $archiveBackupPath -Destination $archivePath }
                catch { $rollbackFailures.Add($_.Exception.Message) }
            }

            if ($rollbackFailures.Count -gt 0) {
                $preserveStage = $true
                throw "Package publish failed and rollback was incomplete. Original error: $($publishFailure.Exception.Message). Rollback errors: $($rollbackFailures -join '; ')"
            }
            throw $publishFailure
        }

        Write-Host "Packaged: $archivePath" -ForegroundColor Green
    }
}
finally {
    if ($preserveStage) {
        Write-Warning "Package staging was preserved for manual recovery: $resolvedStage"
    }
    elseif (Test-Path -LiteralPath $resolvedStage) {
        Remove-Item -LiteralPath $resolvedStage -Recurse -Force
    }
}
