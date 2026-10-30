param([Parameter(Mandatory)][string]$Output)
# Builds player_menu.inc from src/lua/player_menu.lua, the Online entry in the Galactic Conquest menu.
$ErrorActionPreference = 'Stop'
$script = [IO.File]::ReadAllText((Join-Path (Split-Path -Parent $PSScriptRoot) 'src\lua\player_menu.lua')).Replace("`r`n", "`n")
if ($script.Contains(')GCTMENU"')) { throw 'C++ raw literal delimiter occurs in script' }
$header = '// Generated from src/lua/player_menu.lua; do not edit.' + "`n" + 'constexpr char player_menu_query[] = R"GCTMENU(' + $script + ')GCTMENU";' + "`n"
[IO.File]::WriteAllText([IO.Path]::GetFullPath($Output), $header, [Text.Encoding]::ASCII)
