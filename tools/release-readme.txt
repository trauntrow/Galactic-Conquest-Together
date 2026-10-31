Galactic Conquest Together {VERSION}
Galactic Conquest for up to four on Star Wars Battlefront II (2005, Steam)
==============================================================================

Co-op: one player hosts the campaign and up to three friends join. Everyone
sees the same galaxy live: moves, purchases, the AI's turn and the host's
camera. Only the host moves fleets, buys and loads; friends watch the
galaxy, and you all fight each battle together.
Versus: two of you play against each other, one side each, each on your own
PC (see VERSUS below).

WHAT YOU NEED
- Every player: Star Wars Battlefront II (Classic, 2005) from Steam, fully
  updated, with Unofficial Patch 1.3 (r130):
  https://www.moddb.com/games/star-wars-battlefront-ii/downloads/unofficial-v13-patch-r130
- Everyone uses this same dinput8.dll. Update together: a game with an older
  or newer dinput8.dll can't join.
- Each friend is the host's friend on Steam.
- The host has the Steam overlay on (Steam > Settings > In Game). It's on by
  default.

INSTALL (every player)
1. Close the game.
2. In Steam, right-click Star Wars Battlefront II > Manage > Browse local
   files, then open the GameData folder.
3. Copy dinput8.dll from this zip into GameData. If a file with that name is
   already there, another mod is using it and the two can't be used
   together.
4. Start the game from Steam like normal.

PLAY TOGETHER
Friends: start the game and go to the main menu. Accept the host's invite in
  the Steam overlay (Shift+Tab) or in Steam chat; if your game is closed,
  accepting starts it. A message says you've joined and how many players are
  in so far. Stay on the menu: the host's galaxy loads by itself once they
  start.
Host: Single Player > Galactic Conquest > Online > Co-op. Pick a side for a
  new campaign, or Load to continue one. Your party screen opens with the
  Steam overlay: invite up to three friends (Invite friends opens the
  overlay again). Each friend's row says "joining...", then "ready" once
  their game has paired with yours. Press Start when the friends you want
  are ready. The galaxy waits a moment for their games to load it, then you
  play as normal.

Friends can join later too: while the party has room, the host's pause menu
(Esc) has Invite friends. A friend who accepts loads the galaxy while the
host keeps playing, and the host's galaxy says "<name> joined the co-op
game.". A friend who joins during a battle waits in the galaxy for the next
one.

A co-op game lasts as long as the host's galaxy. When the host saves and
quits (or drops), every friend's game goes back to the menus. When a friend
leaves (or drops), the others keep playing and the host's galaxy says
"<name> left the co-op game."; when the last friend leaves, the host's game
offers to save, then goes back to the menus. Back on the party screen ends
the game for every friend. To play again, the host picks Online > Co-op and
sends new invites.

VERSUS
Host: Single Player > Galactic Conquest > Online > Versus.
- New game: the custom setup (era, layout, how to win), then invite your
  friend in the Steam overlay. Both of you pick a side and press Ready; you
  can't both be ready on the same side. The host's game starts the galaxy
  and the friend's loads it.
- Load: a Versus game you saved. The save is on the PC that saved it, so
  that player hosts the load. You both get your sides back, and the side
  whose turn it was goes on.
In the galaxy:
- The side whose turn it is plays while the other player watches it happen.
  Your turn starts when your opponent's ends. While it isn't your turn you
  can look around; anything else just plays the error sound.
- In a battle the attacker's game hosts and the defender's joins, each of
  you on your own side. Each side picks its own bonus card on its own PC;
  Esc on the attacker's waiting card screen lets the AI pick for the
  defender.
- Quitting a battle from its pause menu (Quit or Exit to Windows) loses it
  for your side.
- Either of you can save from the pause menu at any time; load from
  Online > Versus > Load.
- When either of you leaves (saves and quits, exits to Windows, or drops),
  the other's game offers to save, then goes back to the menus. Once the
  game is over, it just ends.

