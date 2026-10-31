# Galactic Conquest Together

Online co-op and versus Galactic Conquest for Star Wars Battlefront II (2005) on Steam.

Up to 4 player co-op. Everyone sees the galaxy live, and you fight every battle together. There's also a Versus
mode where two players take opposite sides.

It's a single `dinput8.dll` you drop into the game folder. No game files get changed.

## What you need

- Star Wars Battlefront II (Classic, 2005) on Steam, fully updated
- [Unofficial Patch 1.3 (r130)](https://www.moddb.com/games/star-wars-battlefront-ii/downloads/unofficial-v13-patch-r130)
- The same version of this mod for everyone playing
- Friends on your Steam friends list, and the Steam overlay turned on for the host

## Install

1. Grab the latest zip from [Releases](https://github.com/trauntrow/Galactic-Conquest-Together/releases).
2. In Steam, right-click Star Wars Battlefront II > Manage > Browse local files, then open `GameData`.
3. Copy `dinput8.dll` into `GameData`.
4. Start the game like normal.

To uninstall, delete `dinput8.dll` and the `gct` folder from `GameData`.

Also available on ModDB and Nexus:

https://www.nexusmods.com/battlefront2/mods/1315

https://www.moddb.com/mods/galactic-conquest-together

## Playing

**Host:** Single Player > Galactic Conquest > Online, pick Co-op or Versus, then invite your friends through the
Steam overlay.

**Friends:** wait on the main menu and accept the invite. The galaxy loads by itself once the host starts.

The README.txt in the zip covers the rest: Versus rules, settings and what to do if something goes wrong. If you
run into a bug, open an issue and attach the newest log from `GameData\gct\logs`.

## Building

You need Windows and Visual Studio 2022 or newer with the C++ desktop workload (x86 tools).

```
build.cmd            debug build in out\Debug, runs all the tests
build.cmd release    release build in out\Release, plus the zip in out\package
```

## Credits

- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu for function hooking (BSD 2-Clause)
- [Lua 5.0.2](https://www.lua.org/) for running the script tests (MIT)

## License

Copyright (C) 2026 trauntrow. Licensed under the GNU General Public License v3.0 or any later version, see
[LICENSE](LICENSE). MinHook and Lua keep their own licenses, included in `third_party`.

This is a fan project. It isn't affiliated with or endorsed by Electronic Arts, Lucasfilm, Disney or Valve.
