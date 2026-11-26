# Flash a UniXNU payload image into LUN0 "preload" through EDL (Sahara programmer + Firehose).
#
# Why EDL and not fastboot: the tablet is normally flashed with `fastboot flash preload`
# (00-先读我.md §3.3), but it sits in EDL (VID_05C6&PID_9008, COM14) between boots.  The same two
# tools the log readbacks already use drive the write:
#   qsaharaserver.exe -p \\.\COMxx -s 13:<unlock>\Huawei865870_devprg.elf   (loads the programmer)
#   fh_loader.exe --port=\\.\COMxx --sendxml=<xml> --search_path=<dir> ...  (writes the image)
#
# Geometry, all from evidence, none typed from memory:
#   preload is LUN0 sectors 2375168..2733567 (07-证据/lun0-partitions.txt:39);
#   LUN0 sectors are 4096 bytes, which is why 358400 sectors = 0x57800000 = 1400 MiB
#   (board.toml:282) and the 208 MiB payload is 53248 sectors.
# The partition is never guessed: the XML carries label="preload" as well as the sector.
#
# Safety: writes only the first `num_partition_sectors` of `preload`.  `userdata` is never
# addressed.  Pass -Go to actually write; without it this is a dry run that only inspects.

param(
    [Parameter(Mandatory = $true)][string]$Image,
    [string]$ExpectSha = "",
    [string]$Port = "COM14",
    [string]$ToolsDir = "",
    [switch]$SkipSahara,
    [switch]$Go
)

$ErrorActionPreference = "Stop"

# The toolbox directory name is non-ASCII; locate the tools by search, never by literal.
# Several copies of fh_loader.exe exist under Desktop (other tool folders); only the pair that
# ships beside qsaharaserver.exe is the one whose programmer protocol matches the readbacks.
# -ToolsDir is an override, not a preference: when it yields a pair, the author's Desktop is not
# consulted at all. That is what makes the script usable on a machine that never had that Desktop.
$findPairs = {
    param([string]$root)
    @(Get-ChildItem -Path $root -Recurse -Filter fh_loader.exe -ErrorAction SilentlyContinue |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.DirectoryName "qsaharaserver.exe") })
}
$pairs = @()
if ($ToolsDir -ne "" -and (Test-Path -LiteralPath $ToolsDir)) {
    $pairs = & $findPairs $ToolsDir
}
if ($pairs.Count -eq 0 -and (Test-Path -LiteralPath "C:\Users\HW\Desktop")) {
    $pairs = & $findPairs "C:\Users\HW\Desktop"
}
if ($pairs.Count -eq 0) {
    $searched = @($ToolsDir, "C:\Users\HW\Desktop") | Where-Object { $_ -ne "" }
    throw "no directory holds both fh_loader.exe and qsaharaserver.exe (searched: $($searched -join ', '); pass -ToolsDir <dir> to point at your toolbox)"
}
$fh = $pairs | Where-Object { $p = Join-Path (Split-Path $_.DirectoryName -Parent) "unlock"; Test-Path -LiteralPath $p } |
    Select-Object -First 1
if (-not $fh) { $fh = $pairs | Select-Object -First 1 }
$tools = $fh.DirectoryName
$sahara = Join-Path $tools "qsaharaserver.exe"
$programmer = Get-ChildItem -Path (Split-Path $tools -Parent) -Recurse -Filter "Huawei865870_devprg.elf" -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $programmer) { throw "Huawei865870_devprg.elf (SM8250/kona programmer) not found in the toolbox" }

$Image = (Resolve-Path -LiteralPath $Image).Path
$imageDir = Split-Path $Image -Parent
$imageName = Split-Path $Image -Leaf
$length = (Get-Item -LiteralPath $Image).Length
if ($length % 4096 -ne 0) { throw "image is not a whole number of 4096-byte sectors: $length" }
$sectors = [int64]($length / 4096)

# preload: first_lba 2375168, last_lba 2733567 (07-证据/lun0-partitions.txt:39)
$preloadFirst = [int64]2375168
$preloadLast = [int64]2733567
if ($preloadFirst + $sectors - 1 -gt $preloadLast) {
    throw "image ($sectors sectors) does not fit preload ($($preloadLast - $preloadFirst + 1) sectors)"
}

Write-Host "fh_loader  : $($fh.FullName)"
Write-Host "programmer : $($programmer.FullName)"
Write-Host "image      : $Image"
Write-Host "bytes      : $length ($sectors sectors of 4096)"
Write-Host "target     : LUN0 preload start_sector=$preloadFirst num_partition_sectors=$sectors"

$hash = (Get-FileHash -LiteralPath $Image -Algorithm SHA256).Hash.ToLower()
Write-Host "sha256     : $hash"
if ($ExpectSha -ne "" -and $hash -ne $ExpectSha.ToLower()) {
    throw "image hash differs from the approved one (expected $ExpectSha)"
}

$dev = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match "QDLoader 9008 \($Port\)" }
if (-not $dev) { throw "no QDLoader 9008 on $Port; put the tablet in EDL first" }
Write-Host "device     : $($dev.Name)"

# The XML is generated per flash so the filename can never drift from the image being written.
$xmlPath = Join-Path $PSScriptRoot "edl\program-preload.xml"
$xml = '<?xml version="1.0" ?>' + "`n" +
       '<data>' + "`n" +
       ('  <program SECTOR_SIZE_IN_BYTES="4096" filename="{0}" label="preload" ' -f $imageName) + "`n" +
       ('           physical_partition_number="0" start_sector="{0}" num_partition_sectors="{1}" />' -f $preloadFirst, $sectors) + "`n" +
       '</data>' + "`n"
[System.IO.File]::WriteAllText($xmlPath, $xml, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "xml        : $xmlPath"
Write-Host $xml

if (-not $Go) { Write-Host "dry run (pass -Go to load the programmer and write)"; return }

Write-Host "== sahara: loading the programmer =="
if ($SkipSahara) {
    # A readback leaves the programmer running, so the port answers Firehose, not Sahara.  Sending
    # Sahara again fails with "FIREHOSE MODE DETECTED" (seen 2026-11-11 11:51); in that state the
    # existing programmer is exactly what fh_loader needs.
    Write-Host "skipped (-SkipSahara): the port is already in Firehose mode"
} else {
    & $sahara -p "\\.\$Port" -s "13:$($programmer.FullName)" 2>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "qsaharaserver failed with $LASTEXITCODE (if it said FIREHOSE MODE DETECTED, re-run with -SkipSahara)" }
}

Write-Host "== firehose: writing preload =="
Push-Location $tools
try {
    & $fh.FullName "--port=\\.\$Port" "--sendxml=$xmlPath" "--search_path=$imageDir" `
        "--noprompt" "--zlpawarehost=1" "--memoryname=ufs" "--loglevel=info" 2>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "fh_loader failed with $LASTEXITCODE" }
}
finally {
    Pop-Location
}

Write-Host "== power: reset =="
$resetXml = Join-Path $PSScriptRoot "edl\power-reset.xml"
[System.IO.File]::WriteAllText($resetXml, '<?xml version="1.0" ?><data><power DelayInSeconds="0" value="reset" /></data>' + "`n",
    (New-Object System.Text.UTF8Encoding($false)))
Push-Location $tools
try {
    & $fh.FullName "--port=\\.\$Port" "--sendxml=$resetXml" "--noprompt" "--memoryname=ufs" "--loglevel=info" 2>&1 | Out-Host
}
finally {
    Pop-Location
}
Write-Host "done; the tablet should now boot the new payload"
