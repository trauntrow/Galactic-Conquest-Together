#pragma once
// Tunnels the game's LAN battle traffic over the Steam session. While a shared battle runs, the
// game's three LAN sockets reach only the tunnel, so nothing touches the real network.
#include "diagnostics.h"
#include <string>

namespace gc {
// At startup: checks the game code that keeps the socket handles, then wraps sendto, send and
// recvfrom in its import table. On failure nothing changes and battles stay single-player.
bool install_battle_sockets(HMODULE game);
// Game thread: while sealed, the LAN sockets reach only the tunnel and real arrivals are dropped.
void battle_seal(bool on);
bool battle_sealed();
// Packet counts for the log. stray: packets sent to an address that belongs to no peer.
struct BattleSealCounts {
    unsigned sent = 0, discarded = 0, dropped = 0, delivered = 0, stray = 0;
};
BattleSealCounts battle_seal_counts();
// Steam thread: the peer's session role (1 the friend, 2 the host, 0 none) and the peers' session
// slots, bit k for slot k (1 to 3).
void battle_tunnel_peer(int role);
void battle_tunnel_peers(unsigned slots);
// Game thread: this PC's part in the sealed battle (1 hosts, 2 joined, 0 none). Either PC may host
// in Versus, so while set it decides the peer's address instead of the session role.
void battle_tunnel_role(int role);
int battle_tunnel_peer_role();  // the effective role, for the log
// Steam thread: the next frame to send (to a peer's slot, or all_peers) and a frame from a peer.
bool battle_tunnel_take(std::string& frame, unsigned* to = nullptr);
bool battle_tunnel_put(const char* frame, size_t size, unsigned from_slot = 1);
// What the socket wrappers do with each packet the game sends, also used by the Steam harness, and
// how many peer packets wait for the game's recvfrom (all, or one slot's).
void battle_tunnel_queue(battle::To to, battle::From from, const char* data, int size,
                         unsigned peer = battle::all_peers);
unsigned battle_tunnel_waiting(unsigned slot = 0);

// Defined in steam_invite.cpp. The host announces each battle (frame F, encode_info text) and its
// end (frame E); the friend reads the newest of each for `session`, its count of sessions opened.
void battle_host_announce(unsigned number, const std::string& text);
void battle_host_over(unsigned number);
unsigned battle_guest_session();
bool battle_guest_offered(unsigned session, unsigned after, unsigned& number, std::string& text);
unsigned battle_guest_over(unsigned session);
// A shared battle runs on this PC, from the join to the return, so the co-op game does not end at
// the menus.
void battle_set_busy(bool busy);
// Frame J. The friend reports what its game did in battle `number`; the host reads a slot's newest
// report when its count differs from `after`, and the slots its session told of the battle.
void battle_guest_report(unsigned number, battle::Part part);
bool battle_host_report(unsigned slot, unsigned after, unsigned& seq, unsigned& number, battle::Part& part);
unsigned battle_host_told(unsigned number);
// The players in this PC's co-op game, itself and its paired friends. The host's battle options
// raise the battle's player count to it.
unsigned battle_party();
// Frame V. The host sends the winning-team mask its game recorded for battle `number`; the friend
// reads the newest result for its `session`, if any.
void battle_host_result(unsigned number, unsigned mask);
bool battle_guest_result(unsigned session, unsigned& number, unsigned& mask);
// Kept current every frame for the goodbye sent before frame B: if the game closes in Versus battle
// `number` (0 for none), a host concedes with `mask` (V) and a client reports its player quit (J).
void battle_closing(unsigned number, bool host, unsigned mask);
}
