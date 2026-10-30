// Winsock wrappers that carry the game's LAN battle traffic through the Steam session.
#include <winsock2.h>  // types only; the DLL never imports ws2_32
#include "battle_net.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>

// While a battle is sealed, what the game sends on its three LAN sockets becomes tunnel frames for
// the Steam thread, and frames from the peers reach its recvfrom as if sent from their addresses.
namespace gc {
namespace {
using SendTo = int(WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
using Send = int(WSAAPI*)(SOCKET, const char*, int, int);
using RecvFrom = int(WSAAPI*)(SOCKET, char*, int, int, sockaddr*, int*);
SendTo real_sendto = nullptr;
Send real_send = nullptr;
RecvFrom real_recvfrom = nullptr;
const DWORD* socket_globals = nullptr;  // 01FA9258: game, broadcast and listener handles
std::atomic<bool> sealed{false};
std::atomic<int> peer_role{0};        // the peer's session role: 1 the friend, 2 the host
std::atomic<int> battle_role{0};      // 1: this PC hosts the sealed battle; 2: it joined it
std::atomic<unsigned> peer_slots{0};  // the peers' session slots, bit k for slot k
std::atomic<unsigned> sent{0}, discarded{0}, dropped{0}, delivered{0}, stray{0};

// The game thread fills `outgoing` and drains the incoming queues; the Steam thread does the
// reverse. Each lock covers one push or pop, and each queue has room for three clients.
constexpr size_t outgoing_limit = 512 * battle::max_peers, incoming_limit = 256 * battle::max_peers;
struct Packet {
    battle::From from = battle::From::game;
    uint16_t port = 0;
    unsigned peer = 1;
    std::string data;
};
struct Out {
    unsigned to = battle::all_peers;
    std::string frame;
};  // to: a peer's slot, or all_peers
SRWLOCK queue_lock = SRWLOCK_INIT;
std::deque<Out> outgoing;
std::deque<Packet> to_game, to_listener;
battle::Mirror mirrors[battle::max_peers + 1];  // game thread only, indexed by peer slot

// Which of the game's sockets `s` is, from the handles the game holds now.
battle::Socket classify(SOCKET s) {
    DWORD handles[3]{};
    if (!socket_globals || !read_memory(socket_globals, handles, sizeof handles)) return battle::Socket::other;
    return battle::classify(s, handles[0], handles[1], handles[2]);
}
// The peer's role: the battle role when the game thread has set one, since either PC may host in
// Versus; otherwise the session role. 0 without a paired peer.
int effective_role() {
    const int session = peer_role.load(std::memory_order_acquire);
    if (!session) return 0;
    const int battle = battle_role.load(std::memory_order_acquire);
    return battle ? battle : session;
}
// The address this game knows a peer by: as battle host, each client at its slot's address; as a
// client, the battle host's.
uint32_t peer_ip(unsigned slot) {
    const int role = effective_role();
    return role == 1 ? battle::client_address(slot) : role == 2 ? battle::host_address : 0;
}
uint16_t peer_game_port() { return effective_role() == 2 ? battle::host_game_port : battle::client_game_port; }
// The peer a packet sent to `ip` goes to: all_peers for a broadcast, the battle host from a client's
// game, else the client at that address. An address that matches no peer counts as stray.
unsigned destination_peer(uint32_t ip) {
    const int role = effective_role();
    if (ip == battle::any_address || role == 0) return battle::all_peers;
    if (role == 2) {
        if (ip != battle::host_address) stray.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    bool odd = false;
    const unsigned slot = battle::destination_slot(ip, peer_slots.load(std::memory_order_acquire), odd);
    if (odd) stray.fetch_add(1, std::memory_order_relaxed);
    return slot;
}

void queue_out(battle::To to, battle::From from, const char* data, int size, unsigned peer) {
    battle_tunnel_queue(to, from, data, size, peer);
}

// True when the battle is sealed and `s` is one of the game's LAN sockets.
bool ours(SOCKET s, battle::Socket& kind) {
    if (!sealed.load(std::memory_order_acquire)) return false;
    kind = classify(s);
    return kind != battle::Socket::other;
}

// The Winsock wrappers. Sealed traffic on the game's sockets goes through the tunnel and reports
// success; everything else passes straight through.
int WSAAPI sendto_wrapper(SOCKET s, const char* data, int size, int flags, const sockaddr* to, int to_size) {
    battle::Socket kind;
    if (!ours(s, kind)) return real_sendto(s, data, size, flags, to, to_size);
    battle::To destination = battle::To::game;
    unsigned peer = battle::all_peers;
    if (to && to_size >= static_cast<int>(sizeof(sockaddr_in)) && to->sa_family == AF_INET) {
        const auto* address = reinterpret_cast<const sockaddr_in*>(to);
        const uint16_t port = _byteswap_ushort(address->sin_port);
        const uint32_t ip = _byteswap_ulong(address->sin_addr.s_addr);
        destination = battle::route(port);
        peer = destination_peer(ip);
        // Records where the game sends to this peer, so that peer's packets appear to come from there.
        if (destination == battle::To::game && kind == battle::Socket::game && peer != battle::all_peers &&
            peer != battle::no_peer)
            mirrors[peer].sent(ip, port, peer_ip(peer));
    }
    queue_out(destination, kind == battle::Socket::broadcast ? battle::From::broadcast : battle::From::game, data, size,
              peer);
    return size;
}
int WSAAPI send_wrapper(SOCKET s, const char* data, int size, int flags) {
    battle::Socket kind;
    if (!ours(s, kind)) return real_send(s, data, size, flags);
    // The broadcast socket is connected to 255.255.255.255:3656, which is every peer's listener.
    queue_out(battle::To::listener, kind == battle::Socket::broadcast ? battle::From::broadcast : battle::From::game,
              data, size, battle::all_peers);
    return size;
}
int WSAAPI recvfrom_wrapper(SOCKET s, char* data, int size, int flags, sockaddr* from, int* from_size) {
    battle::Socket kind;
    if (!ours(s, kind)) return real_recvfrom(s, data, size, flags, from, from_size);
    // Reads and drops whatever really arrived, so it neither reaches the game nor waits for later.
    char scratch[2048];
    sockaddr_in source{};
    for (int i = 0; i < 64; ++i) {
        int source_size = sizeof source;
        if (real_recvfrom(s, scratch, sizeof scratch, 0, reinterpret_cast<sockaddr*>(&source), &source_size) ==
            SOCKET_ERROR)
            break;
        discarded.fetch_add(1, std::memory_order_relaxed);
    }
    // Then hands over the next packet from the peers for this socket; the broadcast socket only sends.
    Packet packet;
    bool have = false;
    if (kind != battle::Socket::broadcast) {
        auto& queue = kind == battle::Socket::listener ? to_listener : to_game;
        AcquireSRWLockExclusive(&queue_lock);
        if (!queue.empty()) {
            packet = std::move(queue.front());
            queue.pop_front();
            have = true;
        }
        ReleaseSRWLockExclusive(&queue_lock);
    }
    if (!have || !data || size <= 0) {
        SetLastError(WSAEWOULDBLOCK);  // WSAGetLastError reads the thread's last error
        return SOCKET_ERROR;
    }
    const battle::Address address =
        mirrors[packet.peer].source(packet.from, packet.port, peer_ip(packet.peer), peer_game_port());
    if (from && from_size && *from_size >= static_cast<int>(sizeof(sockaddr_in))) {
        sockaddr_in shown{};
        shown.sin_family = AF_INET;
        shown.sin_port = _byteswap_ushort(address.port);
        shown.sin_addr.s_addr = _byteswap_ulong(address.ip);
        memcpy(from, &shown, sizeof shown);
        *from_size = sizeof shown;
    }
    const int copy = static_cast<int>(packet.data.size()) < size ? static_cast<int>(packet.data.size()) : size;
    memcpy(data, packet.data.data(), static_cast<size_t>(copy));
    delivered.fetch_add(1, std::memory_order_relaxed);
    if (copy < static_cast<int>(packet.data.size())) {
        SetLastError(WSAEMSGSIZE);
        return SOCKET_ERROR;
    }
    return copy;
}

// The instructions that keep the three handles, at the preferred base: the stores after socket()
// and the pushes before send, sendto and recvfrom. Nothing is wrapped if any of them differ.
struct Site {
    DWORD rva;
    unsigned char bytes[6];
    unsigned size;
    DWORD global;
};
const Site sites[] = {
    {0x217c27, {0xa3}, 5, 0x01fa925c},        // discovery setup: mov [broadcast], eax
    {0x217c95, {0xa3}, 5, 0x01fa9260},        // discovery setup: mov [listener], eax
    {0x217d60, {0xa3}, 5, 0x01fa9258},        // game socket setup: mov [game], eax
    {0x217eef, {0xff, 0x35}, 6, 0x01fa925c},  // packet send: push [broadcast]; send
    {0x217f09, {0xff, 0x35}, 6, 0x01fa9258},  // packet send: push [game]; sendto
    {0x217fa4, {0xff, 0x35}, 6, 0x01fa9258},  // packet poll: push [game]; recvfrom
    {0x21814a, {0xff, 0x35}, 6, 0x01fa9260},  // packet poll: push [listener]; recvfrom
};
// Compares each site's opcode and global address, rebased to where the game is loaded.
bool sites_match(const unsigned char* base) {
    for (const auto& site : sites) {
        unsigned char expected[6]{}, actual[6]{};
        const unsigned opcode = site.size - 4;
        memcpy(expected, site.bytes, opcode);
        const DWORD global = reinterpret_cast<DWORD>(base) + (site.global - 0x400000);
        memcpy(expected + opcode, &global, 4);
        if (!read_memory(base + site.rva, actual, site.size) || memcmp(expected, actual, site.size)) return false;
    }
    return true;
}

// Points one import slot at `replacement`, storing its current target in `original` first.
template <class T> bool replace(void** slot, T& original, T replacement) {
    original = reinterpret_cast<T>(*slot);
    if (!original || original == replacement) return false;
    DWORD protection;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) return false;
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(replacement));
    DWORD unused;
    VirtualProtect(slot, sizeof(void*), protection, &unused);
    return true;
}
}

bool install_battle_sockets(HMODULE game) {
    auto base = reinterpret_cast<unsigned char*>(game);
    if (!sites_match(base)) {
        log("BATTLE_SOCKETS REJECT the game's LAN socket code differs");
        return false;
    }
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const DWORD size = nt->OptionalHeader.SizeOfImage;
    if (!directory.VirtualAddress || directory.VirtualAddress >= size ||
        directory.Size > size - directory.VirtualAddress)
        return false;
    // Winsock is imported by ordinal: 17 recvfrom, 19 send, 20 sendto.
    void** slots[3]{};
    for (DWORD off = 0; off + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= directory.Size;
         off += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress + off);
        if (!desc->Name) break;
        char dll[16]{};
        if (desc->Name >= size || !read_memory(base + desc->Name, dll, sizeof dll - 1) || _stricmp(dll, "WS2_32.dll"))
            continue;
        if (!desc->OriginalFirstThunk || desc->OriginalFirstThunk >= size || desc->FirstThunk >= size) continue;
        for (DWORD i = 0; i < (size - desc->OriginalFirstThunk) / 4 && i < (size - desc->FirstThunk) / 4; ++i) {
            const DWORD name = reinterpret_cast<DWORD*>(base + desc->OriginalFirstThunk)[i];
            if (!name) break;
            if (!IMAGE_SNAP_BY_ORDINAL32(name)) continue;
            auto slot = reinterpret_cast<void**>(base + desc->FirstThunk + 4 * i);
            const DWORD ordinal = IMAGE_ORDINAL32(name);
            if (ordinal == 17) slots[0] = slot;
            else if (ordinal == 19) slots[1] = slot;
            else if (ordinal == 20) slots[2] = slot;
        }
    }
    if (!slots[0] || !slots[1] || !slots[2]) {
        log("BATTLE_SOCKETS REJECT recvfrom, send or sendto is not imported");
        return false;
    }
    socket_globals = reinterpret_cast<const DWORD*>(base + 0x1ba9258);  // 01FA9258 at the preferred base
    // Each original is stored before its slot changes, and the wrappers pass everything through
    // until a battle seals them.
    const bool ok = replace(slots[0], real_recvfrom, &recvfrom_wrapper) &&
                    replace(slots[1], real_send, &send_wrapper) && replace(slots[2], real_sendto, &sendto_wrapper);
    log("BATTLE_SOCKETS %s", ok ? "wrapped" : "FAILED");
    return ok;
}

