#Requires -Version 5.1
# Opt-in merge of an explicit backup into isolated Nonary Extended settings.
# Never writes HKCU\Software\Moonlight Game Streaming Project or its AppData.
# Does not overwrite dest controllerkbmshortcut when it already contains 16,17.
# INI: keep every section; patch shortcut with backup-first. Dest is copied aside first.
# .reg: parse REG_SZ / DWORD / REG_BINARY / QWORD, or reject unsupported forms.
# Portable dest is <extract>\Moonlight Nonary Extended\Moonlight Nonary Extended.ini
param(
    [string]$Backup,
    [string]$PortableDir,
    [switch]$Installed,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$StockOrg = 'Moonlight Game Streaming Project'
$StockApp = 'Moonlight'
$IsolatedOrg = 'Moonlight Nonary Extended'
$IsolatedApp = 'Moonlight Nonary Extended'
$PreserveKey = 'controllerkbmshortcut'

function Test-ForbiddenPath([string]$Path) {
    if ($Path -match [regex]::Escape($StockOrg)) {
        throw "refusing path that names stock Moonlight identity: $Path"
    }
}

function Get-Stamp {
    return (Get-Date).ToString('yyyyMMddHHmmss')
}

function Backup-ExistingFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $bak = "$Path.bak-$(Get-Stamp)"
    Copy-Item -LiteralPath $Path -Destination $bak -Force
    Write-Host "dest backup -> $bak"
    return $bak
}

function New-SettingsDoc {
    return [ordered]@{}
}

function Ensure-Section($Doc, [string]$Section) {
    if (-not $Doc.Contains($Section)) {
        $Doc[$Section] = [ordered]@{}
    }
    return $Doc[$Section]
}

function Set-DocValue($Doc, [string]$Section, [string]$Key, [string]$Type, $Value) {
    $sec = Ensure-Section $Doc $Section
    $sec[$Key] = @{ Type = $Type; Value = $Value }
}

function Get-DocValue($Doc, [string]$Section, [string]$Key) {
    if (-not $Doc.Contains($Section)) { return $null }
    if (-not $Doc[$Section].Contains($Key)) { return $null }
    return $Doc[$Section][$Key]
}

function Find-DocKey($Doc, [string]$Key) {
    foreach ($section in @($Doc.Keys)) {
        if ($Doc[$section].Contains($Key)) {
            return @{ Section = $section; Entry = $Doc[$section][$Key] }
        }
    }
    return $null
}

function Get-EntryText($Entry) {
    if ($null -eq $Entry) { return '' }
    if ($Entry.Type -eq 'ini') {
        return [string]$Entry.Value
    }
    if ($Entry.Type -eq 'string') {
        return ConvertTo-IniEscaped ([string]$Entry.Value)
    }
    if ($Entry.Type -eq 'dword') {
        return ([int]$Entry.Value).ToString()
    }
    if ($Entry.Type -eq 'qword') {
        return ([int64]$Entry.Value).ToString()
    }
    if ($Entry.Type -eq 'binary') {
        return ConvertTo-IniByteArray ([byte[]]$Entry.Value)
    }
    return [string]$Entry.Value
}

function Get-ShortcutText($Entry) {
    if ($null -eq $Entry) { return '' }
    $v = [string]$Entry.Value
    if ($v.Length -ge 2 -and $v.StartsWith('"') -and $v.EndsWith('"')) {
        $v = $v.Substring(1, $v.Length - 2)
    }
    return $v
}

function Test-PaddleShortcut([string]$Value) {
    $parts = @($Value -split ',' | ForEach-Object { $_.Trim().Trim('"') } | Where-Object { $_ })
    return ($parts -contains '16') -and ($parts -contains '17')
}

function ConvertTo-IniEscaped([string]$Text) {
    $needsQuotes = $false
    if ($Text.Length -gt 0 -and ($Text[0] -eq ' ' -or $Text[$Text.Length - 1] -eq ' ')) {
        $needsQuotes = $true
    }
    $sb = New-Object System.Text.StringBuilder
    foreach ($ch in $Text.ToCharArray()) {
        $c = [int][char]$ch
        switch ($ch) {
            "`0" { [void]$sb.Append('\0') }
            "`n" { [void]$sb.Append('\n') }
            "`r" { [void]$sb.Append('\r') }
            "`t" { [void]$sb.Append('\t') }
            '"'  { [void]$sb.Append('\"'); $needsQuotes = $true }
            '\'  { [void]$sb.Append('\\') }
            default {
                if ($ch -eq ';') {
                    $needsQuotes = $true
                    [void]$sb.Append($ch)
                }
                elseif ($c -lt 32 -or $c -eq 127) {
                    [void]$sb.Append('\x')
                    [void]$sb.Append(('{0:X2}' -f $c))
                }
                else {
                    [void]$sb.Append($ch)
                }
            }
        }
    }
    $out = $sb.ToString()
    if ($needsQuotes) { return '"' + $out + '"' }
    return $out
}

