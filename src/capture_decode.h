#pragma once
// Decoding and validation of the host's campaign capture: the typed records campaign_capture.lua
// sends over the GCOBS channel, rebuilt into galaxy snapshots. Pure data; nothing here touches the game.
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gc::capture {

// A captured primitive: nil, boolean, bounded integer or bounded UTF-8 text.
struct Value {
    enum Kind : unsigned char { Null, Bool, Int, Text } kind = Null;
    bool flag = false;
    int number = 0;
    std::string text;
};

// A typed map key, integer or string.
struct Key {
    bool integer = false;
    int number = 0;
    std::string text;
};

// A map entry, or one entry of a nested purchase table.
struct Row {
    Key key;
    Value value;
};

// One entry of a probed map: a value, the name of a kind that cannot be represented, a summary of
// a nested table, or nested purchase rows.
struct Entry {
    enum Kind : unsigned char { Plain, Unsupported, Composition, Nested } kind = Plain;
    Key key;
    Value value;
    std::string text;
    std::vector<Row> entries;
};

// A field whose shape the producer finds out at capture time.
struct Probed {
    enum Kind : unsigned char { Scalar, Map, Unsupported, Error } kind = Scalar;
    Value value;
    std::string text;
    std::vector<Entry> entries;
};

// One snapshot's fields. Probed fields exist from producer 3 on.
struct Data {
    std::map<std::string, Value> scalars;
    std::map<std::string, std::vector<Row>> maps;
    bool has_probed = false;
    std::map<std::string, Probed> probed;
};

// A decoded snapshot. generation is 0 before producer 5 and unchanged_samples is -1 for producer 1,
// the versions without those fields.
struct Snapshot {
    int sequence = 0, source_table = 0, update_ticket = 0, generation = 0, unchanged_samples = -1;
    std::string sha256;
    Data data;
};

// A snapshot that passed validation, with normalized data, its hash and any shape observations.
struct Validated {
    int sequence = 0, source_table = 0, update_ticket = 0, generation = 0, unchanged_samples = -1;
    std::string normalized_sha256;
    Data data;
    std::vector<std::string> model_observations;
};

// Rebuilds snapshots from log lines one at a time, so a live feed and a whole log decode the same.
class Decoder {
public:
    void line(std::string_view text);
    // End of input: reports transactions that never ended.
    void finish();
    int schema_version() const { return producer_ ? producer_ : 1; }
    const std::vector<Snapshot>& snapshots() const { return complete_; }
    const std::vector<std::string>& rejected() const { return rejected_; }
    // Forgets snapshots already handed on, so a long live stream stays bounded.
    void drop_snapshots() { complete_.clear(); }

    // A snapshot whose begin record has arrived and whose end has not.
    struct Pending {
        int count = 0, source = 0, ticket = 0, skipped = -1, version = 0, generation = 0;
        std::map<int, std::vector<std::string>> records;
        bool invalid = false;
    };

private:
    void record(const std::vector<std::string>& parts);
    Snapshot build(int sequence, Pending& p);
    void invalidate();
    std::map<int, Pending> pending_;
    std::vector<Snapshot> complete_;
    std::vector<std::string> rejected_;
    std::vector<int> seen_;
    int producer_ = 0, generation_ = 1, installs_ = 0;
};

// Checks snapshots in order. add() fills out and returns "", or returns why the snapshot is refused.
// version is the producer version, fixed once the first snapshot begins.
class Validator {
public:
    std::string add(const Snapshot& snapshot, int version, Validated& out);

private:
    int previous_ = 0, previous_generation_ = 1;
    std::map<int, int> counts_;
    std::map<std::pair<int, int>, int> last_ticket_;
};

// Validates every decoded snapshot of a whole log, which must hold at least one.
std::string validate(const Decoder& decoder, std::vector<Validated>& out);

// Decodes and validates channel lines as they arrive, and stops for good at the first rejection.
class Stream {
public:
    // Takes one log line and appends the snapshots it completes, once validated.
    bool line(std::string_view text, std::vector<Validated>& out);
    // Takes a tag from find_observer, starting at "GCOBS:", split into lines as the log would hold it.
    bool tag(std::string_view tag, std::vector<Validated>& out);
    bool stopped() const { return !error_.empty(); }
    const std::string& error() const { return error_; }
    int version() const { return decoder_.schema_version(); }

private:
    Decoder decoder_;
    Validator validator_;
    size_t sent_ = 0;
    std::string error_;
};

// Compact JSON with sorted keys and ASCII-only text: the form that is hashed and sent.
std::string canonical_json(const Data& data);
// A JSON string with non-ASCII characters escaped. utf8=false reads the bytes as Latin-1, for raw
// log text.
std::string json_text(std::string_view text, bool utf8 = true);
std::string sha256_hex(std::string_view bytes);
// Sorts every map by key: integer keys first, then string keys.
Data normalize(const Data& data);
// Notes where the data departs from the shapes a galaxy normally has. Informational only.
std::vector<std::string> model_observations(const Data& normalized);
// Parses a snapshot received from the host. The text must be exactly canonical_json() of the
// normalized result, so one state has one encoding and one hash. Returns "" or why it is refused.
std::string parse_data(std::string_view json, int producer, Data& out);
// Splits at CR, LF, CRLF, VT, FF and bytes 0x1C to 0x1E; nel also splits at 0x85 (NEL).
std::vector<std::string_view> split_lines(std::string_view text, bool nel);
}
