# Exact inputs for locally reviewed, uncommitted candidates. Does not change Git.
function Get-MicSourceSnapshot {
    param([string] $ProjectRoot)
    $paths = @(& git -C $ProjectRoot ls-files --cached --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate candidate source inputs.' }
    $inputs = @($paths | Sort-Object -Unique | Where-Object {
        $_ -match '^(src|tests|scripts|cmake|assets)/' -or
        $_ -match '^(CMakeLists\.txt|README(?:\.(?:en|ko))?\.md|PRIVACY\.(?:en|ko)\.md|LICENSE|SECURITY\.md|THIRD_PARTY_NOTICES\.md|CHANGELOG\.md|VERSIONING\.md)$'
    } | ForEach-Object {
        $path = Join-Path $ProjectRoot $_
        $exists = Test-Path -LiteralPath $path -PathType Leaf
        [ordered]@{ path = $_; exists = $exists; sha256 = if ($exists) {
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        } else { $null } }
    })
    $json = ConvertTo-Json -InputObject $inputs -Depth 5 -Compress
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $fingerprint = -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($json)) | ForEach-Object { $_.ToString('x2') }) }
    finally { $sha.Dispose() }
    [pscustomobject]@{ inputs = $inputs; sha256 = $fingerprint }
}
