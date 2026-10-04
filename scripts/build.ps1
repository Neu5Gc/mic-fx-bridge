[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string] $Configuration = 'Release',
    [string] $BuildDirectory = '',
    [string] $CacheDirectory = '',
    [switch] $ValidationOnly,
    [switch] $LocalCandidate
)

$ErrorActionPreference = 'Stop'
if ($LocalCandidate -and ($ValidationOnly -or $Configuration -ne 'Release')) {
    throw 'LocalCandidate requires Release and cannot be combined with ValidationOnly.'
}

$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$versionLine = Select-String -LiteralPath (Join-Path $projectRoot 'CMakeLists.txt') `
    -Pattern '^project\(MicVstBridge VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES CXX\)$' |
    Select-Object -First 1
if ($null -eq $versionLine) { throw 'Unable to read project version.' }
$projectVersion = $versionLine.Matches[0].Groups[1].Value
$versionedDistDirectory = Join-Path (Join-Path $projectRoot 'dist') $projectVersion
if (-not $ValidationOnly -and (Test-Path -LiteralPath $versionedDistDirectory)) {
    throw "Version $projectVersion already exists. Increase the project version for a new delivery, or use -ValidationOnly to test without replacing it."
}
$sourceCommit = (& git -C $projectRoot rev-parse HEAD)
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify source revision.' }
$sourceChanges = @(& git -C $projectRoot status --porcelain)
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect source status.' }
if (-not $ValidationOnly -and -not $LocalCandidate -and $sourceChanges.Count -ne 0) {
    throw 'Commit source changes before producing a versioned delivery, or use -ValidationOnly.'
}
. (Join-Path $PSScriptRoot 'SourceSnapshot.ps1')
$sourceSnapshot = Get-MicSourceSnapshot -ProjectRoot $projectRoot
if ([string]::IsNullOrWhiteSpace($CacheDirectory)) {
    $cacheRoot = Join-Path $env:LOCALAPPDATA 'MicVstBridge'
}
else {
    $cacheRoot = [IO.Path]::GetFullPath($CacheDirectory)
}
New-Item -ItemType Directory -Path $cacheRoot -Force | Out-Null

# Some current MSVC/CMake combinations corrupt intermediate paths containing
# Korean characters. Build through a stable ASCII junction while keeping the
# source tree in its original location.
$sha = [Security.Cryptography.SHA256]::Create()
try {
    $pathBytes = [Text.Encoding]::UTF8.GetBytes($projectRoot.ToLowerInvariant())
    $pathHash = -join ($sha.ComputeHash($pathBytes) | ForEach-Object { $_.ToString('x2') })
}
finally {
    $sha.Dispose()
}

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    # Do not inherit caches that derived dependencies from localized stdout.
    $buildKey = 'build-v5-' + $pathHash.Substring(0, 12) + '-' + $Configuration.ToLowerInvariant()
    $BuildDirectory = Join-Path $cacheRoot $buildKey
}
else {
    $BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
}

$sourceAlias = Join-Path $cacheRoot ('source-' + $pathHash.Substring(0, 12))
if (Test-Path -LiteralPath $sourceAlias) {
    $aliasItem = Get-Item -LiteralPath $sourceAlias -Force
    $aliasTarget = [IO.Path]::GetFullPath([string] @($aliasItem.Target)[0])
    if ($aliasItem.LinkType -ne 'Junction' -or
        -not $aliasTarget.Equals($projectRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Build alias already exists but points elsewhere: $sourceAlias"
    }
}
else {
    New-Item -ItemType Junction -Path $sourceAlias -Target $projectRoot | Out-Null
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Installer (vswhere.exe) was not found.'
}

$vswhereArguments = @(
    '-latest',
    '-products', '*',
    '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
    '-property', 'installationPath'
)
$vsInstall = (& $vswhere @vswhereArguments | Select-Object -Last 1)
if ([string]::IsNullOrWhiteSpace($vsInstall)) {
    throw 'Install Visual Studio Desktop development with C++ first.'
}

$devCmd = Join-Path $vsInstall.Trim() 'Common7\Tools\VsDevCmd.bat'
$vcToolsRoot = Join-Path $vsInstall.Trim() 'VC\Tools\MSVC'
$preferred1444 = Get-ChildItem -LiteralPath $vcToolsRoot -Directory -ErrorAction SilentlyContinue |
    Where-Object Name -Like '14.44.*' |
    Sort-Object Name -Descending |
    Select-Object -First 1
$devArgs = '-arch=x64'
if ($null -ne $preferred1444) {
    $devArgs += ' -vcvars_ver=14.44'
}

$environmentLines = & $env:ComSpec /d /s /c "call `"$devCmd`" $devArgs >nul && set"
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to initialise the Visual Studio C++ environment.'
}
$developerPath = $null
foreach ($line in $environmentLines) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        $name = $line.Substring(0, $separator)
        $value = $line.Substring($separator + 1)
        if ($name -ceq 'PATH') {
            $developerPath = $value
        }
        elseif ($name.Equals('PATH', [StringComparison]::OrdinalIgnoreCase)) {
            if ($null -eq $developerPath) { $developerPath = $value }
        }
        else {
            [Environment]::SetEnvironmentVariable($name, $value, 'Process')
        }
    }
}
if ($null -eq $developerPath) {
    throw 'Visual Studio did not provide a developer PATH.'
}
# Some inherited environments contain both PATH and Path. Keeping both lets a
# child PowerShell select the stale value and lose MSVC tools during CTest.
# Normalize only this process; do not alter user or machine environment settings.
foreach ($environmentName in [Environment]::GetEnvironmentVariables('Process').Keys) {
    if ([string]::Equals($environmentName, 'PATH', [StringComparison]::OrdinalIgnoreCase)) {
        [Environment]::SetEnvironmentVariable($environmentName, $null, 'Process')
    }
}
[Environment]::SetEnvironmentVariable('PATH', $developerPath, 'Process')