void battle_seal(bool on) {
    if (sealed.exchange(on, std::memory_order_acq_rel) == on) return;
    // Each battle starts and ends with empty queues; unsealing also forgets the peers' addresses.
    AcquireSRWLockExclusive(&queue_lock);
    outgoing.clear();
    to_game.clear();
    to_listener.clear();
    ReleaseSRWLockExclusive(&queue_lock);
    if (!on)
        for (battle::Mirror& m : mirrors) m.clear();
}
bool battle_sealed() { return sealed.load(std::memory_order_acquire); }
BattleSealCounts battle_seal_counts() {
    return {sent.load(std::memory_order_relaxed), discarded.load(std::memory_order_relaxed),
            dropped.load(std::memory_order_relaxed), delivered.load(std::memory_order_relaxed),
            stray.load(std::memory_order_relaxed)};
}
void battle_tunnel_peer(int role) { peer_role.store(role, std::memory_order_release); }
void battle_tunnel_peers(unsigned slots) { peer_slots.store(slots & 0xE, std::memory_order_release); }
void battle_tunnel_role(int role) { battle_role.store(role == 1 || role == 2 ? role : 0, std::memory_order_release); }
int battle_tunnel_peer_role() { return effective_role(); }
// Queues one game packet as a tunnel frame for slot `peer`, or every peer. With no paired peer it is
// dropped, which is all a one-PC test needs.
void battle_tunnel_queue(battle::To to, battle::From from, const char* data, int size, unsigned peer) {
    sent.fetch_add(1, std::memory_order_relaxed);
    if (!data || size <= 0 || static_cast<size_t>(size) > battle::tunnel_max_packet || !effective_role() ||
        (peer != battle::all_peers && (peer > battle::max_peers))) {
        dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Out out{peer, battle::encode_tunnel(to, from, 0, data, static_cast<size_t>(size))};
    AcquireSRWLockExclusive(&queue_lock);
    const bool room = outgoing.size() < outgoing_limit;
    if (room) outgoing.push_back(std::move(out));
    ReleaseSRWLockExclusive(&queue_lock);
    if (!room) dropped.fetch_add(1, std::memory_order_relaxed);
}
unsigned battle_tunnel_waiting(unsigned slot) {
    AcquireSRWLockExclusive(&queue_lock);
    size_t waiting = 0;
    for (const auto* queue : {&to_game, &to_listener})
        for (const Packet& p : *queue) waiting += !slot || p.peer == slot ? 1 : 0;
    ReleaseSRWLockExclusive(&queue_lock);
    return static_cast<unsigned>(waiting);
}
bool battle_tunnel_take(std::string& frame, unsigned* to) {
    AcquireSRWLockExclusive(&queue_lock);
    const bool have = !outgoing.empty();
    if (have) {
        frame = std::move(outgoing.front().frame);
        if (to) *to = outgoing.front().to;
        outgoing.pop_front();
    }
    ReleaseSRWLockExclusive(&queue_lock);
    return have;
}
// A frame from the peer in `from_slot`, queued for the game's recvfrom while the battle is sealed.
bool battle_tunnel_put(const char* frame, size_t size, unsigned from_slot) {
    battle::To to;
    battle::From from;
    uint16_t port = 0;
    if (!sealed.load(std::memory_order_acquire) || !from_slot || from_slot > battle::max_peers ||
        !battle::decode_tunnel(frame, size, to, from, port))
        return false;
    Packet packet{from, port, from_slot, std::string(frame + battle::tunnel_header, size - battle::tunnel_header)};
    AcquireSRWLockExclusive(&queue_lock);
    auto& queue = to == battle::To::listener ? to_listener : to_game;
    const bool room = queue.size() < incoming_limit;
    if (room) queue.push_back(std::move(packet));
    ReleaseSRWLockExclusive(&queue_lock);
    if (!room) dropped.fetch_add(1, std::memory_order_relaxed);
    return room;
}

#if GC_BATTLE_NET_TEST
// For tests/battle_test.cpp: the wrappers over fake Winsock functions and socket globals, without
// the game's import table.
void battle_net_test_setup(const DWORD* globals, SendTo fake_sendto, Send fake_send, RecvFrom fake_recvfrom,
                           SendTo& wrapped_sendto, Send& wrapped_send, RecvFrom& wrapped_recvfrom) {
    socket_globals = globals;
    real_sendto = fake_sendto;
    real_send = fake_send;
    real_recvfrom = fake_recvfrom;
    wrapped_sendto = &sendto_wrapper;
    wrapped_send = &send_wrapper;
    wrapped_recvfrom = &recvfrom_wrapper;
}
#endif
}
