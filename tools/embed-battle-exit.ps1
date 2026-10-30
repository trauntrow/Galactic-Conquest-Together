param([Parameter(Mandatory)][string]$Output)
# Builds battle_exit.inc from src/lua/battle_exit.lua, which runs in each shared battle's Lua state.
$ErrorActionPreference = 'Stop'
$script = [IO.File]::ReadAllText((Join-Path (Split-Path -Parent $PSScriptRoot) 'src\lua\battle_exit.lua')).Replace("`r`n", "`n")
if ($script.Contains(')GCTEXIT"')) { throw 'C++ raw literal delimiter occurs in script' }
$header = '// Generated from src/lua/battle_exit.lua; do not edit.' + "`n" + 'constexpr char battle_exit_query[] = R"GCTEXIT(' + $script + ')GCTEXIT";' + "`n"
[IO.File]::WriteAllText([IO.Path]::GetFullPath($Output), $header, [Text.Encoding]::ASCII)
