/*
 * first_fit.h — kimix::FirstFit first-fit/best-fit memory allocator.
 *
 * Manages a contiguous memory region using an intrusive free list.
 *
 *   initialize(size)      — allocate and set up the pool.
 *   allocate(size)        — first-fit allocation, returns Node* or nullptr.
 *   allocate_best_fit(size) — best-fit allocation.
 *   free(node)            — return a node to the free list with coalescing.
 *   dump_free_list()      — debug-print the free list.
 *   buffer()              — raw pointer to the managed buffer.
 *   total_size()          — total managed size in bytes.
 *   begin()/end()         — iterate over free list nodes.
 *
 * Example:
 *   kimix::FirstFit ff(1024);
 *   auto* node = ff.allocate(128);
 *   ff.free(node);
 */
#pragma once

#include "stl/vector.h"
#include "stl/string.h"
#include "spin_mutex.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <utility>

namespace kimix {

// ---------------------------------------------------------------------------
// FirstFit — first-fit / best-fit memory allocator
// Manages a contiguous memory region with a free list.
// ---------------------------------------------------------------------------

class FirstFit {
public:
    struct Node {
        size_t offset = 0;
        size_t size = 0;
        Node* next = nullptr;
        Node* prev = nullptr;
    };

    class Iterator {
    public:
        explicit Iterator(Node* node) noexcept : _node(node) {}

        Node& operator*() const noexcept { return *_node; }
        Node* operator->() const noexcept { return _node; }
        Iterator& operator++() noexcept { _node = _node->next; return *this; }
        Iterator operator++(int) noexcept { Iterator tmp = *this; _node = _node->next; return tmp; }

        bool operator==(const Iterator& other) const noexcept { return _node == other._node; }
        bool operator!=(const Iterator& other) const noexcept { return _node != other._node; }

    private:
        Node* _node;
    };

    FirstFit() = default;

    explicit FirstFit(size_t total_size) {
        initialize(total_size);
    }

    ~FirstFit() {
        if (_buffer) {
            ::operator delete(_buffer, std::align_val_t{16});
        }
    }

    FirstFit(const FirstFit&) = delete;
    FirstFit& operator=(const FirstFit&) = delete;
    FirstFit(FirstFit&& other) noexcept
        : _buffer(std::exchange(other._buffer, nullptr))
        , _total_size(std::exchange(other._total_size, 0))
        , _free_head(std::exchange(other._free_head, nullptr)) {}

    FirstFit& operator=(FirstFit&& other) noexcept {
        if (this != &other) {
            if (_buffer) { ::operator delete(_buffer, std::align_val_t{16}); }
            _buffer = std::exchange(other._buffer, nullptr);
            _total_size = std::exchange(other._total_size, 0);
            _free_head = std::exchange(other._free_head, nullptr);
        }
        return *this;
    }

    void initialize(size_t total_size) {
        _total_size = total_size;
        _buffer = ::operator new(total_size, std::align_val_t{16});
        _free_head = reinterpret_cast<Node*>(_buffer);
        _free_head->offset = sizeof(Node);
        _free_head->size = total_size - sizeof(Node);
        _free_head->next = nullptr;
        _free_head->prev = nullptr;
    }

    // First-fit allocation
    Node* allocate(size_t size) {
        size = (size + 15) & ~size_t{15}; // align to 16
        Node* node = _free_head;
        while (node) {
            if (node->size >= size) {
                return split_node(node, size);
            }
            node = node->next;
        }
        return nullptr;
    }

    // Best-fit allocation
    Node* allocate_best_fit(size_t size) {
        size = (size + 15) & ~size_t{15};
        Node* best = nullptr;
        size_t best_diff = SIZE_MAX;

        Node* node = _free_head;
        while (node) {
            if (node->size >= size && (node->size - size) < best_diff) {
                best = node;
                best_diff = node->size - size;
            }
            node = node->next;
        }

        if (best) {
            return split_node(best, size);
        }
        return nullptr;
    }

    // Return a node to the free list
    void free(Node* node) {
        if (!node) return;

        // Insert sorted by offset
        Node* prev = nullptr;
        Node* curr = _free_head;

        while (curr && curr->offset < node->offset) {
            prev = curr;
            curr = curr->next;
        }

        node->prev = prev;
        node->next = curr;

        if (prev) { prev->next = node; }
        else { _free_head = node; }
        if (curr) { curr->prev = node; }

        // Coalesce with previous
        if (prev) {
            coalesce(prev, node);
            node = prev;
        }
        // Coalesce with next
        if (curr) {
            coalesce(node, curr);
        }
    }

    // Dump the free list (for debugging)
    void dump_free_list() const {
        Node* node = _free_head;
        size_t total_free = 0;
        fprintf(stderr, "Free list:\n");
        while (node) {
            fprintf(stderr, "  offset=%zu size=%zu\n", node->offset, node->size);
            total_free += node->size;
            node = node->next;
        }
        fprintf(stderr, "  Total free: %zu\n", total_free);
    }

    void* buffer() noexcept { return _buffer; }
    size_t total_size() const noexcept { return _total_size; }

    Iterator begin() noexcept { return Iterator{_free_head}; }
    Iterator end() noexcept { return Iterator{nullptr}; }

private:
    Node* split_node(Node* node, size_t size) {
        size_t remaining = node->size - size;

        // Remove node from free list
        if (node->prev) { node->prev->next = node->next; }
        else { _free_head = node->next; }
        if (node->next) { node->next->prev = node->prev; }

        if (remaining >= sizeof(Node) + 16) {
            // Create a new free node from the remainder
            Node* new_node = reinterpret_cast<Node*>(
                reinterpret_cast<uint8_t*>(static_cast<void*>(node)) + node->offset + size
            );
            new_node->offset = 0;
            new_node->size = remaining - sizeof(Node);
            new_node->next = nullptr;
            new_node->prev = nullptr;

            // Return it to free list
            Node* prev = nullptr;
            Node* curr = _free_head;
            while (curr && curr->offset < new_node->offset) {
                prev = curr;
                curr = curr->next;
            }
            new_node->prev = prev;
            new_node->next = curr;
            if (prev) { prev->next = new_node; }
            else { _free_head = new_node; }
            if (curr) { curr->prev = new_node; }

            node->size = size;
        }

        return node;
    }

    void coalesce(Node* left, Node* right) {
        if (left->offset + left->size == right->offset) {
            left->size += sizeof(Node) + right->size;
            // Remove right from list
            if (right->prev) { right->prev->next = right->next; }
            else { _free_head = right->next; }
            if (right->next) { right->next->prev = right->prev; }
        }
    }

    void* _buffer = nullptr;
    size_t _total_size = 0;
    Node* _free_head = nullptr;
};

} // namespace kimix
