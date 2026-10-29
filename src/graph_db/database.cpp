/*
 * database.cpp — the GDI backend facade (see database.h for the contract and the
 * per-command error semantics it inherits from the C# reference backend).
 *
 * STRUCTURE
 *   Every command follows the same three steps, in this order:
 *     1. the transaction gate: a write inside a read-only (collective) scope is
 *        error_read_only_transaction, before anything else is looked at;
 *     2. validation: the schema lookup + the declaration checks, all of which
 *        happen BEFORE any state changes (so a refusal leaves nothing behind);
 *     3. the mutation, immediately followed by its undo record (if a transaction
 *        is open) — and, when the journal is full, an immediate manual undo plus
 *        error_size_limit.
 *   Reads take the locks they need through Graph / EdgeTable and never hold two
 *   shard locks at once: they copy the ids under one lock, then resolve each
 *   entity separately. A read may therefore observe an edge added between the two
 *   passes — the documented epoch-1 read-atomicity contract, unchanged.
 *
 * ROLLBACK (abort)
 *   The journal is replayed in REVERSE: an entity created is freed, an entity
 *   removed is re-adopted (with its payload and, for a vertex, the edges its
 *   cascade removed), an edited payload is overwritten with the saved one, and a
 *   schema registration is re-created with its EXACT id (a recycled id would
 *   dangle every entity that referenced it).
 */
#include "database.h"

#include "transaction.h"

#include <core/clock.h>
#include <core/fiber.h>
#include <core/stl/filesystem.h>

#include <algorithm>
#include <utility>