# Header tracking uses compiler JSON and generated Make depfiles; diagnostics do
# not participate in dependency parsing. Keep the clean-first and release gates.
. (Join-Path $PSScriptRoot 'MsvcDependencies.ps1')
$cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'

$configureArguments = @(
    '-S', $sourceAlias,
    '-B', $BuildDirectory,
    '-G', 'NMake Makefiles',
    "-DCMAKE_BUILD_TYPE=$Configuration"
)
& $cmake @configureArguments
if ($LASTEXITCODE -ne 0) {
    throw 'CMake configure failed; refusing to reuse an earlier build tree.'
}

# Never reuse C++ objects across a release build. On some localised Windows
# installations inconsistent compiler output decoding leaves dependency files
# empty. Reusing those objects after a class layout
# change can produce an ABI-mixed executable that builds and tests but crashes
# at startup. A clean-first build is deliberately preferred over that risk.
& $cmake --build $BuildDirectory --clean-first
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }

# Refuse to copy a binary when the compiler/CMake pair has silently stopped
# recording project-header dependencies. The clean build above is safe for the
# current invocation; this gate also verifies the JSON and generated Make graph.
$dependencyChecks = @(
    [pscustomobject]@{
        File = Join-Path $BuildDirectory 'CMakeFiles\MicVstBridgeEn.dir\src\Main.cpp.obj.d.json'
        Header = Join-Path $sourceAlias 'src\MainComponent.h'
    },
    [pscustomobject]@{
        File = Join-Path $BuildDirectory 'CMakeFiles\MicVstBridgeEn.dir\src\Main.cpp.obj.d.json'
        Header = Join-Path $sourceAlias 'src\AudioEngine.h'
    },
    [pscustomobject]@{
        File = Join-Path $BuildDirectory 'CMakeFiles\MicVstBridgeEn.dir\src\AudioEngine.cpp.obj.d.json'
        Header = Join-Path $sourceAlias 'src\LegacySettings.h'
    },
    [pscustomobject]@{
        File = Join-Path $BuildDirectory 'CMakeFiles\MicVstBridgeEn.dir\src\AudioEngine.cpp.obj.d.json'
        Header = Join-Path $sourceAlias 'src\PluginStatePersistence.h'
    }
)
foreach ($check in $dependencyChecks) {
    Assert-MsvcHeaderDependencies -DependencyFile $check.File -ExpectedHeaders $check.Header
}

