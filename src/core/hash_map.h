/*
 * hash_map.h — kimix::HashMap<K, V, Hash, Compare>: open-addressed buckets of
 * red-black trees with a stable insertion-order node array.
 *
 * The table maps a 64-bit hash to a per-bucket kimix::SmallTreeMap (ordered by
 * kimix::compare<K>), while a flat array of node pointers preserves insertion
 * order for O(1) index-addressed iteration. Nodes live in a kimix::Pool.
 * Capacity grows with a 4/3 load rule (rehash at ~75% load); iteration order
 * stays stable across growth because nodes are never moved out of the array.
 *
 * kimix::HashMap<int, kimix::string> map;
 * auto idx = map.try_emplace(1, "hello").first; // insert or find
 * if (idx) idx.value() = "world";               // index lookup (key() / value())
 * for (auto [k, v] : map) { ... } // const iteration (insertion order)
 * map.remove(1); // erase by key
 *
 * Note on move semantics: kimix::Pool is not movable, so HashMap's move
 * constructor deep-moves the nodes (every node is recreated in the
 * destination's own pool, preserving array index and bucket layout) instead of
 * stealing the source's pool storage. Move assignment keeps the original
 * destroy-then-placement-new shape. This is O(n) rather than O(1), but correct
 * for any pool growth strategy.
 */
#pragma once

#include <utility> // std::to_underlying (used by compare.h) is declared in <utility>