namespace kimix::graph_db {

namespace src_graph_db_database {


[[nodiscard]] bool is_blank_name(kimix::string_view text) noexcept {
    for (char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_array_kind(PropertyKind kind) noexcept {
    // The four array kinds mirror the four container-array declarations: a
    // string_array against a char_ + array property type is an array exactly like
    // an int64_array against int64_ (ABI 7, plan §1.1 — without this arm the facade
    // rejects a legal string-array value as error_wrong_type), and a data_array
    // (ABI 9) is an array against the `data` sentinel dtype for the same reason.
    return kind == PropertyKind::int64_array || kind == PropertyKind::double_array ||
           kind == PropertyKind::string_array || kind == PropertyKind::data_array;
}

} // namespace src_graph_db_database
using namespace src_graph_db_database;

// ---------------------------------------------------------------------------
// Construction / store lifecycle
// ---------------------------------------------------------------------------

Database::Database() noexcept
    : _store(_cold) {
}

Database::Database(GraphOptions const &options) noexcept
    : _store(_cold),
      _options(options) {
    _graph.set_options(_options);
}

Database::~Database() noexcept {
    // An open transaction is aborted (its journal is replayed) before the graph
    // dies, so a caller that leaks one still gets a consistent graph for whoever
    // looks at it next.
    if (_tx != nullptr) {
        (void)abort();
    }
}

void Database::set_options(GraphOptions const &options) {
    _options = options;
    _graph.set_options(_options);
}

GdiStatus Database::open(kimix::filesystem::path const &dir) {
    return open(dir, _options);
}

GdiStatus Database::open(kimix::filesystem::path const &dir,
                         GraphOptions const &options) {
    if (is_open()) {
        return GdiStatus::error_state;
    }
    set_options(options);
    if (dir.empty()) {
        // An in-memory database: no store, so persist() reports error_no_such_file
        // and nothing is ever read back. That is what a purely in-memory GDI
        // session wants.
        return GdiStatus::ok;
    }
    // --- backend selection (cold_store.h is the seam both backends implement) ---
    // The two branches differ only in which store instance is opened and in which
    // one the handle below points at: the save/load path behind it is the same
    // code, so an lmdb database behaves exactly as it did before this choice
    // existed. A failed open of either backend leaves the database closed, and
    // every later persist()/load_store() reports error_no_such_file — the same
    // observable state a bad store path has always produced.
    if (_options.store_backend == ColdStoreBackend::flat_file) {
        // cold_map_size is an LMDB knob: FileColdStore accepts and ignores it.
        if (!_cold_file.open(dir, _options.cold_map_size)) {
            return GdiStatus::error_io;
        }
        _backend = ColdStoreBackend::flat_file;
        _store = ColdStore{_cold_file};
    } else {
        size_t const map_size = _options.cold_map_size != 0
                                    ? _options.cold_map_size
                                    : ColdStorage::default_map_size;
        if (!_cold.open(dir, map_size)) {
            return GdiStatus::error_io;
        }
        _backend = ColdStoreBackend::lmdb;
        _store = ColdStore{_cold};
    }
    // The serializer is built from the live handle here, not in the constructor:
    // which backend it talks to is only known once the store is open.
    _serializer.emplace(_store);
    _serializer->set_schema(&_schema);
    MetaRecord meta;
    if (_store.read_meta(meta)) {
        _serializer->set_meta(meta);
    }
    return load_store();
}

void Database::close() noexcept {
    if (_tx != nullptr) {
        (void)abort();
    }
    // Reset the serializer FIRST: it holds a copy of the handle, so it must not
    // outlive the store it refers to.
    _serializer.reset();
    _cold.close();
    _cold_file.close();
    _store = ColdStore{_cold};
    _backend = ColdStoreBackend::lmdb;
}

GdiStatus Database::persist(bool full, GraphSerializeStats *out) {
    if (!is_open() || !_serializer) {
        return GdiStatus::error_no_such_file;
    }
    // The schema and the options own the metadata a save stamps; hand them over so
    // a store is always self-describing.
    MetaRecord meta = _serializer->meta();
    meta.format_version = kFormatVersion;
    meta.graph_name = _options.graph_name;
    meta.schema_version = _schema.version();
    _serializer->set_meta(meta);
    // A FULL save replaces the file backend's file (it is a complete snapshot of
    // the graph) instead of appending to it; the seam's save_batch() carries no
    // such flag, so the store is told about it here. The store consumes the arm on
    // its next save_batch() and it is cleared again below, so a save that writes
    // nothing can never affect a later incremental one.
    bool const rebuild = full && _backend == ColdStoreBackend::flat_file;
    _cold_file.set_rebuild_next_batch(rebuild);
    GraphSerializeStats const stats = full ? _serializer->save(_graph)
                                           : _serializer->save_incremental(_graph);
    _cold_file.set_rebuild_next_batch(false);
    _last_stats = stats;
    if (out != nullptr) {
        *out = stats;
    }
    return stats.status;
}

GdiStatus Database::load_store(GraphSerializeStats *out) {
    if (!is_open()) {
        return GdiStatus::error_no_such_file;
    }
    GraphSerializeStats const stats = _deserializer.load(_store, _graph, &_schema);
    _last_stats = stats;
    if (out != nullptr) {
        *out = stats;
    }
    return stats.status;
}

// ---------------------------------------------------------------------------
// Internals
// ---------------------------------------------------------------------------

GdiStatus Database::_require_vertex(kimix::Guid const &uid, Vertex *&out) const {
    out = _graph.find(uid);
    return out == nullptr ? GdiStatus::error_vertex : GdiStatus::ok;
}

GdiStatus Database::_validate_property(PropNameId name, PropertyValue const &value) const {
    if (name == kNoPropName || !_schema.has_property_type(name)) {
        return GdiStatus::error_property_type;
    }
    if (value.is_none()) {
        return GdiStatus::error_wrong_type;
    }
    PropertyTypeInfo const declaration = _schema.property_type(name);
    bool const wants_array = declaration.container == GdiContainerType::array;
    if (is_array_kind(value.kind()) != wants_array) {
        // A scalar where an array is declared (or the inverse) is a type error,
        // not a count error.
        return GdiStatus::error_wrong_type;
    }
    if (value.data_type() != declaration.data) {
        // A data_array's data_type() is the `data` sentinel, so this one comparison
        // rejects a data array against an (array, int64) declaration AND an int64
        // array against an (array, data) one — both are error_wrong_type (ABI 9).
        return GdiStatus::error_wrong_type;
    }
    // The length rule lives in ONE place (PropertyValue::matches_declaration):
    // fixed needs exactly `count` elements, max at most `count`, unlimited any
    // number (ABI 8, plan §1.1/§1.3). A refusal is a COUNT error here — the other
    // two refusals above are type errors.
    if (wants_array &&
        !value.matches_declaration(declaration.container, declaration.data,
                                   declaration.size_type, declaration.count)) {
        return GdiStatus::error_count;
    }
    return GdiStatus::ok;
}

GdiStatus Database::_resolve_constraint(Constraint const &constraint,
                                        Constraint const *&out) const {
    // Plan O9: this used to COPY the whole Constraint (`out = constraint`, a
    // kimix::vector<ConstraintCondition> whose every element carries a
    // PropertyValue) on every call — one avoidable allocation per facade read, and
    // a second one on the C ABI path on top of the walk it feeds. Nothing needs
    // the copy: the caller's object is const for the whole call and every consumer
    // takes it by const reference, so handing back the caller's own constraint is
    // equivalent. The RULES are untouched — a caller may hand in a definition or
    // (through the C ABI) a handle, which is already resolved before this point; an
    // empty constraint matches everything; and here the definition itself is
    // validated: a label condition with an ordering op cannot be evaluated.
    // `out` is set only on success (the error paths return before it is written).
    for (auto const &condition : constraint.conditions) {
        if (condition.is_label()) {
            if (condition.op != GdiOp::equal && condition.op != GdiOp::not_equal) {
                return GdiStatus::error_op;
            }
        }
    }
    out = &constraint;
    return GdiStatus::ok;
}

bool Database::_property_values_exist(PropNameId name) const {
    for (auto const &guid : _graph.vertex_guids()) {
        Vertex *vertex = _graph.find(guid);
        if (vertex == nullptr) {
            continue;
        }
        if (vertex->has_property(name)) {
            return true;
        }
    }
    for (auto const &edge : _graph.edge_ids()) {
        PropertyValue value;
        if (_graph.edge_property(edge, name, value)) {
            return true;
        }
    }
    return false;
}

bool Database::_label_is_a_key(LabelId label) const {
    for (auto const &guid : _graph.vertex_guids()) {
        Vertex *vertex = _graph.find(guid);
        if (vertex != nullptr && vertex->key_label() == label) {
            return true;
        }
    }
    return false;
}

bool Database::_journal_vertex_payload(kimix::Guid const &uid) {
    if (_tx == nullptr) {
        return true;
    }
    Vertex *vertex = _graph.find(uid);
    if (vertex == nullptr) {
        return true;
    }
    JournalEntry entry;
    entry.kind = JournalEntry::Kind::vertex_payload;
    entry.uid = uid;
    entry.fat.reset(new JournalEntry::Fat);
    vertex->labels(entry.fat->labels);
    entry.fat->properties = vertex->property_list();
    return _tx->push(std::move(entry));
}

bool Database::_journal_edge_state(kimix::Guid const &edge) {
    if (_tx == nullptr) {
        return true;
    }
    EdgeRecord record;
    if (!_graph.edge_record(edge, record)) {
        return true;
    }
    JournalEntry entry;
    entry.kind = JournalEntry::Kind::edge_state;
    entry.uid = edge;
    entry.fat.reset(new JournalEntry::Fat);
    entry.fat->origin = record.origin;
    entry.fat->target = record.target;
    entry.fat->direction = record.direction;
    entry.fat->labels = record.labels;
    entry.fat->properties = record.properties;
    return _tx->push(std::move(entry));
}

void Database::_strip_label(LabelId label) {
    for (auto const &guid : _graph.vertex_guids()) {
        Vertex *vertex = _graph.find(guid);
        if (vertex == nullptr || !vertex->has_label(label)) {
            continue;
        }
        // Journal BEFORE the strip: the undo entry is the payload that still
        // carries the label.
        (void)_journal_vertex_payload(guid);
        (void)vertex->remove_label(label);
    }
    for (auto const &edge : _graph.edge_ids()) {
        kimix::vector<LabelId> labels;
        if (!_graph.edge_labels(edge, labels) ||
            std::find(labels.begin(), labels.end(), label) == labels.end()) {
            continue;
        }
        (void)_journal_edge_state(edge);
        (void)_graph.edge_remove_label(edge, label);
    }
}

void Database::_strip_property_type(PropNameId name) {
    kimix::vector<PropNameId> names;
    names.push_back(name);
    for (auto const &guid : _graph.vertex_guids()) {
        Vertex *vertex = _graph.find(guid);
        if (vertex == nullptr || !vertex->has_property(name)) {
            continue;
        }
        (void)_journal_vertex_payload(guid);
        (void)vertex->remove_properties(
            kimix::span<PropNameId const>{names.data(), names.size()});
    }
    for (auto const &edge : _graph.edge_ids()) {
        PropertyValue value;
        if (!_graph.edge_property(edge, name, value)) {
            continue;
        }
        (void)_journal_edge_state(edge);
        (void)_graph.edge_remove_properties(edge,
                                            kimix::span<PropNameId const>{names.data(),
                                                                          names.size()});
    }
}

// ---------------------------------------------------------------------------
// Vertices
// ---------------------------------------------------------------------------

GdiStatus Database::create_vertex(VertexKeyView key, kimix::span<const LabelId> labels,
                                  PropertyList const *properties, VertexCreateResult &out) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (key.label != kNoLabel) {
        if (!_schema.has_label(key.label)) {
            return GdiStatus::error_label;
        }
        if (key.external_id.empty()) {
            return GdiStatus::error_empty_name; // GDI's VertexKey.Validate
        }
    } else if (!key.external_id.empty()) {
        return GdiStatus::error_argument;
    }
    for (auto label : labels) {
        if (!_schema.has_label(label)) {
            return GdiStatus::error_label;
        }
    }
    if (properties != nullptr) {
        for (auto const &entry : *properties) {
            GdiStatus const status = _validate_property(entry.name, entry.value);
            if (is_error(status)) {
                return status;
            }
        }
    }

    Graph::VertexSpec spec;
    spec.key_label = key.label;
    spec.external_id = key.external_id;
    spec.labels = labels;
    spec.properties = properties;
    kimix::Guid uid;
    GdiStatus const status = _graph.create_vertex(spec, uid);
    if (is_error(status)) {
        return status; // error_non_unique_id / error_empty_name / error_argument
    }
    // The journal entry for a creation is just its uid: freeing it restores the
    // previous state exactly (the key index entry goes with it).
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::vertex_created;
        entry.uid = uid;
        if (!_tx->push(std::move(entry))) {
            (void)_graph.free_vertex(uid, /*guarded=*/false);
            return GdiStatus::error_size_limit;
        }
    }
    out.uid = uid;
    out.non_unique_id_warning = status == GdiStatus::warning_non_unique_id;
    return status;
}

GdiStatus Database::get_or_create_vertex(VertexKeyView key, kimix::span<const LabelId> labels,
                                         PropertyList const *properties,
                                         VertexMergeResult &out) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (key.label != kNoLabel && !_schema.has_label(key.label)) {
        return GdiStatus::error_label;
    }
    Graph::VertexSpec spec;
    spec.key_label = key.label;
    spec.external_id = key.external_id;
    spec.labels = labels;
    spec.properties = properties;
    kimix::Guid uid;
    bool created = false;
    GdiStatus status = _graph.get_or_create_vertex(spec, uid, created);
    if (is_error(status)) {
        return status;
    }
    if (created && _tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::vertex_created;
        entry.uid = uid;
        if (!_tx->push(std::move(entry))) {
            (void)_graph.free_vertex(uid, /*guarded=*/false);
            return GdiStatus::error_size_limit;
        }
    }
    out.uid = uid;
    out.created = created;
    return GdiStatus::ok;
}

GdiStatus Database::translate_vertex_id(VertexKeyView key, kimix::Guid &out_uid,
                                        bool &found) const {
    return _graph.translate_vertex_id(key, out_uid, found);
}

GdiStatus Database::free_vertex(kimix::Guid const &uid) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    kimix::vector<kimix::unique_ptr<Edge>> removed_edges;
    kimix::unique_ptr<Vertex> removed =
        _graph.remove_vertex(uid, _tx != nullptr ? &removed_edges : nullptr);
    if (!removed) {
        return GdiStatus::error_vertex;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::vertex_removed;
        entry.uid = uid;
        entry.fat.reset(new JournalEntry::Fat);
        entry.fat->labels.clear();
        removed->labels(entry.fat->labels);
        entry.fat->properties = removed->property_list();
        entry.fat->vertex = std::move(removed);
        entry.fat->removed_edges = std::move(removed_edges);
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return GdiStatus::ok;
}

GdiStatus Database::free_vertex_guarded(kimix::Guid const &uid) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (!_graph.contains(uid)) {
        return GdiStatus::error_vertex;
    }
    if (_graph.has_incident_edges(uid, EdgeOrientation::both)) {
        // GDI FreeVertexGuarded: ErrorEdge, and NOTHING removed.
        return GdiStatus::error_edge;
    }
    return free_vertex(uid);
}

