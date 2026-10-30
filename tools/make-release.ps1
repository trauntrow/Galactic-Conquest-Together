param([string]$SourceUrl = '')
# Checks out\Release\dinput8.dll and packages it with README.txt and LICENSE.txt into
# out\package\GalacticConquestTogether-<version>.zip. Anything that isn't a clean release build is refused.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $root 'out\Release\dinput8.dll'
if (!(Test-Path -LiteralPath $dll)) { throw "No $dll; run build.cmd release." }

$versionText = Get-Content -LiteralPath (Join-Path $root 'src\version.h') -Raw
if ($versionText -notmatch '#define GC_VERSION "([0-9]+\.[0-9]+\.[0-9]+)"') { throw 'GC_VERSION not found in src\version.h' }
$version = $Matches[1]
$profileText = Get-Content -LiteralPath (Join-Path $root 'src\profile_data.h') -Raw
if ($profileText -notmatch 'expected_sha256\[\] = "([0-9a-f]{64})"') { throw 'expected_sha256 not found in src\profile_data.h' }
$gameSha = $Matches[1]
if (!$SourceUrl) {
    $SourceUrl = (& git -C $root remote get-url origin) -replace '\.git$', ''
    if ($LASTEXITCODE -ne 0 -or !$SourceUrl) { throw 'No git remote; pass -SourceUrl.' }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$dumpbin = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'VC\Tools\MSVC\**\bin\Hostx64\x86\dumpbin.exe' | Select-Object -First 1
if (!$dumpbin) { throw 'dumpbin.exe not found (Visual Studio C++ x86 tools).' }

# The DLL has to be an x86 release build without debug info.
$problems = New-Object System.Collections.Generic.List[string]
$headers = & $dumpbin /nologo /headers $dll | Out-String
if ($headers -notmatch '14C machine \(x86\)') { $problems.Add('not an x86 image') }
if ($headers -notmatch '(?m)^\s+DLL\s*$') { $problems.Add('not a DLL') }
if ($headers -match '(?m)^\s+[0-9A-F]{8} cv\s' -or $headers -match 'Format: RSDS') { $problems.Add('has a CodeView debug entry (built with /Zi or /DEBUG)') }
$imports = & $dumpbin /nologo /imports $dll | Out-String
$dlls = @([regex]::Matches($imports, '(?m)^ {4}(\S+\.dll)\s*$') | ForEach-Object { $_.Groups[1].Value.ToLowerInvariant() } | Sort-Object -Unique)
# kernel32 only: anything else could load another DLL while the game is still loading its own.
if (($dlls -join ',') -ne 'kernel32.dll') { $problems.Add("imports $($dlls -join ', ') (expected kernel32.dll)") }
# No exception handlers, debugger or remote-thread APIs.
foreach ($name in 'AddVectoredExceptionHandler','AddVectoredContinueHandler','RemoveVectoredExceptionHandler','DebugActiveProcess','CreateRemoteThread','VirtualAllocEx') {
    if ($imports -match "\b$name\b") { $problems.Add("imports $name") }
}
$bytes = [IO.File]::ReadAllBytes($dll)
$ascii = [Text.Encoding]::GetEncoding(28591).GetString($bytes)
# Text only the debug build has, and this checkout's own path.
foreach ($marker in 'GCT debug build','AV ip=','exception observer','STATE flow=','CAPTURE_ALIVE',
                    'FRAME_STATE','FRAME_PROBE_ALIVE','GalaxyFactory','Galaxy GetInstance','STEAM_RELAY host listening',
                    'STEAM_RELAY_STREAM_OPEN','STEAM_OFFER_LOCAL',
                    'STEAM_INVITE request=','.pdb',$root) {
    if ($ascii.Contains($marker)) { $problems.Add("contains developer text '$marker'") }
}
# Text the release build has to have.
foreach ($marker in "GCT Galactic Conquest Together %s release",$version,'PLAYER_LOAD loaded_with_game=','PROFILE %s',
                    'IAT observation','FRAME_PROBE_RUNNING','STEAM_INVITE_READY','PLAYER_SHELL_READY') {
    if (!$ascii.Contains($marker)) { $problems.Add("lacks release text '$marker'") }
}
$info = (Get-Item -LiteralPath $dll).VersionInfo
if ($info.ProductVersion -ne $version -or $info.ProductName -ne 'Galactic Conquest Together') {
    $problems.Add("version resource reads '$($info.ProductName) $($info.ProductVersion)'")
}
if ($problems.Count) {
    $problems | ForEach-Object { Write-Output "PROBLEM: $_" }
    throw "$dll is not a release build; nothing was packaged."
}
$dllSha = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "Release checks passed: x86 DLL, no debug entry, imports $($dlls -join ' + '), no developer text, version $version."

# README.txt, filled in from the template.
$minhook = (Get-Content -LiteralPath (Join-Path $root 'third_party\minhook\LICENSE.txt') -Raw).TrimStart([char]0xFEFF).TrimEnd()
$readme = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'release-readme.txt') -Raw
$readme = $readme.Replace('{VERSION}', $version).Replace('{DLL_SHA256}', $dllSha).Replace('{GAME_SHA256}', $gameSha)
$readme = $readme.Replace('{SOURCE_URL}', $SourceUrl).Replace('{MINHOOK_LICENSE}', $minhook)
if ($readme -match '\{[A-Z0-9_]+\}') { throw "README placeholder left: $($Matches[0])" }
if ($readme -match '[^\x09\x0A\x0D\x20-\x7E]') { throw 'README is not plain ASCII.' }
$readme = ($readme.TrimEnd() -replace "`r?`n", "`r`n") + "`r`n"
$readmeBytes = [Text.Encoding]::ASCII.GetBytes($readme)