function ConvertTo-IniByteArray([byte[]]$Bytes) {
    $inner = [System.Text.Encoding]::GetEncoding(28591).GetString($Bytes)
    $escaped = ConvertTo-IniEscaped $inner
    if ($escaped.Length -ge 2 -and $escaped.StartsWith('"') -and $escaped.EndsWith('"')) {
        $escaped = $escaped.Substring(1, $escaped.Length - 2)
    }
    return '@ByteArray(' + $escaped + ')'
}

function Read-TextFile([string]$Path) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
        return [System.Text.Encoding]::Unicode.GetString($bytes, 2, $bytes.Length - 2)
    }
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        return [System.Text.Encoding]::UTF8.GetString($bytes, 3, $bytes.Length - 3)
    }
    return [System.Text.Encoding]::UTF8.GetString($bytes)
}

function Get-IniDocument([string]$Path) {
    $doc = New-SettingsDoc
    $section = 'General'
    foreach ($raw in ((Read-TextFile $Path) -split "`r`n|`n|`r", -1)) {
        $line = $raw
        if ($line -match '^\s*$') { continue }
        if ($line -match '^\s*[#;]') { continue }
        if ($line -match '^\s*\[(.*)\]\s*$') {
            $section = $Matches[1]
            if (-not $section) { $section = 'General' }
            [void](Ensure-Section $doc $section)
            continue
        }
        $eq = $line.IndexOf('=')
        if ($eq -lt 1) { continue }
        $key = $line.Substring(0, $eq).Trim()
        $val = $line.Substring($eq + 1)
        if (-not $key) { continue }
        Set-DocValue $doc $section $key 'ini' $val
    }
    return $doc
}