GdiStatus Database::create_vertices_bulk(kimix::span<VertexBulkRow const> rows,
                                         bool continue_on_error,
                                         kimix::vector<VertexCreateResult> &results,
                                         kimix::vector<GdiStatus> *row_status,
                                         BulkStats &stats) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    results.clear();
    results.reserve(rows.size());
    if (row_status != nullptr) {
        row_status->clear();
        row_status->reserve(rows.size());
    }

    // ONE transaction for the whole batch: a local scope when the caller has none
    // open, so "N creates, one atomic unit" holds without the caller having to
    // begin anything (GDI's CreateVerticesBulkCommand shape). The local object is
    // built LAZILY: a stack Transaction's destructor aborts the DATABASE's
    // current transaction when still open, which — with a caller transaction
    // open — would abort the CALLER's (plan v3 §4.4: the old eager construction
    // made the native batch unusable inside an engine transaction).
    Transaction *previous = _tx;
    bool const own_scope = previous == nullptr;
    kimix::unique_ptr<Transaction> local;
    if (own_scope) {
        local.reset(new Transaction(*this, /*read_only=*/false, _options.max_journal_bytes));
        _tx = local.get();
    }

    bool aborted = false;
    for (size_t i = 0; i < rows.size(); ++i) {
        VertexCreateResult result;
        GdiStatus status = GdiStatus::ok;
        if (aborted) {
            // Never attempted: reported, but not counted as a failure.
            status = GdiStatus::error_state;
        } else {
            status = create_vertex(VertexKeyView{rows[i].key.label,
                                                 rows[i].key.external_id.bytes()},
                                   kimix::span<const LabelId>{rows[i].labels.data(),
                                                              rows[i].labels.size()},
                                   &rows[i].properties, result);
            if (is_error(status)) {
                ++stats.failed;
            } else if (status == GdiStatus::warning_non_unique_id) {
                ++stats.created;
                ++stats.warning;
            } else {
                ++stats.created;
            }
        }
        if (is_error(status) && !aborted && !continue_on_error) {
            // All-or-nothing: the first failure aborts the whole batch and every
            // remaining row is marked error_state (never attempted). The batch
            // created NOTHING, so the counters are reset — only the row that
            // actually failed stays in `failed`.
            aborted = true;
              if (own_scope) {
                  (void)_rollback(*local);
                  local->clear_journal();
              } else {
                  (void)_rollback(*_tx);
                  _tx->clear_journal();
              }
            stats.created = 0;
            stats.warning = 0;
        }
        results.push_back(result);
        if (row_status != nullptr) {
            row_status->push_back(status);
        }
    }
      if (own_scope) {
          _tx = previous;
          local->clear_journal();
      }
      return aborted ? GdiStatus::error_state : GdiStatus::ok;
}

GdiStatus Database::create_edges_bulk(kimix::span<EdgeBulkRow const> rows,
                                      bool continue_on_error,
                                      kimix::vector<kimix::Guid> &edge_ids,
                                      kimix::vector<GdiStatus> *row_status,
                                      BulkStats &stats) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    edge_ids.clear();
    edge_ids.reserve(rows.size());
    if (row_status != nullptr) {
        row_status->clear();
        row_status->reserve(rows.size());
    }
    // PLAN V3 §5.4: reserve each endpoint's link vectors at its EXACT final
    // degree before the create loop. Without this every vertex's _out_links /
    // _in_links grow 1 -> 2 -> 4 -> 8 for a 6-edge workload: four allocation
    // waves of retained heap plus 25 % capacity slack, which the staged RSS
    // attribution measures as per-edge overhead. The reserve pass runs under
    // the single-writer contract (same as the creates below).
    {
        kimix::unordered_map<kimix::Guid, uint64_t> degrees;
        // ~1/4 of the rows are distinct endpoints on a typical bulk load
        // (graphbench: 50k endpoints for 300k rows); over-reserving the full
        // row count would churn a transient map bigger than the slack it
        // saves. ankerl only grows, never rehashes downward.
        degrees.reserve(rows.size() / 4 + 64);
        for (auto const &row : rows) {
            if (row.origin != row.target) {
                degrees[row.origin] += 1; // outgoing count in the low half
                degrees[row.target] += 1ull << 32; // incoming count in the high half
            }
        }
        for (auto const &entry : degrees) {
            uint64_t const outgoing = entry.second & 0xffffffffull;
            uint64_t const incoming = entry.second >> 32;
            (void)_graph.reserve_links(entry.first, static_cast<size_t>(outgoing),
                                       static_cast<size_t>(incoming));
        }
    }
    bool aborted = false;
    for (auto const &row : rows) {
        kimix::Guid edge;
        GdiStatus status = GdiStatus::ok;
        if (aborted) {
            status = GdiStatus::error_state; // never attempted, not counted
        } else {
            status = edge_create(row.origin, row.target, row.direction,
                                 kimix::span<const LabelId>{row.labels.data(),
                                                            row.labels.size()},
                                 &row.properties, row.has_uid ? &row.uid : nullptr, edge);
            if (is_error(status)) {
                ++stats.failed;
                if (!continue_on_error) {
                    aborted = true;
                }
            } else {
                ++stats.created;
            }
        }
        edge_ids.push_back(edge);
        if (row_status != nullptr) {
            row_status->push_back(status);
        }
    }
    return aborted ? GdiStatus::error_state : GdiStatus::ok;
}

// ---------------------------------------------------------------------------
// Vertex labels and properties
// ---------------------------------------------------------------------------

GdiStatus Database::add_label(kimix::Guid const &uid, LabelId label) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    Vertex *vertex = nullptr;
    GdiStatus status = _require_vertex(uid, vertex);
    if (is_error(status)) {
        return status;
    }
    if (!_schema.has_label(label)) {
        return GdiStatus::error_label;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::vertex_payload;
        entry.uid = uid;
        entry.fat.reset(new JournalEntry::Fat);
        vertex->labels(entry.fat->labels);
        entry.fat->properties = vertex->property_list();
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    // The vertex lock is taken by Vertex itself (level 5, no shard lock held).
    return vertex->add_label(label) ? GdiStatus::ok : GdiStatus::error_label;
}

GdiStatus Database::remove_label(kimix::Guid const &uid, LabelId label) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    Vertex *vertex = nullptr;
    GdiStatus status = _require_vertex(uid, vertex);
    if (is_error(status)) {
        return status;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::vertex_payload;
        entry.uid = uid;
        entry.fat.reset(new JournalEntry::Fat);
        vertex->labels(entry.fat->labels);
        entry.fat->properties = vertex->property_list();
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return vertex->remove_label(label) ? GdiStatus::ok : GdiStatus::error_label;
}

GdiStatus Database::get_labels(kimix::Guid const &uid,
                               kimix::vector<kimix::string> &out) const {
    Vertex *vertex = nullptr;
    GdiStatus const status = _require_vertex(uid, vertex);
    if (is_error(status)) {
        return status;
    }
    out.clear();
    // GDI order: the KEY label first, then the entity's own labels.
    if (vertex->key_label() != kNoLabel) {
        out.push_back(_schema.label_name(vertex->key_label()));
    }
    kimix::vector<LabelId> labels;
    vertex->labels(labels);
    for (auto label : labels) {
        if (label == vertex->key_label()) {
            continue; // already emitted first
        }
        out.push_back(_schema.label_name(label));
    }
    return GdiStatus::ok;
}

GdiStatus Database::add_property(kimix::Guid const &uid, PropNameId name,
                                 PropertyValue value) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    Vertex *vertex = nullptr;
    GdiStatus status = _require_vertex(uid, vertex);
    if (is_error(status)) {
        return status;
    }
    status = _validate_property(name, value);
    if (is_error(status)) {
        return status;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::vertex_payload;
        entry.uid = uid;
        entry.fat.reset(new JournalEntry::Fat);
        vertex->labels(entry.fat->labels);
        entry.fat->properties = vertex->property_list();
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return vertex->add_property(name, std::move(value)) ? GdiStatus::ok
                                                        : GdiStatus::error_property_exists;
}

GdiStatus Database::set_property(kimix::Guid const &uid, PropNameId name,
                                 PropertyValue value) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    Vertex *vertex = nullptr;
    GdiStatus status = _require_vertex(uid, vertex);
    if (is_error(status)) {
        return status;
    }
    status = _validate_property(name, value);
    if (is_error(status)) {
        return status;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::vertex_payload;
        entry.uid = uid;
        entry.fat.reset(new JournalEntry::Fat);
        vertex->labels(entry.fat->labels);
        entry.fat->properties = vertex->property_list();
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return vertex->set_property(name, std::move(value)) ? GdiStatus::ok
                                                        : GdiStatus::error_no_property;
}

