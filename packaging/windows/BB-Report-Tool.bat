<# : BB-Report-Tool.bat - Bloodborne PC Port: system info for bug and crash reports
@echo off
setlocal
set "BBTOOL=%~f0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "Invoke-Expression ([IO.File]::ReadAllText($env:BBTOOL))"
echo.
pause
exit /b
#>

# ------------------------------------------------------------------------------------------
# BB Report Tool - collects what the helpers need for a bug or crash report.
#
# What it does:  reads your PC specs, the port settings, user\last_run.log and the crashes Windows recorded.
# Tip:  run it right after a crash - the game replaces last_run.log on every start.
# What it does NOT do:  no internet, no changes to the game or your settings.
# Result:  BB-Report-<date>.zip on your Desktop + a short summary in your clipboard.
# Privacy:  your Windows user name, PC name and online login are removed from every file.
#           Your online account file (user\bbbridge) and your password are never copied.
#
# Double-click the file. It finds the port folder and its version by itself; if not, it asks.
# ------------------------------------------------------------------------------------------
$ErrorActionPreference = 'SilentlyContinue'
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture
$ToolVersion = '1.7.6'
Add-Type -AssemblyName System.Windows.Forms

$secrets = New-Object System.Collections.ArrayList
function Hide-Private([string]$text) {
    if (-not $text) { return $text }
    $text = $text.Replace($env:USERPROFILE, '%USERPROFILE%')
    # Online play: account names and ids, IP addresses and peer endpoints the port and its network
    # libraries log. Crash addresses and registers stay as they are.
    $text = [regex]::Replace($text, "(The Hunter's Dream as )[^,\r\n]+", '$1<hidden>')
    $text = [regex]::Replace($text, "(?i)\b((?:npid|owner|handle\.data)=|Login packet sent for |online id set to )'[^'\r\n]*'", '$1''<hidden>''')
    $text = [regex]::Replace($text, '(?i)\b(accountId|account_id)=(0x)?[0-9a-f]+', '$1=<hidden>')
    $text = [regex]::Replace($text, '(?i)\b(account id )\d+', '$1<hidden>')
    $text = [regex]::Replace($text, '(?i)\b(addr|peerAddr|localAddr|mappedAddr|server_addr)=0x[0-9a-f]+', '$1=<hidden>')
    $text = [regex]::Replace($text, '(?i)\b(external IP|lan)=[^\s,;)]+', '$1=<ip>')
    $octet = '(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)'
    $text = [regex]::Replace($text, "(?<![\d.])(?!127\.0\.0\.1(?![\d.]))(?!0\.0\.0\.0(?![\d.]))(?:$octet\.){3}$octet(?![\d.])", '<ip>')
    foreach ($s in @($env:USERNAME, (Split-Path -Leaf $env:USERPROFILE), $env:COMPUTERNAME) + $secrets) {
        if ($s -and $s.Length -ge 3 -and $s -notin 'user', 'users', 'admin', 'owner', 'game', 'mods') { $text = [regex]::Replace($text, [regex]::Escape($s), '<hidden>', 'IgnoreCase') }
    }
    return $text
}
function GB($bytes) { '{0:N0} GB' -f ($bytes / 1GB) }
function Val($value) { if ("$value" -ne '') { "$value" } else { 'default' } }

Write-Host ''
Write-Host "  BB Report Tool $ToolVersion - Bloodborne PC Port" -ForegroundColor Yellow
Write-Host '  Collecting info for your #help post. Nothing is sent anywhere.'
Write-Host ''

# --- Port folder --------------------------------------------------------------------------
function Test-Port([string]$dir) {
    $dir -and [IO.File]::Exists((Join-Path $dir 'Bloodborne.exe')) -and [IO.File]::Exists((Join-Path $dir 'bin\bb-probe.exe'))
}
function Add-Candidate($list, [string]$exe) {
    if (-not $exe) { return }
    $dir = Split-Path $exe
    if ((Split-Path -Leaf $dir) -eq 'bin') { $dir = Split-Path $dir }
    if ((Test-Port $dir) -and -not $list.Contains($dir)) { [void]$list.Add($dir) }
}
function Find-Ports {
    # Quick places first: a running launcher or game, programs Windows remembers starting, shortcuts.
    $found = New-Object System.Collections.ArrayList
    Get-Process -Name 'Bloodborne', 'Play Bloodborne', 'bb-probe' | ForEach-Object { Add-Candidate $found $_.Path }
    foreach ($key in 'HKCU:\Software\Classes\Local Settings\Software\Microsoft\Windows\Shell\MuiCache',
                     'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Compatibility Assistant\Store') {
        $item = Get-Item -LiteralPath $key
        if ($item) {
            foreach ($name in $item.GetValueNames()) {
                $exe = $name -replace '\.(FriendlyAppName|ApplicationCompany)$', ''
                if ($exe -match '\\(Bloodborne|Play Bloodborne|bb-probe)\.exe$') { Add-Candidate $found $exe }
            }
        }
    }
    $shell = New-Object -ComObject WScript.Shell
    foreach ($folder in 'Desktop', 'CommonDesktopDirectory', 'StartMenu', 'CommonStartMenu') {
        $path = [Environment]::GetFolderPath($folder)
        if (-not $path) { continue }
        Get-ChildItem -LiteralPath $path -Filter *.lnk -Recurse -Depth 3 | ForEach-Object {
            $target = $shell.CreateShortcut($_.FullName).TargetPath
            if ($target -match '\\(Bloodborne|Play Bloodborne)\.exe$') { Add-Candidate $found $target }
        }
    }
    return , $found
}
function Search-Drives {
    # Slow fallback: every fixed drive, five folder levels deep, system folders skipped, at most a minute.
    $skip = 'Windows', 'Windows.old', 'ProgramData', '$Recycle.Bin', 'System Volume Information', 'Recovery',
            'AppData', 'WindowsApps', 'node_modules', '.git', 'PerfLogs', 'Config.Msi'
    $found = New-Object System.Collections.ArrayList
    $queue = New-Object System.Collections.Queue
    foreach ($drive in [IO.DriveInfo]::GetDrives()) {
        if ($drive.DriveType -eq 'Fixed' -and $drive.IsReady) { $queue.Enqueue(@($drive.RootDirectory.FullName, 0)) }
    }
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($queue.Count -and $watch.Elapsed.TotalSeconds -lt 60) {
        $entry = $queue.Dequeue(); $dir = $entry[0]; $depth = $entry[1]
        if (Test-Port $dir) { [void]$found.Add($dir); continue }
        if ($depth -ge 5) { continue }
        try {
            foreach ($sub in [IO.Directory]::EnumerateDirectories($dir)) {
                if ([IO.Path]::GetFileName($sub) -notin $skip) { $queue.Enqueue(@($sub, ($depth + 1))) }
            }
        } catch { }
    }
    return , $found
}
function Pick-Port($list) {
    # Several installs: take the one the game ran from last (newest user\last_run.log).
    $list | Sort-Object { (Get-Item -LiteralPath (Join-Path $_ 'user\last_run.log')).LastWriteTime } -Descending | Select-Object -First 1
}

