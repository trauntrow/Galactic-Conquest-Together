// Decoding and validation of the host's campaign capture records, and the JSON form they are sent in.
#include "capture_decode.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>
#include "runtime_crypto.h"

namespace gc::capture {
namespace {
// Decoding stops at the first problem, thrown with its reason.
struct Reject : std::runtime_error {
    using std::runtime_error::runtime_error;
};
[[noreturn]] void reject(const std::string& reason) { throw Reject(reason); }

// The capture schema. Scalars and maps come in every snapshot; SIDES are probed from producer 3,
// SAVED, FLEET and GLOBAL from producer 5.
constexpr std::string_view SCALARS[]{"scenario", "turnNumber", "playerTeam", "planetNext", "winnerTeam", "curScreen"};
constexpr std::string_view MAPS[]{"planetTeam",    "planetFleet",  "planetBase",
                                  "teamResources", "lastSelected", "lastFleet"};
constexpr std::string_view SIDES[]{"playerSide", "otherSide", "controllerTeam", "controllerPlayer",
                                   "teamCode",   "teamName",  "fleetPtr"};
constexpr std::string_view SAVED[]{"custom",        "launchMission",   "activeBonus",     "fleetBattle",
                                   "recentPlanets", "planetResources", "battleResources", "soakMode"};
constexpr std::string_view FLEET[]{"fleet.turnNumber", "fleet.planetStart", "fleet.planetNext"};
constexpr std::string_view GLOBAL[]{"global.ifs_purchase_unit_owned", "global.ifs_purchase_tech_cards",
                                    "global.ifs_purchase_tech_using"};
constexpr int PROBED_ALL = 7 + 8 + 3 + 3;  // SIDES, SAVED, FLEET and GLOBAL
// From producer 7: what the host's camera shows.
constexpr std::string_view CAMERA[]{"planetSelected", "fleet.zoomLevel"};
constexpr int INTEGER_LIMIT = 16777215;  // 2^24 - 1: every integer up to it is exact as a float

template <size_t N> bool in(const std::string_view (&set)[N], std::string_view field) {
    return std::find(std::begin(set), std::end(set), field) != std::end(set);
}
// Whether this producer version probes field: SIDES from 3, saved values from 5, CAMERA from 7.
bool probed_field(int version, std::string_view field) {
    if (version == 3 || version == 4) return in(SIDES, field);
    if (version == 7 && in(CAMERA, field)) return true;
    return version >= 5 && (in(SIDES, field) || in(SAVED, field) || in(FLEET, field) || in(GLOBAL, field));
}
// How many probed fields a snapshot of this version carries.
size_t probed_count(int version) {
    return version == 7 ? PROBED_ALL + std::size(CAMERA) : version >= 5 ? PROBED_ALL : version >= 3 ? 7 : 0;
}
// Producer 7 has no emission budget; the others send at most eight snapshots per generation.
int max_sequence(int version) { return version == 7 ? INTEGER_LIMIT : 8; }
// The record count a snapshot of each producer version may have.
bool bounds(int version, int& low, int& high) {
    switch (version) {
    case 1:
    case 2:
        low = 12;
        high = 396;
        return true;
    case 3:
    case 4:
        low = 19;
        high = 851;
        return true;
    case 5:
        low = 12 + PROBED_ALL;
        high = 396 + 65 * PROBED_ALL;
        return true;
    case 6:
        low = 12 + PROBED_ALL;
        high = 396 + 65 * PROBED_ALL + 128 * 3;
        return true;
    case 7:
        low = 12 + PROBED_ALL + 2;
        high = 396 + 65 * (PROBED_ALL + 2) + 128 * 3;
        return true;
    }
    return false;
}

bool digit(char c) { return c >= '0' && c <= '9'; }

// A non-negative envelope number: "0", or up to eight digits without a leading zero.
int envelope_number(std::string_view text) {
    if (text == "0") return 0;
    if (text.empty() || text.size() > 8 || text[0] == '0' || !std::all_of(text.begin(), text.end(), digit))
        reject("invalid envelope number");
    int n = 0;
    for (char c : text) n = n * 10 + (c - '0');
    return n;
}

// One to `limit` bytes as uppercase hex pairs, decoded into out.
bool hex(std::string_view text, size_t limit, std::string& out) {
    if (text.empty() || text.size() % 2 || text.size() > 2 * limit) return false;
    out.clear();
    for (size_t i = 0; i < text.size(); i += 2) {
        int byte = 0;
        for (char c : text.substr(i, 2)) {
            if (digit(c)) byte = byte * 16 + (c - '0');
            else if (c >= 'A' && c <= 'F') byte = byte * 16 + (c - 'A' + 10);
            else return false;
        }
        out += static_cast<char>(byte);
    }
    return true;
}

// Refuses bytes that are not valid UTF-8, with the message Python's strict UTF-8 decoder gives.
void strict_utf8(const std::string& bytes) {
    const size_t n = bytes.size();
    auto at = [&](size_t k) { return static_cast<unsigned char>(bytes[k]); };
    auto more = [&](size_t k) { return at(k) >= 0x80 && at(k) <= 0xBF; };
    for (size_t i = 0; i < n;) {
        const unsigned char c = at(i);
        const size_t left = n - i;
        // fault: 0 data ends early, 1 bad start byte, 2-4 bad continuation byte 1-3.
        int fault;
        if (c < 0x80) {
            ++i;
            continue;
        }
        if (c < 0xE0) {
            if (c < 0xC2) fault = 1;
            else if (left < 2) fault = 0;
            else if (!more(i + 1)) fault = 2;
            else {
                i += 2;
                continue;
            }
        } else if (c < 0xF0) {
            if (left < 3) {
                fault = left < 2 ? 0 : (!more(i + 1) || (at(i + 1) < 0xA0 ? c == 0xE0 : c == 0xED)) ? 2 : 0;
            } else if (!more(i + 1) || (c == 0xE0 && at(i + 1) < 0xA0) || (c == 0xED && at(i + 1) >= 0xA0)) fault = 2;
            else if (!more(i + 2)) fault = 3;
            else {
                i += 3;
                continue;
            }
        } else if (c < 0xF5) {
            if (left < 4) {
                if (left < 2) fault = 0;
                else if (!more(i + 1) || (at(i + 1) < 0x90 ? c == 0xF0 : c == 0xF4)) fault = 2;
                else fault = left >= 3 && !more(i + 2) ? 3 : 0;
            } else if (!more(i + 1) || (c == 0xF0 && at(i + 1) < 0x90) || (c == 0xF4 && at(i + 1) >= 0x90)) fault = 2;
            else if (!more(i + 2)) fault = 3;
            else if (!more(i + 3)) fault = 4;
            else {
                i += 4;
                continue;
            }
        } else fault = 1;
        const size_t end = fault == 0 ? n : fault == 1 ? i + 1 : i + fault - 1;
        const char* why = fault == 0   ? "unexpected end of data"
                          : fault == 1 ? "invalid start byte"
                                       : "invalid continuation byte";
        char text[160];
        if (end == i + 1) sprintf_s(text, "'utf-8' codec can't decode byte 0x%02x in position %zu: %s", c, i, why);
        else sprintf_s(text, "'utf-8' codec can't decode bytes in position %zu-%zu: %s", i, end - 1, why);
        reject(text);
    }
}

// Decodes one typed value: z nil, b boolean, i integer, s hex text. Keys may only be integers or
// text, and text keys hold at most 32 bytes.
Value value(std::string_view tag, std::string_view encoded, bool key = false) {
    Value v;
    if (tag == "z" && encoded == "-" && !key) return v;
    if (tag == "b" && (encoded == "0" || encoded == "1") && !key) {
        v.kind = Value::Bool;
        v.flag = encoded == "1";
        return v;
    }
    if (tag == "i") {
        // -?(0|[1-9][0-9]{0,7})
        std::string_view digits = encoded.substr(!encoded.empty() && encoded[0] == '-');
        if (!digits.empty() && digits.size() <= 8 && (digits == "0" || digits[0] != '0') &&
            std::all_of(digits.begin(), digits.end(), digit)) {
            int n = 0;
            for (char c : digits) n = n * 10 + (c - '0');
            if (n <= INTEGER_LIMIT) {
                v.kind = Value::Int;
                v.number = digits.size() < encoded.size() ? -n : n;
                return v;
            }
        }
    }
    if (tag == "s") {
        v.kind = Value::Text;
        if (encoded == "-") return v;
        if (hex(encoded, key ? 32 : 64, v.text)) {
            strict_utf8(v.text);
            return v;
        }
    }
    reject("invalid typed value");
}

Key key_of(const Value& v) {
    Key k;
    k.integer = v.kind == Value::Int;
    k.number = v.number;
    k.text = v.text;
    return k;
}

bool same_key(const Key& a, const Key& b) {
    return a.integer == b.integer && (a.integer ? a.number == b.number : a.text == b.text);
}

// A key for duplicate checks over (field, tag, key). Text is length-prefixed so no key can pass
// for another.
std::string identity(std::string_view field, std::string_view tag, const Key& key) {
    std::string id = std::to_string(field.size()) + ':';
    id += field;
    id += std::to_string(tag.size()) + ':';
    id += tag;
    id += key.integer ? '#' + std::to_string(key.number) : std::to_string(key.text.size()) + ':' + key.text;
    return id;
}

// A nested table summary: "n=64+", or "n=<0-999>" then " oversized" or " k=<kinds> v=<kinds>".
bool composition(std::string_view text) {
    if (text == "n=64+") return true;
    if (text.substr(0, 2) != "n=") return false;
    text.remove_prefix(2);
    size_t digits = 0;
    while (digits < text.size() && digit(text[digits])) ++digits;
    if (!digits || digits > 3 || (digits > 1 && text[0] == '0')) return false;
    text.remove_prefix(digits);
    if (text == " oversized") return true;
    auto word = [&](std::string_view prefix) {
        if (text.substr(0, prefix.size()) != prefix) return false;
        text.remove_prefix(prefix.size());
        size_t n = 0;
        while (n < text.size() && ((text[n] >= 'a' && text[n] <= 'z') || text[n] == '+')) ++n;
        text.remove_prefix(n);
        return n >= 1 && n <= 24;
    };
    return word(" k=") && word(" v=") && text.empty();
}

// Decodes a probed value. Returns 'u' (unsupported kind), 'e' (error) or 'c' (nested table
// summary) with its text, or 0 with a plain value.
char probed_value(std::string_view tag, std::string_view encoded, Value& plain, std::string& text) {
    if (tag == "u" || tag == "e" || tag == "c") {
        if (!hex(encoded, tag == "c" ? 48 : 32, text)) reject("invalid probed marker");
        strict_utf8(text);
        if (tag == "c" && !composition(text)) reject("invalid nested composition summary");
        return tag[0];
    }
    plain = value(tag, encoded);
    return 0;
}

// Splits at every separator, keeping empty parts.
std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    for (size_t start = 0;;) {
        const size_t end = text.find(separator, start);
        parts.emplace_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) return parts;
        start = end + 1;
    }
}

