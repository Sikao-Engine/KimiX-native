/*
 * status.cpp — the names and messages of GdiStatus (see status.h for the
 * contract; the numeric values themselves are pinned in the header and asserted
 * by tests/unit/graph_db/test_graph_db_status.cpp).
 *
 * Both tables are exhaustive switch-free arrays indexed by the status value:
 * that is one load instead of a branch chain, and adding a code is a matter of
 * adding one entry in each table (never forget one: the array size is
 * kLastStatusValue + 1 and the compiler rejects a short initializer).
 */
#include "status.h"

namespace kimix::graph_db {

namespace src_graph_db_status {

constexpr int32_t kTableSize = kLastStatusValue + 1;

// Identifier per status value, indexed by (int32_t)status.
constexpr char const *kStatusNames[kTableSize] = {
    "ok",                              // 0
    "warning_non_unique_id",           // 1
    "warning_other",                   // 2
    "error_access",                    // 3
    "error_argument",                  // 4
    "error_bad_file",                  // 5
    "error_buffer",                    // 6
    "error_constraint",                // 7
    "error_conversion",                // 8
    "error_count",                     // 9
    "error_database",                  // 10
    "error_datatype",                  // 11
    "error_date",                      // 12
    "error_datetime",                  // 13
    "error_decimal",                   // 14
    "error_edge",                      // 15
    "error_edge_orientation",          // 16
    "error_empty_name",                // 17
    "error_error_code",                // 18
    "error_file_exists",               // 19
    "error_file_in_use",               // 20
    "error_incompatible_transactions", // 21
    "error_index",                     // 22
    "error_label",                     // 23
    "error_name_exists",               // 24
    "error_no_memory",                 // 25
    "error_no_property",               // 26
    "error_no_space",                  // 27
    "error_no_such_file",              // 28
    "error_non_unique_id",             // 29
    "error_not_same",                  // 30
    "error_object_mismatch",           // 31
    "error_op",                        // 32
    "error_op_datatype_mismatch",      // 33
    "error_property_exists",           // 34
    "error_property_type",             // 35
    "error_property_type_exists",      // 36
    "error_range",                     // 37
    "error_read_only_file",            // 38
    "error_read_only_property_type",   // 39
    "error_read_only_transaction",     // 40
    "error_resource",                  // 41
    "error_size",                      // 42
    "error_size_limit",                // 43
    "error_stale",                     // 44
    "error_state",                     // 45
    "error_subconstraint",             // 46
    "error_time",                      // 47
    "error_transaction",               // 48
    "error_uid",                       // 49
    "error_vertex",                    // 50
    "error_wrong_type",                // 51
    "error_invalid_date",              // 52
    "error_communicator",              // 53
    "error_block_size",                // 54
    "error_quota",                     // 55
    "error_truncate",                  // 56
    "error_transaction_critical",      // 57
    "error_transaction_commit_fail",   // 58
    "error_intern",                    // 59
    "error_io",                        // 60
    "error_other",                     // 61
    "error_unknown",                   // 62
};

// Message per status value — the exact strings of GdiErrorTable.GetMessage(),
// so a managed adapter can display them without its own table.
constexpr char const *kStatusMessages[kTableSize] = {
    "Success.",                                                                    // 0
    "The external ID is not unique: another vertex with the same (label, external_id) key exists.", // 1
    "Unspecified warning.",                                                        // 2
    "Access denied.",                                                              // 3
    "Invalid argument.",                                                           // 4
    "Bad file.",                                                                   // 5
    "Buffer too small.",                                                           // 6
    "Invalid constraint.",                                                         // 7
    "Conversion failed.",                                                          // 8
    "Invalid count.",                                                              // 9
    "Database error.",                                                             // 10
    "Invalid datatype.",                                                           // 11
    "Invalid date.",                                                               // 12
    "Invalid datetime.",                                                           // 13
    "Invalid decimal.",                                                            // 14
    "Invalid edge.",                                                               // 15
    "Invalid edge orientation.",                                                   // 16
    "Name is empty.",                                                              // 17
    "Invalid error code.",                                                         // 18
    "File already exists.",                                                        // 19
    "File is in use.",                                                             // 20
    "Incompatible transactions.",                                                  // 21
    "Index error.",                                                                // 22
    "Invalid label.",                                                              // 23
    "Name already exists.",                                                        // 24
    "Out of memory.",                                                              // 25
    "No such property.",                                                           // 26
    "No space left.",                                                              // 27
    "No such file.",                                                               // 28
    "The external ID is not unique.",                                              // 29
    "Objects are not the same.",                                                   // 30
    "Object mismatch.",                                                            // 31
    "Invalid operation.",                                                          // 32
    "Operation/datatype mismatch.",                                                // 33
    "Property already exists.",                                                    // 34
    "Invalid property type.",                                                      // 35
    "Property type already exists.",                                               // 36
    "Out of range.",                                                               // 37
    "File is read-only.",                                                          // 38
    "Property type is read-only.",                                                 // 39
    "Transaction is read-only.",                                                   // 40
    "Resource error.",                                                             // 41
    "Invalid size.",                                                               // 42
    "Size limit exceeded.",                                                        // 43
    "Stale object.",                                                               // 44
    "Invalid state.",                                                              // 45
    "Invalid subconstraint.",                                                      // 46
    "Invalid time.",                                                               // 47
    "Transaction error.",                                                          // 48
    "Invalid UID.",                                                                // 49
    "Invalid vertex.",                                                             // 50
    "Wrong type.",                                                                 // 51
    "Invalid date.",                                                               // 52
    "Invalid communicator.",                                                       // 53
    "Invalid block size.",                                                         // 54
    "Quota exceeded.",                                                             // 55
    "Truncation occurred.",                                                        // 56
    "Transaction is in a critical state.",                                         // 57
    "Transaction commit failed.",                                                  // 58
    "Internal error.",                                                             // 59
    "I/O error.",                                                                  // 60
    "Unspecified error.",                                                          // 61
    "Unknown error.",                                                              // 62
};

} // namespace src_graph_db_status
using namespace src_graph_db_status;

char const *status_name(GdiStatus status) noexcept {
    int32_t const value = static_cast<int32_t>(status);
    if (value < 0 || value >= kTableSize) {
        return "status_unknown";
    }
    return kStatusNames[value];
}

kimix::string status_message(GdiStatus status) {
    int32_t const value = static_cast<int32_t>(status);
    if (value < 0 || value >= kTableSize) {
        return kimix::string{"Unknown GDI status code."};
    }
    return kimix::string{kStatusMessages[value]};
}

} // namespace kimix::graph_db