# LICENSE.txt: the project's GPL text, with Windows line endings like the README.
$licenseText = Get-Content -LiteralPath (Join-Path $root 'LICENSE') -Raw
if ($licenseText -match '[^\x09\x0A\x0C\x0D\x20-\x7E]') { throw 'LICENSE is not plain ASCII.' }
$licenseText = ($licenseText.TrimEnd() -replace "`r?`n", "`r`n") + "`r`n"
$licenseBytes = [Text.Encoding]::ASCII.GetBytes($licenseText)

# The zip holds exactly dinput8.dll, README.txt and LICENSE.txt.
$outDir = Join-Path $root 'out\package'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$zip = Join-Path $outDir "GalacticConquestTogether-$version.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }
# Entry times come from the HEAD commit, so the same commit packs the same zip.
$commitTime = & git -C $root log -1 --format=%ct
if ($LASTEXITCODE -ne 0 -or $commitTime -notmatch '^\d+$') { throw 'Could not read the HEAD commit time.' }
$stamp = [DateTimeOffset]::FromUnixTimeSeconds([long]$commitTime)
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($entryFile in @(@('dinput8.dll', [IO.File]::ReadAllBytes($dll)), @('README.txt', $readmeBytes),
                             @('LICENSE.txt', $licenseBytes))) {
        $entry = $archive.CreateEntry($entryFile[0], [IO.Compression.CompressionLevel]::Optimal)
        $entry.LastWriteTime = $stamp
        $stream = $entry.Open()
        try { $stream.Write($entryFile[1], 0, $entryFile[1].Length) } finally { $stream.Dispose() }
    }
} finally { $archive.Dispose() }

# Read the zip back and make sure the DLL inside is the one that was checked.
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    $names = @($archive.Entries | ForEach-Object { $_.FullName } | Sort-Object)
    if (($names -join ',') -ne 'dinput8.dll,LICENSE.txt,README.txt') { throw "Zip holds $($names -join ', ')" }
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = $archive.GetEntry('dinput8.dll').Open()
    try { $packed = -join ($sha.ComputeHash($stream) | ForEach-Object { $_.ToString('x2') }) } finally { $stream.Dispose() }
    if ($packed -ne $dllSha) { throw 'The zipped dinput8.dll differs from the checked one.' }
} finally { $archive.Dispose() }
$zipSha = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
$lines = ($readme -split "`r`n").Count
Write-Output "Package: $zip ($((Get-Item -LiteralPath $zip).Length) bytes)"
Write-Output "  dinput8.dll $version  SHA-256 $dllSha"
Write-Output "  README.txt  $lines lines"
Write-Output "  LICENSE.txt $(($licenseText -split "`r`n").Count) lines"
Write-Output "  zip         SHA-256 $zipSha"