// Canonical key order: integer keys before string keys, then by value.
bool key_less(const Key& a, const Key& b) {
    if (a.integer != b.integer) return a.integer;
    return a.integer ? a.number < b.number : a.text < b.text;
}

// Canonical JSON encoders. The output is hashed, so it must be byte-exact.
std::string json_value(const Value& v) {
    switch (v.kind) {
    case Value::Null: return "null";
    case Value::Bool: return v.flag ? "true" : "false";
    case Value::Int: return std::to_string(v.number);
    default: return json_text(v.text);
    }
}

std::string json_key(const Key& k) { return k.integer ? std::to_string(k.number) : json_text(k.text); }

// A JSON object with sorted field names. They are ASCII, so byte order is code point order.
std::string object(std::vector<std::pair<std::string_view, std::string>> fields) {
    std::sort(fields.begin(), fields.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string out = "{";
    for (const auto& [name, json] : fields) {
        if (out.size() > 1) out += ',';
        out += json_text(name);
        out += ':';
        out += json;
    }
    return out + '}';
}

std::string json_row(const Row& r) {
    return object({{"key_type", r.key.integer ? "\"integer\"" : "\"string\""},
                   {"key", json_key(r.key)},
                   {"value", json_value(r.value)}});
}

// A probed map entry: its key and exactly one of value, unsupported, composition or entries.
std::string json_entry(const Entry& e) {
    std::vector<std::pair<std::string_view, std::string>> fields{
        {"key_type", e.key.integer ? "\"integer\"" : "\"string\""}, {"key", json_key(e.key)}};
    if (e.kind == Entry::Plain) fields.emplace_back("value", json_value(e.value));
    else if (e.kind == Entry::Unsupported) fields.emplace_back("unsupported", json_text(e.text));
    else if (e.kind == Entry::Composition) fields.emplace_back("composition", json_text(e.text));
    else {
        std::string rows = "[";
        for (const Row& r : e.entries) {
            if (rows.size() > 1) rows += ',';
            rows += json_row(r);
        }
        fields.emplace_back("entries", rows + ']');
    }
    return object(std::move(fields));
}

std::string json_probed(const Probed& p) {
    switch (p.kind) {
    case Probed::Scalar: return object({{"kind", "\"scalar\""}, {"value", json_value(p.value)}});
    case Probed::Unsupported: return object({{"kind", "\"unsupported\""}, {"type", json_text(p.text)}});
    case Probed::Error: return object({{"kind", "\"error\""}, {"reason", json_text(p.text)}});
    default: {
        std::string rows = "[";
        for (const Entry& e : p.entries) {
            if (rows.size() > 1) rows += ',';
            rows += json_entry(e);
        }
        return object({{"kind", "\"map\""}, {"entries", rows + ']'}});
    }
    }
}

// Quotes text the way Python's ascii() does, for observation messages.
std::string py_ascii(const std::string& utf8) {
    std::vector<unsigned> points;
    for (size_t i = 0; i < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        const int extra = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
        unsigned cp = extra ? c & (0x3F >> extra) : c;
        for (int k = 1; k <= extra && i + k < utf8.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
        points.push_back(cp);
        i += extra + 1;
    }
    const bool single = std::find(points.begin(), points.end(), unsigned('\'')) != points.end();
    const bool dbl = std::find(points.begin(), points.end(), unsigned('"')) != points.end();
    const char quote = single && !dbl ? '"' : '\'';
    std::string out(1, quote);
    char buffer[16];
    for (unsigned cp : points) {
        if (cp == static_cast<unsigned>(quote) || cp == '\\') {
            out += '\\';
            out += static_cast<char>(cp);
        } else if (cp == '\t') out += "\\t";
        else if (cp == '\n') out += "\\n";
        else if (cp == '\r') out += "\\r";
        else if (cp >= 0x20 && cp < 0x7F) out += static_cast<char>(cp);
        else {
            if (cp < 0x100) sprintf_s(buffer, "\\x%02x", cp);
            else if (cp < 0x10000) sprintf_s(buffer, "\\u%04x", cp);
            else sprintf_s(buffer, "\\U%08x", cp);
            out += buffer;
        }
    }
    return out + quote;
}

std::string py_ascii(const Key& k) { return k.integer ? std::to_string(k.number) : py_ascii(k.text); }
}

std::vector<std::string_view> split_lines(std::string_view text, bool nel) {
    std::vector<std::string_view> lines;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c != '\n' && c != '\r' && c != 0x0B && c != 0x0C && c != 0x1C && c != 0x1D && c != 0x1E &&
            !(nel && c == 0x85))
            continue;
        lines.push_back(text.substr(start, i - start));
        if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
        start = i + 1;
    }
    if (start < text.size()) lines.push_back(text.substr(start));
    return lines;
}