GdiStatus Database::update_property(kimix::Guid const &uid, PropNameId name,
                                    PropertyValue value) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    Vertex *vertex = nullptr;
    GdiStatus status = _require_vertex(uid, vertex);
    if (is_error(status)) {
        return status;
    }
    status = _validate_property(name, value);
    if (is_error(status)) {
        return status;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::vertex_payload;
        entry.uid = uid;
        entry.fat.reset(new JournalEntry::Fat);
        vertex->labels(entry.fat->labels);
        entry.fat->properties = vertex->property_list();
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    (void)vertex->put_property(name, std::move(value)); // upsert: never an error
    return GdiStatus::ok;
}

GdiStatus Database::remove_properties(kimix::Guid const &uid,
                                      kimix::span<const PropNameId> names) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    Vertex *vertex = nullptr;
    GdiStatus status = _require_vertex(uid, vertex);
    if (is_error(status)) {
        return status;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::vertex_payload;
        entry.uid = uid;
        entry.fat.reset(new JournalEntry::Fat);
        vertex->labels(entry.fat->labels);
        entry.fat->properties = vertex->property_list();
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    (void)vertex->remove_properties(names); // absent names are not an error
    return GdiStatus::ok;
}

GdiStatus Database::get_properties(kimix::Guid const &uid, NamedPropertyList &out) const {
    Vertex *vertex = nullptr;
    GdiStatus const status = _require_vertex(uid, vertex);
    if (is_error(status)) {
        return status;
    }
    out.clear();
    PropertyList properties;
    vertex->properties(properties);
    out.reserve(properties.size());
    for (auto const &entry : properties) {
        NamedProperty item;
        item.name = _schema.property_type(entry.name).name;
        item.value = entry.value;
        out.push_back(std::move(item));
    }
    return GdiStatus::ok;
}

GdiStatus Database::get_property_types(kimix::Guid const &uid, PropertyTypeList &out) const {
    Vertex *vertex = nullptr;
    GdiStatus const status = _require_vertex(uid, vertex);
    if (is_error(status)) {
        return status;
    }
    out.clear();
    PropertyList properties;
    vertex->properties(properties);
    out.reserve(properties.size());
    for (auto const &entry : properties) {
        out.push_back(_schema.property_type(entry.name));
    }
    std::sort(out.begin(), out.end(),
              [](PropertyTypeInfo const &lhs, PropertyTypeInfo const &rhs) {
                  return lhs.name < rhs.name;
              });
    return GdiStatus::ok;
}

// ---------------------------------------------------------------------------
// Edges (delegated to Graph, which owns the shard choreography)
// ---------------------------------------------------------------------------

GdiStatus Database::edge_create(kimix::Guid const &origin, kimix::Guid const &target,
                                EdgeDirectionType direction,
                                kimix::span<const LabelId> labels,
                                PropertyList const *properties, kimix::Guid const *chosen_uid,
                                kimix::Guid &out_edge) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    for (auto label : labels) {
        if (!_schema.has_label(label)) {
            return GdiStatus::error_label;
        }
    }
    if (properties != nullptr) {
        for (auto const &entry : *properties) {
            GdiStatus const status = _validate_property(entry.name, entry.value);
            if (is_error(status)) {
                return status;
            }
        }
    }
    // ABI 8 (plan §3.2): `chosen_uid` names the edge (nullptr = mint). The journal
    // entry then carries that same uid, so an abort frees exactly the edge this call
    // created — and a free + recreate under one uid inside a transaction replays as
    // "free the new record, adopt the old fat record", which is the ordering the
    // journal already had.
    GdiStatus const status = _graph.edge_create(origin, target, direction, labels, properties,
                                               chosen_uid, out_edge);
    if (is_error(status)) {
        return status;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::edge_created;
        entry.uid = out_edge;
        if (!_tx->push(std::move(entry))) {
            (void)_graph.edge_free(out_edge);
            return GdiStatus::error_size_limit;
        }
    }
    return GdiStatus::ok;
}

GdiStatus Database::edge_free(kimix::Guid const &edge) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (_tx != nullptr) {
        // Keep the record so an abort can put it back verbatim.
        EdgeRecord record;
        if (!_graph.edge_record(edge, record)) {
            return GdiStatus::error_uid;
        }
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::edge_removed;
        entry.uid = edge;
        entry.fat.reset(new JournalEntry::Fat);
        entry.fat->origin = record.origin;
        entry.fat->target = record.target;
        entry.fat->direction = record.direction;
        entry.fat->labels = record.labels;
        entry.fat->properties = record.properties;
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return _graph.edge_free(edge);
}

GdiStatus Database::set_edge_origin(kimix::Guid const &edge, kimix::Guid const &origin) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (_tx != nullptr) {
        EdgeRecord record;
        if (!_graph.edge_record(edge, record)) {
            return GdiStatus::error_uid;
        }
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::edge_state;
        entry.uid = edge;
        entry.fat.reset(new JournalEntry::Fat);
        entry.fat->origin = record.origin;
        entry.fat->target = record.target;
        entry.fat->direction = record.direction;
        entry.fat->labels = record.labels;
        entry.fat->properties = record.properties;
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return _graph.edge_set_origin(edge, origin);
}

GdiStatus Database::set_edge_target(kimix::Guid const &edge, kimix::Guid const &target) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (_tx != nullptr) {
        EdgeRecord record;
        if (!_graph.edge_record(edge, record)) {
            return GdiStatus::error_uid;
        }
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::edge_state;
        entry.uid = edge;
        entry.fat.reset(new JournalEntry::Fat);
        entry.fat->origin = record.origin;
        entry.fat->target = record.target;
        entry.fat->direction = record.direction;
        entry.fat->labels = record.labels;
        entry.fat->properties = record.properties;
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return _graph.edge_set_target(edge, target);
}

GdiStatus Database::set_edge_direction(kimix::Guid const &edge,
                                       EdgeDirectionType direction) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (_tx != nullptr) {
        EdgeRecord record;
        if (!_graph.edge_record(edge, record)) {
            return GdiStatus::error_uid;
        }
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::edge_state;
        entry.uid = edge;
        entry.fat.reset(new JournalEntry::Fat);
        entry.fat->origin = record.origin;
        entry.fat->target = record.target;
        entry.fat->direction = record.direction;
        entry.fat->labels = record.labels;
        entry.fat->properties = record.properties;
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return _graph.edge_set_direction(edge, direction);
}

GdiStatus Database::get_edge_endpoints(kimix::Guid const &edge, kimix::Guid &origin,
                                       kimix::Guid &target) const {
    return _graph.edge_endpoints(edge, origin, target) ? GdiStatus::ok
                                                       : GdiStatus::error_uid;
}

GdiStatus Database::add_edge_label(kimix::Guid const &edge, LabelId label) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (!_schema.has_label(label)) {
        return GdiStatus::error_label;
    }
    if (_tx != nullptr) {
        EdgeRecord record;
        if (!_graph.edge_record(edge, record)) {
            return GdiStatus::error_uid;
        }
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::edge_state;
        entry.uid = edge;
        entry.fat.reset(new JournalEntry::Fat);
        entry.fat->origin = record.origin;
        entry.fat->target = record.target;
        entry.fat->direction = record.direction;
        entry.fat->labels = record.labels;
        entry.fat->properties = record.properties;
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return _graph.edge_add_label(edge, label);
}

GdiStatus Database::remove_edge_label(kimix::Guid const &edge, LabelId label) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (_tx != nullptr) {
        EdgeRecord record;
        if (!_graph.edge_record(edge, record)) {
            return GdiStatus::error_uid;
        }
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::edge_state;
        entry.uid = edge;
        entry.fat.reset(new JournalEntry::Fat);
        entry.fat->origin = record.origin;
        entry.fat->target = record.target;
        entry.fat->direction = record.direction;
        entry.fat->labels = record.labels;
        entry.fat->properties = record.properties;
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return _graph.edge_remove_label(edge, label);
}

GdiStatus Database::get_edge_labels(kimix::Guid const &edge,
                                    kimix::vector<kimix::string> &out) const {
    out.clear();
    kimix::vector<LabelId> labels;
    if (!_graph.edge_labels(edge, labels)) {
        return GdiStatus::error_uid;
    }
    out.reserve(labels.size());
    for (auto label : labels) {
        out.push_back(_schema.label_name(label));
    }
    return GdiStatus::ok;
}