$port = $env:BBTOOL_PORT
if (-not $port) {
    $here = Split-Path $env:BBTOOL
    if (Test-Port $here) { $port = $here } elseif (Test-Port (Split-Path $here)) { $port = Split-Path $here }
}
if (-not $port) {
    Write-Host '  Looking for your port folder...'
    $candidates = Find-Ports
    if (-not $candidates.Count) {
        Write-Host '  Searching your drives (this can take up to a minute)...'
        $candidates = Search-Drives
    }
    if ($candidates.Count) {
        $port = Pick-Port $candidates
        Write-Host "  Found the port: $(Hide-Private $port)"
    }
}
if (-not $port) {
    Write-Host '  Choose your port folder (the one with Bloodborne.exe) in the window that opens...'
    $owner = New-Object System.Windows.Forms.Form -Property @{ TopMost = $true }
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = 'Choose your Bloodborne port folder (the one with Bloodborne.exe)'
    if ($dialog.ShowDialog($owner) -eq 'OK') { $port = $dialog.SelectedPath }
}
$portOk = $port -and (Test-Path (Join-Path $port 'Bloodborne.exe'))

# --- Launcher settings (secrets are never copied, only used to hide them) -----------------
$settings = $null
$settingsFile = if ($env:BBTOOL_SETTINGS) { $env:BBTOOL_SETTINGS } else { Join-Path $env:APPDATA 'bbport-launcher\settings.json' }
if (Test-Path $settingsFile) { $settings = Get-Content $settingsFile -Raw -Encoding UTF8 | ConvertFrom-Json }
if ($settings) {
    foreach ($key in 'online_npid', 'online_password', 'player_name') {
        if ($settings.$key) { [void]$secrets.Add([string]$settings.$key) }
    }
}
function Setting($key) { if ($settings) { $settings.$key } }

# --- The port that really ran -------------------------------------------------------------
# Every log names it ('Output: <port>\out'). When the saves folder points into another install
# (an old copy, a fork), the newest log sits there and the search above picks the wrong port.
$firstUser = Setting 'user_dir'
if (-not $firstUser -and $portOk) { $firstUser = Join-Path $port 'user' }
$firstLog = @(@((Join-Path "$firstUser" 'last_run.log'), $(if ($portOk) { Join-Path $port 'last_run.log' })) |
    Where-Object { $_ -and (Test-Path $_) } | ForEach-Object { Get-Item $_ } | Sort-Object LastWriteTime -Descending) | Select-Object -First 1