std::string json_text(std::string_view text, bool utf8) {
    std::string out = "\"";
    char buffer[16];
    auto escape = [&](unsigned cp) {
        sprintf_s(buffer, "\\u%04x", cp);
        out += buffer;
    };
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        unsigned cp = c;
        size_t width = 1;
        if (utf8 && c >= 0x80) {
            const int extra = c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
            cp = c & (0x3F >> extra);
            for (int k = 1; k <= extra && i + k < text.size(); ++k)
                cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
            width = extra + 1;
        }
        i += width;
        if (cp == '"') out += "\\\"";
        else if (cp == '\\') out += "\\\\";
        else if (cp == '\n') out += "\\n";
        else if (cp == '\r') out += "\\r";
        else if (cp == '\t') out += "\\t";
        else if (cp == '\b') out += "\\b";
        else if (cp == '\f') out += "\\f";
        else if (cp >= 0x20 && cp < 0x7F) out += static_cast<char>(cp);
        else if (cp < 0x10000) escape(cp);
        else {
            escape(0xD800 + ((cp - 0x10000) >> 10));
            escape(0xDC00 + ((cp - 0x10000) & 0x3FF));
        }
    }
    return out + '"';
}

std::string canonical_json(const Data& data) {
    std::string scalars, maps, probed;
    for (const auto& [field, v] : data.scalars) {
        scalars += scalars.empty() ? "{" : ",";
        scalars += json_text(field) + ':' + json_value(v);
    }
    for (const auto& [field, rows] : data.maps) {
        maps += maps.empty() ? "{" : ",";
        maps += json_text(field) + ":[";
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i) maps += ',';
            maps += json_row(rows[i]);
        }
        maps += ']';
    }
    std::vector<std::pair<std::string_view, std::string>> fields{{"scalars", scalars.empty() ? "{}" : scalars + '}'},
                                                                 {"maps", maps.empty() ? "{}" : maps + '}'}};
    if (data.has_probed) {
        for (const auto& [field, p] : data.probed) {
            probed += probed.empty() ? "{" : ",";
            probed += json_text(field) + ':' + json_probed(p);
        }
        fields.emplace_back("probed", probed.empty() ? "{}" : probed + '}');
    }
    return object(std::move(fields));
}

