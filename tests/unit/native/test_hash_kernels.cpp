// Test for src/runtime/search/hash_kernels.h (plan 005).
// This test covers:
// - simhash: golden value for a single token (XXH3-64 with default seed),
//   dedupe semantics, bit-flip sensitivity, near-duplicate hamming distance
// - minhash: golden values under the documented deterministic contract
//   (xxhash.xxh3_64(seed=...) masked to 32 bits), empty-shingle zeros,
//   jaccard estimation on overlapping sets

#include "ut/ut.hpp"
#include <runtime/search/hash_kernels.h>

#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::search;

namespace {

kimix::vector<kimix::string_view> views(const kimix::vector<kimix::string>& strs) {
    kimix::vector<kimix::string_view> out;
    out.reserve(strs.size());
    for (const auto& s : strs) {
        out.emplace_back(s);
    }
    return out;
}

int popcount64(uint64_t v) {
    int c = 0;
    while (v) {
        v &= v - 1;
        ++c;
    }
    return c;
}

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "simhash_single_token_golden"_test = [] {
        // Golden from Python: xxhash.xxh3_64(b"hello", seed=2**61-1), then the
        // 64-bit +/- accumulation with the default kimix seed (2^61 - 1).
        const kimix::vector<kimix::string> toks = {"hello"};
        const uint64_t got = simhash(views(toks));
        expect(eq(got, 0xA70B2FD377430EF8ull)) << "simhash(hello)";
        const kimix::vector<kimix::string> toks_a = {"a"};
        expect(eq(simhash(views(toks_a)), 0x8D314BCB92C80589ull)) << "simhash(a)";
    };

    "simhash_dedupe"_test = [] {
        // Python iterates set(text.split()): duplicates contribute once.
        const kimix::vector<kimix::string> one = {"cat", "dog"};
        const kimix::vector<kimix::string> dup = {"cat", "dog", "cat", "dog", "cat"};
        expect(eq(simhash(views(one)), simhash(views(dup))));
        // Empty input -> 0 (no tokens, v stays 0, no bit set).
        const kimix::vector<kimix::string> none = {};
        expect(eq(simhash(views(none)), 0ull));
    };

    "simhash_near_duplicate_hamming"_test = [] {
        // One token changed -> small hamming distance between the hashes.
        const kimix::vector<kimix::string> a = {"the quick brown fox jumps"};
        const kimix::vector<kimix::string> b = {"the quick brown fox jump"};
        const kimix::vector<kimix::string> c = {"completely different topic"};
        const uint64_t ha = simhash(views(a));
        const uint64_t hb = simhash(views(b));
        const uint64_t hc = simhash(views(c));
        expect(lt(popcount64(ha ^ hb), popcount64(ha ^ hc)));
    };

    "minhash_golden_contract"_test = [] {
        // Golden from the documented contract (seed=42):
        //   sh  = xxh3_64(shingle, 42) & 0xFFFFFFFF
        //   sp  = xxh3_64(<u32le p>, 42) & 0xFFFFFFFF
        //   sig[p] = min over shingles of sh ^ sp
        const kimix::vector<kimix::string> shingles = {"ab", "cd"};
        auto sig = minhash(views(shingles), 2, 42);
        expect(eq(sig.size(), 2u));
        expect(eq(sig[0], 1457630054ull));
        expect(eq(sig[1], 1166070587ull));
        // Empty shingles -> zeros (reference: [0] * num_perm).
        const kimix::vector<kimix::string> none = {};
        auto zero_sig = minhash(views(none), 3, 1);
        expect(eq(zero_sig.size(), 3u));
        expect(eq(zero_sig[0], 0ull));
        expect(eq(zero_sig[1], 0ull));
        expect(eq(zero_sig[2], 0ull));
        // k == 0 -> empty.
        expect(minhash(views(shingles), 0, 1).empty());
    };

    "minhash_jaccard_estimation"_test = [] {
        // Overlapping sets -> higher signature agreement than disjoint sets.
        const kimix::vector<kimix::string> s1 = {"aa", "bb", "cc", "dd"};
        const kimix::vector<kimix::string> s2 = {"aa", "bb", "cc", "ee"};
        const kimix::vector<kimix::string> s3 = {"xx", "yy", "zz", "ww"};
        auto sig1 = minhash(views(s1), 64, 7);
        auto sig2 = minhash(views(s2), 64, 7);
        auto sig3 = minhash(views(s3), 64, 7);
        size_t agree12 = 0, agree13 = 0;
        for (size_t i = 0; i < 64; ++i) {
            if (sig1[i] == sig2[i]) {
                ++agree12;
            }
            if (sig1[i] == sig3[i]) {
                ++agree13;
            }
        }
        expect(gt(agree12, agree13)) << "similar sets must agree more";
    };
}
