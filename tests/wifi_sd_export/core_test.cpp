#include "sd_export_core.hpp"
#include <cassert>
#include <iostream>
#include <string>

using namespace demo::sd_export;
int main() {
    std::string path;
    assert(decode_path("nested%2F.hidden%20file.wav", path) && path == "nested/.hidden file.wav");
    assert(decode_path("%E8%AA%9E%E9%9F%B3%2B1.wav", path) && path == "\xE8\xAA\x9E\xE9\x9F\xB3+1.wav");
    assert(decode_path("a+b.wav", path) && path == "a+b.wav");
    assert(decode_path("%252e%252e/file", path) && path == "%2e%2e/file");
    for (const auto* text : {"", "/sd/a", "%2Fsd%2Fa", "../file", "a/../file", "a/%2E%2E/file",
             "a/%2e%2e%20/file", "a/%2e/file", "a//file", "a/", "a\\file", "C:foo", "a%00b",
             "a%0Ab", "%c0%af", "%ed%a0%80", "%f4%90%80%80", "%80", "%e8%aa", "a%", "a%0x"})
        assert(!decode_path(text, path));
    assert(valid_relative_path(std::string(MAX_PATH_BYTES, 'a')));
    assert(!valid_relative_path(std::string(MAX_PATH_BYTES + 1, 'a')));
    std::string deep;
    for (size_t i = 0; i < MAX_DIRECTORY_DEPTH; ++i) deep += "d/";
    assert(valid_relative_path(deep + "file"));
    assert(!valid_relative_path(deep + "extra/file"));

    ByteRange range;
    assert(parse_range("", 100, range) == RangeResult::Full && range.length == 100);
    assert(parse_range("bytes=20-", 100, range) == RangeResult::Partial && range.first == 20 && range.last == 99 && range.length == 80);
    assert(parse_range("bytes=20-30", 100, range) == RangeResult::Partial && range.length == 11);
    assert(parse_range("bytes=90-999", 100, range) == RangeResult::Partial && range.last == 99);
    assert(parse_range("bytes=-20", 100, range) == RangeResult::Partial && range.first == 80);
    assert(parse_range("bytes=-200", 100, range) == RangeResult::Partial && range.first == 0 && range.length == 100);
    assert(parse_range("", 0, range) == RangeResult::Full && range.length == 0);
    assert(parse_range("bytes=0-", 0, range) == RangeResult::Unsatisfiable);
    assert(parse_range("bytes=100-", 100, range) == RangeResult::Unsatisfiable);
    assert(parse_range("bytes=-0", 100, range) == RangeResult::Unsatisfiable);
    assert(parse_range("bytes=20-10", 100, range) == RangeResult::Unsatisfiable);
    for (const auto* text : {"bytes=", "bytes=-", "bytes=+1-", "bytes=1-2,3-4", "bytes=1--2", "Bytes=1-2",
            "bytes=18446744073709551616-", "bytes=0-18446744073709551616", "bytes= 1-2", "bytes=1-2 "})
        assert(parse_range(text, 100, range) == RangeResult::Invalid);
    assert(parse_range("bytes=9223372036854775808-", UINT64_MAX, range) == RangeResult::Partial);
    assert(range.first == UINT64_C(9223372036854775808) && range.length == UINT64_C(9223372036854775807));
    assert(parse_range("bytes=2147483650-", UINT32_MAX, range) == RangeResult::Partial);
    assert(range.first == UINT64_C(2147483650) && range.last == UINT64_C(4294967294) && range.length == UINT64_C(2147483645));

    assert(file_record("dir/a\"b", 7, "hash", "\"tag\"") ==
        "{\"type\":\"file\",\"path\":\"dir/a\\\"b\",\"size\":7,\"sha256\":\"hash\",\"etag\":\"\\\"tag\\\"\"}\n");
    assert(json_string("a\nb\r\t") == "\"a\\u000ab\\u000d\\u0009\"");
    assert(file_record("large.bin", UINT32_MAX, "hash", "\"tag\"").find("\"size\":4294967295") != std::string::npos);

    CompletionReceipt receipt;
    const std::string hash(64, 'a');
    const std::string body = "{\"session_id\":\"0000000100000002\",\"manifest_sha256\":\"" + hash +
        "\",\"file_count\":9007199254740993,\"total_bytes\":18446744073709551615}";
    assert(parse_receipt(body, receipt) && receipt.file_count == UINT64_C(9007199254740993) && receipt.total_bytes == UINT64_MAX);
    assert(!parse_receipt(body + "x", receipt));
    assert(!parse_receipt(body.substr(0, body.size() - 1) + ",\"file_count\":1}", receipt));
    assert(!parse_receipt("{}", receipt));
    assert(!parse_receipt("{\"session_id\":\"0000000100000002\",\"manifest_sha256\":\"" + hash +
        "\",\"file_count\":1.0,\"total_bytes\":0}", receipt));
    assert(!parse_receipt("{\"session_id\":\"0000000100000002\",\"manifest_sha256\":\"" + hash +
        "\",\"file_count\":01,\"total_bytes\":0}", receipt));
    assert(!parse_receipt("{\"session_id\":\"0000000100000002\",\"manifest_sha256\":\"" + hash +
        "\",\"file_count\":0,\"total_bytes\":18446744073709551616}", receipt));
    std::cout << "PASS: SD export UTF-8 paths, traversal boundaries, 64-bit ranges, exact NDJSON and completion receipts\n";
}