std::string sha256_hex(std::string_view bytes) {
    std::array<unsigned char, 32> digest{};
    if (!request_hash(reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<unsigned>(bytes.size()),
                      digest))
        throw std::runtime_error("SHA-256 unavailable");
    return request_hex(digest.data(), static_cast<unsigned>(digest.size()));
}

// A bad record spoils every transaction still open.
void Decoder::invalidate() {
    for (auto& entry : pending_) entry.second.invalid = true;
}

// Only two kinds of line matter: a producer install and a state record.
void Decoder::line(std::string_view text) {
    static constexpr std::string_view installed = " CHANNEL GCOBS:installed:campaign_v";
    static constexpr std::string_view state = " CHANNEL GCOBS:state:";
    if (text.find(installed) != std::string_view::npos) {
        // A shell rebuild can re-arm the producer and restart its sequence numbers, so every
        // install after the first opens a new generation.
        if (++installs_ > 1) {
            ++generation_;
            seen_.clear();
            for (const auto& entry : pending_)
                rejected_.push_back("incomplete sequence " + std::to_string(entry.first) + " in generation " +
                                    std::to_string(generation_ - 1));
            pending_.clear();
        }
        return;
    }
    const size_t at = text.find(state);
    if (at == std::string_view::npos) return;
    const std::string_view payload = text.substr(at + state.size());
    if (payload.size() > 200) {
        invalidate();
        rejected_.push_back("oversized record");
        return;
    }
    try {
        record(split(payload, ':'));
    } catch (const Reject& error) {
        invalidate();
        rejected_.push_back(error.what());
    }
}

void Decoder::finish() {
    for (const auto& entry : pending_) rejected_.push_back("incomplete sequence " + std::to_string(entry.first));
}

// Records: begin:version:seq:count:source:ticket[:skipped], item:seq:index:<5 fields>,
// end:seq:count, or rejected:<reason> from the producer itself.
void Decoder::record(const std::vector<std::string>& parts) {
    if (parts[0] == "rejected") {
        std::string reason = "producer rejected capture ";
        for (size_t i = 1; i < parts.size(); ++i) {
            if (i > 1) reason += ':';
            reason += parts[i];
        }
        rejected_.push_back(reason);
        return;
    }
    if (parts[0] == "begin" && (parts.size() == 6 || parts.size() == 7)) {
        const int version = envelope_number(parts[1]), seq = envelope_number(parts[2]),
                  count = envelope_number(parts[3]);
        const int source = envelope_number(parts[4]), ticket = envelope_number(parts[5]);
        const int skipped = parts.size() == 7 ? envelope_number(parts[6]) : -1;
        int low = 0, high = 0;
        if (!bounds(version, low, high) || (skipped < 0) != (version == 1)) reject("unsupported producer version");
        if (producer_ && producer_ != version) reject("mixed producer versions in one log");
        if (seq < 1 || seq > max_sequence(version) || count < low || count > high || source <= 0 || ticket <= 0)
            reject("invalid envelope");
        if (skipped > 100000) reject("invalid skipped-sample count");
        if (std::find(seen_.begin(), seen_.end(), seq) != seen_.end()) {
            if (auto found = pending_.find(seq); found != pending_.end()) found->second.invalid = true;
            reject("duplicate sequence");
        }
        seen_.push_back(seq);
        producer_ = version;
        Pending p;
        p.count = count;
        p.source = source;
        p.ticket = ticket;
        p.skipped = skipped;
        p.version = version;
        p.generation = generation_;
        pending_[seq] = std::move(p);
    } else if (parts[0] == "item" && parts.size() == 8) {
        const int seq = envelope_number(parts[1]), index = envelope_number(parts[2]);
        const auto found = pending_.find(seq);
        if (found == pending_.end()) reject("item without begin");
        Pending& p = found->second;
        if (p.records.count(index) || index < 1 || index > p.count) {
            p.invalid = true;
            reject("duplicate/out-of-range item");
        }
        p.records[index].assign(parts.begin() + 3, parts.end());
    } else if (parts[0] == "end" && parts.size() == 3) {
        const int seq = envelope_number(parts[1]), count = envelope_number(parts[2]);
        const auto found = pending_.find(seq);
        if (found == pending_.end()) reject("incomplete or invalid transaction");
        Pending p = std::move(found->second);
        pending_.erase(found);
        if (p.invalid || count != p.count || static_cast<int>(p.records.size()) != count)
            reject("incomplete or invalid transaction");
        complete_.push_back(build(seq, p));
    } else {
        reject("malformed record");
    }
}