#include "compare.h"
#include "pool.h"
#include "stl/hash.h" // kimix::hash<K> specializations for the default Hash

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace kimix {

namespace detail {

// Red-black tree fix-up helpers over Node* links (parent/left/right/color).
// Same algorithm as the vstl TreeMapUtility; only the member names differ.
template <typename Node>
class TreeMapUtility {
private:
    static void _left_rotate(Node *x, Node *&root) {
        Node *y = x->right;
        x->right = y->left;
        if (y->left != nullptr) {
            y->left->parent = x;
        }
        y->parent = x->parent;
        if (x->parent == nullptr) {
            root = y;
        } else if (x == x->parent->left) {
            x->parent->left = y;
        } else {
            x->parent->right = y;
        }
        y->left = x;
        x->parent = y;
    }
    static void _right_rotate(Node *x, Node *&root) {
        Node *y = x->left;
        x->left = y->right;
        if (y->right != nullptr) {
            y->right->parent = x;
        }
        y->parent = x->parent;
        if (x->parent == nullptr) {
            root = y;
        } else if (x == x->parent->right) {
            x->parent->right = y;
        } else {
            x->parent->left = y;
        }
        y->right = x;
        x->parent = y;
    }
    static void _fix_delete(Node *x, Node *&root, Node *&t_null_parent) {
        Node *s;
        bool x_is_null;
        while (x != root && ((x_is_null = (x == nullptr)) || x->color == 0)) {
            auto &&x_parent = x_is_null ? t_null_parent : x->parent;
            if (x == x_parent->left) {
                s = x_parent->right;
                if (s->color == 1) {
                    // case 3.1
                    s->color = 0;
                    x_parent->color = 1;
                    _left_rotate(x_parent, root);
                    s = x_parent->right;
                }
                bool left_null = s->left == nullptr || s->left->color == 0;
                bool right_null = s->right == nullptr || s->right->color == 0;
                if (left_null && right_null) {
                    // case 3.2
                    s->color = 1;
                    x = x_parent;
                } else {
                    if (right_null) {
                        // case 3.3
                        s->left->color = 0;
                        s->color = 1;
                        _right_rotate(s, root);
                        s = x_parent->right;
                    }
                    // case 3.4
                    s->color = x_parent->color;
                    x_parent->color = 0;
                    s->right->color = 0;
                    _left_rotate(x_parent, root);
                    x = root;
                    break;
                }
            } else {
                s = x_parent->left;
                if (s->color == 1) {
                    // case 3.1
                    s->color = 0;
                    x_parent->color = 1;
                    _right_rotate(x_parent, root);
                    s = x_parent->left;
                }
                bool left_null = s->left == nullptr || s->left->color == 0;
                bool right_null = s->right == nullptr || s->right->color == 0;
                if (left_null && right_null) {
                    // case 3.2
                    s->color = 1;
                    x = x_parent;
                } else {
                    if (left_null) {
                        // case 3.3
                        s->right->color = 0;
                        s->color = 1;
                        _left_rotate(s, root);
                        s = x_parent->left;
                    }
                    // case 3.4
                    s->color = x_parent->color;
                    x_parent->color = 0;
                    s->left->color = 0;
                    _right_rotate(x_parent, root);
                    x = root;
                    break;
                }
            }
        }
        if (x != nullptr)
            x->color = 0;
    }
    // find the node with the minimum key
    static Node *_minimum(Node *node) {
        while (node->left != nullptr) {
            node = node->left;
        }
        return node;
    }
    static void _rb_transplant(Node *u, Node *v, Node *&root, Node *&t_null_parent) {
        if (u->parent == nullptr) {
            root = v;
        } else if (u == u->parent->left) {
            u->parent->left = v;
        } else {
            u->parent->right = v;
        }
        if (v == nullptr)
            t_null_parent = u->parent;
        else
            v->parent = u->parent;
    }

public:
    static void fix_insert(Node *k, Node *&root) {
        Node *u;
        while (k->parent->color == 1) {
            if (k->parent == k->parent->parent->right) {
                u = k->parent->parent->left;// uncle
                if (u != nullptr && u->color == 1) {
                    // case 3.1
                    u->color = 0;
                    k->parent->color = 0;
                    k->parent->parent->color = 1;
                    k = k->parent->parent;
                } else {
                    if (k == k->parent->left) {
                        // case 3.2.2
                        k = k->parent;
                        _right_rotate(k, root);
                    }
                    // case 3.2.1
                    k->parent->color = 0;
                    k->parent->parent->color = 1;
                    _left_rotate(k->parent->parent, root);
                }
            } else {
                u = k->parent->parent->right;// uncle
                if (u != nullptr && u->color == 1) {
                    // mirror case 3.1
                    u->color = 0;
                    k->parent->color = 0;
                    k->parent->parent->color = 1;
                    k = k->parent->parent;
                } else {
                    if (k == k->parent->right) {
                        // mirror case 3.2.2
                        k = k->parent;
                        _left_rotate(k, root);
                    }
                    // mirror case 3.2.1
                    k->parent->color = 0;
                    k->parent->parent->color = 1;
                    _right_rotate(k->parent->parent, root);
                }
            }
            if (k == root) {
                break;
            }
        }
        root->color = 0;
    }
    static void delete_one_node(Node *z, Node *&root) {
        Node *t_null_parent = nullptr;
        Node *x;
        Node *y;
        y = z;
        int y_original_color = y->color;
        if (z->left == nullptr) {
            x = z->right;
            _rb_transplant(z, z->right, root, t_null_parent);
        } else if (z->right == nullptr) {
            x = z->left;
            _rb_transplant(z, z->left, root, t_null_parent);
        } else {
            y = _minimum(z->right);
            y_original_color = y->color;
            x = y->right;
            if (y->parent == z) {
                if (x)
                    x->parent = y;
                else
                    t_null_parent = y;
            } else {
                _rb_transplant(y, y->right, root, t_null_parent);
                y->right = z->right;
                y->right->parent = y;
            }
            _rb_transplant(z, y, root, t_null_parent);
            y->left = z->left;
            y->left->parent = y;
            y->color = z->color;
        }
        if (y_original_color == 0) {
            _fix_delete(x, root, t_null_parent);
        }
    }
};

// Map-constructible trait: V must be constructible from Args..., except for
// the key-only set specialization (V == void) which requires no value args.
template <typename T, typename... Args>
struct MapConstructible {
    static constexpr bool value = std::is_constructible_v<T, Args...>;
};
template <typename... Args>
struct MapConstructible<void, Args...> {
    static constexpr bool value = (sizeof...(Args) == 0);
};

}// namespace detail

template <typename K, typename V>
struct TreeElement {
    K first;
    V second;
    template <typename A, typename... B>
    TreeElement(A &&a, B &&...b)
        : first(std::forward<A>(a)),
          second(std::forward<B>(b)...) {
    }
};
template <typename K, typename V>
struct ConstTreeElement {
    const K first;
    V second;
    template <typename A, typename... B>
    ConstTreeElement(A &&a, B &&...b)
        : first(std::forward<A>(a)),
          second(std::forward<B>(b)...) {
    }
};

template <typename K, typename V, typename Compare = compare<K>>
class SmallTreeMap {
    inline static const Compare comp;

public:
    using Element = std::conditional_t<std::is_same_v<V, void>, K, TreeElement<K, V>>;
    using ConstElement = std::conditional_t<std::is_same_v<V, void>, K, ConstTreeElement<K, V>>;
    static auto &&get_first(auto &&ele) {
        if constexpr (std::is_same_v<V, void>) {
            return ele;
        } else {
            return ele.first;
        }
    }
    // data structure that represents a node in the tree
    struct Node {
        size_t array_index;
        size_t hash_value;
        Node *parent;// pointer to the parent
        Node *left;  // pointer to left child
        Node *right; // pointer to right child
        bool color;  // 1 -> Red, 0 -> Black
        Element data;// holds the key
        template <typename A, typename... B>
        Node(A &&a, B &&...b)
            : data(std::forward<A>(a), std::forward<B>(b)...) {
        }
    };

private:
    using TreeMapUtility = detail::TreeMapUtility<Node>;

    Node *_root;
    template <typename Key>
    Node *_search_tree(Node *node, Key const &key) const {
        while (node != nullptr) {
            auto comp_result = comp(get_first(node->data), key);
            static_assert(std::is_same_v<decltype(comp_result), int32_t>, "compare result must be int32");
            if (comp_result == 0) {
                return node;
            }
            if (comp_result > 0) {
                node = node->left;
            } else {
                node = node->right;
            }
        }
        return nullptr;
    }

    template <typename Key>
    std::pair<Node *, int32_t> _search_closest_tree(Node *node, Key const &key, Node *last_node, int32_t last_flag) const {
        while (node != nullptr) {
            auto comp_result = comp(get_first(node->data), key);
            static_assert(std::is_same_v<decltype(comp_result), int32_t>, "compare result must be int32");
            if (comp_result == 0) {
                return {node, 0};
            }
            if (comp_result > 0) {
                last_node = node;
                last_flag = 1;
                node = node->left;
            } else {
                last_node = node;
                last_flag = -1;
                node = node->right;
            }
        }
        return {last_node, last_flag};
    }

    void _delete_one_node(Node *z, auto &pool) {
        detail::TreeMapUtility<Node>::delete_one_node(z, _root);
        pool.destroy(z);
    }

    template <typename Key, typename POOL>
    bool _delete_node(Node *node, Key const &key, POOL &pool, size_t &array_index) {
        // find the node containing key
        Node *z = nullptr;
        while (node != nullptr) {
            auto comp_result = comp(get_first(node->data), key);
            static_assert(std::is_same_v<decltype(comp_result), int32_t>, "compare result must be int32");
            if (comp_result == 0) {
                z = node;
                break;
            }
            if (comp_result <= 0) {
                node = node->right;
            } else {
                node = node->left;
            }
        }
        if (z == nullptr) {
            return false;
        }
        array_index = z->array_index;
        _delete_one_node(z, pool);
        return true;
    }

    void _fix_insert(Node *k) {
        detail::TreeMapUtility<Node>::fix_insert(k, _root);
    }
    Node *_get_root() {
        return this->_root;
    }

public:
    SmallTreeMap() {
        _root = nullptr;
    }

    SmallTreeMap(SmallTreeMap const &) = delete;
    SmallTreeMap(SmallTreeMap &&o)
        : _root(o._root) {
        o._root = nullptr;
    };
    template <typename Key>
    Node *find(Key const &k) const {
        return _search_tree(this->_root, k);
    }
    template <typename POOL, typename Key, typename... Value>
    std::pair<Node *, bool> try_insert(POOL &pool, Key &&key, Value &&...value) {
        // Ordinary Binary Search Insertion
        Node *y = nullptr;
        Node *x = this->_root;
        int comp_result;
        while (x != nullptr) {
            y = x;
            auto m_comp_result = comp(get_first(x->data), key);
            static_assert(std::is_same_v<decltype(m_comp_result), int32_t>, "compare result must be int32");
            comp_result = m_comp_result;
            if (comp_result > 0) {
                x = x->left;
            } else if (comp_result == 0) {
                return {x, false};
            } else {
                x = x->right;
            }
        }
        Node *node = pool.create(std::forward<Key>(key), std::forward<Value>(value)...);
        node->left = nullptr;
        node->right = nullptr;
        node->color = true;// new node must be red
        // y is parent of x
        node->parent = y;
        if (y == nullptr) {
            _root = node;
        } else if (comp_result > 0) {
            y->left = node;
        } else {
            y->right = node;
        }

        // if new node is a root node, simply return
        if (node->parent == nullptr) {
            node->color = false;
            return {node, true};
        }

        // if the grandparent is null, simply return
        if (node->parent->parent == nullptr) {
            return {node, true};
        }
        // Fix the tree
        _fix_insert(node);
        return {node, true};
    }
    template <typename POOL, typename Key, typename... Value>
        requires(std::is_same_v<V, void> || std::is_move_constructible_v<V>)
    std::pair<Node *, bool> insert_or_assign(POOL &pool, Key &&key, Value &&...value) {
        // Ordinary Binary Search Insertion
        Node *y = nullptr;
        Node *x = this->_root;
        int comp_result;
        while (x != nullptr) {
            y = x;
            auto m_comp_result = comp(get_first(x->data), key);
            static_assert(std::is_same_v<decltype(m_comp_result), int32_t>, "compare result must be int32");
            comp_result = m_comp_result;
            if (comp_result > 0) {
                x = x->left;
            } else if (comp_result == 0) {
                if constexpr (!std::is_same_v<V, void>) {
                    x->data.second.~V();
                    new (&x->data.second) V(std::forward<Value>(value)...);
                }
                return {x, false};
            } else {
                x = x->right;
            }
        }
        Node *node = pool.create(std::forward<Key>(key), std::forward<Value>(value)...);
        node->parent = nullptr;
        node->left = nullptr;
        node->right = nullptr;
        node->color = true;// new node must be red
        // y is parent of x
        node->parent = y;
        if (y == nullptr) {
            _root = node;
        } else if (comp_result > 0) {
            y->left = node;
        } else {
            y->right = node;
        }

        // if new node is a root node, simply return
        if (node->parent == nullptr) {
            node->color = false;
            return {node, true};
        }

        // if the grandparent is null, simply return
        if (node->parent->parent == nullptr) {
            return {node, true};
        }
        // Fix the tree
        _fix_insert(node);
        return {node, true};
    }
    template <typename POOL>
    Node *weak_insert(POOL &pool, Node *node) {
        // Ordinary Binary Search Insertion
        Node *y = nullptr;
        Node *x = this->_root;
        int comp_result;
        while (x != nullptr) {
            y = x;
            auto m_comp_result = comp(get_first(x->data), get_first(node->data));
            static_assert(std::is_same_v<decltype(m_comp_result), int32_t>, "compare result must be int32");
            comp_result = m_comp_result;
            if (comp_result > 0) {
                x = x->left;
            } else if (comp_result == 0) {
                return x;
            } else {
                x = x->right;
            }
        }
        node->parent = nullptr;
        node->left = nullptr;
        node->right = nullptr;
        node->color = true;// new node must be red
        // y is parent of x
        node->parent = y;
        if (y == nullptr) {
            _root = node;
        } else if (comp_result > 0) {
            y->left = node;
        } else {
            y->right = node;
        }

        // if new node is a root node, simply return
        if (node->parent == nullptr) {
            node->color = false;
            return node;
        }

        // if the grandparent is null, simply return
        if (node->parent->parent == nullptr) {
            return node;
        }
        // Fix the tree
        _fix_insert(node);
        return node;
    }
    template <typename POOL, typename Key>
    bool remove(POOL &pool, Key const &data, size_t &array_index) {
        return _delete_node(this->_root, data, pool, array_index);
    }
    template <typename POOL>
    void remove(POOL &pool, Node *ite) {
        _delete_one_node(ite, pool);
    }
};

namespace hashmap_detail {

template <typename HashMap>
struct HashMapIndex : public HashMap::IndexBase {
    HashMapIndex() noexcept = default;
    HashMapIndex(const HashMap *map, typename HashMap::LinkNode *node) noexcept
        : HashMap::IndexBase(map, node) {}
    typename HashMap::KeyType const &key() const noexcept {
        using Map = typename HashMap::Map;
        return Map::get_first(this->node->data);
    }
    inline typename HashMap::ValueType &value() const noexcept {
        return this->node->data.second;
    }
    using HashMap::IndexBase::operator bool;
};

template <typename HashMap>
struct HashSetIndex : public HashMap::IndexBase {

private:
    using HashMapType = HashMap;
    using MapType = typename HashMapType::Map;
    using K = typename HashMap::KeyType;

public:
    HashSetIndex() noexcept = default;
    HashSetIndex(const HashMapType *map, typename HashMapType::LinkNode *node) noexcept
        : HashMapType::IndexBase(map, node) {}
    K const &get() const noexcept {
        return MapType::get_first(this->node->data);
    }
    K const *operator->() const noexcept {
        return &MapType::get_first(this->node->data);
    }
    K &operator*() const noexcept {
        return MapType::get_first(this->node->data);
    }
    using HashMap::IndexBase::operator bool;
};
template <typename V, typename HashMap>
struct HashSetIndexType {
    using type = HashMapIndex<HashMap>;
};

template <typename HashMap>
struct HashSetIndexType<void, HashMap> {
    using type = HashSetIndex<HashMap>;
};

}// namespace hashmap_detail

template <typename K, typename V = void, typename Hash = hash<K>, typename Compare = compare<K>>
class HashMap {

public:
    using KeyType = K;
    using ValueType = V;
    using HashType = Hash;
    using Map = SmallTreeMap<K, V, Compare>;
    using LinkNode = typename Map::Node;
    using NodePair = typename Map::ConstElement;
    using MoveNodePair = typename Map::Element;

    struct Iterator {
        friend class HashMap;

    private:
        LinkNode **ii;

    public:
        Iterator(LinkNode **ii) : ii(ii) {}
        bool operator==(const Iterator &ite) const noexcept {
            return ii == ite.ii;
        }
        void operator++() noexcept {
            ++ii;
        }
        void operator++(int) noexcept {
            ii++;
        }
        Iterator &operator+=(size_t i) noexcept {
            ii += i;
            return *this;
        }
        Iterator &operator-=(size_t i) noexcept {
            ii -= i;
            return *this;
        }
        Iterator operator+(size_t i) const noexcept {
            return Iterator{ii + i};
        }
        Iterator operator-(size_t i) const noexcept {
            return Iterator{ii - i};
        }
        NodePair *operator->() const noexcept {
            return reinterpret_cast<NodePair *>(&(*ii)->data);
        }
        NodePair &operator*() const noexcept {
            return reinterpret_cast<NodePair &>((*ii)->data);
        }
    };
    struct MoveIterator {
        friend class HashMap;

    private:
        LinkNode **ii;

    public:
        MoveIterator(LinkNode **ii) : ii(ii) {}
        bool operator==(const MoveIterator &ite) const noexcept {
            return ii == ite.ii;
        }
        void operator++() noexcept {
            ++ii;
        }
        void operator++(int) noexcept {
            ii++;
        }
        MoveIterator &operator+=(size_t i) noexcept {
            ii += i;
            return *this;
        }
        MoveIterator &operator-=(size_t i) noexcept {
            ii -= i;
            return *this;
        }
        MoveIterator operator+(size_t i) const noexcept {
            return MoveIterator{ii + i};
        }
        MoveIterator operator-(size_t i) const noexcept {
            return MoveIterator{ii - i};
        }
        MoveNodePair *operator->() const noexcept {
            return &(*ii)->data;
        }
        MoveNodePair &&operator*() const noexcept {
            return std::move((*ii)->data);
        }
    };

public:
    struct IndexBase {
        friend class HashMap;

        template <typename T>
        friend struct hashmap_detail::HashMapIndex;
        template <typename T>
        friend struct hashmap_detail::HashSetIndex;

    private:
        const HashMap *map;
        LinkNode *node;
        IndexBase(const HashMap *map, LinkNode *node) noexcept : map(map), node(node) {}

    public:
        IndexBase() noexcept : map(nullptr), node(nullptr) {}
        bool operator==(const IndexBase &a) const noexcept {
            return node == a.node;
        }
        operator bool() const noexcept {
            return node;
        }
        bool operator!() const noexcept {
            return !operator bool();
        }
        bool operator!=(const IndexBase &a) const noexcept {
            return !operator==(a);
        }
    };

    using Index = typename hashmap_detail::HashSetIndexType<V, HashMap>::type;

    Index get_index(Iterator const &ite) {
        return Index(this, *ite.ii);
    }
    Index get_index(MoveIterator const &ite) {
        return Index(this, *ite.ii);
    }

private:
    using NodePtr = LinkNode *;
    using NodePtrAllocator = kimix::allocator<NodePtr>;

    LinkNode **_node_array;
    Pool<LinkNode> pool;
    size_t _size;
    size_t _capacity;

    inline static const Hash _hasher;
    void _get_new_link_node(size_t hash_value, LinkNode *new_node) {
        new_node->hash_value = hash_value;
        new_node->array_index = _size;
        _node_array[_size] = new_node;
        _size++;
    }
    void _delete_link_node(size_t array_index) {
        if (array_index != (_size - 1)) {
            auto ite = _node_array + (_size - 1);
            (*ite)->array_index = array_index;
            _node_array[array_index] = *ite;
        }
        _size--;
    }
    static size_t _get_pow2_size(size_t capacity) noexcept {
        size_t ssize = 1;
        while (ssize < capacity)
            ssize <<= 1;
        return ssize;
    }
    static size_t _get_hash(size_t hash, size_t size) noexcept {
        return hash & (size - 1);
    }
    void _try_init() {
        if (_node_array) [[likely]]
            return;
        auto capacity = _capacity;
        if (capacity < 16) capacity = 16;
        capacity = _get_pow2_size(capacity);
        // First half: flat node list (insertion order). Second half: one
        // bucket tree per slot — SmallTreeMap is just a Node* root, so the
        // zero-filled pointer slots are valid default-constructed maps.
        _node_array = NodePtrAllocator{}.allocate(capacity * 2, alignof(NodePtr));
        std::memset(_node_array + capacity, 0, capacity * sizeof(LinkNode *));
        _capacity = capacity;
        _size = 0;
    }
    void _resize(size_t new_capacity) noexcept {
        if (_capacity >= new_capacity) return;
        LinkNode **new_node = NodePtrAllocator{}.allocate(new_capacity * 2, alignof(NodePtr));
        std::memcpy(new_node, _node_array, sizeof(LinkNode *) * _size);
        auto node_vec = new_node + new_capacity;
        std::memset(node_vec, 0, sizeof(LinkNode *) * new_capacity);
        for (size_t i = 0; i < _size; ++i) {
            auto node = _node_array[i];
            size_t hash_value = node->hash_value;
            hash_value = _get_hash(hash_value, new_capacity);
            Map *target_tree = reinterpret_cast<Map *>(&node_vec[hash_value]);
            target_tree->weak_insert(pool, node);
        }
        NodePtrAllocator{}.deallocate(_node_array, _capacity * 2);
        _node_array = new_node;
        _capacity = new_capacity;
    }
    static Index _empty_index() noexcept {
        return Index(nullptr, nullptr);
    }
    void remove(LinkNode *node) {
        size_t hash_value = _get_hash(node->hash_value, _capacity);
        Map *target_tree = reinterpret_cast<Map *>(_node_array + _capacity + hash_value);
        auto array_index = node->array_index;
        target_tree->remove(pool, node);
        _delete_link_node(array_index);
    }
    void _try_resize() {
        _try_init();
        size_t target_capacity = (size_t)(((_size + 1) * 4 + 2) / 3);
        if (target_capacity < 16) target_capacity = 16;
        if (target_capacity > _capacity) {
            _resize(_get_pow2_size(target_capacity));
        }
    }

public:
    decltype(auto) begin() const & {
        return Iterator(_node_array);
    }
    decltype(auto) begin() && {
        return MoveIterator(_node_array);
    }
    decltype(auto) end() const & {
        return Iterator(_node_array + _size);
    }
    decltype(auto) end() && {
        return MoveIterator(_node_array + _size);
    }
    //////////////////Construct & Destruct
    HashMap(size_t capacity) noexcept {
        // Match the initial table size to _try_resize()'s growth rule
        // (((size + 1) * 4 + 2) / 3): a plain pow2 round-up of the hint may
        // sit below the 3/4 load threshold, forcing a full rehash of nearly
        // all elements right before the last inserts. Reserving for 4/3 of
        // the hinted element count guarantees `capacity` inserts fit.
        if (capacity < 16) capacity = 16;
        capacity = _get_pow2_size((capacity * 4 + 2) / 3);
        _node_array = NodePtrAllocator{}.allocate(capacity * 2, alignof(NodePtr));
        std::memset(_node_array + capacity, 0, capacity * sizeof(LinkNode *));
        _capacity = capacity;
        _size = 0;
    }
    // kimix::Pool is not movable, so a move is a deep move: every node is
    // recreated in the destination's own pool at the same array index and
    // linked into its bucket tree (O(n) instead of the original O(1) steal).
    HashMap(HashMap &&map) {
        _size = map._size;
        _capacity = map._capacity;
        _node_array = nullptr;
        if (map._node_array) {
            _node_array = NodePtrAllocator{}.allocate(_capacity * 2, alignof(NodePtr));
            std::memset(_node_array + _capacity, 0, _capacity * sizeof(LinkNode *));
            for (size_t i = 0; i < _size; ++i) {
                auto src = map._node_array[i];
                auto dst = pool.create(std::move(src->data));
                dst->hash_value = src->hash_value;
                dst->array_index = i;
                _node_array[i] = dst;
                auto bucket = _get_hash(dst->hash_value, _capacity);
                reinterpret_cast<Map *>(_node_array + _capacity + bucket)->weak_insert(pool, dst);
            }
            // Now release the source's own storage. Its nodes were moved out of
            // but their destructors still have to run (~Pool frees the raw
            // blocks without destroying anything), and its node array has to go
            // back to the allocator. Dropping the array (what this used to do)
            // leaked one buffer per move -- harmless-looking and invisible until
            // the KIMIX_MEMORY_STRICT tracker reported it.
            size_t const source_capacity = map._capacity;
            for (size_t i = 0; i < map._size; ++i) {
                map._node_array[i]->~LinkNode();
            }
            NodePtrAllocator{}.deallocate(map._node_array, source_capacity * 2);
            map._node_array = nullptr;
            map._size = 0;
            map._capacity = 0;
        }
    }
    HashMap(HashMap const &map) = delete;

    HashMap &operator=(HashMap &&map) {
        this->~HashMap();
        new (this) HashMap(std::move(map));
        return *this;
    }
    ~HashMap() noexcept {
        if (!_node_array) return;
        for (size_t i = 0; i < _size; ++i) {
            _node_array[i]->~LinkNode();
        }
        NodePtrAllocator{}.deallocate(_node_array, _capacity * 2);
    }
    HashMap() noexcept {
        _node_array = nullptr;
        _size = 0;
        _capacity = 0;
    }
    ///////////////////////
    template <typename Key, typename... ARGS>
        requires(std::is_constructible_v<K, Key &&> && detail::MapConstructible<V, ARGS &&...>::value && (std::is_same_v<V, void> || std::is_move_constructible_v<V>))
    Index force_emplace(Key &&key, ARGS &&...args) {
        _try_resize();

        size_t hash_origin_value = _hasher(std::forward<Key>(key));
        size_t hash_value = _get_hash(hash_origin_value, _capacity);
        auto node_vec = _node_array + _capacity;

        Map *map = reinterpret_cast<Map *>(&node_vec[hash_value]);
        auto insert_result = map->insert_or_assign(pool, std::forward<Key>(key), std::forward<ARGS>(args)...);
        //Add create
        if (insert_result.second) {
            _get_new_link_node(hash_origin_value, insert_result.first);
        }
        return Index(this, insert_result.first);
    }

    template <typename Key, typename... ARGS>
        requires(std::is_constructible_v<K, Key &&> && detail::MapConstructible<V, ARGS &&...>::value)
    std::pair<Index, bool> try_emplace(Key &&key, ARGS &&...args) {
        _try_resize();

        size_t hash_origin_value = _hasher(std::forward<Key>(key));
        size_t hash_value = _get_hash(hash_origin_value, _capacity);
        auto node_vec = _node_array + _capacity;

        Map *map = reinterpret_cast<Map *>(&node_vec[hash_value]);
        auto insert_result = map->try_insert(pool, std::forward<Key>(key), std::forward<ARGS>(args)...);
        //Add create
        if (insert_result.second) {
            _get_new_link_node(hash_origin_value, insert_result.first);
        }
        return {Index(this, insert_result.first), insert_result.second};
    }

    template <typename Key, typename... ARGS>
        requires(std::is_constructible_v<K, Key &&> && detail::MapConstructible<V, ARGS &&...>::value)
    Index emplace(Key &&key, ARGS &&...args) {
        return try_emplace(std::forward<Key>(key), std::forward<ARGS>(args)...).first;
    }

    void reserve(size_t capacity) noexcept {
        _try_init();
        // Same 4/3 rule as the capacity-hint constructor: reserve() should
        // guarantee `capacity` inserts without a rehash.
        size_t new_capacity = _get_pow2_size((capacity * 4 + 2) / 3);
        _resize(new_capacity);
    }
    template <typename Key>
    Index find(Key &&key) const noexcept {
        if (!_node_array) [[unlikely]]
            return _empty_index();
        size_t hash_origin_value = _hasher(std::forward<Key>(key));
        size_t hash_value = _get_hash(hash_origin_value, _capacity);
        Map *map = reinterpret_cast<Map *>(_node_array + _capacity + hash_value);
        auto node = map->find(std::forward<Key>(key));
        if (node)
            return {this, node};
        return _empty_index();
    }

    template <typename Key>
        requires(!std::is_same_v<std::remove_cvref_t<Key>, Index> &&
                 !std::is_same_v<std::remove_cvref_t<Key>, Iterator> &&
                 !std::is_same_v<std::remove_cvref_t<Key>, MoveIterator>)
    void remove(Key &&key) noexcept {
        if (!_node_array) [[unlikely]]
            return;
        size_t hash_origin_value = _hasher(key);
        size_t hash_value = _get_hash(hash_origin_value, _capacity);
        Map *map = reinterpret_cast<Map *>(_node_array + _capacity + hash_value);
        size_t array_index;
        if (map->remove(pool, std::forward<Key>(key), array_index)) {
            _delete_link_node(array_index);
        }
    }

    void remove(const Index &ite) noexcept {
        remove(ite.node);
    }

    void remove(Iterator const &iter) noexcept {
        remove(*iter.ii);
    }

    void remove(MoveIterator const &iter) noexcept {
        remove(*iter.ii);
    }

    void clear() noexcept {
        if (_size == 0) return;
        auto node_vec = _node_array + _capacity;
        std::memset(node_vec, 0, _capacity * sizeof(LinkNode *));
        for (size_t i = 0; i < _size; ++i) {
            pool.destroy(_node_array[i]);
        }
        _size = 0;
    }
    [[nodiscard]] size_t size() const noexcept { return _size; }
    [[nodiscard]] bool empty() const noexcept { return _size == 0; }
    [[nodiscard]] size_t capacity() const noexcept { return _capacity; }
};
}// namespace kimix