function ConvertFrom-RegSz([string]$Quoted) {
    if ($Quoted.Length -ge 2 -and $Quoted.StartsWith('"') -and $Quoted.EndsWith('"')) {
        $inner = $Quoted.Substring(1, $Quoted.Length - 2)
        return ($inner -replace '\\"','"' -replace '\\\\','\')
    }
    throw "unsupported REG_SZ form: $Quoted"
}

function ConvertFrom-RegDword([string]$Hex) {
    $u = [Convert]::ToUInt32($Hex, 16)
    return [BitConverter]::ToInt32([BitConverter]::GetBytes($u), 0)
}

function ConvertFrom-RegHexBytes([string]$HexList) {
    $hex = ($HexList -replace '[^0-9a-fA-F]', '')
    if ($hex.Length -eq 0) { return [byte[]]@() }
    if ($hex.Length % 2 -ne 0) { throw "truncated hex value: $HexList" }
    $out = New-Object byte[] ($hex.Length / 2)
    for ($i = 0; $i -lt $out.Length; $i++) {
        $out[$i] = [Convert]::ToByte($hex.Substring($i * 2, 2), 16)
    }
    return $out
}

function Get-RegAppPrefixes {
    return @(
        "HKEY_CURRENT_USER\Software\$StockOrg\$StockApp",
        "HKCU\Software\$StockOrg\$StockApp",
        "HKEY_CURRENT_USER\Software\$IsolatedOrg\$IsolatedApp",
        "HKCU\Software\$IsolatedOrg\$IsolatedApp"
    )
}

function Get-RegOrgPrefixes {
    return @(
        "HKEY_CURRENT_USER\Software\$StockOrg",
        "HKCU\Software\$StockOrg",
        "HKEY_CURRENT_USER\Software\$IsolatedOrg",
        "HKCU\Software\$IsolatedOrg"
    )
}

function ConvertFrom-RegKeyToSection([string]$KeyPath) {
    $n = $KeyPath.Trim()
    if ($n.StartsWith('[-')) {
        throw "refusing .reg key-delete form: $KeyPath"
    }
    foreach ($prefix in (Get-RegAppPrefixes)) {
        if ($n.Equals($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            return 'General'
        }
        if ($n.Length -gt $prefix.Length -and $n.StartsWith($prefix + '\', [StringComparison]::OrdinalIgnoreCase)) {
            $rel = $n.Substring($prefix.Length + 1)
            if (-not $rel) { return 'General' }
            return $rel
        }
    }
    foreach ($org in (Get-RegOrgPrefixes)) {
        if ($n.Equals($org, [StringComparison]::OrdinalIgnoreCase)) {
            return $null
        }
    }
    throw "unsupported registry key (not stock/isolated Moonlight app tree): $KeyPath"
}

function Join-RegContinuations([string[]]$Lines) {
    $joined = New-Object System.Collections.Generic.List[string]
    $buf = ''
    foreach ($line in $Lines) {
        $cur = $line
        if ($buf) { $cur = $buf + $cur.TrimStart() }
        if ($cur.TrimEnd().EndsWith('\')) {
            $buf = $cur.TrimEnd().TrimEnd('\')
            continue
        }
        [void]$joined.Add($cur)
        $buf = ''
    }
    if ($buf) { throw 'truncated hex/multiline .reg continuation' }
    return $joined
}

function Get-RegDocument([string]$Path) {
    $text = Read-TextFile $Path
    $rawLines = @($text -split "`r`n|`n|`r", -1)
    $lines = Join-RegContinuations $rawLines
    $doc = New-SettingsDoc
    $section = $null
    $sawHeader = $false
    foreach ($line in $lines) {
        if ($line -match '^\s*$') { continue }
        if ($line -match '^\s*;') { continue }
        if ($line -match '^\s*Windows Registry Editor Version 5\.00\s*$' -or $line -match '^\s*REGEDIT4\s*$') {
            $sawHeader = $true
            continue
        }
        if ($line -match '^\s*\[(.+)\]\s*$') {
            $section = ConvertFrom-RegKeyToSection $Matches[1]
            continue
        }
        if ($null -eq $section) { continue }
        if ($line -match '^\s*@=') {
            throw "unsupported .reg default-value form: $line"
        }
        if ($line -notmatch '^\s*"([^"]+)"=(.*)$') {
            throw "unsupported .reg line (not a typed value): $line"
        }
        $name = $Matches[1]
        $rhs = $Matches[2].Trim()
        if ($rhs -eq '-') {
            throw "unsupported .reg value-delete form for $name"
        }
        if ($rhs.StartsWith('"')) {
            Set-DocValue $doc $section $name 'string' (ConvertFrom-RegSz $rhs)
            continue
        }
        if ($rhs -match '^dword:([0-9a-fA-F]{1,8})$') {
            Set-DocValue $doc $section $name 'dword' (ConvertFrom-RegDword $Matches[1])
            continue
        }
        if ($rhs -match '^hex\(b\):(.+)$') {
            $bytes = ConvertFrom-RegHexBytes $Matches[1]
            if ($bytes.Length -ne 8) { throw "QWORD hex(b) must be 8 bytes: $name" }
            Set-DocValue $doc $section $name 'qword' ([BitConverter]::ToInt64($bytes, 0))
            continue
        }
        if ($rhs -match '^hex:(.*)$') {
            Set-DocValue $doc $section $name 'binary' (ConvertFrom-RegHexBytes $Matches[1])
            continue
        }
        throw "unsupported .reg type for $name (refusing silent corruption): $rhs"
    }
    if (-not $sawHeader) {
        throw 'backup .reg missing Windows Registry Editor header'
    }
    return $doc
}

function Get-BackupDoc([string]$Backup) {
    if (-not (Test-Path -LiteralPath $Backup)) { throw "backup not found: $Backup" }
    $item = Get-Item -LiteralPath $Backup
    if ($item.PSIsContainer) {
        $ini = Get-ChildItem -LiteralPath $Backup -Filter '*.ini' | Select-Object -First 1
        if (-not $ini) { throw "no .ini in backup folder $Backup" }
        return @{ Kind = 'ini'; Path = $ini.FullName; Doc = (Get-IniDocument $ini.FullName) }
    }
    $ext = $item.Extension.ToLowerInvariant()
    if ($ext -eq '.ini') {
        return @{ Kind = 'ini'; Path = $item.FullName; Doc = (Get-IniDocument $item.FullName) }
    }
    if ($ext -eq '.reg') {
        return @{ Kind = 'reg'; Path = $item.FullName; Doc = (Get-RegDocument $item.FullName) }
    }
    throw "backup must be .ini, .reg, or a folder containing an .ini"
}

function Merge-SettingsDoc($Src, $Dst) {
    $copied = 0
    $skipped = 0
    $destShortcut = Find-DocKey $Dst $PreserveKey
    $destHasPaddles = $false
    if ($destShortcut) {
        $destHasPaddles = Test-PaddleShortcut (Get-ShortcutText $destShortcut.Entry)
    }
    foreach ($section in @($Src.Keys)) {
        foreach ($key in @($Src[$section].Keys)) {
            if ($key -eq $PreserveKey -and $destHasPaddles) {
                $skipped++
                continue
            }
            $entry = $Src[$section][$key]
            Set-DocValue $Dst $section $key $entry.Type $entry.Value
            $copied++
        }
    }
    return @{ copied = $copied; skipped = $skipped }
}

function Write-IniDocument([string]$Path, $Doc) {
    Test-ForbiddenPath $Path
    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($section in @($Doc.Keys)) {
        [void]$lines.Add("[$section]")
        foreach ($key in @($Doc[$section].Keys)) {
            $entry = $Doc[$section][$key]
            [void]$lines.Add("$key=$(Get-EntryText $entry)")
        }
        [void]$lines.Add('')
    }
    $enc = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllLines($Path, $lines.ToArray(), $enc)
}

function Get-PortableIniPath([string]$ExtractDir) {
    Test-ForbiddenPath $ExtractDir
    return (Join-Path (Join-Path $ExtractDir $IsolatedOrg) "$IsolatedApp.ini")
}

function Backup-IsolatedRegistry {
    $key = "HKCU\Software\$IsolatedOrg\$IsolatedApp"
    if (-not (Test-Path -LiteralPath "HKCU:\Software\$IsolatedOrg\$IsolatedApp")) {
        return $null
    }
    $bak = Join-Path $env:TEMP "nonary-extended-hkcu-$(Get-Stamp).reg"
    & reg.exe export $key $bak /y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "reg export of isolated dest failed: $key" }
    Write-Host "dest backup -> $bak"
    return $bak
}

function Write-IsolatedRegistry($Doc) {
    $base = "HKCU:\Software\$IsolatedOrg\$IsolatedApp"
    Test-ForbiddenPath $base
    $existingShortcut = $null
    if (Test-Path -LiteralPath $base) {
        $props = Get-ItemProperty -LiteralPath $base -ErrorAction SilentlyContinue
        if ($props -and $props.PSObject.Properties[$PreserveKey]) {
            $existingShortcut = "$($props.$PreserveKey)"
        }
    }
    else {
        New-Item -Path $base -Force | Out-Null
    }
    $keepPaddles = Test-PaddleShortcut ([string]$existingShortcut)
    foreach ($section in @($Doc.Keys)) {
        $keyPath = $base
        if ($section -ne 'General') {
            $keyPath = Join-Path $base $section
        }
        Test-ForbiddenPath $keyPath
        if (-not (Test-Path -LiteralPath $keyPath)) {
            New-Item -Path $keyPath -Force | Out-Null
        }
        foreach ($name in @($Doc[$section].Keys)) {
            if ($name -eq $PreserveKey -and $keepPaddles) { continue }
            $entry = $Doc[$section][$name]
            $type = $entry.Type
            $value = $entry.Value
            if ($type -eq 'dword') {
                New-ItemProperty -LiteralPath $keyPath -Name $name -PropertyType DWord -Value ([int]$value) -Force | Out-Null
            }
            elseif ($type -eq 'qword') {
                New-ItemProperty -LiteralPath $keyPath -Name $name -PropertyType QWord -Value ([int64]$value) -Force | Out-Null
            }
            elseif ($type -eq 'binary') {
                New-ItemProperty -LiteralPath $keyPath -Name $name -PropertyType Binary -Value ([byte[]]$value) -Force | Out-Null
            }
            elseif ($type -eq 'ini' -and ([string]$value).StartsWith('@ByteArray(') -and ([string]$value).EndsWith(')')) {
                $inner = ([string]$value).Substring(11, ([string]$value).Length - 12)
                $bytes = [System.Text.Encoding]::GetEncoding(28591).GetBytes($inner)
                New-ItemProperty -LiteralPath $keyPath -Name $name -PropertyType Binary -Value $bytes -Force | Out-Null
            }
            else {
                New-ItemProperty -LiteralPath $keyPath -Name $name -PropertyType String -Value (Get-ShortcutText $entry) -Force | Out-Null
            }
        }
    }
}

function Assert-Contains([string]$Haystack, [string]$Needle, [string]$Msg) {
    if ($Haystack -notmatch [regex]::Escape($Needle)) { throw $Msg }
}

function Invoke-SelfTest {
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("ml-port-import-" + [guid]::NewGuid().ToString('n'))
    New-Item -ItemType Directory -Path $tmp | Out-Null
    try {
        $src = Join-Path $tmp 'backup.ini'
        @(
            '[General]'
            "$PreserveKey=16,17"
            '[streamsettings]'
            'bitrate=50000'
        ) | Set-Content -LiteralPath $src -Encoding ASCII

        $destDir = Join-Path $tmp 'portable'
        New-Item -ItemType Directory -Path $destDir | Out-Null
        $destIni = Get-PortableIniPath $destDir
        New-Item -ItemType Directory -Path (Split-Path -Parent $destIni) -Force | Out-Null
        @(
            '[General]'
            "$PreserveKey=4,5"
            'width=1920'
            '[streamsettings]'
            'fps=120'
        ) | Set-Content -LiteralPath $destIni -Encoding ASCII

        $srcDoc = (Get-BackupDoc $src).Doc
        $dstDoc = Get-IniDocument $destIni
        $null = Merge-SettingsDoc $srcDoc $dstDoc
        Write-IniDocument $destIni $dstDoc
        $got = Read-TextFile $destIni
        Assert-Contains $got "$PreserveKey=16,17" 'did not apply backup 16,17 over dest 4,5'
        Assert-Contains $got 'bitrate=50000' 'did not copy missing key'
        Assert-Contains $got 'width=1920' 'dropped dest-only key'
        Assert-Contains $got 'fps=120' 'dropped dest-only grouped key'
        Assert-Contains $got '[streamsettings]' 'flattened grouped streamsettings into General'
        $genMatch = [regex]::Match($got, '(?ms)\[General\](.*?)(?=\[|\z)')
        if ($genMatch.Success -and $genMatch.Groups[1].Value -match 'bitrate=') {
            throw 'streamsettings keys leaked into General'
        }
        if ($got -notmatch '(?ms)\[streamsettings\][^\[]*bitrate=50000') {
            throw 'bitrate not kept under [streamsettings]'
        }

        $src2 = Join-Path $tmp 'backup2.ini'
        @(
            '[General]'
            "$PreserveKey=4,5"
        ) | Set-Content -LiteralPath $src2 -Encoding ASCII
        $map2 = (Get-BackupDoc $src2).Doc
        $dst2 = Get-IniDocument $destIni
        $null = Merge-SettingsDoc $map2 $dst2
        Write-IniDocument $destIni $dst2
        $got2 = Read-TextFile $destIni
        Assert-Contains $got2 "$PreserveKey=16,17" 'overwrote existing 16,17'

        $freshDir = Join-Path $tmp 'fresh'
        $freshIni = Get-PortableIniPath $freshDir
        New-Item -ItemType Directory -Path (Split-Path -Parent $freshIni) -Force | Out-Null
        Copy-Item -LiteralPath $src -Destination $freshIni
        $freshText = Read-TextFile $freshIni
        Assert-Contains $freshText '[streamsettings]' 'fresh INI copy dropped sections'
        Assert-Contains $freshText 'bitrate=50000' 'fresh INI copy dropped grouped keys'
        $expectedLeaf = ($IsolatedOrg + '/' + $IsolatedApp + '.ini')
        $freshNorm = $freshIni.Replace('\', '/')
        if (-not $freshNorm.EndsWith($expectedLeaf)) {
            throw "portable dest is not nested org/app ini: $freshIni"
        }

        $regPath = Join-Path $tmp 'stock.reg'
        $reg = @(
            'Windows Registry Editor Version 5.00'
            ''
            "[HKEY_CURRENT_USER\Software\$StockOrg\$StockApp]"
            "`"$PreserveKey`"=`"16,17`""
            '"width"=dword:00000780'
            '"controllerkbmmappings"=hex:7b,22,61,22,3a,22,62,22,7d'
        ) -join "`r`n"
        [System.IO.File]::WriteAllText($regPath, $reg, [System.Text.Encoding]::Unicode)
        $regDoc = Get-RegDocument $regPath
        $width = Get-DocValue $regDoc 'General' 'width'
        if ($width.Type -ne 'dword' -or [int]$width.Value -ne 1920) {
            throw "dword not parsed to integer 1920: $($width.Type)=$($width.Value)"
        }
        $regIni = Join-Path $tmp 'from-reg.ini'
        Write-IniDocument $regIni $regDoc
        $regText = Read-TextFile $regIni
        if ($regText -match 'dword:') { throw 'wrote literal dword: into INI' }
        Assert-Contains $regText 'width=1920' 'dword did not become decimal INI value'
        Assert-Contains $regText "$PreserveKey=16,17" 'reg string shortcut missing'
        Assert-Contains $regText '@ByteArray(' 'REG_BINARY not converted to @ByteArray'

        $badReg = Join-Path $tmp 'bad.reg'
        $bad = @(
            'Windows Registry Editor Version 5.00'
            "[HKEY_CURRENT_USER\Software\$StockOrg\$StockApp]"
            '"list"=hex(7):66,00,6f,00,6f,00,00,00,00,00'
        ) -join "`r`n"
        [System.IO.File]::WriteAllText($badReg, $bad, [System.Text.Encoding]::Unicode)
        $threw = $false
        try { $null = Get-RegDocument $badReg } catch { $threw = $true }
        if (-not $threw) { throw 'hex(7) MULTI_SZ was silently accepted' }

        $rootIni = Join-Path $destDir "$IsolatedApp.ini"
        if (Test-Path -LiteralPath $rootIni) { throw 'wrote portable ini at extract root' }

        $stockSrcDir = Join-Path $tmp $StockOrg
        New-Item -ItemType Directory -Path $stockSrcDir | Out-Null
        $stockSrcIni = Join-Path $stockSrcDir 'Moonlight.ini'
        Copy-Item -LiteralPath $src -Destination $stockSrcIni
        $fromStock = Get-BackupDoc $stockSrcIni
        if (-not (Find-DocKey $fromStock.Doc $PreserveKey)) { throw 'refused backup source under stock org path' }

        $badDest = Join-Path $tmp $StockOrg
        $threwDest = $false
        try { $null = Get-PortableIniPath $badDest } catch { $threwDest = $true }
        if (-not $threwDest) { throw 'accepted dest path that names stock Moonlight identity' }

        Write-Host 'self-test ok'
    }
    finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force
    }
}

if ($SelfTest) {
    Invoke-SelfTest
    return
}

if (-not $Backup) { throw 'specify -Backup <ini|reg|folder>' }
if (-not $Installed -and -not $PortableDir) {
    throw 'specify -PortableDir <extract-dir> or -Installed (isolated HKCU only)'
}

$backupInfo = Get-BackupDoc $Backup
$map = $backupInfo.Doc
$sc = Find-DocKey $map $PreserveKey
$scText = ''
if ($sc) { $scText = Get-ShortcutText $sc.Entry }
Write-Host "backup keys: $((@($map.Keys | ForEach-Object { $map[$_].Keys }).Count)); $PreserveKey=$scText"

if ($PortableDir) {
    $ini = Get-PortableIniPath $PortableDir
    if (-not (Test-Path -LiteralPath $ini) -and $backupInfo.Kind -eq 'ini') {
        $dir = Split-Path -Parent $ini
        if (-not (Test-Path -LiteralPath $dir)) {
            New-Item -ItemType Directory -Force -Path $dir | Out-Null
        }
        Copy-Item -LiteralPath $backupInfo.Path -Destination $ini
        Write-Host "portable INI copied (all sections preserved) -> $ini"
    }
    else {
        $null = Backup-ExistingFile $ini
        $dst = New-SettingsDoc
        if (Test-Path -LiteralPath $ini) {
            $dst = Get-IniDocument $ini
        }
        $r = Merge-SettingsDoc $map $dst
        Write-IniDocument $ini $dst
        Write-Host "portable merge copied=$($r.copied) skipped=$($r.skipped) -> $ini"
    }
}

if ($Installed) {
    $null = Backup-IsolatedRegistry
    Write-IsolatedRegistry $map
    Write-Host "installed merge -> HKCU\Software\$IsolatedOrg\$IsolatedApp (16,17 not overwritten; stock HKCU untouched)"
}
