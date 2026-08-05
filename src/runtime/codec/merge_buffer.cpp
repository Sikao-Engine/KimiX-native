/*
 * merge_buffer.cpp -- see merge_buffer.h (plan 007).
 */

#include <runtime/codec/merge_buffer.h>

namespace kimix {
namespace runtime {
namespace codec {

namespace {
// Kinds whose consecutive same-kind parts merge (plan 007 kind rules).
bool is_mergeable_kind(kimix::string_view kind) noexcept {
    return kind == "text" || kind == "args";
}
} // namespace

bool WireMergeBuffer::append(kimix::string_view kind,
                             kimix::string_view delta) noexcept {
    if (empty()) {
        // First part of a merge group: any kind starts the group.
        kind_.assign(kind.data(), kind.size());
        data_.assign(delta.data(), delta.size());
        return true;
    }
    if (kind == kimix::string_view(kind_) && is_mergeable_kind(kind)) {
        data_.append(delta.data(), delta.size());
        return true;
    }
    return false; // not mergeable -- caller flushes first
}

kimix::string_view WireMergeBuffer::snapshot() const noexcept {
    return kimix::string_view(data_);
}

void WireMergeBuffer::reset() noexcept {
    kind_.clear();
    data_.clear();
}

bool WireMergeBuffer::empty() const noexcept {
    return data_.empty();
}

kimix::string_view WireMergeBuffer::kind() const noexcept {
    return kimix::string_view(kind_);
}

} // namespace codec
} // namespace runtime
} // namespace kimix
