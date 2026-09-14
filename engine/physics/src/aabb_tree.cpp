// A dynamic AABB tree, after Box2D's b2DynamicTree. Leaves hold proxies in
// fat boxes (the box plus a margin, stretched along the way it last moved);
// internal nodes hold the union of their children. A proxy that moves
// within its fat box costs nothing; one that leaves it is pulled out and
// reinserted where the surface-area heuristic says it inflates the tree
// least, and AVL-style rotations on the way back up keep the tree balanced.
//
// Pairs come from walking the tree against itself: two subtrees whose boxes
// miss are done with in one test, however many leaves they hold. Queries
// and ray casts walk it the same way, which is why this one becomes the
// engine's spatial index, not only its broadphase.
#include <tynima/physics/broadphase.h>

#include <tynima/core/assert.h>

#include "broadphase_common.h"

#include <algorithm>
#include <utility>

namespace tynima::physics {

namespace {

constexpr std::int32_t kNull = -1;

struct TreeProxy : Proxy {
    std::int32_t node = kNull;
};

struct Node {
    Aabb box; // fat for a leaf, the union of the children otherwise
    std::int32_t parent = kNull; // the next free node while on the free list
    std::int32_t child1 = kNull;
    std::int32_t child2 = kNull;
    std::int32_t height = -1; // 0 for a leaf, -1 while free
    TreeProxy* proxy = nullptr;

    [[nodiscard]] bool is_leaf() const noexcept { return child1 == kNull; }
};

class AabbTree final : public Broadphase {
public:
    AabbTree(std::uint32_t capacity, float margin) : proxies_(capacity), margin_(margin) {
        // n leaves need n - 1 internal nodes; one spare for the swap in insert.
        nodes_.resize(2 * static_cast<std::size_t>(capacity) + 1);
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            nodes_[i].parent = i + 1 < nodes_.size() ? static_cast<std::int32_t>(i + 1) : kNull;
        }
        free_ = nodes_.empty() ? kNull : 0;
        stack_.reserve(256);
        pair_stack_.reserve(256);
    }

    const char* name() const noexcept override { return "AABB tree"; }

    ProxyHandle add(const Aabb& box, std::uint64_t user_data) override {
        const ProxyHandle handle = proxies_.add(box, user_data);
        if (!handle) {
            return handle;
        }
        TreeProxy* p = proxies_.get(handle);
        p->node = allocate_node();
        Node& leaf = nodes_[static_cast<std::size_t>(p->node)];
        leaf.box = box.expanded(margin_);
        leaf.height = 0;
        leaf.proxy = p;
        insert_leaf(p->node);
        return handle;
    }

    void update(ProxyHandle proxy, const Aabb& box) override {
        TreeProxy* p = proxies_.get(proxy);
        if (p == nullptr) {
            return;
        }
        const math::Vec3 displacement = box.center() - p->box.center();
        p->box = box;
        Node& leaf = nodes_[static_cast<std::size_t>(p->node)];
        if (leaf.box.contains(box)) {
            return; // still inside its fat box: the tree does not change
        }
        remove_leaf(p->node);
        // Fatten, and stretch in the direction of travel so a body moving
        // steadily is not reinserted every frame.
        Aabb fat = box.expanded(margin_);
        const math::Vec3 d = displacement * 2.0f;
        if (d.x < 0.0f) fat.min.x += d.x; else fat.max.x += d.x;
        if (d.y < 0.0f) fat.min.y += d.y; else fat.max.y += d.y;
        if (d.z < 0.0f) fat.min.z += d.z; else fat.max.z += d.z;
        leaf.box = fat;
        insert_leaf(p->node);
    }

    bool remove(ProxyHandle proxy) override {
        TreeProxy* p = proxies_.get(proxy);
        if (p == nullptr) {
            return false;
        }
        remove_leaf(p->node);
        free_node(p->node);
        return proxies_.remove(proxy);
    }