// Builds a snapshot from a complete transaction, checking every record against the schema.
Snapshot Decoder::build(int sequence, Pending& p) {
    Data data;
    std::set<std::string> markers, keys, probed_markers, child_keys;
    struct Parent {
        std::string field;
        Key key;
        std::vector<Row> rows;
    };
    std::vector<Parent> children;
    for (int index = 1; index <= p.count; ++index) {
        const auto& r = p.records[index];
        const std::string &field = r[0], &kt = r[1], &kv = r[2], &vt = r[3], &vv = r[4];
        if (probed_field(p.version, field)) {
            // A nested purchase row: kv holds the outer and inner keys as tag/value/tag/value.
            if (kt == "p") {
                if (p.version < 6 || !in(GLOBAL, field)) reject("nested path outside purchase schema");
                const auto path = split(kv, '/');
                if (path.size() != 4) reject("invalid nested path");
                const Key outer = key_of(value(path[0], path[1], true)), inner = key_of(value(path[2], path[3], true));
                const std::string id = identity(field, path[0], outer) + identity("", path[2], inner);
                if (!child_keys.insert(id).second) reject("duplicate nested key");
                auto parent = std::find_if(children.begin(), children.end(),
                                           [&](const Parent& c) { return c.field == field && same_key(c.key, outer); });
                if (parent == children.end()) parent = children.insert(children.end(), Parent{field, outer, {}});
                size_t leaves = 0;
                for (const Parent& c : children)
                    if (c.field == field) leaves += c.rows.size();
                if (parent->rows.size() >= 64 || leaves >= 128) reject("nested purchase exceeds bound");
                parent->rows.push_back(Row{inner, value(vt, vv)});
                continue;
            }
            // Records arrive sorted, so a map entry can come before its own marker; accept both.
            if (kt == "z" && kv == "-" && vt == "t" && vv == "-") {  // the field's map marker
                if (!probed_markers.insert(field).second) reject("duplicate probed map marker");
                const auto inserted = data.probed.try_emplace(field);
                if (inserted.second) inserted.first->second.kind = Probed::Map;
                if (inserted.first->second.kind != Probed::Map) reject("probed field has two shapes");
            } else if (kt == "z" && kv == "-") {  // a whole-field value
                if (const auto found = data.probed.find(field); found != data.probed.end())
                    reject(found->second.kind == Probed::Map ? "probed field has two shapes"
                                                             : "duplicate probed field");
                Probed row;
                const char marker = probed_value(vt, vv, row.value, row.text);
                if (marker == 'c') reject("a nested summary cannot describe a whole field");
                row.kind = marker == 'u' ? Probed::Unsupported : marker == 'e' ? Probed::Error : Probed::Scalar;
                data.probed[field] = std::move(row);
            } else {  // one map entry
                const Key k = key_of(value(kt, kv, true));
                if (!keys.insert(identity(field, kt, k)).second) reject("duplicate probed key");
                const auto inserted = data.probed.try_emplace(field);
                if (inserted.second) inserted.first->second.kind = Probed::Map;
                Probed& row = inserted.first->second;
                if (row.kind != Probed::Map) reject("probed field has two shapes");
                if (row.entries.size() >= 64) reject("probed map exceeds bound");
                const bool nested = vt == "t" && vv == "-";
                if (nested && (p.version < 6 || !in(GLOBAL, field))) reject("nested table outside purchase schema");
                Entry entry;
                entry.key = k;
                const char marker = nested ? 0 : probed_value(vt, vv, entry.value, entry.text);
                if (marker == 'e') reject("a field error cannot describe one entry");
                entry.kind = nested          ? Entry::Nested
                             : marker == 'u' ? Entry::Unsupported
                             : marker == 'c' ? Entry::Composition
                                             : Entry::Plain;
                row.entries.push_back(std::move(entry));
            }
        } else if (in(SCALARS, field)) {
            if (kt != "z" || kv != "-" || data.scalars.count(field)) reject("invalid/duplicate scalar");
            data.scalars[field] = value(vt, vv);
        } else if (in(MAPS, field)) {
            if (kt == "z" && kv == "-" && vt == "t" && vv == "-") {
                if (!markers.insert(field).second) reject("duplicate map marker");
                data.maps[field];
            } else {
                const Key k = key_of(value(kt, kv, true));
                if (!keys.insert(identity(field, kt, k)).second) reject("duplicate map key");
                auto& entries = data.maps[field];
                if (entries.size() >= 64) reject("map exceeds bound");
                entries.push_back(Row{k, value(vt, vv)});
            }
        } else {
            reject("unknown field");
        }
    }
    if (data.scalars.size() != std::size(SCALARS) || markers.size() != std::size(MAPS)) reject("missing schema fields");
    // Nested purchase rows go under their parent entry, which must be a nested-table marker.
    for (Parent& c : children) {
        Entry* parent = nullptr;
        if (const auto found = data.probed.find(c.field); found != data.probed.end())
            for (Entry& e : found->second.entries)
                if (same_key(e.key, c.key)) {
                    parent = &e;
                    break;
                }
        if (!parent || parent->kind != Entry::Nested) reject("nested entries without a table marker");
        parent->entries = std::move(c.rows);
    }
    if (p.version >= 3) {
        if (data.probed.size() != probed_count(p.version)) reject("missing probed fields");
        std::set<std::string> maps;
        for (const auto& [field, row] : data.probed)
            if (row.kind == Probed::Map) maps.insert(field);
        if (probed_markers != maps) reject("probed entries without a map marker");
        data.has_probed = true;
    }
    Snapshot s;
    s.sequence = sequence;
    s.source_table = p.source;
    s.update_ticket = p.ticket;
    s.generation = p.version >= 5 ? p.generation : 0;
    s.unchanged_samples = p.skipped;
    s.sha256 = sha256_hex(canonical_json(data));
    s.data = std::move(data);
    return s;
}

Data normalize(const Data& data) {
    Data result = data;
    auto by_key = [](const auto& a, const auto& b) { return key_less(a.key, b.key); };
    for (auto& entry : result.maps) std::sort(entry.second.begin(), entry.second.end(), by_key);
    for (auto& entry : result.probed) {
        for (Entry& e : entry.second.entries) std::sort(e.entries.begin(), e.entries.end(), by_key);
        std::sort(entry.second.entries.begin(), entry.second.entries.end(), by_key);
    }
    return result;
}

