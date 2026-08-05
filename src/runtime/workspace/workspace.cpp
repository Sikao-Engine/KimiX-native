/*
 * workspace.cpp — Workspace snapshot, diff, and changed-files kernel
 * implementation.
 */

#include <runtime/workspace/workspace.h>

#include <runtime/diff/diff_engine.h>

#include <core/kimix_core.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>

namespace kimix {
namespace runtime {
namespace workspace {
namespace {

namespace krd = kimix::runtime::diff;

// Decode UTF-8 bytes with the same replacement behavior as Python's
// ``bytes.decode("utf-8", errors="replace")``: invalid start bytes and
// truncated multi-byte sequences become U+FFFD.
kimix::string decode_utf8_replace(kimix::string_view bytes) {
    kimix::string out;
    out.reserve(bytes.size());

    const char *p = bytes.data();
    const char *end = p + bytes.size();
    while (p < end) {
        const unsigned char c = static_cast<unsigned char>(*p);
        const size_t remaining = static_cast<size_t>(end - p);

        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++p;
        } else if ((c & 0xe0) == 0xc0 && remaining >= 2 &&
                   (static_cast<unsigned char>(p[1]) & 0xc0) == 0x80) {
            out.append(p, 2);
            p += 2;
        } else if ((c & 0xf0) == 0xe0 && remaining >= 3 &&
                   (static_cast<unsigned char>(p[1]) & 0xc0) == 0x80 &&
                   (static_cast<unsigned char>(p[2]) & 0xc0) == 0x80) {
            out.append(p, 3);
            p += 3;
        } else if ((c & 0xf8) == 0xf0 && remaining >= 4 &&
                   (static_cast<unsigned char>(p[1]) & 0xc0) == 0x80 &&
                   (static_cast<unsigned char>(p[2]) & 0xc0) == 0x80 &&
                   (static_cast<unsigned char>(p[3]) & 0xc0) == 0x80) {
            out.append(p, 4);
            p += 4;
        } else {
            // U+FFFD REPLACEMENT CHARACTER
            out.append("\xef\xbf\xbd");
            ++p;
        }
    }

