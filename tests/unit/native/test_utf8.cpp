// Benchmark + test for src/runtime/common (utf8, json_pretty, text_util).
// This is the performance benchmark home for the runtime/common kernels:
// - utf8_code_point_count / decode_cp / is_ascii / utf8_byte_length
// - json_pretty pretty-printing throughput (1 KB / 64 KB / 1 MB docs)
// - text_util inline helpers (append_utf8, py_isspace_cp, append_json_escaped,
//   ltrim_py_ws / trim_py_ws, shorten_utf8)
// Correctness is exercised inside every bench case (expected counts and
// round-trip invariants) so a benchmark never silently times a broken path.
// See bench_util.h for the timing contract; results go to stderr as [bench].

#include "ut/ut.hpp"
#include "bench_util.h"
#include <runtime/common/utf8.h>
#include <runtime/common/text_util.h>
#include <runtime/common/json_pretty.h>

#include <yyjson.h>
#include <mimalloc.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::common;

namespace {

kimix::string_view sv(const std::string& s) { return kimix::string_view(s); }

// ---- mimalloc-backed yyjson allocator (mirrors tests/unit/ext/test_yyjson.cpp)
static void* bench_malloc(void* ctx, size_t size) {
    (void)ctx;
    return mi_malloc(size);
}

static void* bench_realloc(void* ctx, void* ptr, size_t old_size, size_t size) {
    (void)ctx;
    (void)old_size;
    return mi_realloc(ptr, size);
}

static void bench_free(void* ctx, void* ptr) {
    (void)ctx;
    mi_free(ptr);
}

static const yyjson_alc kYyjsonAlc = {
    bench_malloc,
    bench_realloc,
    bench_free,
    NULL,
};

// Deterministic LCG for data generation.
static uint32_t bench_lcg(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state;
}

// Mixed-width UTF-8: repeated 1/2/3/4-byte code-point cycles (a, U+00E9,
// U+4E2D, U+1F600) followed by ASCII filler up to exactly `target` bytes.
// All bytes are written with hex escapes so the file is pure ASCII.
static std::string make_mixed_utf8(size_t target, size_t* out_cps) {
    const std::string seq = std::string("a", 1) + "\xC3\xA9" +
                            "\xE4\xB8\xAD" + "\xF0\x9F\x98\x80";
    std::string s;
    s.reserve(target);
    size_t cps = 0;
    while (s.size() + seq.size() <= target) {
        s += seq;
        cps += 4; // one 1/2/3/4-byte code point per cycle
    }
    while (s.size() < target) {
        s.push_back('b');
        ++cps;
    }
    *out_cps = cps;
    return s;
}

// Compact JSON text with a realistic nested shape: meta object + items array
// of objects with strings (escape-heavy name values), ints, floats, bools and
// nested arrays. Grows until >= target_bytes bytes (then closed "]}"). The
// returned document is valid and parses with the mimalloc allocator.
static std::string make_json_text(size_t target_bytes) {
    std::string doc;
    doc.reserve(target_bytes + 256);
    doc += "{\"meta\":{\"version\":1,\"pi\":3.14,\"flag\":true,\"empty\":{},"
           "\"list\":[1,2,3]},\"items\":[";
    size_t n = 0;
    for (;;) {
        char id[48];
        const int idlen = std::snprintf(id, sizeof(id), "%zu", n);
        if (n != 0) {
            doc += ',';
        }
        doc += "{\"id\":";
        doc.append(id, static_cast<size_t>(idlen));
        doc += ",\"name\":\"item ";
        doc.append(id, static_cast<size_t>(idlen));
        doc += " \\\"quoted\\\"\\nwith\\ttabs \xC3\xA9\xE4\xB8\xAD\","
               "\"tags\":[\"t1\",\"t2\",\"t3\"],\"score\":0.5}";
        ++n;
        if (doc.size() >= target_bytes) {
            break;
        }
    }
    doc += "]}";
    return doc;
}

// Mostly-plain bytes with one escape-worthy class every `special_every` bytes:
// '"' (0x22), '\\' (0x5C), '\n' (0x0A) and the control byte 0x01.
static std::string make_escape_mix(size_t target, size_t special_every) {
    std::string s;
    s.reserve(target);
    for (size_t i = 0; i < target; ++i) {
        if (i % special_every == 0) {
            s.push_back('"');
        } else if (i % (special_every + 7u) == 0) {
            s.push_back('\\');
        } else if (i % (special_every + 13u) == 0) {
            s.push_back('\n');
        } else if (i % (special_every + 31u) == 0) {
            s.push_back(static_cast<char>(0x01));
        } else {
            s.push_back('x');
        }
    }
    return s;
}

// Build a JSON doc of ~`target` bytes once, pretty-print it repeatedly, and
// verify the output round-trips (identical "meta"."version" after re-parse).
static void bench_json_pretty(size_t target_bytes, const char* name) {
    const std::string src = make_json_text(target_bytes);
    yyjson_doc* doc = yyjson_read_opts(
        const_cast<char*>(src.data()), src.size(), 0,
        const_cast<yyjson_alc*>(&kYyjsonAlc), nullptr);
    expect(doc != nullptr) << name << " parse of source";
    if (doc == nullptr) {
        return;
    }
    const yyjson_val* root = yyjson_doc_get_root(doc);
    const yyjson_val* meta = yyjson_obj_get(root, "meta");
    expect(meta != nullptr) << name << " meta present";
    const yyjson_val* version =
        meta != nullptr ? yyjson_obj_get(meta, "version") : nullptr;
    expect(version != nullptr && yyjson_get_uint(version) == 1u)
        << name << " version";
    const yyjson_val* items = yyjson_obj_get(root, "items");
    expect(items != nullptr && yyjson_is_arr(items)) << name << " items array";

    kimix::string out;
    pretty_write_doc(doc, out);
    expect(!out.empty() && out[0] == '{') << name << " pretty prefix";
    expect(out.size() >= src.size()) << name << " pretty grows doc";

    // Round-trip: re-parse the pretty output.
    yyjson_doc* back = yyjson_read_opts(
        const_cast<char*>(out.data()), out.size(), 0,
        const_cast<yyjson_alc*>(&kYyjsonAlc), nullptr);
    expect(back != nullptr) << name << " re-parse";
    if (back != nullptr) {
        const yyjson_val* broot = yyjson_doc_get_root(back);
        const yyjson_val* bmeta = yyjson_obj_get(broot, "meta");
        const yyjson_val* bver =
            bmeta != nullptr ? yyjson_obj_get(bmeta, "version") : nullptr;
        expect(bver != nullptr && yyjson_get_uint(bver) == 1u)
            << name << " round-trip version";
        yyjson_doc_free(back);
    }

    size_t total = 0;
    kimix_bench::run(name, [&] {
        out.clear();
        pretty_write_doc(doc, out);
        total += out.size();
    }, 1, static_cast<double>(src.size()));
    kimix_bench::sink(total);
    yyjson_doc_free(doc);
}

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "utf8_sanity"_test = [] {
        // ASCII bulk count
        const std::string ascii(4096, 'a');
        expect(eq(utf8_code_point_count(sv(ascii)), ascii.size()));
        expect(is_ascii(sv(ascii)));
        // Mixed multi-byte input (hex escapes: é / 中 / 😀)
        const std::string mixed =
            std::string("a\xC3\xA9", 3) +
            std::string("\xE4\xB8\xAD", 3) +
            std::string("\xF0\x9F\x98\x80", 4) + "b";
        const std::size_t cps = utf8_code_point_count(sv(mixed));
        expect(eq(cps, 5u));
        expect(!is_ascii(sv(mixed)));
        // Invalid bytes: lone continuation + out-of-range lead each count 1.
        {
            const std::string inv = std::string("\x80", 1) +
                                    std::string("\xFF", 1) + "z";
            expect(eq(utf8_code_point_count(sv(inv)), 3u));
            const char* it = inv.data();
            const char* end = it + inv.size();
            expect(eq(decode_cp(it, end), 0xFFFDu));
            expect(eq(decode_cp(it, end), 0xFFFDu));
            expect(eq(decode_cp(it, end), 0x7Au));
            expect(it == end);
        }
        // utf8_byte_length boundaries
        expect(eq(utf8_byte_length(0x00), 1u));
        expect(eq(utf8_byte_length(0x7F), 1u));
        expect(eq(utf8_byte_length(0x80), 2u));
        expect(eq(utf8_byte_length(0x7FF), 2u));
        expect(eq(utf8_byte_length(0x800), 3u));
        expect(eq(utf8_byte_length(0xFFFF), 3u));
        expect(eq(utf8_byte_length(0x10000), 4u));
        expect(eq(utf8_byte_length(0x10FFFF), 4u));
    };

