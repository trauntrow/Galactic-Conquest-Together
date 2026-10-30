// Command-line driver for the capture decoder and save candidate code. Its modes print decoded JSON,
// write candidate files for the Lua tests, and check the recorded fixtures.
#include "../src/capture_decode.h"
#include "../src/save_candidate.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

using namespace gc::capture;
namespace fs = std::filesystem;

// A whole file, byte for byte.
static std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A decoded snapshot as JSON.
static std::string snapshot_json(const Snapshot& s) {
    std::ostringstream out;
    out << "{\"sequence\":" << s.sequence << ",\"source_table\":" << s.source_table
        << ",\"update_ticket\":" << s.update_ticket;
    if (s.generation) out << ",\"generation\":" << s.generation;
    if (s.unchanged_samples >= 0) out << ",\"unchanged_samples\":" << s.unchanged_samples;
    out << ",\"sha256\":\"" << s.sha256 << "\",\"data\":" << canonical_json(s.data) << '}';
    return out.str();
}

// A validated snapshot as JSON, with its model observations.
static std::string validated_json(const Validated& v) {
    std::ostringstream out;
    out << "{\"sequence\":" << v.sequence << ",\"source_table\":" << v.source_table
        << ",\"update_ticket\":" << v.update_ticket;
    if (v.generation) out << ",\"generation\":" << v.generation;
    if (v.unchanged_samples >= 0) out << ",\"unchanged_samples\":" << v.unchanged_samples;
    out << ",\"normalized_sha256\":\"" << v.normalized_sha256 << "\",\"data\":" << canonical_json(v.data)
        << ",\"model_observations\":[";
    for (size_t i = 0; i < v.model_observations.size(); ++i)
        out << (i ? "," : "") << json_text(v.model_observations[i]);
    out << "]}";
    return out.str();
}

// decode <log>: decodes and validates a whole log and prints the result as one JSON document.
static void decode(const fs::path& path) {
    const std::string text = read(path);
    Decoder decoder;
    for (std::string_view line : split_lines(text, true)) decoder.line(line);
    decoder.finish();
    std::string out = "{\"schema_version\":" + std::to_string(decoder.schema_version()) + ",\"snapshots\":[";
    for (size_t i = 0; i < decoder.snapshots().size(); ++i)
        out += (i ? "," : "") + snapshot_json(decoder.snapshots()[i]);
    out += "],\"rejected\":[";
    for (size_t i = 0; i < decoder.rejected().size(); ++i)
        out += (i ? "," : "") + json_text(decoder.rejected()[i], false);
    std::vector<Validated> validated;
    const std::string reason = validate(decoder, validated);
    if (!reason.empty()) out += "],\"validated\":{\"error\":" + json_text(reason, false) + '}';
    else {
        out += "],\"validated\":{\"snapshots\":[";
        for (size_t i = 0; i < validated.size(); ++i) out += (i ? "," : "") + validated_json(validated[i]);
        out += "]}";
    }
    puts((out + '}').c_str());
}

// stream <log>: feeds the log line by line to a capture Stream and prints the hashes it would send.
static void stream(const fs::path& path) {
    const std::string text = read(path);
    Stream live;
    std::vector<Validated> sent;
    for (std::string_view line : split_lines(text, true))
        if (!live.line(line, sent)) break;
    std::string out = "{\"snapshots\":[";
    for (size_t i = 0; i < sent.size(); ++i) out += std::string(i ? "," : "") + '"' + sent[i].normalized_sha256 + '"';
    out += "],\"error\":" + (live.stopped() ? json_text(live.error(), false) : std::string("null")) + '}';
    puts(out.c_str());
}

// parse <producer> <json>: checks a received snapshot's JSON as the friend's PC does.
static void parse(int producer, const fs::path& path) {
    Data data;
    const std::string reason = parse_data(read(path), producer, data);
    puts(("{\"ok\":" + std::string(reason.empty() ? "true" : "false") + ",\"reason\":" + json_text(reason, false) + '}')
             .c_str());
}

// candidate <producer> <json>: the friend's native save candidate for a received snapshot, as hex.
static void candidate(int producer, const fs::path& path) {
    Data data;
    std::string reason = parse_data(read(path), producer, data), wire;
    if (reason.empty()) reason = gc::candidate::guest_wire(data, producer, wire);
    std::string hex;
    char b[3];
    for (unsigned char c : wire) {
        sprintf_s(b, "%02x", c);
        hex += b;
    }
    puts(("{\"ok\":" + std::string(reason.empty() ? "true" : "false") + ",\"reason\":" + json_text(reason, false) +
          ",\"wire\":\"" + hex + "\"}")
             .c_str());
}

