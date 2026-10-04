[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $SourceDirectory
)

$ErrorActionPreference = 'Stop'
$sourceRoot = (Resolve-Path -LiteralPath $SourceDirectory).Path
$hangulLiteral = [regex]::new('u8"(?:\\.|[^"\\])*\p{IsHangulSyllables}(?:\\.|[^"\\])*"')
$uiTextCall = [regex]::new('(?:mic_daw::)?uiText\s*\(\s*$')
$violations = [Collections.Generic.List[string]]::new()
$strictUtf8 = [Text.UTF8Encoding]::new($false, $true)

Get-ChildItem -LiteralPath $sourceRoot -File -Recurse -Include '*.cpp', '*.h' | ForEach-Object {
    try {
        $content = $strictUtf8.GetString([IO.File]::ReadAllBytes($_.FullName))
    }
    catch {
        throw "Invalid UTF-8 source file: $($_.FullName). $($_.Exception.Message)"
    }
    foreach ($match in $hangulLiteral.Matches($content)) {
        $prefixLength = [Math]::Min($match.Index, 256)
        $prefix = $content.Substring($match.Index - $prefixLength, $prefixLength)
        if (-not $uiTextCall.IsMatch($prefix)) {
            $line = 1 + ($content.Substring(0, $match.Index) -split "`n").Count - 1
            $relativePath = $_.FullName.Substring($sourceRoot.Length).TrimStart(
                [IO.Path]::DirectorySeparatorChar,
                [IO.Path]::AltDirectorySeparatorChar)
            $violations.Add("${relativePath}:$line")
        }
    }
}

if ($violations.Count -gt 0) {
    Write-Error ("Hangul UI literals must be the Korean argument of uiText():`n" +
        ($violations -join "`n"))
}

Write-Host 'All Hangul UI literals are routed through uiText().'