    return out;
}

// Lower-case file extension including the leading dot.
kimix::string lower_extension(kimix::string_view rel_path) {
    kimix::filesystem::path p(rel_path);
    kimix::string ext = kimix::to_string(p.extension());
    for (char &ch : ext) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return ext;
}

// True if `content` should be treated as text given the optional extension set.
bool is_text_content(const kimix::string &content, kimix::string_view rel_path,
                     const text_extensions_t &text_extensions) {
    if (text_extensions) {
        const kimix::string ext = lower_extension(rel_path);
        if (text_extensions->find(ext) != text_extensions->end()) {
            return true;
        }
    }
    return content.find('\0') == kimix::string::npos;
}

// Append a minimal binary marker hunk for `rel_path`.
void append_binary_hunk(kimix::string &out, kimix::string_view rel_path,
                        bool has_old, bool has_new) {
    out += "--- a/";
    out.append(rel_path.data(), rel_path.size());
    out += '\n';
    out += "+++ b/";
    out.append(rel_path.data(), rel_path.size());
    out += '\n';

    if (!has_old && has_new) {
        out += "@@ -0,0 +1 @@\n+Binary file\n";
    } else if (has_old && !has_new) {
        out += "@@ -1 +0,0 @@\n-Binary file\n";
    } else {
        out += "@@ -1 +1 @@\n-Binary file\n+Binary file\n";
    }
}

// Collect the sorted union of keys from two snapshots.
kimix::vector<kimix::string> union_keys(const snapshot_t &before,
                                        const snapshot_t &after) {
    kimix::vector<kimix::string> keys;
    keys.reserve(before.size() + after.size());
    for (const auto &kv : before) {
        keys.push_back(kv.first);
    }
    for (const auto &kv : after) {
        keys.push_back(kv.first);
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

} // namespace

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------

snapshot_t snapshot(kimix::string_view root,
                    const kimix::vector<kimix::string> &ignore_dirs,
                    size_t max_file_bytes) {
    snapshot_t result;

    const kimix::filesystem::path root_path(root);
    kimix::unordered_set<kimix::string, string_hash> ignore_set;
    for (const auto &name : ignore_dirs) {
        ignore_set.insert(name);
    }

    std::error_code ec;
    for (auto it =
             kimix::filesystem::recursive_directory_iterator(root_path, ec);
         it != kimix::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }

        const auto &entry = *it;

        if (entry.is_symlink(ec)) {
            continue;
        }

        if (entry.is_directory(ec)) {
            const kimix::string dir_name =
                kimix::to_string(entry.path().filename());
            if (ignore_set.find(dir_name) != ignore_set.end()) {
                it.disable_recursion_pending();
            }
            continue;
        }

        if (!entry.is_regular_file(ec) || ec) {
            continue;
        }

        const uintmax_t size = entry.file_size(ec);
        if (ec || size > static_cast<uintmax_t>(max_file_bytes)) {
            continue;
        }

        kimix::filesystem::path rel_path =
            kimix::filesystem::relative(entry.path(), root_path, ec);
        if (ec) {
            continue;
        }
        rel_path = rel_path.lexically_normal();
        const std::string key_std = rel_path.generic_string();
        const kimix::string key(key_std.c_str(), key_std.size());

        kimix::BinaryFileStream stream(kimix::to_string(entry.path()));
        if (!stream) {
            continue;
        }

        const auto data = stream.read_all();
        kimix::string content;
        content.assign(reinterpret_cast<const char *>(data.data()),
                       data.size());

        result.emplace(std::move(key), std::move(content));
    }

    return result;
}

// ---------------------------------------------------------------------------
// Diff
// ---------------------------------------------------------------------------

kimix::string diff_snapshots(const snapshot_t &before, const snapshot_t &after,
                             const text_extensions_t &text_extensions,
                             size_t context_lines) {
    kimix::string result;
    const kimix::vector<kimix::string> keys = union_keys(before, after);

    for (const auto &key : keys) {
        const auto old_it = before.find(key);
        const auto new_it = after.find(key);

        const bool has_old = old_it != before.end();
        const bool has_new = new_it != after.end();

        if (has_old && has_new && old_it->second == new_it->second) {
            continue;
        }

        const kimix::string empty;
        const kimix::string &old_content = has_old ? old_it->second : empty;
        const kimix::string &new_content = has_new ? new_it->second : empty;

        const kimix::string_view key_view(key);
        const kimix::string &content_for_text_check =
            has_new ? new_content : old_content;
        if (is_text_content(content_for_text_check, key_view,
                            text_extensions)) {
            const kimix::string old_text = decode_utf8_replace(old_content);
            const kimix::string new_text = decode_utf8_replace(new_content);
            const kimix::string chunk =
                krd::unified_diff(old_text, new_text, key_view, true, "\n");
            result += chunk;
        } else {
            append_binary_hunk(result, key_view, has_old, has_new);
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Changed files
// ---------------------------------------------------------------------------

kimix::vector<std::pair<kimix::string, kimix::string>>
changed_files(const snapshot_t &before, const snapshot_t &after) {
    kimix::vector<std::pair<kimix::string, kimix::string>> result;
    const kimix::vector<kimix::string> keys = union_keys(before, after);
    result.reserve(keys.size());

    for (const auto &key : keys) {
        const auto old_it = before.find(key);
        const auto new_it = after.find(key);

        const bool has_old = old_it != before.end();
        const bool has_new = new_it != after.end();

        if (has_old && has_new && old_it->second == new_it->second) {
            continue;
        }

        result.emplace_back(key, key);
    }

    return result;
}

} // namespace workspace
} // namespace runtime
} // namespace kimix