// wire <log> <index> <out>: writes the candidate for one validated snapshot, for the Lua tests.
static int wire_file(const fs::path& log, int index, const fs::path& out) {
    Decoder decoder;
    const std::string text = read(log);
    for (std::string_view line : split_lines(text, true)) decoder.line(line);
    std::vector<Validated> validated;
    if (!validate(decoder, validated).empty() || index < 0 || index >= static_cast<int>(validated.size())) return 1;
    std::string wire;
    const std::string reason = gc::candidate::guest_wire(validated[index].data, decoder.schema_version(), wire);
    if (!reason.empty()) {
        fprintf(stderr, "capture_parity: %s\n", reason.c_str());
        return 1;
    }
    std::ofstream(out, std::ios::binary).write(wire.data(), static_cast<std::streamsize>(wire.size()));
    return 0;
}

// Every value of "<name>": "<64 hex>" in a recorded JSON file, in order.
static std::vector<std::string> hashes(const std::string& json, const std::string& name) {
    std::vector<std::string> found;
    const std::string needle = '"' + name + "\": \"";
    for (size_t at = json.find(needle); at != std::string::npos; at = json.find(needle, at + 1))
        found.push_back(json.substr(at + needle.size(), 64));
    return found;
}

// fixtures <dir>: every fixture log must decode, validate and stream to its recorded hashes.
static int fixtures(const fs::path& dir) {
    int compared = 0, failed = 0;
    for (const auto& item : fs::directory_iterator(dir)) {
        const fs::path log = item.path();
        if (log.extension() != ".log") continue;
        std::string base = log.stem().string();
        if (base.size() > 7 && base.compare(base.size() - 7, 7, "-galaxy") == 0) base.resize(base.size() - 7);
        const fs::path decoded = dir / (base + ".json"), report = dir / (base + "-validation.json");
        if (!fs::exists(decoded)) continue;
        const std::string text = read(log);
        Decoder decoder;
        // Fed as the DLL feeds it: the CHANNEL tags alone, not whole lines.
        Stream live;
        std::vector<Validated> sent;
        for (std::string_view line : split_lines(text, true)) {
            decoder.line(line);
            const size_t at = line.find(" CHANNEL ");
            if (at != std::string_view::npos) live.tag(line.substr(at + 9), sent);
        }
        decoder.finish();
        std::vector<std::string> mine, live_hashes;
        for (const Snapshot& s : decoder.snapshots()) mine.push_back(s.sha256);
        for (const Validated& v : sent) live_hashes.push_back(v.normalized_sha256);
        std::vector<Validated> validated;
        const std::string reason = validate(decoder, validated);
        std::vector<std::string> normalized;
        for (const Validated& v : validated) normalized.push_back(v.normalized_sha256);
        bool ok = decoder.rejected().empty() && reason.empty() && !live.stopped() &&
                  mine == hashes(read(decoded), "sha256") && live_hashes == normalized;
        if (fs::exists(report)) ok = ok && normalized == hashes(read(report), "normalized_sha256");
        // What a host sends, a friend accepts unchanged.
        for (const Validated& v : validated) {
            Data received;
            const std::string json = canonical_json(v.data);
            ok = ok && parse_data(json, decoder.schema_version(), received).empty() && canonical_json(received) == json;
        }
        printf("%s %s: %zu snapshots\n", ok ? "PASS" : "FAIL", log.filename().string().c_str(), mine.size());
        ++compared;
        failed += !ok;
    }
    // Ten logs have recorded output; fewer means the fixtures moved.
    if (compared < 10) {
        printf("FAIL: only %d fixture logs have recorded output\n", compared);
        return 1;
    }
    if (failed) return 1;
    printf("capture_parity PASS: %d fixture logs decode, validate and stream to the recorded hashes\n", compared);
    return 0;
}

// Runs one mode; exit code 2 means bad arguments, 3 an error while decoding.
int main(int argc, char** argv) {
    const std::string mode = argc > 2 ? argv[1] : "";
    if (mode != "decode" && mode != "stream" && !(mode == "fixtures" && argc == 3) &&
        !((mode == "parse" || mode == "candidate") && argc > 3) && !(mode == "wire" && argc == 5)) {
        fputs("usage: capture_parity decode|stream <log>... | parse|candidate <producer> <json>... |"
              " wire <log> <index> <out> | fixtures <dir>\n",
              stderr);
        return 2;
    }
    try {
        if (mode == "fixtures") return fixtures(argv[2]);
        if (mode == "wire") return wire_file(argv[2], atoi(argv[3]), argv[4]);
        if (mode == "parse" || mode == "candidate") {
            for (int i = 3; i < argc; ++i)
                mode == "parse" ? parse(atoi(argv[2]), argv[i]) : candidate(atoi(argv[2]), argv[i]);
            return 0;
        }
        // One JSON document per line, one line per log.
        for (int i = 2; i < argc; ++i) mode == "decode" ? decode(argv[i]) : stream(argv[i]);
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "capture_parity: %s\n", error.what());
        return 3;
    }
    return 2;
}
