/*
 * vertex.cpp — content access, dependency bookkeeping and aligned allocation
 * for kimix::graph_db::Vertex. See vertex.h for the API contract, the lock
 * discipline and the cache-line argument.
 *
 * Every method here assumes the caller already holds the right lock: the
 * content accessors take the vertex lock themselves, while the dependency
 * mutators are called by Graph under the shard UNIQUE lock of this vertex's
 * shard (documented on each private helper in vertex.h). Nothing in this file
 * aborts, throws or allocates on a failure path.
 */
  #include "vertex.h"

  #include "utils.h"


#include <algorithm>
#include <utility>

namespace kimix::graph_db {

// ---------------------------------------------------------------------------
// Dirty-flag protocol (the ONE place the flag is written — see vertex.h)
// ---------------------------------------------------------------------------

void Vertex::_mark_dirty() noexcept {
    if (VertexDirtySink *const sink = _dirty_sink.load(std::memory_order_acquire);
        sink != nullptr) {
        // The owner sets the flag AND inserts the guid into its dirty index under
        // one mutex, AND bumps the epoch there (see Graph::note_vertex_dirty) — the
        // flip and its stamp have to be one atomic step for the save's conditional
        // clear to be sound.
        sink->note_vertex_dirty(this);
        return;
    }
    // No owner (not published yet, or removed): store directly, and still stamp the
    // write, so a vertex marked dirty outside a graph is collected with the correct
    // epoch once it is adopted.
    _dirty.store(true, std::memory_order_release);
    _dirty_epoch.fetch_add(1, std::memory_order_acq_rel);
}

void Vertex::_clear_dirty() noexcept {
    if (VertexDirtySink *const sink = _dirty_sink.load(std::memory_order_acquire);
        sink != nullptr) {
        sink->clear_vertex_dirty(this);
        return;
    }
    _dirty.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Vertex::Vertex() noexcept
    : _guid{true} {
}

Vertex::Vertex(kimix::Guid const &guid) noexcept
    : _guid(guid) {
}

  Vertex::Vertex(kimix::Guid const &guid,
                 kimix::span<const std::byte> initial_content)
      : _guid(guid) {
      set_content(initial_content);
  }

  Vertex::Vertex(kimix::Guid const &guid,
                 kimix::span<const std::byte> initial_content, ContentDigest content_digest)
      : _guid(guid), _content_digest(content_digest) {
      set_content(initial_content);
  }

// ---------------------------------------------------------------------------
// Content (vertex lock: shared for reads, unique for writes)
// ---------------------------------------------------------------------------

void Vertex::content(kimix::vector<std::byte> &out) const {
    VertexSharedLock guard(_mutex);
    out = _content;
}

kimix::vector<std::byte> Vertex::content_copy() const {
    VertexSharedLock guard(_mutex);
    return _content;
}

size_t Vertex::content_size() const {
    VertexSharedLock guard(_mutex);
    return _content.size();
}

kimix::MD5 Vertex::content_hash() const {
    VertexSharedLock guard(_mutex);
    return _content_hash;
}

  void Vertex::set_content(kimix::span<const std::byte> content) {
      // The digest is a function of the INCOMING bytes, so it is computed BEFORE the
      // lock is taken. That is the point of the split: an MD5 over a large blob
      // inside the unique section is exactly the long holder a spin lock must never
      // have (every reader of THIS vertex would spin for the whole hash, and the
      // section is bounded only by the caller's content size). Only the copy that
      // installs the bytes, the cached hash and the dirty flip stay under the lock —
      // an assignment and two stores.
      kimix::MD5 const digest = content_digest_of(content, _content_digest);
      VertexUniqueLock guard(_mutex);
      _content.assign(content.begin(), content.end());
      _content_hash = digest;
      _mark_dirty();
  }

  void Vertex::touch() {
      // Recomputing the digest needs the STABLE bytes, but hashing them under the
      // unique lock is the long section this method used to be. So the bytes are
      // copied under a SHARED lock (readers keep running), the digest is computed
      // outside every lock, and the unique lock is re-taken only to publish it.
      //
      // CALLER-SERIALIZED CONTRACT: touch() is meant for the codec/loader, which
      // calls it on a record it has just materialized, before the vertex is visible
      // to any reader. A set_content() that lands between the two phases publishes
      // its own digest first and this publish then overwrites it with the digest of
      // the bytes this call saw — i.e. a concurrent writer of the SAME vertex makes
      // the cached digest reflect one of the two valid contents, never a torn blob.
      // Do not use touch() as a way to observe concurrent writes.
      kimix::vector<std::byte> bytes;
      {
          VertexSharedLock guard(_mutex);
          bytes = _content;
      }
      kimix::MD5 const digest = content_digest_of(
          kimix::span<const std::byte>{bytes.data(), bytes.size()}, _content_digest);
      VertexUniqueLock guard(_mutex);
      _content_hash = digest;
      _mark_dirty();
  }

// ---------------------------------------------------------------------------
// Dependencies
// ---------------------------------------------------------------------------

bool Vertex::depends_on(kimix::Guid const &guid) const noexcept {
    // The link list holds one entry per outgoing EDGE, so a parallel edge is
    // found the same way a distinct-neighbour probe would find it.
    return std::any_of(_out_links.begin(), _out_links.end(),
                       [&guid](EdgeLink const &link) { return link.neighbour == guid; });
}

void Vertex::reserve_links(size_t outgoing, size_t incoming) {
    // Only ever grows: each reserve() is itself a no-op below the current
    // capacity, so a second call with a smaller count cannot shrink a container
    // or move an element out of it.
    _out_links.reserve(outgoing);
    _in_links.reserve(incoming);
}





void Vertex::_clear_links() noexcept {
    _out_links.clear();
    _in_links.clear();
}

// ---------------------------------------------------------------------------
// Edge links (caller holds this vertex's shard UNIQUE lock)
// ---------------------------------------------------------------------------

void Vertex::_add_out_link(kimix::Guid const &edge, Vertex *neighbour) {
    kimix::Guid const neighbour_guid = neighbour != nullptr ? neighbour->guid() : kimix::Guid{};
    _out_links.push_back(EdgeLink{edge, neighbour_guid});
    // A link change IS a vertex-record change: the serialized adjacency lives in
    // the endpoint records, so a free/move that skips this dirty flag resurrects
    // the old edge as a bare synthesized one on the next load.
    _mark_dirty();
}

void Vertex::_add_in_link(kimix::Guid const &edge, Vertex *neighbour) {
    kimix::Guid const neighbour_guid = neighbour != nullptr ? neighbour->guid() : kimix::Guid{};
    _in_links.push_back(EdgeLink{edge, neighbour_guid});
    _mark_dirty();
}

bool Vertex::_remove_out_link(kimix::Guid const &edge, Vertex *neighbour) {
    // The link list is the one adjacency representation (plan v3 §5.2):
    // removing the entry removes everything there is to remove.
    (void)neighbour; // kept in the signature: the caller resolves the endpoint
    for (size_t i = 0; i < _out_links.size(); ++i) {
        if (_out_links[i].edge == edge) {
            _out_links.erase(_out_links.begin() + static_cast<ptrdiff_t>(i));
            _mark_dirty();
            return true;
        }
    }
    return false;
}

bool Vertex::_remove_in_link(kimix::Guid const &edge, Vertex *neighbour) {
    (void)neighbour; // kept in the signature: the caller resolves the endpoint
    for (size_t i = 0; i < _in_links.size(); ++i) {
        if (_in_links[i].edge == edge) {
            _in_links.erase(_in_links.begin() + static_cast<ptrdiff_t>(i));
            _mark_dirty();
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Typed payload: labels and properties (vertex lock)
// ---------------------------------------------------------------------------

bool Vertex::add_property(PropNameId name, PropertyValue value) {
    VertexUniqueLock guard(_mutex);
    if (_properties.find(name) != _properties.end()) {
        return false;
    }
    _properties.emplace(name, std::move(value));
    _mark_dirty();
    return true;
}

bool Vertex::set_property(PropNameId name, PropertyValue value) {
    VertexUniqueLock guard(_mutex);
    auto it = _properties.find(name);
    if (it == _properties.end()) {
        return false;
    }
    it->second = std::move(value);
    _mark_dirty();
    return true;
}

bool Vertex::put_property(PropNameId name, PropertyValue value) {
    VertexUniqueLock guard(_mutex);
    auto it = _properties.find(name);
    bool const replaced = it != _properties.end();
    if (replaced) {
        it->second = std::move(value);
    } else {
        _properties.emplace(name, std::move(value));
    }
    _mark_dirty();
    return replaced;
}

size_t Vertex::remove_properties(kimix::span<PropNameId const> names) {
    VertexUniqueLock guard(_mutex);
    size_t removed = 0;
    for (auto name : names) {
        auto it = _properties.find(name);
        if (it == _properties.end()) {
            continue; // GDI: removing an absent property is not an error
        }
        _properties.erase(it);
        ++removed;
    }
    if (removed != 0) {
        _mark_dirty();
    }
    return removed;
}

bool Vertex::has_property(PropNameId name) const {
    VertexSharedLock guard(_mutex);
    return _properties.find(name) != _properties.end();
}

bool Vertex::property(PropNameId name, PropertyValue &out) const {
    VertexSharedLock guard(_mutex);
    auto it = _properties.find(name);
    if (it == _properties.end()) {
        return false;
    }
    out = it->second;
    return true;
}

void Vertex::properties(PropertyList &out) const {
    VertexSharedLock guard(_mutex);
    out.clear();
    out.reserve(_properties.size());
    for (auto const &entry : _properties) {
        PropertyEntry item;
        item.name = entry.first;
        item.value = entry.second;
        out.push_back(std::move(item));
    }
}

PropertyList Vertex::property_list() const {
    PropertyList out;
    properties(out);
    return out;
}

size_t Vertex::property_count() const {
    VertexSharedLock guard(_mutex);
    return _properties.size();
}

bool Vertex::add_label(LabelId label) {
    VertexUniqueLock guard(_mutex);
    if (std::find(_labels.begin(), _labels.end(), label) != _labels.end()) {
        return false;
    }
    _labels.push_back(label);
    _mark_dirty();
    return true;
}

bool Vertex::remove_label(LabelId label) {
    VertexUniqueLock guard(_mutex);
    auto it = std::find(_labels.begin(), _labels.end(), label);
    if (it == _labels.end()) {
        return false;
    }
    _labels.erase(it);
    _mark_dirty();
    return true;
}

bool Vertex::has_label(LabelId label) const {
    VertexSharedLock guard(_mutex);
    return std::find(_labels.begin(), _labels.end(), label) != _labels.end();
}

void Vertex::labels(kimix::vector<LabelId> &out) const {
    VertexSharedLock guard(_mutex);
    out = _labels;
}

void Vertex::assign_payload(LabelId key_label, kimix::span<const std::byte> external_id,
                            kimix::span<const LabelId> labels, PropertyList const &properties) {
    _key_label = key_label;
    _external_id = ExternalId::from_bytes(external_id);
    _labels.clear();
    for (auto label : labels) {
        if (label != kNoLabel) {
            _labels.push_back(label);
        }
    }
    _properties.clear();
    for (auto const &entry : properties) {
        _properties.emplace(entry.name, entry.value);
    }
}

void Vertex::restore_payload(kimix::span<const LabelId> labels,
                             PropertyList const &properties) {
    VertexUniqueLock guard(_mutex);
    _labels.clear();
    for (auto label : labels) {
        if (label != kNoLabel) {
            _labels.push_back(label);
        }
    }
    _properties.clear();
    for (auto const &entry : properties) {
        _properties.emplace(entry.name, entry.value);
    }
    _mark_dirty();
}

VertexKey Vertex::key() const {
    VertexKey out;
    out.label = _key_label;
    out.external_id = _external_id;
    return out;
}

// ---------------------------------------------------------------------------
// Over-aligned allocation (alignof(Vertex) == kCacheLineSize)
// ---------------------------------------------------------------------------

void *Vertex::operator new(size_t size) noexcept {
    return kimix::allocate_aligned(size, kCacheLineSize);
}

void *Vertex::operator new(size_t size, std::align_val_t align) noexcept {
    auto const requested = static_cast<size_t>(align);
    return kimix::allocate_aligned(size, requested > kCacheLineSize ? requested : kCacheLineSize);
}

void Vertex::operator delete(void *ptr) noexcept {
    // kimix::deallocate releases aligned blocks allocated by kimix::allocate_aligned too.
    kimix::deallocate(ptr);
}

void Vertex::operator delete(void *ptr, size_t) noexcept {
    kimix::deallocate(ptr);
}

void Vertex::operator delete(void *ptr, std::align_val_t) noexcept {
    kimix::deallocate(ptr);
}

void Vertex::operator delete(void *ptr, size_t, std::align_val_t) noexcept {
    kimix::deallocate(ptr);
}

} // namespace kimix::graph_db