#include <core/first_fit.h>
#include <core/pool.h>
#include <core/stl/format.h>
#include <core/stl/string.h>

namespace kimix {

// ---------------------------------------------------------------------------
// Pool-based free list node management (detail helpers)
// ---------------------------------------------------------------------------

namespace detail {

// Thread-local pool of FirstFit::Node for fast allocation
static Pool<FirstFit::Node> &first_fit_node_pool() noexcept {
    static Pool<FirstFit::Node> pool;
    return pool;
}

FirstFit::Node *first_fit_allocate_node(size_t offset, size_t size) noexcept {
    auto node = first_fit_node_pool().create();
    node->offset = offset;
    node->size = size;
    node->next = nullptr;
    node->prev = nullptr;
    return node;
}

void first_fit_free_node(FirstFit::Node *node) noexcept {
    first_fit_node_pool().destroy(node);
}

} // namespace detail

// ---------------------------------------------------------------------------
// String-based free list dump (utility function)
// ---------------------------------------------------------------------------

kimix::string dump_free_list(const FirstFit &fit) noexcept {
    kimix::string result;
    result.reserve(256);
    size_t total_free = 0;
    result.append("Free list:\n");
    for (auto &node : const_cast<FirstFit &>(fit)) {
        result.append(kimix::format("  offset={} size={}\n", node.offset, node.size));
        total_free += node.size;
    }
    result.append(kimix::format("  Total free: {}\n", total_free));
    return result;
}

} // namespace kimix