& $ctest --test-dir $BuildDirectory --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }

if ($ValidationOnly) {
    Write-Host 'Validation complete; delivered executables and latest pointer were not changed.'
    return
}
$finalSnapshot = Get-MicSourceSnapshot -ProjectRoot $projectRoot
if ((& git -C $projectRoot rev-parse HEAD) -ne $sourceCommit -or
    $finalSnapshot.sha256 -ne $sourceSnapshot.sha256 -or
    (-not $LocalCandidate -and @(& git -C $projectRoot status --porcelain).Count -ne 0)) {
    throw 'Source inputs changed during the build; refusing to deliver inconsistent binaries.'
}

$builtEditions = @(
    [pscustomobject]@{
        Language = 'en'
        Executable = Join-Path $BuildDirectory "MicVstBridgeEn_artefacts\$Configuration\Mic FX Bridge.exe"
    }
)

foreach ($edition in $builtEditions) {
    if (-not (Test-Path -LiteralPath $edition.Executable -PathType Leaf)) {
        throw "Built $($edition.Language) executable was not found: $($edition.Executable)"
    }
    $actualVersion = (Get-Item -LiteralPath $edition.Executable).VersionInfo.ProductVersion
    if ($actualVersion -ne $projectVersion) {
        throw "Executable version $actualVersion differs from $projectVersion."
    }
}

$stageDirectory = Join-Path (Join-Path $projectRoot 'dist') ('.build-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stageDirectory | Out-Null
$versionedOutputs = @{}
$manifestEditions = @()

foreach ($edition in $builtEditions) {
    $destinationDirectory = Join-Path $stageDirectory $edition.Language
    New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
    $destinationExe = Join-Path $destinationDirectory 'Mic FX Bridge.exe'
    try {
        Copy-Item -LiteralPath $edition.Executable -Destination $destinationExe `
            -Force -ErrorAction Stop
    }
    catch {
        throw "Unable to replace the $($edition.Language) release executable. Close it if it is running, then build again: $destinationExe. $($_.Exception.Message)"
    }

    $hash = (Get-FileHash -LiteralPath $destinationExe -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne (Get-FileHash -LiteralPath $edition.Executable -Algorithm SHA256).Hash.ToLowerInvariant()) {
        throw 'Staged executable differs from build output.'
    }
    Set-Content -LiteralPath ($destinationExe + '.sha256') `
        -Value "$hash  $(Split-Path -Leaf $destinationExe)" -Encoding ascii
    $versionedOutputs[$edition.Language] = Join-Path $versionedDistDirectory ($edition.Language + '\Mic FX Bridge.exe')
    $manifestEditions += [ordered]@{ language = $edition.Language; file = $edition.Language + '/Mic FX Bridge.exe'; sha256 = $hash }
}

$manifest = [ordered]@{ version = $projectVersion; sourceCommit = $sourceCommit.Trim(); sourceState = $(if ($LocalCandidate) { "uncommitted-local-candidate" } else { "committed" }); sourceSnapshotSha256 = $sourceSnapshot.sha256; sourceInputs = $sourceSnapshot.inputs; configuration = $Configuration; createdUtc = [DateTime]::UtcNow.ToString('o'); editions = $manifestEditions }
[IO.File]::WriteAllText((Join-Path $stageDirectory 'build-manifest.json'), ($manifest | ConvertTo-Json -Depth 7), [Text.UTF8Encoding]::new($false))
# Directory.Move refuses an existing destination, including a concurrent build.
[IO.Directory]::Move($stageDirectory, $versionedDistDirectory)
$latest = "# Latest local build: $projectVersion`n`nBase source: $($sourceCommit.Trim())`nState: $($manifest.sourceState)`nInput snapshot: $($sourceSnapshot.sha256)`n`n- English: $projectVersion/en/Mic FX Bridge.exe`n`nSee $projectVersion/build-manifest.json for SHA-256 and build provenance.`n"
[IO.File]::WriteAllText((Join-Path $projectRoot 'dist/LATEST.md'), $latest, [Text.UTF8Encoding]::new($false))

Write-Host ''
Write-Host "Build complete (English): $($versionedOutputs['en'])" -ForegroundColor Green