if ($firstLog) {
    $outLine = Select-String -Path $firstLog.FullName -Pattern '^Output: (.+)\\out\s*$' | Select-Object -Last 1
    if ($outLine) {
        $ran = $outLine.Matches[0].Groups[1].Value
        if ((Test-Port $ran) -and $ran.TrimEnd('\') -ne "$port".TrimEnd('\')) { $port = $ran; $portOk = $true }
    }
}

# --- Port version -------------------------------------------------------------------------
function Get-PortVersion([string]$dir) {
    # The launcher runs a small script inside itself (--script); its own VERSION is then readable.
    # Only builds with scripts\ and run.py have that mode, older ones would open the launcher window.
    if (-not (Test-Path -LiteralPath (Join-Path $dir 'scripts')) -or -not (Test-Path -LiteralPath (Join-Path $dir 'run.py'))) { return $null }
    $tmp = Join-Path $env:TEMP ('bbtool-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tmp | Out-Null
    $py = Join-Path $tmp 'version.py'; $out = Join-Path $tmp 'version.out'
    @(
        'import sys',
        'f = sys._getframe()',
        'while f is not None:',
        '    g = f.f_globals',
        "    if 'VERSION' in g and 'PATCH_VERSION' in g:",
        "        open(sys.argv[1], 'w').write(str(g['VERSION']))",
        '        break',
        '    f = f.f_back'
    ) | Set-Content -LiteralPath $py -Encoding ASCII
    $p = Start-Process -FilePath (Join-Path $dir 'Bloodborne.exe') -ArgumentList '--script', "`"$py`"", "`"$out`"" `
        -WorkingDirectory $dir -WindowStyle Hidden -PassThru
    if ($p -and -not $p.WaitForExit(15000)) { $p.Kill() }
    $v = $null
    if (Test-Path -LiteralPath $out) { $v = (Get-Content -LiteralPath $out -Raw).Trim() }
    Remove-Item -LiteralPath $tmp -Recurse -Force
    if ($v -match '^\d+(\.\d+){1,3}$') { return $v }
    return $null
}

$portVersion = $env:BBTOOL_VERSION
if (-not $portVersion -and $portOk -and (Test-Path (Join-Path $port 'version.txt'))) {
    $portVersion = (Get-Content (Join-Path $port 'version.txt') -TotalCount 1).Trim()
}
if (-not $portVersion -and $portOk) { $portVersion = Get-PortVersion $port }
if (-not $portVersion) {
    Write-Host ''
    $portVersion = Read-Host '  Port version (bottom left in the launcher, e.g. 1.7.2). Type it and press Enter'
}
if (-not $portVersion) { $portVersion = 'not given' }

# --- Game dump: title ID and game version from sce_sys\param.sfo ---------------------------
function Read-Sfo([string]$path) {
    $b = [IO.File]::ReadAllBytes($path)
    if ($b.Length -lt 20 -or $b[1] -ne 0x50 -or $b[2] -ne 0x53 -or $b[3] -ne 0x46) { return $null }
    $keys = [BitConverter]::ToUInt32($b, 8); $values = [BitConverter]::ToUInt32($b, 12)
    $count = [BitConverter]::ToUInt32($b, 16); $result = @{}
    for ($i = 0; $i -lt $count; $i++) {
        $e = 20 + $i * 16
        $p = $keys + [BitConverter]::ToUInt16($b, $e); $name = ''
        while ($b[$p] -ne 0) { $name += [char]$b[$p]; $p++ }
        $fmt = [BitConverter]::ToUInt16($b, $e + 2); $size = [BitConverter]::ToUInt32($b, $e + 4)
        $off = $values + [BitConverter]::ToUInt32($b, $e + 12)
        if ($fmt -eq 0x404) { $result[$name] = [BitConverter]::ToUInt32($b, $off) }
        else { $result[$name] = [Text.Encoding]::UTF8.GetString($b, $off, $size).TrimEnd([char]0) }
    }
    return $result
}
$gameDir = Setting 'game_dir'
if (-not $gameDir -and $portOk) { $gameDir = Join-Path (Split-Path $port) 'CUSA03173' }
$game = 'game folder not found'
if ($gameDir -and (Test-Path (Join-Path $gameDir 'eboot.bin'))) {
    $sfo = Read-Sfo (Join-Path $gameDir 'sce_sys\param.sfo')
    if ($sfo) { $game = "$($sfo['TITLE_ID']) v$($sfo['APP_VER'])" } else { $game = 'eboot.bin found, param.sfo unreadable' }
    if ($sfo -and $sfo['APP_VER'] -ne '01.09') { $game += '  (not 1.09: FPS patches off, 30 FPS)' }
} elseif ($gameDir) { $game = 'no eboot.bin in the chosen game folder' }

# --- Windows, CPU, memory -----------------------------------------------------------------
$os = Get-CimInstance Win32_OperatingSystem
$nt = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$osLine = "$($os.Caption -replace 'Microsoft ', '') $($nt.DisplayVersion) (build $($os.BuildNumber).$($nt.UBR))"

$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$avx = 'unknown'
try {
    Add-Type -Namespace BBTool -Name Cpu -MemberDefinition '[DllImport("kernel32.dll")] public static extern bool IsProcessorFeaturePresent(uint feature);'
    $avx = if ([BBTool.Cpu]::IsProcessorFeaturePresent(40)) { 'AVX2 yes' } elseif ([BBTool.Cpu]::IsProcessorFeaturePresent(39)) { 'AVX yes, no AVX2' } else { 'NO AVX' }
} catch { }
$cpuLine = "$($cpu.Name.Trim()) ($($cpu.NumberOfCores)C/$($cpu.NumberOfLogicalProcessors)T, $avx)"

$cs = Get-CimInstance Win32_ComputerSystem
$pageMb = (Get-CimInstance Win32_PageFileUsage | Measure-Object AllocatedBaseSize -Sum).Sum
$pageText = if ($pageMb) { GB ($pageMb * 1MB) } elseif ($cs.AutomaticManagedPagefile) { 'automatic' } else { 'NONE (page file off)' }
$ramLine = "$(GB $cs.TotalPhysicalMemory), page file $pageText (commit limit $(GB ($os.TotalVirtualMemorySize * 1KB)))"

# --- Graphics cards: driver, AMD Adrenalin / NVIDIA version, VRAM -------------------------
$classKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
$gpuRegs = Get-ChildItem $classKey | Where-Object { $_.PSChildName -match '^\d{4}$' } | ForEach-Object { Get-ItemProperty $_.PSPath }
$gpuLines = @(); $displayLines = @(); $gpuInfo = @()
foreach ($vc in Get-CimInstance Win32_VideoController) {
    if ($vc.Name -match 'Basic Display|Remote|Virtual|Parsec|Meta') { continue }
    $reg = $gpuRegs | Where-Object { $_.DriverDesc -eq $vc.Name } | Select-Object -First 1
    $vram = ''; $vramBytes = 0
    if ($reg.'HardwareInformation.qwMemorySize') { $vramBytes = [uint64]$reg.'HardwareInformation.qwMemorySize'; $vram = ', ' + (GB $vramBytes) }
    # The processor's own graphics (laptops and many desktops have one next to the real card).
    $integrated = ($vc.Name -match 'Intel' -and $vc.Name -notmatch 'Arc\(TM\) [AB]\d') -or $vc.Name -match 'Vega \d+ Graphics' -or
        ($vc.Name -match 'Radeon' -and $vc.Name -notmatch 'RX|Pro|Radeon VII')
    $gpuInfo += [pscustomobject]@{ Name = $vc.Name; Date = $vc.DriverDate; Vram = $vramBytes; Integrated = $integrated }
    $driver = $vc.DriverVersion
    if ($vc.Name -match 'NVIDIA') {
        $digits = ($vc.DriverVersion -split '\.')[-2..-1] -join ''
        $digits = $digits.Substring($digits.Length - 5)
        $driver = "$($digits.Substring(0, 3)).$($digits.Substring(3)) ($($vc.DriverVersion))"
    } elseif ($reg.RadeonSoftwareVersion) {
        $driver = "$($reg.RadeonSoftwareVersion) ($($vc.DriverVersion))"
    }
    $date = ''
    if ($vc.DriverDate) { $date = ', ' + $vc.DriverDate.ToString('yyyy-MM-dd') }
    $gpuLines += "$($vc.Name)$vram, driver $driver$date"
    if ($vc.CurrentHorizontalResolution) {
        $displayLines += "$($vc.CurrentHorizontalResolution)x$($vc.CurrentVerticalResolution) @ $($vc.CurrentRefreshRate) Hz"
    }
}
if (-not $gpuLines) { $gpuLines = @('not found') }
if (-not $displayLines) { $displayLines = @('unknown') }
# The card the game most likely runs on: a dedicated one if there is one.
$gameGpu = @(@($gpuInfo | Where-Object { -not $_.Integrated }) + @($gpuInfo)) | Select-Object -First 1

# --- Free disk space ----------------------------------------------------------------------
$space = @()
$drives = [ordered]@{}
foreach ($pair in @(@('port', $port), @('game', $gameDir))) {
    if (-not $pair[1]) { continue }
    $drive = Split-Path -Qualifier $pair[1]
    if ($drives.Contains($drive)) { $drives[$drive] += " + $($pair[0])" } else { $drives[$drive] = $pair[0] }
}
foreach ($drive in $drives.Keys) {
    $disk = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$drive'"
    $fs = if ($disk.FileSystem) { $disk.FileSystem } else { 'unknown file system' }
    if ($disk) { $space += "$drive $(GB $disk.FreeSpace) free, $fs ($($drives[$drive]))" }
}
# Mods need folder links (junctions), which FAT32/exFAT drives can't hold.
$gameFs = $null
if ($gameDir) { $gameFs = (Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$(Split-Path -Qualifier $gameDir)'").FileSystem }

# --- Port settings ------------------------------------------------------------------------
$ini = @{}
$iniFile = if ($portOk) { Join-Path $port 'bbport.ini' }
if ($iniFile -and (Test-Path $iniFile)) {
    foreach ($line in Get-Content $iniFile) { if ($line -match '^\s*([^#;=]+?)\s*=\s*(.*)$') { $ini[$Matches[1]] = $Matches[2] } }
}
$settingsLine = 'upscaler {0} preset {1}, output {2}, fps {3} cap {4}, two-stage {5}, readbacks {6}, fullscreen {7}, online {8} ({9})' -f `
    (Val $ini['upscaler']), (Val $ini['preset']), (Val $ini['output_res']), (Val (Setting 'fps_mode')),
    (Val (Setting 'frame_cap')), (Val (Setting 'draw_pipe')), (Val (Setting 'readbacks')), (Val (Setting 'fullscreen')),
    (Val (Setting 'online')), (Val (Setting 'online_pool'))
if (Setting 'extra_env') { $settingsLine += ", extra: $(Setting 'extra_env')" }

# --- Mods ---------------------------------------------------------------------------------
$modsDir = Setting 'mods_dir'
if (-not $modsDir -and $portOk) { $modsDir = Join-Path $port 'mods' }
$modsLine = 'none'
$activeMods = 0
if ($modsDir -and (Test-Path $modsDir)) {
    $off = @()
    if ($portOk -and (Test-Path (Join-Path $port 'mods.json'))) {
        $off = @((Get-Content (Join-Path $port 'mods.json') -Raw | ConvertFrom-Json).disabled)
    }
    $mods = @(Get-ChildItem $modsDir -Directory | ForEach-Object { if ($off -contains $_.Name) { "$($_.Name) (off)" } else { $_.Name } })
    if ($mods) { $modsLine = "$($mods.Count): " + ($mods -join ', ') }
    if ((Setting 'mods_enabled') -eq $false) { $modsLine += '  [mods switched off in launcher]' }
    else { $activeMods = @($mods | Where-Object { $_ -notlike '* (off)' }).Count }
    if ($modsLine.Length -gt 300) { $modsLine = $modsLine.Substring(0, 297) + '...' }
}

# --- Third-party patches (shadPS4/GoldHEN XML files the launcher applies on top of its own) ---
$extPatches = @()
if ($portOk -and (Test-Path (Join-Path $port 'patches.json'))) {
    $extPatches = @((Get-Content (Join-Path $port 'patches.json') -Raw | ConvertFrom-Json).enabled | Where-Object { $_ })
}
$patchLine = 'none'
if ($extPatches) {
    $patchLine = "$($extPatches.Count): " + (($extPatches | ForEach-Object { ($_ -split '/', 2)[-1] }) -join ', ')
    if ($patchLine.Length -gt 300) { $patchLine = $patchLine.Substring(0, 297) + '...' }
}

# --- Overlays that often break Vulkan games -----------------------------------------------
$overlays = @(Get-Process | Where-Object { $_.ProcessName -in 'RTSS', 'MSIAfterburner', 'Overwolf' } |
    Select-Object -ExpandProperty ProcessName -Unique)
$overlayLine = if ($overlays) { $overlays -join ', ' } else { 'none found' }

# --- Log: user\last_run.log ---------------------------------------------------------------
$userDir = Setting 'user_dir'
if (-not $userDir -and $portOk) { $userDir = Join-Path $port 'user' }
$logs = @()
foreach ($candidate in @((Join-Path $userDir 'last_run.log'), $(if ($portOk) { Join-Path $port 'last_run.log' }))) {
    if ($candidate -and (Test-Path $candidate)) { $logs += Get-Item $candidate }
}
$log = $logs | Sort-Object LastWriteTime -Descending | Select-Object -First 1
$logLine = 'not found (start the game once, then run this tool again)'
$errorLine = '-'
$logText = $null
$fullLogText = $null
if ($log) {
    $logLine = "last_run.log $($log.LastWriteTime.ToString('yyyy-MM-dd HH:mm')), $('{0:N1}' -f ($log.Length / 1MB)) MB"
    $allLines = @(Get-Content $log.FullName -Tail 50000 -Encoding UTF8)
    $fullLogText = $allLines -join "`r`n"
    # An in-game restart appends a second start to the same file: only the last one is checked
    # (it begins with the eboot line, plus the 'Mods:' lines the launcher writes just before it).
    $starts = @(for ($i = 0; $i -lt $allLines.Count; $i++) { if ($allLines[$i] -match '\| entry=0x[0-9a-f]+ \| image=') { $i } })
    $first = if ($starts.Count) { $starts[-1] } else { 0 }
    while ($first -gt 0 -and $allLines[$first - 1] -match '^Mods: ') { $first-- }
    $logText = $allLines[$first..($allLines.Count - 1)] -join "`r`n"
    if ($starts.Count -gt 1) { $logLine += ", $($starts.Count) starts in the file, only the last one checked" }
    $vramPeak = ([regex]::Matches($logText, '(?m)^Memory: VRAM (\d+) MiB') | ForEach-Object { [int]$_.Groups[1].Value } | Measure-Object -Maximum).Maximum
    if ($vramPeak) { $logLine += ", VRAM peak $vramPeak MiB" }
    # The line that names the cause beats the generic 'STOP: GPU library assertion failed' after it.
    # error=10 only counts from cpp-httplib (SSL): P2P lines like error=10054 are harmless.
    $hit = [regex]::Matches($logText, '(?im)^.*(ErrorOut\w+|Device lost|Unimplemented PM4|Fault: (read|write) access|Fault: exception|PermissionError|Mods: \[|Lance McDonald|Login rejected|cpp-httplib failed.*error=10\b|SSL server verification failed|Failed to create graphics pipeline|cannot open boot file|Unknown data_format|Out of order flip|shaderBinarySize|MiB of guest memory).*$') |
        Select-Object -Last 1
    if (-not $hit) {
        # Warnings, stubs and statistics are never the error.
        $hit = [regex]::Matches($logText, '(?im)^(?!.*<Warning>)(?!.*\((STUBBED|DUMMY)\)).*(STOP:|Assertion Failed|Unimplemented|exception|fatal).*$') | Select-Object -Last 1
    }
    if ($hit) { $errorLine = $hit.Value.Trim(); if ($errorLine.Length -gt 160) { $errorLine = $errorLine.Substring(0, 157) + '...' } }

    # Mods and third-party patches as the port really loaded them (load order, XMLs switched on
    # in the file itself); folders in mods\ that aren't mods don't count.
    # The port's own layers, switched on in its settings, are not mods: quit_text (the Quit to Desktop menu
    # texts), rebirth (Rebirth at the Doll), hud_layout (HUD position), fov (field of view).
    $builtInMods = @('quit_text', 'rebirth', 'hud_layout', 'fov')
    $loadedMods = @([regex]::Matches($logText, '(?m)^Mods: (.+): \d+ files\s*$') | ForEach-Object { $_.Groups[1].Value })
    if ($loadedMods) {
        $userMods = @($loadedMods | Where-Object { $builtInMods -notcontains $_ })
        $ownLayers = @($loadedMods | Where-Object { $builtInMods -contains $_ })
        $activeMods = $userMods.Count
        $modsLine = if ($userMods) { "$($userMods.Count) loaded (log, in load order): " + ($userMods -join ', ') } else { 'none' }
        if ($ownLayers) { $modsLine += "  [built-in: $($ownLayers -join ', ')]" }
        if ($modsLine.Length -gt 300) { $modsLine = $modsLine.Substring(0, 297) + '...' }
    }
    $logPatches = @([regex]::Matches($logText, '(?m)^Patches: external (.+?) \(\d+ writes\)\s*$') | ForEach-Object { $_.Groups[1].Value.Trim() })
    if ($logPatches) {
        $extPatches = $logPatches
        $patchLine = "$($extPatches.Count) (log): " + (($extPatches | ForEach-Object { ($_ -split '/', 2)[-1] }) -join ', ')
        if ($patchLine.Length -gt 300) { $patchLine = $patchLine.Substring(0, 297) + '...' }
    } elseif ($extPatches -and $logText -match '(?m)^Patches: \d+ writes') {
        # patches.json lists them, but the port applied none (e.g. the XML isn't in its patches folder).
        $patchLine = "none applied (patches.json lists $($extPatches.Count), the log shows none)"
        $extPatches = @()
    }
}

# Hunter's Dream / bbbridge account names are hidden too (that file itself is never copied).
$bridgeState = if ($userDir) { Join-Path $userDir 'bbbridge\bbbridge_state.json' }
if ($bridgeState -and (Test-Path $bridgeState)) {
    $bs = Get-Content $bridgeState -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($bs.accounts) {
        foreach ($a in $bs.accounts.PSObject.Properties) {
            [void]$secrets.Add([string]$a.Name)
            foreach ($k in 'bbhost_name', 'online_id') { if ($a.Value.$k) { [void]$secrets.Add([string]$a.Value.$k) } }
        }
    }
}

# --- Crashes Windows remembers (they survive a new start, unlike last_run.log) -------------
# The launcher replaces last_run.log on every start, so after a restart the crash is only here.
$since = (Get-Date).AddDays(-14)
$crashEvents = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000, 1002; StartTime = $since } -MaxEvents 300 |
    Where-Object { "$($_.Properties[0].Value)" -match '^(bb-probe|Bloodborne|Play Bloodborne)\.exe$' })
$gpuResets = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Display'; Id = 4101; StartTime = $since } -MaxEvents 50)
$crashLines = foreach ($e in $crashEvents | Select-Object -First 8) {
    $p = $e.Properties
    if ($e.Id -eq 1002) { "$($e.TimeCreated.ToString('yyyy-MM-dd HH:mm'))  $($p[0].Value) stopped responding (hang)" }
    else {
        $code = "$($p[6].Value)" -replace '^0x', ''
        if ($code -match '^\d+$' -and $code.Length -gt 8) { $code = '{0:x8}' -f [uint32][int64]$code }
        $offset = "$($p[7].Value)" -replace '^0x', ''
        "$($e.TimeCreated.ToString('yyyy-MM-dd HH:mm'))  $($p[0].Value) crashed in $($p[3].Value) (code 0x$code, offset 0x$offset)"
    }
}
$crashLine = if ($crashEvents) { "$($crashEvents.Count) in 14 days, last: $(($crashLines | Select-Object -First 1) -replace '^\S+ \S+  ', '')" } else { 'none recorded by Windows (14 days)' }
$gpuLine = if ($gpuResets) { "$($gpuResets.Count) driver resets in 14 days, last $($gpuResets[0].TimeCreated.ToString('yyyy-MM-dd HH:mm'))" } else { 'none (14 days)' }
$lastCrash = if ($crashEvents) { $crashEvents[0].TimeCreated }

# Does the log belong to the crash? A crash that is clearly older than the log means a newer start replaced it.
$logNote = ''
if ($log) {
    $tail = (Get-Content $log.FullName -Tail 60 -Encoding UTF8) -join "`n"
    $endsWithError = $tail -match '(STOP:|Fault:|Device lost|exception|fatal|ErrorOut|PermissionError|Traceback|Mods: \[)'
    if ($lastCrash -and -not $endsWithError -and $lastCrash -lt $log.LastWriteTime.AddMinutes(-2)) {
        $logNote = 'log is from a later start than the last crash - the crash log was replaced'
    } elseif (-not $endsWithError -and -not $lastCrash) {
        $logNote = 'no error at the end of the log'
    }
}

# --- Hints: what the report already points to ---------------------------------------------
# Rules checked against the port's README, docs\WINDOWS_TROUBLESHOOTING.md, release notes and
# launcher (1.7.3). They can still be wrong. Rank 1 = the log names the cause, 2 = a setting or
# file that is clearly off, 3 = general advice. The summary shows the three best.
$hints = New-Object System.Collections.ArrayList
function Hint([string]$text, [int]$rank = 3) { [void]$hints.Add([pscustomobject]@{ Rank = $rank; Order = $hints.Count; Text = $text }) }
function LogHas([string]$pattern) { [bool]($logText -and $logText -match $pattern) }
$newest = '1.9.1'  # newest port version; BB Helper fills in the current one when it hands out the tool
try { if ($portVersion -match '^\d' -and [version]$portVersion -lt [version]$newest) {
    Hint "Port $portVersion is older than ${newest}: update first (launcher > Update, bottom left) - newer versions fix many crashes and online problems." 1 } } catch { }
# Fixed in 1.7.3: SRT walker crash, WinError 32 at PLAY, online SSL check (release notes windows-v1.7.3).
$before173 = $false
try { $before173 = $portVersion -match '^\d' -and [version]$portVersion -lt [version]'1.7.3' } catch { }
$gpuName = "$($gameGpu.Name)"
$amd = $gpuName -match 'AMD|Radeon'
$upscaler = "$($ini['upscaler'])"

# Game files
if (LogHas 'Lance McDonald') {
    Hint "eboot.bin has Lance McDonald's 60 fps patch (the game crashes at the gestures menu): copy eboot.bin from a clean dump of the 1.09 update into the game folder. The port has its own 60 FPS." 1 }
$missingFiles = LogHas 'open\(/app0/dvdroot_ps4/(shader/gx\w+|other/systex)[^)]*\) -> not found'
if ($missingFiles) {
    Hint 'Game files are missing (log: dvdroot_ps4\shader\... not found). The game folder needs eboot.bin, sce_module, sce_sys AND the full dvdroot_ps4 (about 29 GB). Often only the 1.09 update folder was picked: copy the update into the base game folder (replace files) and pick that folder.' 1 }
if ($sfo -and $sfo['APP_VER'] -ne '01.09') {
    Hint "Game is v$($sfo['APP_VER']), not 1.09: copy the 1.09 update into the game folder. Without it the port's patches stay off (30 FPS) and online co-op does not work." 2 }
elseif (LogHas 'is not the 01\.09 build') {
    Hint "Log: eboot.bin is not the clean 1.09 build, so the port's patches stay off (30 FPS, no effects) and online co-op will not match. Copy eboot.bin from the 1.09 update into the game folder." 2 }

# Folders: cloud sync and mixed-up launcher folders
$cloud = @()
foreach ($pair in @(@('port', $port), @('game', $gameDir), @('saves', (Setting 'user_dir')))) {
    if ($pair[1] -and "$($pair[1])" -match '\\(OneDrive[^\\]*|Dropbox|Google Drive|My Drive|iCloudDrive)\\') { $cloud += $pair[0] }
}
if ($cloud) {
    Hint "The $($cloud -join ', ') folder is inside a cloud-synced folder (OneDrive, Dropbox ...): the sync locks files and moves them to the cloud while the game reads them (gray screen, freezes, broken saves). Move the folders out, e.g. to C:\Games, and pick them again in the launcher." 2 }
$userDirSetting = "$(Setting 'user_dir')"
if ($userDirSetting -and $gameDir -and $userDirSetting.TrimEnd('\') -eq "$gameDir".TrimEnd('\')) {
    Hint "The saves folder is set to the game folder: on the launcher page Game & effects, clear the Saves folder field (empty = the port's own user folder), so saves and caches stay apart from the game files." 3 }
$patchesDirSetting = "$(Setting 'patches_dir')"
if ($patchesDirSetting -and ((Test-Path (Join-Path $patchesDirSetting 'eboot.bin')) -or (Test-Path (Join-Path $patchesDirSetting 'sce_sys')))) {
    Hint "The third-party patches folder points to a game or update folder: it is only for .xml patch files. On the launcher page Mods & patches, clear the Patches folder field (empty = the port's own patches folder)." 3 }
foreach ($pair in @(@('Saves folder', 'Game & effects', "$(Setting 'user_dir')"), @('Mods folder', 'Mods & patches', "$(Setting 'mods_dir')"))) {
    # A folder inside another port install: saves, shader cache and mods of that install get mixed in.
    $other = $null; $d = $pair[2]
    for ($i = 0; $i -lt 3 -and $d; $i++) {
        if ((Test-Port $d) -and $d.TrimEnd('\') -ne "$port".TrimEnd('\')) { $other = $d; break }
        $d = Split-Path $d
    }
    if ($other) {
        Hint "The $($pair[0]) points into another port install ($(Split-Path -Leaf $other)): this port then uses that install's saves, shader cache or mods. Copy what you need into this port's folder, then clear the $($pair[0]) field on the launcher page $($pair[1]) (empty = this port's own folder)." 2 }
}

# Crashes and stops the log names
if (LogHas 'WinError 32') {
    if ($before173) {
        Hint "Launcher stopped with WinError 32: another program, usually the antivirus, holds a file in out\. Update to 1.7.3 first (launcher > Advanced > Check for updates): it waits and tries again instead of giving up. Still there after the update? Add the port folder to the exclusions of the antivirus you actually use." 1 }
    else {
        Hint 'Launcher stopped with WinError 32: another program, usually the antivirus, holds a file in out\. Add the port folder to the exclusions of the antivirus you actually use; if that is not enough, move the port folder to a local folder like C:\Games.' 1 } }
if (LogHas 'ErrorOutOfDeviceMemory') {
    Hint "Log: ErrorOutOfDeviceMemory = the graphics card ran out of memory (VRAM). Graphics: Preset Balanced or Performance, DLSS on RTX cards, Model detail 'As in the game', Object motion vectors off; close the browser. A bigger page file does NOT help here." 1 }
if (LogHas 'cannot create \d+ MiB of guest memory') {
    Hint "Log: 'cannot create ... MiB of guest memory' = the Windows page file is off or too small for the port. Win+R > sysdm.cpl > Advanced > Performance Settings > Advanced > Virtual memory > Change > tick 'Automatically manage paging file size for all drives' > OK, then restart the PC." 1 }
if (LogHas 'ErrorOutOfHostMemory') {
    Hint 'Log: ErrorOutOfHostMemory = system memory ran out: turn the page file on or make it bigger and close other programs (the port needs about 6 GB free, 10 GB at 1440p/4K).' 1 }
if (LogHas 'Device lost') {
    $text = "Log: 'Device lost' = the GPU hung and Windows reset the driver."
    $fixOn = (LogHas 'draw preparation workers: 0') -and (LogHas 'Object motion: off')
    if ($amd) { $text += ' Known fix for AMD: Advanced > Two-stage GPU pipeline Off, Extra variables BB_COPY_GPU_BUFFERS=1 BB_PREP_WORKERS=0, Graphics > Object motion vectors off, then Clear shader cache once.' }
    elseif ($fixOn) { $text += ' The known fix (Object motion vectors off, BB_PREP_WORKERS=0) is already on here: update the driver, close overlays, undo GPU overclocks and try another upscaler.' }
    else { $text += ' Try Graphics > Object motion vectors off and Advanced > Extra variables BB_PREP_WORKERS=0; also update the driver, close overlays and undo GPU overclocks.' }
    Hint $text 1
}
$beforePm4Fix = $true  # 1.7.7 fixed the 'Unimplemented PM4' crash; newer ports don't need the workaround
try { $beforePm4Fix = [version]$portVersion -lt [version]'1.7.7' } catch { }
if ((LogHas 'Unimplemented PM4') -and $beforePm4Fix) {
    Hint "Log: 'Unimplemented PM4' (crash right after a save loads): update to 1.7.7 or newer, it fixes this. Until then: Advanced > Two-stage GPU pipeline Off and Extra variables BB_COPY_GPU_BUFFERS=1." 1 }
elseif (LogHas 'Unimplemented') {
    Hint "Log: 'Unimplemented' = the game used something the port can't do yet. Note what you were doing at that moment." 2 }
if (LogHas 'Failed to create graphics pipeline') {
    Hint "Log: 'Failed to create graphics pipeline' = shader cache damaged by earlier crashes: Advanced > Clear shader cache, then start again." 1 }
$srtCrash = LogHas 'bb-probe\.exe\+0x1b5bf2'
if ($srtCrash) {
    if ($before173) {
        Hint "Log: crash at bb-probe.exe+0x1b5bf2 after 'Patched SRT walker' warnings (also the black screen at the cutscene after Rom). Fixed in 1.7.3: update the port (launcher > Advanced > Check for updates)." 1 }
    else {
        Hint "Log: crash at bb-probe.exe+0x1b5bf2 after 'Patched SRT walker' warnings, although 1.7.3 should fix it: report it with this zip and say what you were doing." 1 } }
if (-not $missingFiles -and -not $srtCrash -and (LogHas 'Fault: (write|read) access')) {
    Hint "Log shows a Fault = a crash inside the game code. Write down what you were doing; helpers match the 'guest offset 0x...' with known crashes in #bug-tracker." 1 }
if (LogHas 'Out of order flip IRQ') {
    Hint "Log: 'Out of order flip IRQ' = the port's video output got the game's frames in the wrong order and stopped. That's a port bug, not your PC: note when it happened and report it with this zip." 1 }
if (LogHas 'exception 0xc000001d') {
    $text = "Log: 'exception 0xc000001d' = illegal instruction, the CPU met an instruction it doesn't have."
    if (LogHas 'Legacy CPU translations run instead') { $text += ' The port translates those for older CPUs and missed this one: no setting fixes that, report it with this zip.' }
    else { $text += ' Report it with this zip.' }
    Hint $text 1 }
if (LogHas 'shaderBinarySize \d+ is not equal to Program size') {
    Hint "Log: the game read a damaged shader ('shaderBinarySize ... Program size'): the game files are broken, usually from extraction. Extract the base game and the 1.09 update again with PKG Extractor 1.1, search the game folder for size:empty (must find nothing), then Advanced > Clear shader cache." 1 }
if ($portOk -and (Test-Path (Join-Path $port 'scripts'))) {
    # Since 1.7 every start runs these scripts; one missing = an incomplete or mixed install.
    $missingScripts = @()
    try { if ($portVersion -match '^\d' -and [version]$portVersion -ge [version]'1.7') {
        $missingScripts = @('prepare.py', 'link_libc.py', 'link_modules.py', 'mods.py', 'patches.py', 'content_profile.py', 'cpu_compat.py') |
            Where-Object { -not (Test-Path (Join-Path $port "scripts\$_")) } } } catch { }
    if ($missingScripts) {
        Hint "Port files are missing (scripts\$($missingScripts -join ', scripts\')): the install is incomplete or mixed with an older version. Extract the newest release into a new, empty folder, copy your user folder over, and check that your antivirus doesn't remove files." 1 }
}
$clientRejected = LogHas 'Server rejected this client'
if ($clientRejected) {
    Hint "Online: the shadNet server refuses this port build ('Server rejected this client ... doesn't accept your shadPS4 build'), so every login there fails with code 5 - your account is fine. Play online through The Hunter's Dream for now: Online co-op > Co-op pool: The Hunter's Dream > Sign in with Discord." 1 }
if (-not $clientRejected -and $logText -and $logText -match 'Login rejected error code (\d+)') {
    $code = $Matches[1]
    $current = $false
    try { $current = $portVersion -match '^\d' -and [version]$portVersion -ge [version]'1.7.1' } catch { }
    $what = switch ($code) {
        '5' { if ($current) { "the shadNet server refused the login. Since 14 Oct 2026 it refuses the port in general, whatever the account: play online through The Hunter's Dream (Online co-op > Co-op pool: The Hunter's Dream > Sign in with Discord)" }
              else { "update the port (older versions are signed out by shadNet), and play online through The Hunter's Dream: the shadNet server currently refuses the port anyway" } }
        '6' { 'already signed in, often right after a disconnect: close the game completely, wait a few minutes, then try again' }
        '7' { 'unknown account: enter the shadNet account name, not the email' }
        '8' { 'wrong password: enter it again under Online co-op' }
        default { 'check the account under Online co-op' } }
    Hint "Online login error code ${code}: $what." 1 }
$onlineServer = "$(Setting 'online_server')".Trim()
if ((Setting 'online') -eq $true -and "$(Setting 'online_pool')" -ne 'huntersdream' -and $onlineServer -match '^(127\.0\.0\.1|localhost)(:|$)') {
    Hint "Online co-op: the shadNet server is set to $onlineServer - that's the local address of bbbridge (The Hunter's Dream), not shadNet. On the Online co-op page set Server address to srv.shadps4.net:31313 (or empty it) and empty WebAPI address." 2 }
if (LogHas '(cpp-httplib failed.*error=10\b|SSL server verification failed)') {
    if ($before173) {
        Hint "Online: the SSL check of the server failed ('failed to get server information'). Update to 1.7.3 (launcher > Advanced > Check for updates): it fixes this, no certmgr changes needed. Never install certificate files someone sends you." 1 }
    else {
        Hint "Online: the SSL check of the server failed ('failed to get server information'), although 1.7.3 should fix it: report it with this zip. No certmgr changes needed; never install certificate files someone sends you." 1 } }
if ($logText -and -not $endsWithError) {
    # Freeze: the last real line is the game waiting on the GPU (statistics lines go on regardless).
    $quiet = '^(Memory:|Runtime: save data|Frame stats|Frame pacing|Texture cache:|Texture helper|GPU profile|Draw preparation|Render state|texture set|scene proxies|Object motion:|\s*[\d.]+ ms/frame)'
    $lastLine = $logText -split "`r?`n" | Where-Object { $_.Trim() -and $_ -notmatch $quiet } | Select-Object -Last 1
    if ($lastLine -match 'Stall during rendering') {
        Hint 'The log ends while the game waits on the GPU (a freeze, not a crash): close RivaTuner/Afterburner and other overlays, try once with online play off, then Advanced > Two-stage GPU pipeline Off and Graphics > Object motion vectors off.' 2 }
}

# Graphics card and settings
if ((LogHas 'DLSS: unavailable \(not an NVIDIA GPU\)') -and ($gpuInfo | Where-Object { $_.Name -match 'RTX' })) {
    Hint 'The game ran on the wrong graphics chip, not on your NVIDIA card: Windows Settings > System > Display > Graphics: add Bloodborne.exe, bin\bb-probe.exe and bin\bb-gpu-capabilities.exe from the port folder and set them to High performance.' 1 }
if (($gpuName -match 'RTX') -and ($upscaler -in @('fsr4', 'fsr411', 'fsr3'))) {
    Hint "RTX card with upscaler '$upscaler': switch to DLSS (Graphics > Upscaler). On RTX cards it looks best and costs far less GPU time than FSR 4." 2 }
elseif (($upscaler -in @('fsr4', 'fsr411')) -and $gpuName -and ($gpuName -notmatch 'RX (7|9)\d{3}')) {
    Hint "FSR 4 is very slow on $gpuName (RX 6000: about 6 FPS at 4K): use FSR 3 (Graphics > Upscaler)." 2 }
$vramGb = if ($gameGpu.Vram) { [math]::Round($gameGpu.Vram / 1GB) } else { 0 }
if ($vramGb -gt 0 -and $vramGb -le 4) {
    Hint "Only $vramGb GB VRAM on ${gpuName}: below what the port handles well. Output 1920x1080, Preset Performance, Model detail 'As in the game', Object motion vectors off, close the browser; big areas can still crash." 2 }
elseif ($vramGb -gt 4 -and $vramGb -le 6) {
    Hint "Only $vramGb GB VRAM on ${gpuName}: output 1920x1080 or Preset Balanced/Performance, Model detail not 'Highest (-2)', close the browser." 2 }
$diag = @()
if ((Setting 'frame_stats') -eq $true -or (LogHas '(?m)^Frame stats:')) { $diag += 'Frame statistics' }
if ((Setting 'gpu_profile') -eq $true -or (LogHas '(?m)^GPU profile:')) { $diag += 'GPU time per pass' }
if ((Setting 'vk_validation') -eq $true -or (LogHas 'VK_LAYER_KHRONOS_validation')) { $diag += 'Vulkan validation layers' }
if ($diag) { Hint "Diagnostics options are on ($($diag -join ', ')): turn them off under Advanced > Diagnostics, they slow the game down." 2 }
if ($overlays) { Hint "Overlay running ($($overlays -join ', ')): close it and try again, overlays often break Vulkan games." 2 }
if ($crashEvents) {
    $module = "$($crashEvents[0].Properties[3].Value)"
    if ($module -match '^(amdvlk64|amdxx64|atiu|nvoglv64|nvwgf2umx|nvgpucomp64|vulkan-1|igvk64)') { Hint "Windows: the last crash happened in the graphics driver ($module). Update or clean-reinstall the GPU driver." 2 }
}
if ($gpuResets) { Hint "Windows recorded $($gpuResets.Count) GPU driver reset(s) in 14 days: the GPU hung. Update the driver and undo GPU/RAM overclocks; if the log says 'Device lost', see that hint." 2 }
if ((Setting 'present_mode') -eq 'FifoRelaxed' -or (LogHas 'Unknown present mode FifoRelaxed')) {
    Hint "Present mode 'FIFO Relaxed' is not supported by the game and falls back to Mailbox: pick FIFO (VSync) or Mailbox in the launcher." 3 }
if ($gameGpu.Date -and $gameGpu.Date -lt (Get-Date).AddDays(-180)) {
    Hint "GPU driver for $gpuName is from $($gameGpu.Date.ToString('yyyy-MM-dd')): update it (black screens and crashes are often driver issues)." 3 }

# Mods and third-party patches
$fatDrive = $gameFs -in @('exFAT', 'FAT32', 'FAT')
if (LogHas 'Mods: \[WinError 1\]') {
    Hint "Mods can't load (log: WinError 1 Incorrect function): the port links the game's folders for mods, and that only works on NTFS drives$(if ($fatDrive) { " (your game folder is on $gameFs)" } else { '; network, USB and cloud drives often fail too' }). Move the game folder, best with the port folder, to an NTFS drive such as C:. Without mods the game runs where it is." 1 }
elseif ($fatDrive -and $activeMods) {
    Hint "Your game folder is on a $gameFs drive, but mods need NTFS (the port links folders). Move the game folder to an NTFS drive such as C:, or play without mods." 2 }
if (LogHas 'Mods: 0 game files replaced') {
    Hint "Log: your mods replaced no game file: check each mod's folders (unpacked, mods\<name>\dvdroot_ps4\... or mods\<name>\chr\...)." 2 }
elseif ($activeMods) { Hint "$activeMods mod(s) on: test once without mods (launcher > Mods & patches)." 3 }
if ($extPatches -match 'Debug Dash') {
    Hint "Third-party patch 'Restore Debug Dash' is on: L3 switches on a fly/fast-move mode (L2 up, L1 down, R2 noclip). Turn the patch off in Mods & patches if your character floats or moves too fast." 2 }
if ($extPatches) {
    Hint "$($extPatches.Count) third-party patch(es) on: test once without them (Mods & patches). The port has its own FPS, resolution and effect options; doubling them can break things." 3 }

# PC
if ($avx -eq 'NO AVX') { Hint 'CPU has no AVX (many Pentium and Celeron models): the port cannot run on it.' 1 }
elseif (LogHas 'Legacy CPU translations run instead') {
    Hint 'Log: this CPU lacks BMI1/MOVBE/LZCNT (e.g. Intel 2nd/3rd gen), so the port runs it with translated instructions - best effort, the log itself says some can still be wrong. Crashes or graphics errors that no setting fixes can come from that: report them with the log.' 2 }
elseif ($avx -eq 'AVX yes, no AVX2') { Hint 'Older CPU without AVX2 (e.g. Intel 2nd/3rd gen): the port runs it with translated instructions, best effort.' 3 }
$commitGb = $os.TotalVirtualMemorySize * 1KB / 1GB
if ($pageText -like 'NONE*' -and $cs.TotalPhysicalMemory -lt 32GB) {
    Hint 'Page file is off: turn it on (Windows: Advanced system settings > Performance > Virtual memory). The port needs about 6 GB free memory, 10 GB at 1440p/4K.' 3 }
elseif ($commitGb -lt 20) { Hint ("Only {0:N0} GB memory incl. page file: a bigger page file helps against crashes after a while." -f $commitGb) 3 }
$cap = "$(Setting 'frame_cap')"
if ($cap -match '^\d+$' -and [int]$cap -gt 120) { Hint "FPS limit ${cap}: keep it at 120 or lower (the game's timing above 120 FPS isn't checked everywhere)." 3 }
foreach ($drive in $drives.Keys) {
    $disk = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$drive'"
    if ($disk -and $disk.FreeSpace -lt 10GB) { Hint "Only $(GB $disk.FreeSpace) free on drive $drive - the shader cache and saves need room." 3 }
}
if ($logNote -like 'log is from a later start*') { Hint 'The crash is no longer in last_run.log (the game was started again). Make it crash again and run this tool right after.' 1 }
$hintList = @($hints | Sort-Object Rank, Order | ForEach-Object { $_.Text })

# --- Summary for the clipboard (fits into one Discord message) ----------------------------
$summary = @(
    "BB Report Tool $ToolVersion | $(Get-Date -Format 'yyyy-MM-dd HH:mm')"
    "Port version : $portVersion"
    "Game         : $game"
    "OS           : $osLine"
    "CPU          : $cpuLine"
    "RAM          : $ramLine"
) + ($gpuLines | ForEach-Object { "GPU          : $_" }) + @(
    "Display      : $($displayLines -join ' | ')"
    "Disk         : $($space -join ' | ')"
    "Settings     : $settingsLine"
    "Mods         : $modsLine"
    "Ext. patches : $patchLine"
    "Overlays     : $overlayLine"
    "Log          : $logLine$(if ($logNote) { " ($logNote)" })"
    "Last error   : $errorLine"
    "Win crashes  : $crashLine"
    "GPU resets   : $gpuLine"
)
# Discord allows 2000 characters per message (longer pastes become message.txt, which the helpers'
# bot rejects): show fewer hints, then shorten the long lines, until the summary fits.
function Build-Summary([int]$shown, [int]$cut) {
    $lines = @($summary | ForEach-Object { if ($cut -and $_.Length -gt $cut) { $_.Substring(0, $cut - 3) + '...' } else { $_ } })
    if ($hintList.Count) {
        $lines += @($hintList | Select-Object -First $shown | ForEach-Object { $h = "Hint         : $_"; if ($cut -and $h.Length -gt $cut) { $h.Substring(0, $cut - 3) + '...' } else { $h } })
        if ($hintList.Count -gt $shown) { $lines += "               (+$($hintList.Count - $shown) more in hints.txt)" }
    } else { $lines += 'Hints        : nothing obvious - the helpers will read the log' }
    Hide-Private ("``````" + "`r`n" + ($lines -join "`r`n") + "`r`n" + "``````")
}
$summaryText = $null
foreach ($try in @(@(3, 0), @(2, 0), @(1, 0), @(1, 200), @(1, 140), @(1, 100))) {
    $summaryText = Build-Summary $try[0] $try[1]
    if ($summaryText.Length -le 1990) { break }
}
if ($summaryText.Length -gt 1990) { $summaryText = $summaryText.Substring(0, 1983) + "`r`n" + '```' }
$summary = @($summaryText -split "`r`n" | Where-Object { $_ -ne '```' })

# --- Zip: summary, scrubbed log, port settings (never settings.json secrets) --------------
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'  # with seconds: a second run never replaces the first zip
$stage = Join-Path $env:TEMP "BB-Report-$stamp"
New-Item -ItemType Directory -Force $stage | Out-Null
Set-Content (Join-Path $stage 'report.txt') $summaryText -Encoding UTF8
if ($fullLogText) { Set-Content (Join-Path $stage 'last_run.log') (Hide-Private $fullLogText) -Encoding UTF8 }
$eventText = @('Crashes and hangs of the port (Windows Application log, 14 days):') +
    $(if ($crashLines) { $crashLines } else { '  none' }) + @('', 'GPU driver resets (Windows System log, Display 4101, 14 days):') +
    $(if ($gpuResets) { $gpuResets | Select-Object -First 8 | ForEach-Object { "$($_.TimeCreated.ToString('yyyy-MM-dd HH:mm'))  $($_.Message -replace '\s+', ' ')" } } else { '  none' })
Set-Content (Join-Path $stage 'windows-crashes.txt') (Hide-Private ($eventText -join "`r`n")) -Encoding UTF8
$hintText = @("Hints for the helper (found automatically by BB Report Tool $ToolVersion - they can be wrong):", '') +
    $(if ($hintList.Count) { $hintList | ForEach-Object { "- $_" } } else { '- nothing obvious, see last_run.log' })
Set-Content (Join-Path $stage 'hints.txt') (Hide-Private ($hintText -join "`r`n")) -Encoding UTF8
if ($portOk) {
    foreach ($name in 'bbport.ini', 'input.ini', 'mods.json', 'patches.json') {
        $file = Join-Path $port $name
        if (Test-Path $file) { Set-Content (Join-Path $stage $name) (Hide-Private (Get-Content $file -Raw)) -Encoding UTF8 }
    }
}
if ($settings) {
    # Only known, harmless keys: new launcher settings (e.g. accounts) are left out by default.
    $safeKeys = 'ui_language', 'game_dir', 'user_dir', 'dlc_dir', 'mods_dir', 'mods_enabled', 'patches_dir', 'language',
        'voice_language', 'fullscreen', 'hdr', 'present_mode', 'fps_mode', 'frame_cap', 'draw_pipe', 'readbacks',
        'frames_ahead', 'frame_stats', 'gpu_profile', 'vk_validation', 'extra_env', 'close_on_play', 'check_updates',
        'online', 'online_server', 'online_upnp', 'online_community', 'online_pool', 'bridge_live_map'
    $safe = [ordered]@{}
    foreach ($k in $safeKeys) { if ($settings.PSObject.Properties.Name -contains $k) { $safe[$k] = $settings.$k } }
    Set-Content (Join-Path $stage 'launcher-settings.json') (Hide-Private ($safe | ConvertTo-Json)) -Encoding UTF8
}
$outDir = $env:BBTOOL_OUT
if (-not $outDir) { $outDir = [Environment]::GetFolderPath('Desktop') }
$zip = Join-Path $outDir "BB-Report-$stamp.zip"
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
Remove-Item $stage -Recurse -Force

if (-not $env:BBTOOL_OUT) { Set-Clipboard -Value $summaryText }  # tests (BBTOOL_OUT) keep the clipboard

Write-Host ''
Write-Host ($summary -join "`n") -ForegroundColor Gray
Write-Host ''
if (-not $portOk) { Write-Host '  Port folder not found: the report has no port settings and no log.' -ForegroundColor Red }
if ($logNote -like 'log is from a later start*') {
    Write-Host '  Your last crash is not in last_run.log anymore: the game was started again after it.' -ForegroundColor Yellow
    Write-Host '  The crash info from Windows is in the report. Best: make it crash again, then run this tool right away.' -ForegroundColor Yellow
}
Write-Host '  Done!' -ForegroundColor Green
Write-Host '  1. The summary is in your clipboard: paste it into your #help post (Ctrl+V).'
Write-Host "  2. Attach this file to the post: $zip"
if (-not $env:BBTOOL_OUT) { Start-Process explorer.exe "/select,`"$zip`"" }