    bool valid(ProxyHandle proxy) const noexcept override { return proxies_.valid(proxy); }
    std::uint64_t user_data(ProxyHandle proxy) const override { return proxies_.user_data(proxy); }
    Aabb box(ProxyHandle proxy) const override { return proxies_.box(proxy); }
    std::uint32_t proxy_count() const noexcept override { return proxies_.count(); }

    void find_pairs(std::vector<ProxyPair>& out) override {
        out.clear();
        if (root_ == kNull) {
            return;
        }
        pair_stack_.clear();
        pair_stack_.push_back({root_, root_});
        while (!pair_stack_.empty()) {
            const auto [a, b] = pair_stack_.back();
            pair_stack_.pop_back();
            const Node& na = nodes_[static_cast<std::size_t>(a)];
            const Node& nb = nodes_[static_cast<std::size_t>(b)];
            if (a == b) {
                // A subtree against itself: its two halves against each
                // other, and each half against itself.
                if (!na.is_leaf()) {
                    pair_stack_.push_back({na.child1, na.child2});
                    pair_stack_.push_back({na.child1, na.child1});
                    pair_stack_.push_back({na.child2, na.child2});
                }
                continue;
            }
            if (!na.box.overlaps(nb.box)) {
                continue;
            }
            if (na.is_leaf() && nb.is_leaf()) {
                if (na.proxy->box.overlaps(nb.proxy->box)) {
                    out.push_back(ordered_pair(na.proxy->self, nb.proxy->self));
                }
            } else if (nb.is_leaf() || (!na.is_leaf() && na.box.surface_area() > nb.box.surface_area())) {
                pair_stack_.push_back({na.child1, b});
                pair_stack_.push_back({na.child2, b});
            } else {
                pair_stack_.push_back({a, nb.child1});
                pair_stack_.push_back({a, nb.child2});
            }
        }
    }

    void query(const Aabb& box, ProxyVisitor visit, void* user) const override {
        if (root_ == kNull) {
            return;
        }
        stack_.clear();
        stack_.push_back(root_);
        while (!stack_.empty()) {
            const Node& node = nodes_[static_cast<std::size_t>(stack_.back())];
            stack_.pop_back();
            if (!node.box.overlaps(box)) {
                continue;
            }
            if (node.is_leaf()) {
                if (node.proxy->box.overlaps(box)) {
                    visit(user, node.proxy->self, node.proxy->user_data);
                }
            } else {
                stack_.push_back(node.child1);
                stack_.push_back(node.child2);
            }
        }
    }

    void cast_ray(const math::Vec3& origin, const math::Vec3& direction, float max_t, ProxyVisitor visit,
                  void* user) const override {
        if (root_ == kNull) {
            return;
        }
        stack_.clear();
        stack_.push_back(root_);
        while (!stack_.empty()) {
            const Node& node = nodes_[static_cast<std::size_t>(stack_.back())];
            stack_.pop_back();
            if (!node.box.intersects_ray(origin, direction, max_t)) {
                continue;
            }
            if (node.is_leaf()) {
                if (node.proxy->box.intersects_ray(origin, direction, max_t)) {
                    visit(user, node.proxy->self, node.proxy->user_data);
                }
            } else {
                stack_.push_back(node.child1);
                stack_.push_back(node.child2);
            }
        }
    }