    // --- utf8 kernels ------------------------------------------------------

    "bench_utf8_count_ascii_1mb"_test = [] {
        const std::string ascii(size_t{1} << 20, 'a');
        expect(eq(utf8_code_point_count(sv(ascii)), ascii.size()));
        size_t total = 0;
        kimix_bench::run("utf8/count_ascii_1mb", [&] {
            total += utf8_code_point_count(sv(ascii));
        }, 1, static_cast<double>(ascii.size()));
        kimix_bench::sink(total);
    };

    "bench_utf8_count_mixed_1mb"_test = [] {
        size_t cps_expected = 0;
        const std::string mixed = make_mixed_utf8(size_t{1} << 20, &cps_expected);
        expect(eq(utf8_code_point_count(sv(mixed)), cps_expected));
        expect(!is_ascii(sv(mixed)));
        size_t total = 0;
        kimix_bench::run("utf8/count_mixed_1mb", [&] {
            total += utf8_code_point_count(sv(mixed));
        }, 1, static_cast<double>(mixed.size()));
        kimix_bench::sink(total);
    };

    "bench_utf8_count_invalid_1mb"_test = [] {
        // Worst case: every byte is an invalid lead (0xFF / 0x80 alternation);
        // each must count as exactly one code point.
        std::string invalid;
        invalid.reserve(size_t{1} << 20);
        for (size_t i = 0; i < (size_t{1} << 20); ++i) {
            invalid.push_back((i & 1u) != 0u ? static_cast<char>(0xFF)
                                             : static_cast<char>(0x80));
        }
        expect(eq(utf8_code_point_count(sv(invalid)), invalid.size()));
        size_t total = 0;
        kimix_bench::run("utf8/count_invalid_1mb", [&] {
            total += utf8_code_point_count(sv(invalid));
        }, 1, static_cast<double>(invalid.size()));
        kimix_bench::sink(total);
    };