GdiStatus Database::add_edge_property(kimix::Guid const &edge, PropNameId name,
                                      PropertyValue value) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    GdiStatus const status = _validate_property(name, value);
    if (is_error(status)) {
        return status;
    }
    if (_tx != nullptr) {
        EdgeRecord record;
        if (!_graph.edge_record(edge, record)) {
            return GdiStatus::error_uid;
        }
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::edge_state;
        entry.uid = edge;
        entry.fat.reset(new JournalEntry::Fat);
        entry.fat->origin = record.origin;
        entry.fat->target = record.target;
        entry.fat->direction = record.direction;
        entry.fat->labels = record.labels;
        entry.fat->properties = record.properties;
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return _graph.edge_add_property(edge, name, std::move(value));
}

GdiStatus Database::set_edge_property(kimix::Guid const &edge, PropNameId name,
                                      PropertyValue value) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    GdiStatus const status = _validate_property(name, value);
    if (is_error(status)) {
        return status;
    }
    if (_tx != nullptr) {
        EdgeRecord record;
        if (!_graph.edge_record(edge, record)) {
            return GdiStatus::error_uid;
        }
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::edge_state;
        entry.uid = edge;
        entry.fat.reset(new JournalEntry::Fat);
        entry.fat->origin = record.origin;
        entry.fat->target = record.target;
        entry.fat->direction = record.direction;
        entry.fat->labels = record.labels;
        entry.fat->properties = record.properties;
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return _graph.edge_set_property(edge, name, std::move(value));
}

GdiStatus Database::update_edge_property(kimix::Guid const &edge, PropNameId name,
                                         PropertyValue value) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    GdiStatus const status = _validate_property(name, value);
    if (is_error(status)) {
        return status;
    }
    if (_tx != nullptr) {
        EdgeRecord record;
        if (!_graph.edge_record(edge, record)) {
            return GdiStatus::error_uid;
        }
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::edge_state;
        entry.uid = edge;
        entry.fat.reset(new JournalEntry::Fat);
        entry.fat->origin = record.origin;
        entry.fat->target = record.target;
        entry.fat->direction = record.direction;
        entry.fat->labels = record.labels;
        entry.fat->properties = record.properties;
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return _graph.edge_put_property(edge, name, std::move(value));
}

GdiStatus Database::remove_edge_properties(kimix::Guid const &edge,
                                           kimix::span<const PropNameId> names) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (_tx != nullptr) {
        EdgeRecord record;
        if (!_graph.edge_record(edge, record)) {
            return GdiStatus::error_uid;
        }
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::edge_state;
        entry.uid = edge;
        entry.fat.reset(new JournalEntry::Fat);
        entry.fat->origin = record.origin;
        entry.fat->target = record.target;
        entry.fat->direction = record.direction;
        entry.fat->labels = record.labels;
        entry.fat->properties = record.properties;
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return _graph.edge_remove_properties(edge, names);
}

GdiStatus Database::get_edge_properties(kimix::Guid const &edge,
                                        NamedPropertyList &out) const {
    out.clear();
    PropertyList properties;
    if (!_graph.edge_properties(edge, properties)) {
        return GdiStatus::error_uid;
    }
    out.reserve(properties.size());
    for (auto const &entry : properties) {
        NamedProperty item;
        item.name = _schema.property_type(entry.name).name;
        item.value = entry.value;
        out.push_back(std::move(item));
    }
    return GdiStatus::ok;
}

GdiStatus Database::get_edge_property_types(kimix::Guid const &edge,
                                            PropertyTypeList &out) const {
    out.clear();
    PropertyList properties;
    if (!_graph.edge_properties(edge, properties)) {
        return GdiStatus::error_uid;
    }
    out.reserve(properties.size());
    for (auto const &entry : properties) {
        out.push_back(_schema.property_type(entry.name));
    }
    std::sort(out.begin(), out.end(),
              [](PropertyTypeInfo const &lhs, PropertyTypeInfo const &rhs) {
                  return lhs.name < rhs.name;
              });
    return GdiStatus::ok;
}

// ---------------------------------------------------------------------------
// Filtered, oriented, paged reads
// ---------------------------------------------------------------------------

GdiStatus Database::get_edges_of_vertex(kimix::Guid const &uid,
                                        Constraint const &constraint,
                                        EdgeOrientation orientation, int64_t cap,
                                        EdgePage &out) const {
    out = EdgePage{};
    if (cap < 0) {
        return GdiStatus::error_argument;
    }
    if (!_graph.contains(uid)) {
        return GdiStatus::error_vertex;
    }
    Constraint const *resolved_ref = nullptr;
    GdiStatus status = _resolve_constraint(constraint, resolved_ref);
    if (is_error(status)) {
        return status;
    }
    Constraint const &resolved = *resolved_ref;
    kimix::vector<EdgeId> ids;
    _graph.incident_edges(uid, orientation, ids);
    kimix::vector<EdgeRecord> matches;
    matches.reserve(ids.size());
    for (auto const &id : ids) {
        EdgeRecord record;
        if (!_graph.edge_record(id, record)) {
            continue; // removed between the two passes (epoch-1 read atomicity)
        }
        // §2.6: an EDGE read filters the edge's own labels and properties.
        if (!kimix::graph_db::matches(resolved, record.labels, record.properties)) {
            continue;
        }
        matches.push_back(std::move(record));
    }
    out = make_page(std::move(matches), cap);
    return GdiStatus::ok;
}

GdiStatus Database::get_neighbor_vertices(kimix::Guid const &uid,
                                          Constraint const &constraint,
                                          EdgeOrientation orientation, int64_t cap,
                                          VertexPage &out) const {
    out = VertexPage{};
    if (cap < 0) {
        return GdiStatus::error_argument;
    }
    if (!_graph.contains(uid)) {
        return GdiStatus::error_vertex;
    }
    Constraint const *resolved_ref = nullptr;
    GdiStatus status = _resolve_constraint(constraint, resolved_ref);
    if (is_error(status)) {
        return status;
    }
    Constraint const &resolved = *resolved_ref;
    if (resolved.conditions.empty()) {
        // Plan v3 §4.1 (the degree-counter epoch): the UNFILTERED neighbourhood
        // read no longer resolves every neighbour's own record — the labels /
        // properties / matches work below is pure overhead when the constraint
        // matches everything, and the per-edge edge_endpoints round-trips cost
        // two more hash lookups per link. One shard SHARED lock snapshot of the
        // oriented guid projection, deduped here; distinct-neighbour semantics
        // are unchanged (parallel edges, both-orientation overlap and self
        // loops collapse exactly as before). The count probe (cap == 0) counts
        // the distinct guids without materialising any items.
        AdjacencyBuffer adjacency;
        if (!_graph.oriented_adjacency(uid, orientation, adjacency)) {
            return GdiStatus::error_vertex;
        }
        if (cap == 0) {
            kimix::unordered_set<kimix::Guid> seen;
            seen.reserve(adjacency.size());
            for (auto const &guid : adjacency) {
                seen.insert(guid);
            }
            out.total_count = seen.size();
            return GdiStatus::ok;
        }
        kimix::vector<kimix::Guid> matches;
        matches.reserve(adjacency.size());
        kimix::unordered_set<kimix::Guid> seen;
        seen.reserve(adjacency.size());
        for (auto const &guid : adjacency) {
            if (seen.insert(guid).second) {
                matches.push_back(guid);
            }
        }
        out = make_page(std::move(matches), cap);
        return GdiStatus::ok;
    }
    // §6.6: the filtered path uses the SAME one-lock adjacency snapshot as the
    // unfiltered path (the link entries carry the neighbour guid, so the old
    // incident_edges + per-edge edge_endpoints round-trips - a shard-locked
    // map lookup PER EDGE - are gone), and evaluates the constraint through
    // the zero-copy probe (no per-neighbour property_list deep copy: a
    // 256-char string property was a heap alloc + memcpy per neighbour).
    AdjacencyBuffer adjacency;
    if (!_graph.oriented_adjacency(uid, orientation, adjacency)) {
        return GdiStatus::error_vertex;
    }
    kimix::vector<kimix::Guid> matches;
    kimix::unordered_set<kimix::Guid> seen;
    matches.reserve(adjacency.size());
    seen.reserve(adjacency.size());
    kimix::vector<LabelId> labels; // reused across neighbours
    for (auto const &guid : adjacency) {
        if (!seen.insert(guid).second) {
            continue; // a neighbour is returned once (multi-edges collapse)
        }
        Vertex *other = _graph.find(guid);
        if (other == nullptr) {
            continue; // a missing endpoint is skipped, not an error
        }
        // §5.10: a NEIGHBOURHOOD read filters the neighbour vertex, not the edge.
        labels.clear();
        other->labels(labels);
        auto probe = [other](PropNameId name) -> PropertyValue const * {
            return other->find_property(name);
        };
        if (!kimix::graph_db::matches_probe(
                resolved, kimix::span<const LabelId>{labels.data(), labels.size()},
                probe)) {
            continue;
        }
        matches.push_back(guid);
    }
    out = make_page(std::move(matches), cap);
    return GdiStatus::ok;
}

