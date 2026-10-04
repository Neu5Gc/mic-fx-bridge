function Assert-MsvcHeaderDependencies {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string] $DependencyFile,
        [Parameter(Mandatory)] [string[]] $ExpectedHeaders
    )

    if (-not (Test-Path -LiteralPath $DependencyFile -PathType Leaf)) {
        throw "MSVC dependency evidence is missing: $DependencyFile"
    }
    $strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
    $content = [IO.File]::ReadAllText($DependencyFile, $strictUtf8)
    $json = $content | ConvertFrom-Json
    if ($json.Version -notin @('1.1', '1.2') -or
        $null -eq $json.Data -or $json.Data.Source -isnot [string] -or
        $json.Data.Includes -isnot [Array]) {
        throw "Malformed MSVC dependency JSON: $DependencyFile"
    }
    $paths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($path in @($json.Data.Source) + @($json.Data.Includes)) {
        if ($path -isnot [string] -or $path -notmatch '^(?:[A-Za-z]:[/\\]|\\\\)' -or
            -not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Malformed MSVC dependency evidence: $DependencyFile"
        }
        [void] $paths.Add([IO.Path]::GetFullPath($path))
    }
    foreach ($header in $ExpectedHeaders) {
        if (-not $paths.Contains([IO.Path]::GetFullPath($header))) {
            throw "MSVC header dependency tracking is unavailable ($header): $DependencyFile"
        }
    }
    if (-not $DependencyFile.EndsWith('.d.json', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unexpected dependency JSON path: $DependencyFile"
    }
    $makeFile = $DependencyFile.Substring(0, $DependencyFile.Length - 5)
    if (-not (Test-Path -LiteralPath $makeFile -PathType Leaf)) {
        throw "Generated Make dependency evidence is missing: $makeFile"
    }
    $makeLines = @([IO.File]::ReadAllLines($makeFile, $strictUtf8))
    $expectedEscaped = @($json.Data.Includes | ForEach-Object {
        $_.Replace('\', '/').Replace('$', '$$').Replace('#', '\#').Replace(':', '\:').Replace(' ', '\ ')
    })
    $actual = @($makeLines | Select-Object -Skip 1 | ForEach-Object {
        ($_ -replace ' \\$', '').Trim()
    })
    if ($expectedEscaped.Count -ne $actual.Count -or
        ($expectedEscaped.Count -gt 0 -and @(Compare-Object $expectedEscaped $actual).Count -ne 0)) {
        throw "Generated Make dependencies differ from compiler JSON: $makeFile"
    }
}