    "bench_utf8_decode_mixed_1mb"_test = [] {
        size_t cps_expected = 0;
        const std::string mixed = make_mixed_utf8(size_t{1} << 20, &cps_expected);
        size_t count = 0;
        {
            const char* it = mixed.data();
            const char* end = it + mixed.size();
            while (it < end) {
                (void)decode_cp(it, end);
                ++count;
            }
            expect(eq(count, cps_expected));
            expect(it == end);
        }
        size_t total = 0;
        kimix_bench::run("utf8/decode_mixed_1mb", [&] {
            const char* it = mixed.data();
            const char* end = it + mixed.size();
            size_t n = 0;
            while (it < end) {
                (void)decode_cp(it, end);
                ++n;
            }
            total += n;
        }, 1, static_cast<double>(mixed.size()));
        kimix_bench::sink(total);
    };

    "bench_utf8_is_ascii_ascii_1mb"_test = [] {
        const std::string ascii(size_t{1} << 20, 'a');
        expect(is_ascii(sv(ascii)));
        size_t total = 0;
        kimix_bench::run("utf8/is_ascii_ascii_1mb", [&] {
            total += is_ascii(sv(ascii)) ? 1u : 0u;
        }, 1, static_cast<double>(ascii.size()));
        kimix_bench::sink(total);
    };

    "bench_utf8_is_ascii_latefail_1mb"_test = [] {
        // One non-ASCII byte at the very end: the 8-byte fast path scans the
        // whole buffer before failing, so this prices the scan, not the fail.
        std::string late = std::string(size_t{1} << 20, 'a');
        late.back() = static_cast<char>(0x80);
        expect(!is_ascii(sv(late)));
        size_t total = 0;
        kimix_bench::run("utf8/is_ascii_latefail_1mb", [&] {
            total += is_ascii(sv(late)) ? 1u : 0u;
        }, 1, static_cast<double>(late.size()));
        kimix_bench::sink(total);
    };