GdiStatus Database::count_neighbors(kimix::Guid const &uid, Constraint const &constraint,
                                      EdgeOrientation orientation, size_t &out) const {
    out = 0;
    // cap == 0: the count probe — the callee's total_count is the full match
    // count, and an unfiltered probe now answers from one adjacency snapshot
    // (plan v3 §4.1) instead of walking every incident edge.
    VertexPage page;
    GdiStatus const status = get_neighbor_vertices(uid, constraint, orientation, 0, page);
    if (is_error(status)) {
        return status;
    }
    out = page.total_count;
    return GdiStatus::ok;
}

GdiStatus Database::exists_neighbor(kimix::Guid const &uid, Constraint const &constraint,
                                    EdgeOrientation orientation, bool &out) const {
    out = false;
    // cap == 1 is the short circuit: GDI's ExistsNeighbor only needs a bool, so the
    // read stops at the first match (`total_count` is therefore not meaningful
    // here and is left at the page's own count).
    VertexPage page;
    GdiStatus const status = get_neighbor_vertices(uid, constraint, orientation, 1, page);
    if (is_error(status)) {
        return status;
    }
    out = page.total_count > 0;
    return GdiStatus::ok;
}

// ---------------------------------------------------------------------------
// Native multi-hop traversal (the G3 capability)
// ---------------------------------------------------------------------------

GdiStatus Database::traverse_bfs(kimix::Guid const &start, Constraint const &constraint,
                                 EdgeOrientation orientation, int64_t cap_per_level,
                                 uint32_t max_depth,
                                 kimix::vector<Graph::TraversalVertex> &out) const {
    out.clear();
    Constraint const *resolved_ref = nullptr;
    GdiStatus const status = _resolve_constraint(constraint, resolved_ref);
    if (is_error(status)) {
        return status;
    }
    return _graph.traverse_bfs(start, *resolved_ref, orientation, cap_per_level, max_depth, out);
}

  GdiStatus Database::shortest_path(kimix::Guid const &from, kimix::Guid const &to,
                                    uint32_t max_depth, kimix::vector<kimix::Guid> &out) const {
      out.clear();
      return _graph.shortest_path(from, to, max_depth, out);
  }

  GdiStatus Database::shortest_path(kimix::Guid const &from, kimix::Guid const &to,
                                    Constraint const &edge_constraint,
                                    EdgeOrientation orientation, uint32_t max_depth,
                                    kimix::vector<Graph::PathStep> &out) const {
      out.clear();
      Constraint const *resolved_ref = nullptr;
      GdiStatus const status = _resolve_constraint(edge_constraint, resolved_ref);
      if (is_error(status)) {
          return status;
      }
      return _graph.shortest_path(from, to, *resolved_ref, orientation, max_depth, out);
  }

  GdiStatus Database::has_path(kimix::Guid const &from, kimix::Guid const &to,
                               Constraint const &edge_constraint, EdgeOrientation orientation,
                               bool &out) const {
      out = false;
      Constraint const *resolved_ref = nullptr;
      GdiStatus const status = _resolve_constraint(edge_constraint, resolved_ref);
      if (is_error(status)) {
          return status;
      }
      out = _graph.has_path(from, to, *resolved_ref, orientation);
      return GdiStatus::ok;
  }

  // The EDGE-filtered BFS facade: exactly the edge-filtered shortest_path form's shape
  // — validate/resolve the constraint reference FIRST (an ordering op on a label
  // condition is error_op here, before a single shard is touched), then forward. The
  // bound checks (max_depth range, max_vertices < 1) and the empty-start check live in
  // ONE layer, Graph::traverse_bfs_edges, because that is where the filtered walk
  // validates its own bounds too; the C ABI pre-checks them only to fix the error
  // ORDER (argument before constraint), never to change an answer.
  // ABI 8 (plan §2.2): `starts` carries one or more start vertices; `depth` in the
  // result is the distance to the NEAREST start and the cap counts the starts.
  GdiStatus Database::traverse_bfs_edges(kimix::span<const kimix::Guid> starts,
                                         Constraint const &edge_constraint,
                                         EdgeOrientation orientation, uint32_t max_depth,
                                         uint32_t max_vertices,
                                         Graph::BfsEdgesResult &out) const {
      out.vertices.clear();
      out.edges.clear();
      out.truncated = false;
      Constraint const *resolved_ref = nullptr;
      GdiStatus const status = _resolve_constraint(edge_constraint, resolved_ref);
      if (is_error(status)) {
          return status;
      }
      return _graph.traverse_bfs_edges(starts, *resolved_ref, orientation, max_depth,
                                       max_vertices, out);
  }

  // ---------------------------------------------------------------------------
  // Global scan (the G1 capability)
  GdiStatus Database::scan_vertices(Constraint const &constraint, int64_t cap,
                                    VertexPage &out) const {
      out = VertexPage{};
      if (cap < 0) {
          return GdiStatus::error_argument;
      }
      Constraint const *resolved_ref = nullptr;
      GdiStatus const status = _resolve_constraint(constraint, resolved_ref);
      if (is_error(status)) {
          return status;
      }
      Constraint const &resolved = *resolved_ref;
      bool const filtered = !resolved.conditions.empty();
      kimix::vector<kimix::Guid> found;
      kimix::vector<LabelId> labels; // filtered path only, reused across vertices
      _graph.for_each_vertex([&](Vertex const &vertex) {
          if (filtered) {
              // §5.10: the filter applies to the scanned VERTEX itself. The
              // labels()/property_list() reads take the vertex's leaf SHARED
              // lock while the shard SHARED lock is held — the documented
              // shard-before-vertex order.
              labels.clear();
vertex.labels(labels);
// §6.6 zero-copy constraint probe (see scan_shard).
auto probe = [&vertex](PropNameId name) -> PropertyValue const * {
return vertex.find_property(name);
};
if (!kimix::graph_db::matches_probe(
resolved, kimix::span<const LabelId>{labels.data(), labels.size()},
probe)) {
return true; // keep scanning
}
          }
          found.push_back(vertex.guid());
          return true;
      });
      out = make_page(std::move(found), cap);
      return GdiStatus::ok;
  }

  // ---------------------------------------------------------------------------
  // Global aggregates (the G4 capability)
  namespace src_graph_db_database {
  // One shard's share of an aggregate. Integer and double sums accumulate in
  // SEPARATE buckets so an int64_ declaration sums exactly (no widening), and
  // the merge adds the buckets in shard order — deterministic floats.
  struct AggregatePartial {
      uint64_t matched{0};
      uint64_t valued{0};
      int64_t int_sum{0};
      double double_sum{0.0};
      bool has_extreme{false};
      PropertyValue extreme;
      GdiStatus status{GdiStatus::ok};
  };
  // Fold one contributing value into a partial. `numeric` is the sum/avg path,
  // `want_min`/`want_max` the extremal path; a kind the reduction cannot order
  // (defensive: the declaration check already guaranteed homogeneous kinds)
  // records error_datatype in the partial instead of throwing.
  void aggregate_accumulate(AggregatePartial &partial, GdiAggregate function,
                            PropertyValue const &value) noexcept {
      switch (function) {
      case GdiAggregate::min:
      case GdiAggregate::max: {
          if (!partial.has_extreme) {
              partial.extreme = value;
              partial.has_extreme = true;
              return;
          }
          kimix::optional<int32_t> const order = compare_values(value, partial.extreme);
          if (!order.has_value()) {
              partial.status = GdiStatus::error_datatype;
              return;
          }
          bool const take = function == GdiAggregate::min ? *order < 0 : *order > 0;
          if (take) {
              partial.extreme = value; // strict: a tie keeps the earlier shard's
          }                            // value, which is the deterministic pick
          return;
      }
      case GdiAggregate::sum:
      case GdiAggregate::avg:
          if (value.kind() == PropertyKind::int64_value) {
              partial.int_sum += value.as_int64();
          } else {
              partial.double_sum += value.as_double();
          }
          return;
      case GdiAggregate::count:
          return; // count never reaches here; the payload is `matched`
      }
      partial.status = GdiStatus::error_argument;
  }
  } // namespace src_graph_db_database
  using namespace src_graph_db_database;

  GdiStatus Database::aggregate_vertices(AggregateSpec const &spec, AggregateResult &out) const {
      out = AggregateResult{};
      switch (spec.function) {
      case GdiAggregate::count:
      case GdiAggregate::min:
      case GdiAggregate::max:
      case GdiAggregate::sum:
      case GdiAggregate::avg:
          break;
      default:
          return GdiStatus::error_argument;
      }
      Constraint const *resolved_ref = nullptr;
      GdiStatus const status = _resolve_constraint(spec.constraint, resolved_ref);
      if (is_error(status)) {
          return status;
      }
      Constraint const &resolved = *resolved_ref;
      // The legality matrix, decided from the REGISTERED declaration before any
      // scan: count reduces rows (no property needed); min/max order values
      // (numeric cross-kind via compare_values, strings ordinal via char_); sum/
      // avg need a numeric scalar declaration.
      GdiDataType declared = GdiDataType::int64_;
      if (spec.function != GdiAggregate::count) {
          if (spec.property == kNoPropName) {
              return GdiStatus::error_argument;
          }
          if (!_schema.has_property_type(spec.property)) {
              return GdiStatus::error_property_type;
          }
          PropertyTypeInfo const declaration = _schema.property_type(spec.property);
          declared = declaration.data;
          bool const scalar = declaration.container == GdiContainerType::single;
          bool const ordered =
              scalar && (declared == GdiDataType::int64_ || declared == GdiDataType::double_ ||
                         declared == GdiDataType::char_);
          bool const numeric = scalar && (declared == GdiDataType::int64_ ||
                                          declared == GdiDataType::double_);
          bool const legal = spec.function == GdiAggregate::min ||
                             spec.function == GdiAggregate::max ? ordered : numeric;
          if (!legal) {
              return GdiStatus::error_op;
          }
      }
      bool const filtered = !resolved.conditions.empty();
      bool const reduces_property = spec.function != GdiAggregate::count;
      // One partial per shard; every shard index gets exactly one write (its own
      // scan), disjoint across fibers, so no synchronization is needed.
      kimix::vector<AggregatePartial> partials(kGraphShardCount);
      auto scan_shard = [&](uint32_t shard, AggregatePartial &partial) noexcept {
          kimix::vector<LabelId> labels; // filtered path only, reused per vertex
          _graph.for_each_vertex(shard, shard + 1, [&](Vertex const &vertex) {
              if (filtered) {
labels.clear();
vertex.labels(labels);
// §6.6 zero-copy: probe the borrowed values through one leaf
// shared lock instead of deep-copying every property per vertex.
auto probe = [&vertex](PropNameId name) -> PropertyValue const * {
return vertex.find_property(name);
};
if (!kimix::graph_db::matches_probe(
resolved, kimix::span<const LabelId>{labels.data(), labels.size()},
probe)) {
return true;
}
}
++partial.matched;
if (!reduces_property) {
return true;
}
// §6.6 zero-copy reduce: one visitor pass, no property copies.
vertex.visit_properties([&](PropNameId name, PropertyValue const &value) {
if (name != spec.property) {
return true;
}
++partial.valued;
aggregate_accumulate(partial, spec.function, value);
return false;
});
return true;
          });
      };
      // Fan-out: one fiber per shard when a scheduler with workers is bound;
      // the same shards inline when unbound (the C ABI path) — both produce the
      // same partials, merged in shard order below.
      uint32_t const workers = kimix::fiber::worker_thread_count();
      if (workers <= 1) {
          for (uint32_t shard = 0; shard < static_cast<uint32_t>(kGraphShardCount); ++shard) {
              scan_shard(shard, partials[shard]);
          }
      } else {
          kimix::fiber::parallel(static_cast<uint32_t>(kGraphShardCount),
                                 [&](uint32_t shard) noexcept {
                                     scan_shard(shard, partials[shard]);
                                 },
                                 /*internal_jobs=*/1u);
      }
      // Merge in shard order: the sums add partial-by-partial (deterministic
      // floats), the extremal fold keeps the earlier shard on a tie, and the
      // first partial-level error wins.
      int64_t int_total = 0;
      double double_total = 0.0;
      PropertyValue extreme;
      bool has_extreme = false;
      for (auto const &partial : partials) {
          if (is_error(partial.status)) {
              return partial.status;
          }
          out.matched += partial.matched;
          out.valued += partial.valued;
          int_total += partial.int_sum;
          double_total += partial.double_sum;
          if (partial.has_extreme) {
              if (!has_extreme) {
                  extreme = partial.extreme;
                  has_extreme = true;
              } else {
                  kimix::optional<int32_t> const order =
                      compare_values(partial.extreme, extreme);
                  if (!order.has_value()) {
                      return GdiStatus::error_datatype;
                  }
                  bool const take = spec.function == GdiAggregate::min ? *order < 0 : *order > 0;
                  if (take) {
                      extreme = partial.extreme;
                  }
              }
          }
      }
      switch (spec.function) {
      case GdiAggregate::count:
          if (out.matched > 0) {
              out.has_value = true;
              out.value = PropertyValue::make_int64(static_cast<int64_t>(out.matched));
          }
          return GdiStatus::ok;
      case GdiAggregate::min:
      case GdiAggregate::max:
          if (has_extreme) {
              out.has_value = true;
              out.value = std::move(extreme);
          }
          return GdiStatus::ok;
      case GdiAggregate::sum:
          if (out.valued > 0) {
              out.has_value = true;
              out.value = declared == GdiDataType::double_
                              ? PropertyValue::make_double(double_total)
                              : PropertyValue::make_int64(int_total);
          }
          return GdiStatus::ok;
      case GdiAggregate::avg:
          if (out.valued > 0) {
              out.has_value = true;
              double const sum = static_cast<double>(int_total) + double_total;
              out.value = PropertyValue::make_double(sum / static_cast<double>(out.valued));
          }
          return GdiStatus::ok;
      }
      return GdiStatus::error_argument;
  }