std::vector<std::string> model_observations(const Data& data) {
    std::vector<std::string> notes;
    std::set<std::string> planets;
    std::set<int> teams;
    static const std::vector<Row> none;
    auto rows = [&](const char* field) -> const std::vector<Row>& {
        const auto found = data.maps.find(field);
        return found == data.maps.end() ? none : found->second;
    };
    for (const Row& r : rows("planetTeam"))
        if (!r.key.integer) planets.insert(r.key.text);
    for (const Row& r : rows("teamResources"))
        if (r.key.integer) teams.insert(r.key.number);
    auto planet = [&](const Key& k) { return !k.integer && planets.count(k.text); };
    for (const char* field : {"planetTeam", "planetFleet"}) {
        for (const Row& r : rows(field)) {
            const bool number = r.value.kind == Value::Int;
            if (r.key.integer || !number)
                notes.push_back(std::string(field) + ": entry differs from observed planet-to-integer shape");
            if (!strcmp(field, "planetFleet") && !planet(r.key))
                notes.push_back(std::string(field) + ": key absent from ownership map: " + py_ascii(r.key));
            if (number && !teams.count(r.value.number) && r.value.number != 0)
                notes.push_back(std::string(field) +
                                ": value absent from resource-team keys: " + std::to_string(r.value.number));
        }
    }
    for (const char* field : {"planetBase", "lastSelected", "lastFleet"}) {
        for (const Row& r : rows(field)) {
            if (!r.key.integer || r.value.kind != Value::Text)
                notes.push_back(std::string(field) + ": entry differs from observed team-to-planet shape");
            else if (!planets.count(r.value.text))
                notes.push_back(std::string(field) +
                                ": referenced planet absent from ownership map: " + py_ascii(r.value.text));
        }
    }
    for (const Row& r : rows("teamResources"))
        if (!r.key.integer || r.value.kind != Value::Int)
            notes.push_back("teamResources: entry differs from observed integer resource shape");
    auto scalar = [&](const char* field) {
        const auto found = data.scalars.find(field);
        return found == data.scalars.end() ? Value{} : found->second;
    };
    const Value turn = scalar("turnNumber"), player = scalar("playerTeam"), target = scalar("planetNext");
    if (turn.kind != Value::Int || turn.number < 0)
        notes.push_back("turnNumber differs from observed nonnegative integer shape");
    if (player.kind != Value::Int || !teams.count(player.number))
        notes.push_back("playerTeam is not an integer key in teamResources");
    if (target.kind != Value::Null && (target.kind != Value::Text || !planets.count(target.text)))
        notes.push_back("planetNext is absent from ownership map");
    for (const auto& [field, row] : data.probed) {
        if (row.kind == Probed::Error)
            notes.push_back(field + ": producer could not represent this field: " + row.text);
        else if (row.kind == Probed::Unsupported)
            notes.push_back(field + ": value kind is not representable: " + row.text);
        else if (row.kind == Probed::Map) {
            for (const Entry& e : row.entries) {
                if (e.kind == Entry::Unsupported)
                    notes.push_back(field + ": entry value kind is not representable: " + e.text);
                else if (e.kind == Entry::Composition)
                    notes.push_back(field + ": entry holds an uncaptured nested table: " + e.text);
                else if (e.key.integer && !teams.count(e.key.number))
                    notes.push_back(field + ": key absent from resource-team keys: " + std::to_string(e.key.number));
            }
        }
    }
    std::sort(notes.begin(), notes.end());
    notes.erase(std::unique(notes.begin(), notes.end()), notes.end());
    return notes;
}

// Checks that span snapshots; the Decoder has already checked each one's fields and hash. A
// re-armed producer restarts its sequence numbers, so order is checked within a generation.
std::string Validator::add(const Snapshot& s, int version, Validated& out) {
    const int gen = version >= 5 ? s.generation : 1;
    if (gen < 1 || gen > 64 || gen < previous_generation_) return "invalid or non-increasing generation";
    if (gen > previous_generation_) {
        previous_ = 0;
        previous_generation_ = gen;
    }
    if (++counts_[gen] > max_sequence(version)) return "more than eight snapshots in one generation";
    auto integer = [](int v) { return v >= 1 && v <= INTEGER_LIMIT; };
    if (s.sequence < 1 || s.sequence > max_sequence(version) || s.sequence <= previous_ || !integer(s.source_table) ||
        !integer(s.update_ticket))
        return "invalid snapshot sequence/source/ticket";
    if (version >= 2 && (s.unchanged_samples < 0 || s.unchanged_samples > 100000))
        return "invalid unchanged-sample count";
    // Tickets count per install, so they restart with each generation.
    int& last = last_ticket_[{gen, s.source_table}];
    if (s.update_ticket <= last) return "non-increasing source update ticket";
    previous_ = s.sequence;
    last = s.update_ticket;
    out.sequence = s.sequence;
    out.source_table = s.source_table;
    out.update_ticket = s.update_ticket;
    out.generation = version >= 5 ? gen : 0;
    out.unchanged_samples = version >= 2 ? s.unchanged_samples : -1;
    out.data = normalize(s.data);
    out.normalized_sha256 = sha256_hex(canonical_json(out.data));
    out.model_observations = model_observations(out.data);
    return {};
}

std::string validate(const Decoder& decoder, std::vector<Validated>& out) {
    if (decoder.snapshots().empty()) return "expected at least one complete snapshot";
    Validator validator;
    for (const Snapshot& s : decoder.snapshots()) {
        Validated v;
        std::string reason = validator.add(s, decoder.schema_version(), v);
        if (!reason.empty()) return reason;
        out.push_back(std::move(v));
    }
    return {};
}

// Validates the snapshots the line completes, then drops them from the decoder to bound memory.
bool Stream::line(std::string_view text, std::vector<Validated>& out) {
    if (stopped()) return false;
    decoder_.line(text);
    if (!decoder_.rejected().empty()) {
        error_ = "capture transport rejected: " + decoder_.rejected().front();
        return false;
    }
    while (sent_ < decoder_.snapshots().size()) {
        Validated v;
        std::string reason = validator_.add(decoder_.snapshots()[sent_++], decoder_.schema_version(), v);
        if (!reason.empty()) {
            error_ = reason;
            return false;
        }
        out.push_back(std::move(v));
    }
    decoder_.drop_snapshots();
    sent_ = 0;
    return true;
}

bool Stream::tag(std::string_view tag, std::vector<Validated>& out) {
    std::string text = " CHANNEL ";
    text.append(tag);
    for (std::string_view piece : split_lines(text, false))
        if (!line(piece, out)) return false;
    return !stopped();
}