    "bench_utf8_byte_length_hot"_test = [] {
        // 64k code points across all width ranges (LCG in [1, 0x110000) plus
        // the exact boundary values), fully representative of hot callers.
        std::vector<uint32_t> cps(65536);
        uint32_t st = 0x12345678u;
        const uint32_t bounds[] = {0, 0x7F, 0x80, 0x7FF, 0x800, 0xFFFF,
                                   0x10000, 0x10FFFF, 0xFFFFFFFF};
        size_t expected = 0;
        for (size_t i = 0; i < cps.size(); ++i) {
            cps[i] = (i < 64u) ? bounds[i % 9u]
                               : (bench_lcg(st) % 0x110000u) + 1u;
            expected += utf8_byte_length(cps[i]);
        }
        size_t check = 0;
        for (uint32_t cp : cps) {
            check += utf8_byte_length(cp);
        }
        expect(eq(check, expected));
        size_t total = 0;
        kimix_bench::run("utf8/byte_length_64k_cp", [&] {
            size_t sum = 0;
            for (uint32_t cp : cps) {
                sum += utf8_byte_length(cp);
            }
            total += sum;
        }, cps.size(), 0.0);
        kimix_bench::sink(total);
    };

    // --- text_util helpers -------------------------------------------------

    "bench_text_append_utf8_reserved"_test = [] {
        // One 1/2/3/4-byte cycle per iteration, 512 cycles -> 2048 cps,
        // 5120 output bytes.
        const std::size_t kCycles = 512u;
        const std::size_t kBytes = kCycles * 10u;
        const std::size_t kOps = kCycles * 4u;
        kimix::string ref;
        kimix::string out;
        out.reserve(kBytes + 1u);
        {
            kimix::string build;
            build.reserve(kBytes);
            for (std::size_t i = 0; i < kCycles; ++i) {
                append_utf8(build, 0x61u);
                append_utf8(build, 0xE9u);
                append_utf8(build, 0x4E2Du);
                append_utf8(build, 0x1F600u);
            }
            ref = build;
            expect(eq(ref.size(), kBytes));
        }
        // Correctness of the measured path (one full encode).
        for (std::size_t i = 0; i < kCycles; ++i) {
            append_utf8(out, 0x61u);
            append_utf8(out, 0xE9u);
            append_utf8(out, 0x4E2Du);
            append_utf8(out, 0x1F600u);
        }
        expect(eq(out, ref));
        out.clear();
        size_t total = 0;
        kimix_bench::run("text/append_utf8_2k_cp_reserved", [&] {
            out.clear();
            for (std::size_t i = 0; i < kCycles; ++i) {
                append_utf8(out, 0x61u);
                append_utf8(out, 0xE9u);
                append_utf8(out, 0x4E2Du);
                append_utf8(out, 0x1F600u);
            }
            total += out.size();
        }, kOps, static_cast<double>(kBytes));
        kimix_bench::sink(total);
    };

    "bench_text_append_utf8_grow"_test = [] {
        const std::size_t kCycles = 512u;
        const std::size_t kBytes = kCycles * 10u;
        const std::size_t kOps = kCycles * 4u;
        std::size_t total = 0;
        kimix_bench::run("text/append_utf8_2k_cp_grow", [&] {
            kimix::string fresh;
            for (std::size_t i = 0; i < kCycles; ++i) {
                append_utf8(fresh, 0x61u);
                append_utf8(fresh, 0xE9u);
                append_utf8(fresh, 0x4E2Du);
                append_utf8(fresh, 0x1F600u);
            }
            total += fresh.size();
        }, kOps, static_cast<double>(kBytes));
        kimix_bench::sink(total);
        expect(eq(total % kBytes, 0u)); // every iteration produced kBytes
    };