// ---------------------------------------------------------------------------
// Constraints
// ---------------------------------------------------------------------------

GdiStatus Database::create_constraint(Constraint const &definition, ConstraintHandle &out) {
    return _schema.create_constraint(definition, out);
}

GdiStatus Database::add_label_condition(ConstraintHandle handle, LabelId label, GdiOp op) {
    if (!_schema.has_label(label)) {
        return GdiStatus::error_label;
    }
    return _schema.add_label_condition(handle, label, op);
}

GdiStatus Database::add_property_condition(ConstraintHandle handle, PropNameId property,
                                           GdiOp op, PropertyValue value, uint32_t count) {
    if (!_schema.has_property_type(property)) {
        return GdiStatus::error_property_type;
    }
    return _schema.add_property_condition(handle, property, op, std::move(value), count);
}

GdiStatus Database::free_constraint(ConstraintHandle handle) {
    return _schema.free_constraint(handle);
}

// ---------------------------------------------------------------------------
// Schema commands
// ---------------------------------------------------------------------------

GdiStatus Database::create_label(kimix::string_view name, LabelId &out) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    LabelId id = kNoLabel;
    GdiStatus const status = _schema.create_label(name, id);
    if (is_error(status)) {
        return status;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::label_created;
        entry.label = id;
        if (!_tx->push(std::move(entry))) {
            (void)_schema.free_label(id);
            return GdiStatus::error_size_limit;
        }
    }
    out = id;
    return GdiStatus::ok;
}

GdiStatus Database::get_label_by_name(kimix::string_view name, LabelId &out,
                                      bool &found) const {
    found = _schema.find_label(name, out);
    return GdiStatus::ok;
}

GdiStatus Database::get_all_labels(NamedLabelList &out) const {
    out.clear();
    for (auto id : _schema.all_labels()) {
        NamedLabel item;
        item.id = id;
        item.name = _schema.label_name(id);
        out.push_back(std::move(item));
    }
    return GdiStatus::ok;
}

GdiStatus Database::update_label(LabelId id, kimix::string_view new_name) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (_tx != nullptr) {
        kimix::string const previous = _schema.label_name(id);
        if (previous.empty()) {
            return GdiStatus::error_label;
        }
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::label_renamed;
        entry.fat.reset(new JournalEntry::Fat);
        entry.label = id;
        entry.fat->name = previous;
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return _schema.rename_label(id, new_name);
}

GdiStatus Database::free_label(LabelId id) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (!_schema.has_label(id)) {
        return GdiStatus::error_label;
    }
    if (_label_is_a_key(id)) {
        // Freeing the key label of a live vertex would orphan its key index
        // entry, so it is refused rather than silently breaking the primary key.
        return GdiStatus::error_state;
    }
    if (_tx != nullptr) {
        // The undo entry is the REGISTRATION (so the name comes back with the same
        // id) plus one entry per entity the cascade touches, pushed by _strip_label.
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::label_freed;
        entry.fat.reset(new JournalEntry::Fat);
        entry.label = id;
        entry.fat->name = _schema.label_name(id);
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    GdiStatus const status = _schema.free_label(id);
    if (is_error(status)) {
        return status;
    }
    _strip_label(id); // GDI FreeLabel cascades to every entity
    return GdiStatus::ok;
}