BATTLES
In co-op, when the host starts a battle, every friend's game joins it
("Joining your friend's battle..."). You all play on the host's side from the
start: no team balancing, no warm-up, and nobody waits to respawn, not even
the AI. The battle is private: it runs over your Steam connection and never
shows up in a server browser. Afterwards every game goes back to the galaxy,
which shows the result.
- Friend: Back on "Joining your friend's battle..." keeps you in the galaxy,
  and the others fight without you.
- If a friend's game can't join, or leaves mid-battle, the others keep going
  with the AI, and a short note in the host's galaxy says who afterwards.
- If the host quits the battle, every friend's game leaves it too.
To fight battles alone, the host sets battles=single in gct.ini (below).

IF SOMETHING GOES WRONG
- The game closes right as it starts, or Windows says Smart App Control
  blocked part of an app: Smart App Control blocks unsigned files like this
  dinput8.dll, and it has no exception for a single file. If you trust this
  file, turn it off in Windows Security > App & browser control > Smart App
  Control settings, then start the game again. Recent Windows updates let
  you turn it back on later, but while it's on the game won't start with
  this DLL.
- Steam shows the game running but no window shows up: stop it in Steam and
  start it again. Please send the short log that launch left in
  GameData\gct\logs.
- No Online button: dinput8.dll isn't in GameData, or the game isn't the
  current Steam version with Unofficial Patch 1.3 r130 installed. Any other
  version is left alone.
- "...different version..." or "...does not match...": every player needs
  this same dinput8.dll.
- The overlay doesn't open: turn the Steam overlay on, or have your friends
  join from your name in their Steam friends list.
- A friend's row stays on "joining...": the friend has to accept the invite,
  and their game needs the DLL. Invite friends sends the invite again; Back
  ends the co-op game for everyone.
- "Your party is full": a co-op game takes the host and three friends.
- The friend's galaxy doesn't show up: the friend goes back to the menus and
  their game loads it from there. Otherwise restart and accept a new invite.
- The friend sees "Your galaxy has stopped following your friend's": the
  friend quits to the menus, then the host sends a new invite.
- "Could not join your friend's battle" (in Versus, "...your opponent's
  battle"): your galaxy catches up when that battle ends. Your game waits up
  to two and a half minutes for the battle to load. If it keeps happening,
  send both newest logs (below); in co-op the host can set battles=single in
  the meantime.
- No enemy markers or lock-on in a battle: send both newest logs (below),
  and say which of you hosted the battle.
- The screen goes black in a battle but you can still hear the game and
  shoot: turn off Discord's in-game overlay (in Discord's settings, under
  Game Overlay), then restart the game.
- Versus, "The two galaxies no longer agree": quit to the menus; whoever
  saved last hosts Online > Versus > Load.
To report a problem, open an issue at the Source link below and attach the
newest file from GameData\gct\logs.

SETTINGS: GameData\gct\gct.ini shows up after the first start.
  log=on or off     whether each game session writes a log
  keep_logs=20      how many logs to keep (0 keeps all)
  battles=shared    the host's battles: shared (the default) or single (the
                    host fights alone); only the host's setting counts

UNINSTALL: delete dinput8.dll and the gct folder from GameData.

ABOUT
dinput8.dll {VERSION}, SHA-256:
  {DLL_SHA256}
It only works with the BattlefrontII.exe that Unofficial Patch 1.3 r130
installs over Steam build 6217431, SHA-256:
  {GAME_SHA256}
With any other build it just passes DirectInput through. It changes no game
files and only writes to GameData\gct.
Source: {SOURCE_URL}
Copyright (C) 2026 trauntrow. This is free software under the GNU General
Public License, version 3 or any later version (LICENSE.txt in this zip).
It comes with no warranty.
Fan-made. Not affiliated with or endorsed by Electronic Arts, Lucasfilm,
Disney or Valve.

------------------------------------------------------------------------------
Third-party notice: dinput8.dll contains MinHook, under this license:

{MINHOOK_LICENSE}