    "bench_text_py_isspace_64k"_test = [] {
        std::vector<uint32_t> cps(65536);
        uint32_t st = 0x9E3779B9u;
        size_t expected = 0;
        for (size_t i = 0; i < cps.size(); ++i) {
            if (i % 256u == 0) {
                // Sprinkle known Python-whitespace code points.
                cps[i] = ((i / 256u) % 2u == 0u) ? 0x20u
                                                 : 0x0009u + (i / 256u) % 5u;
            } else {
                cps[i] = (bench_lcg(st) % 0x110000u) + 1u;
            }
            expected += py_isspace_cp(cps[i]) ? 1u : 0u;
        }
        size_t check = 0;
        for (uint32_t cp : cps) {
            check += py_isspace_cp(cp) ? 1u : 0u;
        }
        expect(eq(check, expected));
        expect(expected > 0u);
        size_t total = 0;
        kimix_bench::run("text/py_isspace_64k_cp", [&] {
            const size_t off = total % cps.size();
            size_t sum = 0;
            for (size_t i = 0; i < cps.size(); ++i) {
                sum += py_isspace_cp(cps[(off + i) % cps.size()]) ? 1u : 0u;
            }
            total += sum;
        }, cps.size(), 0.0);
        kimix_bench::sink(total);
    };

    "bench_text_json_escape_mixed_1mb"_test = [] {
        const std::string data = make_escape_mix(size_t{1} << 20, 97u);
        kimix::string ref;
        append_json_escaped(ref, sv(data));
        kimix::string out;
        append_json_escaped(out, sv(data));
        expect(eq(out, ref));
        out.clear();
        size_t total = 0;
        kimix_bench::run("text/json_escape_mixed_1mb", [&] {
            out.clear();
            const size_t off = total & 0xFFFu;
            append_json_escaped(out,
                                kimix::string_view(data.data() + off,
                                                   data.size() - off));
            total += out.size();
        }, 1, static_cast<double>(data.size()));
        // Full-view re-check after the loop.
        out.clear();
        append_json_escaped(out, sv(data));
        expect(eq(out, ref));
        kimix_bench::sink(total);
    };

    "bench_text_json_escape_dense_1mb"_test = [] {
        const std::string data = make_escape_mix(size_t{1} << 20, 3u);
        kimix::string ref;
        append_json_escaped(ref, sv(data));
        kimix::string out;
        append_json_escaped(out, sv(data));
        expect(eq(out, ref));
        out.clear();
        size_t total = 0;
        kimix_bench::run("text/json_escape_dense_1mb", [&] {
            out.clear();
            const size_t off = total & 0xFFFu;
            append_json_escaped(out,
                                kimix::string_view(data.data() + off,
                                                   data.size() - off));
            total += out.size();
        }, 1, static_cast<double>(data.size()));
        out.clear();
        append_json_escaped(out, sv(data));
        expect(eq(out, ref));
        kimix_bench::sink(total);
    };

    "bench_text_ltrim_all_space_1mb"_test = [] {
        const std::string spaces(size_t{1} << 20, ' ');
        expect(ltrim_py_ws(sv(spaces)).empty());
        size_t total = 0;
        kimix_bench::run("text/ltrim_all_space_1mb", [&] {
            total += ltrim_py_ws(sv(spaces)).size();
        }, 1, static_cast<double>(spaces.size()));
        kimix_bench::sink(total);
    };

    "bench_text_trim_all_space_1mb"_test = [] {
        const std::string spaces(size_t{1} << 20, ' ');
        expect(trim_py_ws(sv(spaces)).empty());
        size_t total = 0;
        kimix_bench::run("text/trim_all_space_1mb", [&] {
            total += trim_py_ws(sv(spaces)).size();
        }, 1, static_cast<double>(spaces.size()));
        kimix_bench::sink(total);
    };

    "bench_text_shorten_1mb_100"_test = [] {
        size_t mixed_cps = 0;
        const std::string text =
            make_mixed_utf8(size_t{1} << 20, &mixed_cps);
        const kimix::string res = shorten_utf8(sv(text), 100u);
        expect(eq(utf8_code_point_count(sv(std::string(res.data(), res.size()))),
                  100u));
        size_t total = 0;
        kimix_bench::run("text/shorten_1mb_100", [&] {
            const kimix::string r = shorten_utf8(sv(text), 100u);
            total += r.size();
        }, 1, static_cast<double>(text.size()));
        kimix_bench::sink(total);
    };

    // --- json_pretty -------------------------------------------------------

    "bench_json_pretty_1k"_test = [] {
        bench_json_pretty(1024u, "json_pretty/1k");
    };

    "bench_json_pretty_64k"_test = [] {
        bench_json_pretty(65536u, "json_pretty/64k");
    };

    "bench_json_pretty_1m"_test = [] {
        bench_json_pretty(size_t{1} << 20, "json_pretty/1m");
    };
}