    // Every link, height and box in the tree agrees with its children, and
    // every proxy is in it exactly once.
    bool self_check() const override {
        if (root_ == kNull) {
            return proxies_.count() == 0;
        }
        if (nodes_[static_cast<std::size_t>(root_)].parent != kNull) {
            return false;
        }
        std::uint32_t leaves = 0;
        stack_.clear();
        stack_.push_back(root_);
        while (!stack_.empty()) {
            const std::int32_t index = stack_.back();
            stack_.pop_back();
            const Node& node = nodes_[static_cast<std::size_t>(index)];
            if (node.height < 0) {
                return false; // a free node in the tree
            }
            if (node.is_leaf()) {
                if (node.child2 != kNull || node.height != 0 || node.proxy == nullptr ||
                    node.proxy->node != index || !node.box.contains(node.proxy->box)) {
                    return false;
                }
                ++leaves;
                continue;
            }
            const Node& c1 = nodes_[static_cast<std::size_t>(node.child1)];
            const Node& c2 = nodes_[static_cast<std::size_t>(node.child2)];
            if (c1.parent != index || c2.parent != index) {
                return false;
            }
            if (node.height != 1 + std::max(c1.height, c2.height)) {
                return false;
            }
            if (node.box != c1.box.merged(c2.box)) {
                return false;
            }
            stack_.push_back(node.child1);
            stack_.push_back(node.child2);
        }
        return leaves == proxies_.count();
    }

private:
    std::int32_t allocate_node() {
        TY_ASSERT(free_ != kNull, "the AABB tree ran out of nodes; capacity should cover every proxy");
        const std::int32_t index = free_;
        Node& node = nodes_[static_cast<std::size_t>(index)];
        free_ = node.parent;
        node = Node{};
        return index;
    }

    void free_node(std::int32_t index) {
        Node& node = nodes_[static_cast<std::size_t>(index)];
        node = Node{};
        node.parent = free_;
        free_ = index;
    }

    [[nodiscard]] Node& node(std::int32_t index) noexcept { return nodes_[static_cast<std::size_t>(index)]; }

    void insert_leaf(std::int32_t leaf) {
        if (root_ == kNull) {
            root_ = leaf;
            node(leaf).parent = kNull;
            return;
        }

        // Descend to the sibling that costs least by the surface-area
        // heuristic: the growth of every box on the way down, plus the box
        // the new parent will need.
        const Aabb leaf_box = node(leaf).box;
        std::int32_t index = root_;
        while (!node(index).is_leaf()) {
            const std::int32_t child1 = node(index).child1;
            const std::int32_t child2 = node(index).child2;
            const float area = node(index).box.surface_area();
            const float combined_area = node(index).box.merged(leaf_box).surface_area();
            const float cost = 2.0f * combined_area;
            const float inherited = 2.0f * (combined_area - area);
            const auto child_cost = [&](std::int32_t child) {
                const Aabb merged = leaf_box.merged(node(child).box);
                if (node(child).is_leaf()) {
                    return merged.surface_area() + inherited;
                }
                return merged.surface_area() - node(child).box.surface_area() + inherited;
            };
            const float cost1 = child_cost(child1);
            const float cost2 = child_cost(child2);
            if (cost < cost1 && cost < cost2) {
                break;
            }
            index = cost1 < cost2 ? child1 : child2;
        }
        const std::int32_t sibling = index;

        const std::int32_t old_parent = node(sibling).parent;
        const std::int32_t new_parent = allocate_node();
        node(new_parent).parent = old_parent;
        node(new_parent).box = leaf_box.merged(node(sibling).box);
        node(new_parent).height = node(sibling).height + 1;
        if (old_parent != kNull) {
            if (node(old_parent).child1 == sibling) {
                node(old_parent).child1 = new_parent;
            } else {
                node(old_parent).child2 = new_parent;
            }
        } else {
            root_ = new_parent;
        }
        node(new_parent).child1 = sibling;
        node(new_parent).child2 = leaf;
        node(sibling).parent = new_parent;
        node(leaf).parent = new_parent;

        refit_upwards(node(leaf).parent);
    }

    void remove_leaf(std::int32_t leaf) {
        if (leaf == root_) {
            root_ = kNull;
            return;
        }
        const std::int32_t parent = node(leaf).parent;
        const std::int32_t grandparent = node(parent).parent;
        const std::int32_t sibling = node(parent).child1 == leaf ? node(parent).child2 : node(parent).child1;
        if (grandparent != kNull) {
            if (node(grandparent).child1 == parent) {
                node(grandparent).child1 = sibling;
            } else {
                node(grandparent).child2 = sibling;
            }
            node(sibling).parent = grandparent;
            free_node(parent);
            refit_upwards(grandparent);
        } else {
            root_ = sibling;
            node(sibling).parent = kNull;
            free_node(parent);
        }
    }

