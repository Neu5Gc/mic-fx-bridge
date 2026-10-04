param(
    [string] $SourceDirectory = (Join-Path $PSScriptRoot '..\src'),
    [switch] $SelfTestOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Keep this script ASCII so Windows PowerShell 5.1 can read it without a BOM.
# Tokenize comments and character/raw/string literals together: stripping
# comments first would corrupt strings containing URLs or comment markers.
$cppTokens = [regex]::new(@'
    (?<comment> // (?: [^\r\n]* \\ \r?\n )* [^\r\n]* | /\* [\s\S]*? \*/ )
  | (?<raw> (?<rawPrefix>u8|u|U|L)? R" (?<delimiter>[^ ()\\\t\v\f\r\n]{0,16}) \( [\s\S]*? \) \k<delimiter> " )
  | (?<string> (?<prefix>u8|u|U|L)? " (?: \\ [\s\S] | [^"\\\r\n] )* " )
  | (?<number> [0-9] [a-zA-Z0-9_'.]* )
  | (?<character> (?:u8|u|U|L)? ' (?: \\ [\s\S] | [^'\\\r\n] )* ' )
  | (?<identifier> [a-zA-Z_] [a-zA-Z0-9_]* )
  | (?<space> \s+ )
  | :: | [\s\S]
'@, [System.Text.RegularExpressions.RegexOptions]::IgnorePatternWhitespace)

function Find-UnsafeUtf8Literal {
    param([string] $Text, [string] $Path = '<fixture>')

    $history = [System.Collections.Generic.List[string]]::new()
    $wrapperScopes = [System.Collections.Generic.Stack[bool]]::new()
    foreach ($token in $cppTokens.Matches($Text)) {
        if ($token.Groups['comment'].Success -or $token.Groups['space'].Success) {
            continue
        }

        $value = $token.Value
        if ($value -in @('(', '{', '[')) {
            $count = $history.Count
            $isUtf8Wrapper = $count -gt 0 -and $history[$count - 1] -eq 'CharPointer_UTF8'
            if ($count -ge 3) {
                $isUtf8Wrapper = $isUtf8Wrapper -or (
                    $history[$count - 1] -eq 'fromUTF8' -and
                    $history[$count - 2] -eq '::' -and
                    $history[$count - 3] -eq 'String')
            }
            $wrapperScopes.Push($isUtf8Wrapper)
        }
        elseif ($value -in @(')', '}', ']')) {
            if ($wrapperScopes.Count -gt 0) { [void] $wrapperScopes.Pop() }
        }
        elseif ($token.Groups['string'].Success -or $token.Groups['raw'].Success) {
            $hasEncodingPrefix = $token.Groups['prefix'].Success -or $token.Groups['rawPrefix'].Success
            $isExplicitUtf8 = $wrapperScopes.Count -gt 0 -and $wrapperScopes.Peek()
            if (-not $hasEncodingPrefix -and -not $isExplicitUtf8 -and $value -match '[^\x00-\x7F]') {
                [pscustomobject]@{
                    Path = $Path
                    Line = $Text.Substring(0, $token.Index).Split("`n").Length
                }
            }
        }
        $history.Add($value)
    }
}

function Assert-Fixture {
    param([string] $Name, [string] $Text, [int] $Expected)
    $actual = @(Find-UnsafeUtf8Literal -Text $Text).Count
    if ($actual -ne $Expected) {
        throw "Scanner self-test '$Name': expected $Expected violation(s), got $actual."
    }
}

# These fixtures ensure a green source scan cannot hide a broken scanner.
$hangul = [string] [char] 0xAC00
Assert-Fixture 'ordinary literal' ('label.setText("' + $hangul + '");') 1
Assert-Fixture 'concatenation' ('juce::String("ASCII") + "' + $hangul + '";') 1
Assert-Fixture 'encoded literals' ('u8"' + $hangul + '"; L"' + $hangul + '"; u"' + $hangul + '"; U"' + $hangul + '";') 0
Assert-Fixture 'raw narrow literal' ('R"tag(' + $hangul + ')tag";') 1
Assert-Fixture 'raw UTF8 literal' ('u8R"tag(' + $hangul + ')tag";') 0
Assert-Fixture 'comments' ('// "' + $hangul + '"' + "`n" + '/* "' + $hangul + '" */') 0
Assert-Fixture 'continued comment' ('// ignored \' + "`n" + '"' + $hangul + '"') 0
Assert-Fixture 'characters and digit separators' ("auto x = 1'000; auto ch = L'" + $hangul + "';") 0
Assert-Fixture 'comment markers inside string' ('u8"https://host/*' + $hangul + '*/"; "' + $hangul + '";') 1
Assert-Fixture 'escaped quotes' ('"escaped \"quote\" ' + $hangul + '";') 1
Assert-Fixture 'explicit UTF8 wrappers' ('juce::String::fromUTF8(/* note */ "' + $hangul + '"); juce::CharPointer_UTF8 { "' + $hangul + '" };') 0
Assert-Fixture 'adjacent UTF8 arguments' ('juce::String::fromUTF8("ASCII" "' + $hangul + '");') 0
Assert-Fixture 'nested unsafe conversion' ('juce::String::fromUTF8(unsafeCall("' + $hangul + '"));') 1

$strictUtf8 = [System.Text.UTF8Encoding]::new($false, $true)
$rejectedInvalidUtf8 = $false
try { [void] $strictUtf8.GetString([byte[]] @(0xC3, 0x28)) }
catch [System.Text.DecoderFallbackException] { $rejectedInvalidUtf8 = $true }
if (-not $rejectedInvalidUtf8) { throw 'Strict UTF-8 decoding self-test failed.' }

if ($SelfTestOnly) {
    Write-Output 'UTF-8 literal scanner self-tests passed.'
    exit 0
}

$files = @(Get-ChildItem -LiteralPath $SourceDirectory -Recurse -File |
    Where-Object { $_.Extension -in @('.h', '.cpp') })
if ($files.Count -eq 0) { throw "No C++ source files found in $SourceDirectory" }

$violations = @(
    foreach ($file in $files) {
        try { $source = $strictUtf8.GetString([System.IO.File]::ReadAllBytes($file.FullName)) }
        catch { throw "Invalid UTF-8 source file: $($file.FullName). $($_.Exception.Message)" }
        Find-UnsafeUtf8Literal -Text $source -Path $file.FullName
    }
)

if ($violations.Count -gt 0) {
    foreach ($violation in $violations) {
        Write-Output ("{0}:{1}: non-ASCII narrow string; use u8 or an explicit UTF-8 wrapper." -f
            $violation.Path, $violation.Line)
    }
    exit 1
}

Write-Output "UTF-8 literal regression passed ($($files.Count) source files; scanner self-tests passed)."
