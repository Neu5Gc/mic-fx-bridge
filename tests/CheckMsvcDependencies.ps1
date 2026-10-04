[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $TestBinaryDirectory,
    [Parameter(Mandatory)] [string] $CMake,
    [Parameter(Mandatory)] [string] $JuceSourceDirectory
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\scripts\MsvcDependencies.ps1')
$module = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\cmake\MsvcJsonDependencies.cmake')).Replace('\', '/')
$converter = Join-Path $PSScriptRoot '..\cmake\WriteMsvcDepfile.cmake'
$fixture = Join-Path $TestBinaryDirectory ('msvc-json-' + [guid]::NewGuid().ToString('N'))
$source = Join-Path $fixture 'source with spaces'
$build = Join-Path $fixture 'build with spaces'
New-Item -ItemType Directory -Path $source -Force | Out-Null
$header = Join-Path $source 'value #$.h'
Set-Content -LiteralPath (Join-Path $source 'CMakeLists.txt') -Encoding ascii -Value @(
    'cmake_minimum_required(VERSION 3.22)',
    'project(MsvcDependencyRegression LANGUAGES CXX)',
    ('include("' + $module + '")'),
    'add_executable(probe main.cpp)',
    'target_compile_options(probe PRIVATE /utf-8)',
    'add_library(headerless STATIC headerless.cpp)',
    'add_library(deep_probe STATIC deep.cpp)',
    'target_compile_features(deep_probe PRIVATE cxx_std_20)',
    'target_compile_definitions(deep_probe PRIVATE JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 JUCE_STANDALONE_APPLICATION=1)',
    'target_compile_options(deep_probe PRIVATE /utf-8)',
    ('target_include_directories(deep_probe PRIVATE "' + $JuceSourceDirectory.Replace('\', '/') + '/modules")')
)
Set-Content -LiteralPath (Join-Path $source 'main.cpp') -Encoding ascii -Value '#include "outer.h"', 'int main() { return value; }'
Set-Content -LiteralPath (Join-Path $source 'outer.h') -Encoding ascii -Value '#include "value #$.h"'
Set-Content -LiteralPath $header -Encoding ascii -Value 'constexpr int value = 0;'
Set-Content -LiteralPath (Join-Path $source 'deep.cpp') -Encoding ascii -Value '#include <juce_audio_basics/juce_audio_basics.cpp>'
Set-Content -LiteralPath (Join-Path $source 'headerless.cpp') -Encoding ascii -Value 'int headerless = 1;'
& $CMake -S $source -B $build -G 'NMake Makefiles' -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'Dependency fixture configure failed.' }
& $CMake --build $build
if ($LASTEXITCODE -ne 0) { throw 'Dependency fixture initial build failed.' }
$dependency = Join-Path $build 'CMakeFiles\probe.dir\main.cpp.obj.d.json'
$object = Join-Path $build 'CMakeFiles\probe.dir\main.cpp.obj'
$executable = Join-Path $build 'probe.exe'
Assert-MsvcHeaderDependencies -DependencyFile $dependency -ExpectedHeaders $header
Assert-MsvcHeaderDependencies -DependencyFile (Join-Path $build 'CMakeFiles\headerless.dir\headerless.cpp.obj.d.json') -ExpectedHeaders (Join-Path $source 'headerless.cpp')
& $executable
if ($LASTEXITCODE -ne 0) { throw 'Initial fixture returned an unexpected value.' }
$before = (Get-FileHash -LiteralPath $object -Algorithm SHA256).Hash
Start-Sleep -Milliseconds 1100
Set-Content -LiteralPath $header -Encoding ascii -Value 'constexpr int value = 7;'
& $CMake --build $build
if ($LASTEXITCODE -ne 0) { throw 'Dependency fixture incremental build failed.' }
if ((Get-FileHash -LiteralPath $object -Algorithm SHA256).Hash -eq $before) {
    throw 'Transitive header edit did not rebuild the affected object.'
}
& $executable
if ($LASTEXITCODE -ne 7) { throw 'Incremental executable did not reflect the header edit.' }

for ($iteration = 0; $iteration -lt 3; ++$iteration) {
    if ($iteration -gt 0) {
        & $CMake --build $build --clean-first
        if ($LASTEXITCODE -ne 0) { throw 'Deep dependency fixture rebuild failed.' }
    }
    $deepDependency = Join-Path $build 'CMakeFiles\deep_probe.dir\deep.cpp.obj.d'
    $json = Get-Content -LiteralPath ($deepDependency + '.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $expected = @($json.Data.Includes)
    if ($expected.Count -lt 100) { throw 'Deep fixture did not report its include graph.' }
    Assert-MsvcHeaderDependencies -DependencyFile ($deepDependency + '.json') -ExpectedHeaders $expected
    $expectedEscaped = @($expected | ForEach-Object {
        $_.Replace('\', '/').Replace('$', '$$').Replace('#', '\#').Replace(':', '\:').Replace(' ', '\ ')
    })
    $actual = @(Get-Content -LiteralPath $deepDependency -Encoding UTF8 | Select-Object -Skip 1 | ForEach-Object {
        ($_ -replace ' \\$', '').Trim()
    })
    if (@(Compare-Object $expectedEscaped $actual).Count -ne 0) {
        throw 'Generated Make depfile differs from compiler JSON.'
    }
}

$invalid = Join-Path $fixture 'invalid.json'
$invalidObject = Join-Path $fixture 'invalid.obj'
$invalidDepfile = Join-Path $fixture 'invalid.d'
function Assert-RejectedJson($Content) {
    if ($null -ne $Content) { [IO.File]::WriteAllText($invalid, $Content) }
    [IO.File]::WriteAllText($invalidObject, 'generated object placeholder')
    [IO.File]::WriteAllText($invalidDepfile, 'stale dependency placeholder')
    $previousErrorAction = $ErrorActionPreference
    try {
        # Windows PowerShell turns native stderr into ErrorRecords. These
        # expected compiler-evidence failures must be checked by exit status.
        $ErrorActionPreference = 'Continue'
        & $CMake "-DINPUT=$invalid" "-DOUTPUT=$invalidDepfile" "-DOBJECT=$invalidObject" -P $converter *> $null
    }
    finally { $ErrorActionPreference = $previousErrorAction }
    if ($LASTEXITCODE -eq 0 -or (Test-Path $invalidObject) -or (Test-Path $invalidDepfile)) {
        throw 'Invalid JSON was accepted or left a reusable object/depfile.'
    }
}
# Missing, empty, malformed, unsupported schema, missing graph and missing file.
Assert-RejectedJson $null
Assert-RejectedJson ''
Assert-RejectedJson '{bad json'
Assert-RejectedJson '{"Version":"9.0","Data":{}}'
Assert-RejectedJson '{"Version":"1.2","Data":{}}'
$badGraph = @{ Version='1.2'; Data=@{ Source=(Join-Path $source 'main.cpp'); Includes=@((Join-Path $source 'absent.h')) } } | ConvertTo-Json -Depth 5
Assert-RejectedJson $badGraph
$validWrongHeader = @{ Version='1.2'; Data=@{ Source=(Join-Path $source 'main.cpp'); Includes=@((Join-Path $source 'outer.h')) } } | ConvertTo-Json -Depth 5
[IO.File]::WriteAllText($invalid, $validWrongHeader)
$rejected = $false
try { Assert-MsvcHeaderDependencies -DependencyFile $invalid -ExpectedHeaders $header }
catch { $rejected = $true }
if (-not $rejected) { throw 'Missing expected header was accepted.' }
# Force a rename I/O failure: an output directory cannot be replaced by a file.
$blockedOutput = Join-Path $fixture 'output-directory'
New-Item -ItemType Directory -Path $blockedOutput | Out-Null
[IO.File]::WriteAllText($invalidObject, 'generated object placeholder')
$previousErrorAction = $ErrorActionPreference
try {
    $ErrorActionPreference = 'Continue'
    & $CMake "-DINPUT=$invalid" "-DOUTPUT=$blockedOutput" "-DOBJECT=$invalidObject" -P $converter *> $null
}
finally { $ErrorActionPreference = $previousErrorAction }
if ($LASTEXITCODE -eq 0 -or (Test-Path $invalidObject)) {
    throw 'Conversion I/O failure left a reusable object.'
}
Write-Host 'MSVC JSON dependencies: transitive rebuild, headerless source, three deep graphs, escaped paths, seven rejection cases and I/O failure cleanup passed.'