namespace {
// A parsed JSON value. Canonical text has no whitespace and only printable ASCII, so the reader
// refuses anything else early; parse_data still compares the exact text at the end.
struct Node {
    enum Kind : unsigned char { Null, Bool, Int, Text, Array, Object } kind = Null;
    bool flag = false;
    long long number = 0;
    std::string text;
    std::vector<Node> items;
    std::vector<std::pair<std::string, Node>> fields;
    const Node* get(std::string_view name) const {
        for (const auto& f : fields)
            if (f.first == name) return &f.second;
        return nullptr;
    }
};

// Appends a code point as UTF-8.
void append_utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Strict reader for canonical JSON: no whitespace, integers only, at most eight levels deep.
class Reader {
public:
    explicit Reader(std::string_view text) : text_(text) {}
    Node document() {
        Node root = value(0);
        if (at_ != text_.size()) reject("malformed JSON");
        return root;
    }

private:
    std::string_view text_;
    size_t at_ = 0;
    char peek() const { return at_ < text_.size() ? text_[at_] : '\0'; }
    void expect(char c) {
        if (peek() != c) reject("malformed JSON");
        ++at_;
    }
    bool word(std::string_view w) {
        if (text_.substr(at_, w.size()) != w) return false;
        at_ += w.size();
        return true;
    }
    Node value(int depth) {
        if (depth > 8) reject("JSON nested too deeply");
        Node n;
        if (peek() == '{') {
            ++at_;
            n.kind = Node::Object;
            if (peek() == '}') {
                ++at_;
                return n;
            }
            for (;;) {
                std::string name = string();
                expect(':');
                if (n.get(name)) reject("duplicate JSON field");
                n.fields.emplace_back(std::move(name), value(depth + 1));
                if (peek() == ',') {
                    ++at_;
                    continue;
                }
                expect('}');
                return n;
            }
        }
        if (peek() == '[') {
            ++at_;
            n.kind = Node::Array;
            if (peek() == ']') {
                ++at_;
                return n;
            }
            for (;;) {
                n.items.push_back(value(depth + 1));
                if (peek() == ',') {
                    ++at_;
                    continue;
                }
                expect(']');
                return n;
            }
        }
        if (peek() == '"') {
            n.kind = Node::Text;
            n.text = string();
            return n;
        }
        if (word("null")) return n;
        if (word("true")) {
            n.kind = Node::Bool;
            n.flag = true;
            return n;
        }
        if (word("false")) {
            n.kind = Node::Bool;
            return n;
        }
        // -?(0|[1-9][0-9]*) with at most nine digits, more than any later bound needs.
        const size_t start = at_;
        if (peek() == '-') ++at_;
        size_t digits = 0;
        while (digit(peek())) {
            ++at_;
            ++digits;
        }
        if (!digits || digits > 9 || (digits > 1 && text_[at_ - digits] == '0')) reject("malformed JSON");
        n.kind = Node::Int;
        n.number = std::stoll(std::string(text_.substr(start, at_ - start)));
        return n;
    }
    unsigned hex4() {
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = peek();
            ++at_;
            if (digit(c)) v = v * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = v * 16 + (c - 'A' + 10);
            else reject("malformed JSON escape");
        }
        return v;
    }
    std::string string() {
        expect('"');
        std::string out;
        for (;;) {
            if (at_ >= text_.size()) reject("malformed JSON");
            const unsigned char c = static_cast<unsigned char>(text_[at_++]);
            if (c == '"') return out;
            if (c < 0x20 || c >= 0x7F) reject("snapshot JSON is not in canonical form");
            if (c != '\\') {
                out += static_cast<char>(c);
                continue;
            }
            const char e = peek();
            ++at_;
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                // Surrogates only as complete pairs, so the text is always valid UTF-8.
                unsigned cp = hex4();
                if (cp >= 0xD800 && cp < 0xDC00) {
                    if (!word("\\u")) reject("unpaired surrogate in JSON text");
                    const unsigned low = hex4();
                    if (low < 0xDC00 || low > 0xDFFF) reject("unpaired surrogate in JSON text");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp < 0xE000) reject("unpaired surrogate in JSON text");
                append_utf8(out, cp);
                break;
            }
            default: reject("malformed JSON escape");
            }
        }
    }
};

// Requires an object with exactly these fields.
void exact(const Node& n, std::initializer_list<std::string_view> names, const std::string& label) {
    if (n.kind != Node::Object || n.fields.size() != names.size()) reject(label + ": unexpected or missing fields");
    for (std::string_view name : names)
        if (!n.get(name)) reject(label + ": unexpected or missing fields");
}

// A captured primitive: nil, boolean, integer within INTEGER_LIMIT, or text of up to limit bytes.
Value primitive(const Node& n, const std::string& label, size_t limit = 64) {
    Value v;
    if (n.kind == Node::Null) return v;
    if (n.kind == Node::Bool) {
        v.kind = Value::Bool;
        v.flag = n.flag;
        return v;
    }
    if (n.kind == Node::Int && n.number >= -INTEGER_LIMIT && n.number <= INTEGER_LIMIT) {
        v.kind = Value::Int;
        v.number = static_cast<int>(n.number);
        return v;
    }
    if (n.kind == Node::Text && n.text.size() <= limit) {
        v.kind = Value::Text;
        v.text = n.text;
        return v;
    }
    reject(label + ": value violates captured primitive type/size bounds");
}

// Non-empty text of up to limit bytes.
std::string short_text(const Node* n, const std::string& label, size_t limit = 32) {
    if (!n || n->kind != Node::Text || n->text.empty() || n->text.size() > limit)
        reject(label + ": expected bounded non-empty text");
    return n->text;
}

