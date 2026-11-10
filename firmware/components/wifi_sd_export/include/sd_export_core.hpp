#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace demo::sd_export {
inline constexpr size_t MAX_PATH_BYTES = 1024;
inline constexpr size_t MAX_DIRECTORY_DEPTH = 16;

enum class FileKind { Regular, Directory, Missing, Error };
enum class DeleteResult { Deleted, InvalidPath, MatchRequired, MatchFailed, Missing, NotFile, IoError };

inline int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline bool valid_utf8(std::string_view text) {
    size_t i = 0;
    while (i < text.size()) {
        uint32_t cp = static_cast<uint8_t>(text[i++]);
        size_t extra = 0;
        uint32_t minimum = 0;
        if (cp < 0x80) { if (!cp) return false; continue; }
        if (cp >= 0xc2 && cp <= 0xdf) { extra = 1; minimum = 0x80; cp &= 0x1f; }
        else if (cp >= 0xe0 && cp <= 0xef) { extra = 2; minimum = 0x800; cp &= 0x0f; }
        else if (cp >= 0xf0 && cp <= 0xf4) { extra = 3; minimum = 0x10000; cp &= 0x07; }
        else return false;
        if (text.size() - i < extra) return false;
        for (size_t n = 0; n < extra; ++n) {
            uint8_t next = static_cast<uint8_t>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (next & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
}

inline bool valid_relative_path(std::string_view path) {
    if (path.empty() || path.size() > MAX_PATH_BYTES || path.front() == '/' ||
        path.back() == '/' || !valid_utf8(path)) return false;
    size_t begin = 0, directories = 0;
    while (begin < path.size()) {
        size_t end = path.find('/', begin);
        if (end == std::string_view::npos) end = path.size();
        auto part = path.substr(begin, end - begin);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') return false;
        for (char c : part) if (c == '\\' || c == ':' || static_cast<uint8_t>(c) < 0x20) return false;
        if (end != path.size() && ++directories > MAX_DIRECTORY_DEPTH) return false;
        begin = end + 1;
    }
    return true;
}

// Decode exactly once; '+' is a literal plus in this path query. Encoded '/'
// is allowed, but the resulting complete path must pass segment validation.
inline bool decode_path(std::string_view encoded, std::string& decoded) {
    decoded.clear();
    if (encoded.size() > MAX_PATH_BYTES * 3) return false;
    for (size_t i = 0; i < encoded.size(); ++i) {
        char c = encoded[i];
        if (c == '%') {
            if (i + 2 >= encoded.size()) return false;
            int hi = hex_digit(encoded[i + 1]), lo = hex_digit(encoded[i + 2]);
            if (hi < 0 || lo < 0) return false;
            c = static_cast<char>((hi << 4) | lo); i += 2;
        }
        decoded.push_back(c);
        if (decoded.size() > MAX_PATH_BYTES) return false;
    }
    return valid_relative_path(decoded);
}

// The caller already owns a paused/authenticated SD session. Keep the destructive
// operation behind path, precondition and regular-file checks, in that order.
template <typename Inspect, typename Remove>
DeleteResult delete_regular_file(std::string_view path, std::string_view if_match,
                                 std::string_view current_etag, Inspect inspect, Remove remove) {
    if (!valid_relative_path(path)) return DeleteResult::InvalidPath;
    if (if_match.empty()) return DeleteResult::MatchRequired;
    if (if_match != current_etag) return DeleteResult::MatchFailed;
    switch (inspect()) {
        case FileKind::Missing: return DeleteResult::Missing;
        case FileKind::Directory: return DeleteResult::NotFile;
        case FileKind::Error: return DeleteResult::IoError;
        case FileKind::Regular: break;
    }
    return remove() ? DeleteResult::Deleted : DeleteResult::IoError;
}

inline std::string json_string(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (char c : value) {
        uint8_t byte = static_cast<uint8_t>(c);
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (byte < 0x20) { out += "\\u00"; out += hex[byte >> 4]; out += hex[byte & 15]; }
        else out += c;
    }
    out += '"';
    return out;
}

inline bool parse_u64(std::string_view text, uint64_t& value) {
    if (text.empty()) return false;
    value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        unsigned digit = static_cast<unsigned>(c - '0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return true;
}

enum class RangeResult { Full, Partial, Unsatisfiable, Invalid };
struct ByteRange { uint64_t first{0}, last{0}, length{0}; };
inline RangeResult parse_range(std::string_view header, uint64_t size, ByteRange& range) {
    range = {0, size ? size - 1 : 0, size};
    if (header.empty()) return RangeResult::Full;
    constexpr std::string_view prefix = "bytes=";
    if (header.substr(0, prefix.size()) != prefix) return RangeResult::Invalid;
    auto spec = header.substr(prefix.size());
    size_t dash = spec.find('-');
    if (dash == std::string_view::npos || spec.find('-', dash + 1) != std::string_view::npos ||
        spec.find(',') != std::string_view::npos) return RangeResult::Invalid;
    auto left = spec.substr(0, dash), right = spec.substr(dash + 1);
    uint64_t first = 0, last = 0;
    if (left.empty()) {
        uint64_t suffix = 0;
        if (!parse_u64(right, suffix)) return RangeResult::Invalid;
        if (!size || !suffix) return RangeResult::Unsatisfiable;
        first = suffix < size ? size - suffix : 0;
        last = size - 1;
    } else {
        if (!parse_u64(left, first)) return RangeResult::Invalid;
        if (!right.empty() && !parse_u64(right, last)) return RangeResult::Invalid;
        if (!size || first >= size) return RangeResult::Unsatisfiable;
        if (right.empty() || last >= size) last = size - 1;
        if (last < first) return RangeResult::Unsatisfiable;
    }
    range = {first, last, last - first + 1};
    return RangeResult::Partial;
}

inline std::string file_record(std::string_view path, uint64_t size,
                               std::string_view sha256, std::string_view etag) {
    return "{\"type\":\"file\",\"path\":" + json_string(path) + ",\"size\":" +
        std::to_string(size) + ",\"sha256\":" + json_string(sha256) +
        ",\"etag\":" + json_string(etag) + "}\n";
}

struct CompletionReceipt {
    std::string session_id, manifest_sha256;
    uint64_t file_count{0}, total_bytes{0};
};
// Deliberately parse integer tokens as uint64_t, rather than through a JSON
// library's double representation. Receipts have exactly these four fields.
inline bool parse_receipt(std::string_view text, CompletionReceipt& result) {
    size_t pos = 0;
    auto space = [&] { while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
        text[pos] == '\r' || text[pos] == '\n')) ++pos; };
    auto quoted = [&](std::string& value) {
        value.clear();
        if (pos >= text.size() || text[pos++] != '"') return false;
        while (pos < text.size()) {
            char c = text[pos++];
            if (c == '"') return true;
            if (static_cast<uint8_t>(c) < 0x20) return false;
            if (c == '\\') {
                if (pos >= text.size()) return false;
                c = text[pos++];
                if (c != '"' && c != '\\' && c != '/') return false;
            }
            value += c;
        }
        return false;
    };
    CompletionReceipt parsed;
    unsigned seen = 0;
    space();
    if (pos >= text.size() || text[pos++] != '{') return false;
    for (;;) {
        space(); std::string key;
        if (!quoted(key)) return false;
        space(); if (pos >= text.size() || text[pos++] != ':') return false;
        space(); unsigned bit = 0;
        if (key == "session_id" || key == "manifest_sha256") {
            bit = key == "session_id" ? 1U : 2U;
            std::string value;
            if (!quoted(value)) return false;
            if (bit == 1) parsed.session_id = value; else parsed.manifest_sha256 = value;
        } else if (key == "file_count" || key == "total_bytes") {
            bit = key == "file_count" ? 4U : 8U;
            size_t begin = pos;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
            uint64_t value = 0;
            auto token = text.substr(begin, pos - begin);
            if ((token.size() > 1 && token.front() == '0') || !parse_u64(token, value)) return false;
            if (bit == 4) parsed.file_count = value; else parsed.total_bytes = value;
        } else return false;
        if (seen & bit) return false;
        seen |= bit; space();
        if (pos >= text.size()) return false;
        char separator = text[pos++];
        if (separator == '}') break;
        if (separator != ',') return false;
    }
    space();
    if (seen != 15 || pos != text.size() || parsed.session_id.size() != 16 ||
        parsed.manifest_sha256.size() != 64) return false;
    for (char c : parsed.session_id) if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
    for (char c : parsed.manifest_sha256) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    result = parsed;
    return true;
}
}  // namespace demo::sd_export