GdiStatus Database::create_property_type(PropertyTypeInfo const &info, PropNameId &out) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    PropNameId id = kNoPropName;
    GdiStatus const status = _schema.create_property_type(info, id);
    if (is_error(status)) {
        return status;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::property_type_created;
        entry.property = id;
        if (!_tx->push(std::move(entry))) {
            (void)_schema.free_property_type(id);
            return GdiStatus::error_size_limit;
        }
    }
    out = id;
    return GdiStatus::ok;
}

GdiStatus Database::get_property_type_by_name(kimix::string_view name, PropNameId &out,
                                             bool &found) const {
    found = _schema.find_property_type(name, out);
    return GdiStatus::ok;
}

GdiStatus Database::get_all_property_types(PropertyTypeList &out) const {
    out = _schema.all_property_types();
    return GdiStatus::ok;
}

GdiStatus Database::update_property_type(PropNameId id, PropertyTypeInfo const &info) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    // Name validation comes FIRST, exactly like the reference backend's
    // RequireName-then-lookup order.
    if (is_blank_name(info.name)) {
        return GdiStatus::error_empty_name;
    }
    if (!_schema.has_property_type(id)) {
        return GdiStatus::error_property_type;
    }
    // The declaration rules live in ONE place (schema.h): an out-of-range size
    // type or a non-fixed `single` is error_argument, a fixed/max count < 1 is
    // error_count, and `unlimited` ignores the count (normalized to 0 on store).
    GdiStatus const declaration_status = validate_property_type_declaration(info);
    if (is_error(declaration_status)) {
        return declaration_status;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::property_type_updated;
        entry.fat.reset(new JournalEntry::Fat);
        entry.property = id;
        entry.fat->info = _schema.property_type(id);
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    // A live value pins the declaration's shape (see Schema::update_property_type).
    bool const values_exist = _property_values_exist(id);
    return _schema.update_property_type(id, info, values_exist);
}

GdiStatus Database::free_property_type(PropNameId id) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (!_schema.has_property_type(id)) {
        return GdiStatus::error_property_type;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::property_type_freed;
        entry.fat.reset(new JournalEntry::Fat);
        entry.property = id;
        entry.fat->info = _schema.property_type(id);
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    GdiStatus const status = _schema.free_property_type(id);
    if (is_error(status)) {
        return status;
    }
    _strip_property_type(id); // GDI FreePropertyType removes every value of it
    return GdiStatus::ok;
}

// ---------------------------------------------------------------------------
// Transactions
// ---------------------------------------------------------------------------

bool Database::transaction_is_read_only() const noexcept {
    return _tx != nullptr && _tx->read_only();
}

GdiStatus Database::begin_transaction(bool read_only) {
    if (_tx != nullptr) {
        // One write transaction at a time: the container's single-writer contract
        // surfaced as a status (GDI ErrorTransaction).
        return GdiStatus::error_transaction;
    }
    _tx = new Transaction(*this, read_only, _options.max_journal_bytes);
    return GdiStatus::ok;
}

GdiStatus Database::commit() {
    if (_tx == nullptr) {
        return GdiStatus::error_transaction;
    }
    Transaction *tx = _tx;
    _tx = nullptr;
    tx->_closed = true;
    tx->clear_journal();
    // A write transaction dirties what it touched, so the incremental save below
    // (when requested) writes exactly that. Everything a command marked dirty
    // already is left as it is.
    if (_options.persist_on_commit && !tx->read_only() && is_open()) {
        delete tx;
        return persist(/*full=*/false);
    }
    delete tx;
    return GdiStatus::ok;
}

GdiStatus Database::abort() {
    if (_tx == nullptr) {
        return GdiStatus::error_transaction;
    }
    Transaction *tx = _tx;
    _tx = nullptr;
    tx->_closed = true;
    GdiStatus const status = _rollback(*tx);
    delete tx;
    return status;
}

GdiStatus Database::begin_collective() {
    GdiStatus const status = begin_transaction(/*read_only=*/true);
    if (is_error(status)) {
        return status;
    }
    _tx->_collective = true;
    return GdiStatus::ok;
}

GdiStatus Database::close_collective(bool commit_it) {
    if (_tx == nullptr || !_tx->collective()) {
        // A collective scope is begun through begin_collective(); closing a
        // writable transaction this way is a caller error.
        return GdiStatus::error_transaction;
    }
    return commit_it ? commit() : abort();
}

// ---------------------------------------------------------------------------
// Rollback
// ---------------------------------------------------------------------------

GdiStatus Database::_rollback(Transaction &tx) {
    kimix::vector<JournalEntry> journal = tx.take_journal();
    GdiStatus status = GdiStatus::ok;
    // REVERSE order: the last mutation is undone first, so an entity is always
    // restored before the mutation that depended on it is undone.
    for (size_t i = journal.size(); i > 0; --i) {
        JournalEntry &entry = journal[i - 1];
        switch (entry.kind) {
        case JournalEntry::Kind::vertex_created: {
            kimix::unique_ptr<Vertex> removed = _graph.remove_vertex(entry.uid);
            (void)removed;
            break;
        }
        case JournalEntry::Kind::vertex_removed: {
            // Re-adopt the vertex with its payload, then every edge its cascade
            // removed. The key index entry comes back with adopt_vertex.
            if (entry.fat->vertex) {
                entry.fat->vertex->restore_payload(
                    kimix::span<const LabelId>{entry.fat->labels.data(), entry.fat->labels.size()},
                    entry.fat->properties);
                (void)_graph.adopt_vertex(std::move(entry.fat->vertex));
            }
            for (auto &edge : entry.fat->removed_edges) {
                if (edge) {
                    (void)_graph.adopt_edge(std::move(edge));
                }
            }
            break;
        }
        case JournalEntry::Kind::vertex_payload: {
            Vertex *vertex = _graph.find(entry.uid);
            if (vertex != nullptr) {
                vertex->restore_payload(
                    kimix::span<const LabelId>{entry.fat->labels.data(), entry.fat->labels.size()},
                    entry.fat->properties);
            }
            break;
        }
        case JournalEntry::Kind::edge_created:
            (void)_graph.edge_free(entry.uid);
            break;
        case JournalEntry::Kind::edge_removed: {
            kimix::unique_ptr<Edge> restored{new Edge(entry.uid, entry.fat->origin, entry.fat->target,
                                                     entry.fat->direction)};
            restored->assign_payload(kimix::span<const LabelId>{entry.fat->labels.data(),
                                                               entry.fat->labels.size()},
                                     entry.fat->properties);
            restored->clear_dirty();
            (void)_graph.adopt_edge(std::move(restored));
            break;
        }
        case JournalEntry::Kind::edge_state: {
            // Endpoints first (they move the adjacency), then the payload.
            kimix::Guid origin;
            kimix::Guid target;
            if (!_graph.edge_endpoints(entry.uid, origin, target)) {
                break;
            }
            if (origin != entry.fat->origin) {
                (void)_graph.edge_set_origin(entry.uid, entry.fat->origin);
            }
            if (target != entry.fat->target) {
                (void)_graph.edge_set_target(entry.uid, entry.fat->target);
            }
            (void)_graph.edge_set_direction(entry.uid, entry.fat->direction);
            _graph.edge_restore_payload(
                entry.uid,
                kimix::span<const LabelId>{entry.fat->labels.data(), entry.fat->labels.size()},
                entry.fat->properties);
            break;
        }
        case JournalEntry::Kind::label_created:
            (void)_schema.free_label(entry.label);
            break;
        case JournalEntry::Kind::label_renamed:
            (void)_schema.rename_label(entry.label, entry.fat->name);
            break;
        case JournalEntry::Kind::label_freed: {
            LabelTableEntry restored;
            restored.id = entry.label;
            restored.name = entry.fat->name;
            (void)_schema.restore_label(restored);
            break;
        }
        case JournalEntry::Kind::property_type_created:
            (void)_schema.free_property_type(entry.property);
            break;
        case JournalEntry::Kind::property_type_updated: {
            PropertyTypeInfo const current = _schema.property_type(entry.property);
            (void)current;
            (void)_schema.update_property_type(entry.property, entry.fat->info, false);
            break;
        }
        case JournalEntry::Kind::property_type_freed:
            (void)_schema.restore_property_type(entry.fat->info);
            break;
        }
    }
    return status;
}

} // namespace kimix::graph_db
