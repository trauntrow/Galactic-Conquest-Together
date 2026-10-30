param([Parameter(Mandatory)][string]$Output)
# Builds screen_observer.inc: the capture, ready-check and screen observer scripts as one C++ string.
$ErrorActionPreference = 'Stop'
$lua = Join-Path (Split-Path -Parent $PSScriptRoot) 'src\lua'
$script = [IO.File]::ReadAllText((Join-Path $lua 'screen_observer.lua'))
$script = [IO.File]::ReadAllText((Join-Path $lua 'player_hold.lua')) + "`n" + $script
$script = "local capture_version = 7`n" + [IO.File]::ReadAllText((Join-Path $lua 'campaign_capture.lua')) + "`n" + $script
if ($script.Contains(')GCTOBS"')) { throw 'C++ raw literal delimiter occurs in script' }
$header = '// Generated from src/lua/screen_observer.lua; do not edit.' + "`n" + 'constexpr char query[] = R"GCTOBS(' + $script + ')GCTOBS";' + "`n"
[IO.File]::WriteAllText([IO.Path]::GetFullPath($Output), $header, [Text.Encoding]::ASCII)