// A typed key from key_type and key. The same key twice in one map is refused.
Key typed_key(const Node& row, const std::string& label, std::set<std::string>& seen) {
    const Node *type = row.get("key_type"), *key = row.get("key");
    Key k;
    if (type && type->kind == Node::Text && type->text == "integer") {
        if (!key || key->kind != Node::Int || key->number < -INTEGER_LIMIT || key->number > INTEGER_LIMIT)
            reject(label + ": invalid integer key");
        k.integer = true;
        k.number = static_cast<int>(key->number);
    } else if (type && type->kind == Node::Text && type->text == "string") {
        if (!key || key->kind != Node::Text || key->text.size() > 32) reject(label + ": invalid string key");
        k.text = key->text;
    } else reject(label + ": unknown key type");
    if (!seen.insert(identity(label, "", k)).second) reject(label + ": duplicate typed key");
    return k;
}

// A map row: exactly key_type, key and value.
Row row_of(const Node& n, const std::string& label, std::set<std::string>& seen) {
    exact(n, {"key_type", "key", "value"}, label);
    Row r;
    r.key = typed_key(n, label, seen);
    r.value = primitive(*n.get("value"), label);
    return r;
}

// One probed field: a scalar, an unsupported kind, an error or a map.
Probed probed_of(const std::string& field, const Node& n) {
    const Node* kind = n.get("kind");
    if (n.kind != Node::Object || !kind || kind->kind != Node::Text) reject(field + ": unknown probed shape");
    Probed p;
    if (kind->text == "scalar") {
        exact(n, {"kind", "value"}, field);
        p.kind = Probed::Scalar;
        p.value = primitive(*n.get("value"), field);
    } else if (kind->text == "unsupported") {
        exact(n, {"kind", "type"}, field);
        p.kind = Probed::Unsupported;
        p.text = short_text(n.get("type"), field);
    } else if (kind->text == "error") {
        exact(n, {"kind", "reason"}, field);
        p.kind = Probed::Error;
        p.text = short_text(n.get("reason"), field);
    } else if (kind->text == "map") {
        exact(n, {"kind", "entries"}, field);
        const Node& rows = *n.get("entries");
        if (rows.kind != Node::Array || rows.items.size() > 64) reject(field + ": probed map exceeds bounds");
        p.kind = Probed::Map;
        std::set<std::string> seen;
        size_t leaves = 0;
        for (const Node& e : rows.items) {
            // Besides its key, an entry has exactly one of value, unsupported, composition or entries.
            if (e.kind != Node::Object || e.fields.size() != 3 || !e.get("key_type") || !e.get("key"))
                reject(field + ": unexpected probed entry fields");
            Entry entry;
            entry.key = typed_key(e, field, seen);
            if (const Node* v = e.get("value")) {
                entry.kind = Entry::Plain;
                entry.value = primitive(*v, field);
            } else if (e.get("unsupported")) {
                entry.kind = Entry::Unsupported;
                entry.text = short_text(e.get("unsupported"), field);
            } else if (e.get("composition")) {
                entry.kind = Entry::Composition;
                entry.text = short_text(e.get("composition"), field, 48);
            } else if (const Node* children = e.get("entries")) {
                if (!in(GLOBAL, field) || children->kind != Node::Array || children->items.size() > 64)
                    reject("nested entry outside purchase bounds");
                entry.kind = Entry::Nested;
                std::set<std::string> child_seen;
                for (const Node& c : children->items) {
                    if (++leaves > 128) reject("nested purchase exceeds bound");
                    entry.entries.push_back(row_of(c, field, child_seen));
                }
            } else reject(field + ": unexpected probed entry fields");
            p.entries.push_back(std::move(entry));
        }
    } else reject(field + ": unknown probed shape");
    return p;
}
}

// producer decides which probed fields must be present, as it does for the Decoder.
std::string parse_data(std::string_view json, int producer, Data& out) {
    try {
        const Node root = Reader(json).document();
        const bool has_probed = root.kind == Node::Object && root.get("probed");
        if (has_probed) exact(root, {"scalars", "maps", "probed"}, "data");
        else exact(root, {"scalars", "maps"}, "data");
        if (has_probed != (producer >= 3)) reject("probed fields do not match the capture schema version");
        Data data;
        const Node& scalars = *root.get("scalars");
        if (scalars.kind != Node::Object || scalars.fields.size() != std::size(SCALARS))
            reject("scalars: unexpected or missing fields");
        for (std::string_view field : SCALARS) {
            const Node* v = scalars.get(field);
            if (!v) reject("scalars: unexpected or missing fields");
            data.scalars[std::string(field)] = primitive(*v, std::string(field));
        }
        const Node& maps = *root.get("maps");
        if (maps.kind != Node::Object || maps.fields.size() != std::size(MAPS))
            reject("maps: unexpected or missing fields");
        for (std::string_view name : MAPS) {
            const std::string field(name);
            const Node* rows = maps.get(name);
            if (!rows) reject("maps: unexpected or missing fields");
            if (rows->kind != Node::Array || rows->items.size() > 64) reject(field + ": map exceeds bounds");
            std::set<std::string> seen;
            auto& target = data.maps[field];
            for (const Node& r : rows->items) target.push_back(row_of(r, field, seen));
        }
        if (has_probed) {
            const Node& probed = *root.get("probed");
            if (probed.kind != Node::Object || probed.fields.size() != probed_count(producer))
                reject("probed fields do not match the capture schema version");
            for (const auto& [field, row] : probed.fields) {
                if (!probed_field(producer, field)) reject("probed fields do not match the capture schema version");
                Probed p = probed_of(field, row);
                if (producer < 6)
                    for (const Entry& e : p.entries)
                        if (e.kind == Entry::Nested) reject("nested purchase data requires producer 6");
                data.probed[field] = std::move(p);
            }
            data.has_probed = true;
        }
        Data normalized = normalize(data);
        if (canonical_json(normalized) != json) reject("snapshot JSON is not in canonical form");
        out = std::move(normalized);
        return {};
    } catch (const Reject& error) { return error.what(); } catch (const std::exception&) {
        return "snapshot JSON could not be read";
    }
}
}