    // Walks to the root re-balancing and recomputing boxes and heights.
    void refit_upwards(std::int32_t index) {
        while (index != kNull) {
            index = balance(index);
            Node& n = node(index);
            const Node& c1 = node(n.child1);
            const Node& c2 = node(n.child2);
            n.height = 1 + std::max(c1.height, c2.height);
            n.box = c1.box.merged(c2.box);
            index = n.parent;
        }
    }

    // One AVL rotation at `a` if its subtrees differ in height by more than
    // one. Returns the node now in a's place.
    std::int32_t balance(std::int32_t a) {
        Node& na = node(a);
        if (na.is_leaf() || na.height < 2) {
            return a;
        }
        const std::int32_t b = na.child1;
        const std::int32_t c = na.child2;
        Node& nb = node(b);
        Node& nc = node(c);
        const std::int32_t lean = nc.height - nb.height;

        if (lean > 1) {
            // c is the taller child: rotate it up over a.
            const std::int32_t f = nc.child1;
            const std::int32_t g = nc.child2;
            Node& nf = node(f);
            Node& ng = node(g);
            nc.child1 = a;
            nc.parent = na.parent;
            na.parent = c;
            if (nc.parent != kNull) {
                if (node(nc.parent).child1 == a) {
                    node(nc.parent).child1 = c;
                } else {
                    node(nc.parent).child2 = c;
                }
            } else {
                root_ = c;
            }
            if (nf.height > ng.height) {
                nc.child2 = f;
                na.child2 = g;
                ng.parent = a;
                na.box = nb.box.merged(ng.box);
                nc.box = na.box.merged(nf.box);
                na.height = 1 + std::max(nb.height, ng.height);
                nc.height = 1 + std::max(na.height, nf.height);
            } else {
                nc.child2 = g;
                na.child2 = f;
                nf.parent = a;
                na.box = nb.box.merged(nf.box);
                nc.box = na.box.merged(ng.box);
                na.height = 1 + std::max(nb.height, nf.height);
                nc.height = 1 + std::max(na.height, ng.height);
            }
            return c;
        }

        if (lean < -1) {
            // b is the taller child: rotate it up over a.
            const std::int32_t d = nb.child1;
            const std::int32_t e = nb.child2;
            Node& nd = node(d);
            Node& ne = node(e);
            nb.child1 = a;
            nb.parent = na.parent;
            na.parent = b;
            if (nb.parent != kNull) {
                if (node(nb.parent).child1 == a) {
                    node(nb.parent).child1 = b;
                } else {
                    node(nb.parent).child2 = b;
                }
            } else {
                root_ = b;
            }
            if (nd.height > ne.height) {
                nb.child2 = d;
                na.child1 = e;
                ne.parent = a;
                na.box = nc.box.merged(ne.box);
                nb.box = na.box.merged(nd.box);
                na.height = 1 + std::max(nc.height, ne.height);
                nb.height = 1 + std::max(na.height, nd.height);
            } else {
                nb.child2 = e;
                na.child1 = d;
                nd.parent = a;
                na.box = nc.box.merged(nd.box);
                nb.box = na.box.merged(ne.box);
                na.height = 1 + std::max(nc.height, nd.height);
                nb.height = 1 + std::max(na.height, ne.height);
            }
            return b;
        }
        return a;
    }

    ProxyTable<TreeProxy> proxies_;
    std::vector<Node> nodes_;
    std::int32_t root_ = kNull;
    std::int32_t free_ = kNull;
    float margin_;
    mutable std::vector<std::int32_t> stack_;                   // scratch for queries
    std::vector<std::pair<std::int32_t, std::int32_t>> pair_stack_; // scratch for find_pairs
};

} // namespace

std::unique_ptr<Broadphase> create_aabb_tree(std::uint32_t capacity, float margin) {
    return std::make_unique<AabbTree>(capacity, margin);
}

} // namespace tynima::physics
