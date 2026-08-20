// EntityRegistry — hierarchical entity tree keyed by full slash-separated
// path (e.g. "vehicle/body/bcm"). Pure logic: no HTTP, no sockets, no JSON.
#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "sovd/adapter.h"

namespace sovd {

enum class EntityType { Vehicle, Area, Component };

// Phase 8: unbounded entity-tree depth is a DoS on a safety-adjacent
// interface (a pathological config, or -- once Phase 4's config loader
// ever reads anything untrusted -- a crafted one, could otherwise recurse
// registry/proxy logic arbitrarily deep). 16 is far beyond any real vehicle
// topology (the demo tops out at 3: vehicle/body/bcm) but still a real
// ceiling, not a formality.
inline constexpr int kMaxEntityPathDepth = 16;

std::string entity_type_to_string(EntityType t);

struct Entity {
    std::string path;
    EntityType type = EntityType::Component;
    std::string parent; // "" for a root entity
    const sovd_vtable_t *vtable = nullptr; // nullptr => grouping node, no backend
    sovd_adapter_ctx *adapter_ctx = nullptr;

    bool has_backend() const { return vtable != nullptr; }
};

// Thread-safe. Pointers returned by find()/list_all() stay valid across
// concurrent add_entity() calls (std::unordered_map never relocates live
// elements), but are invalidated by remove_entity() of that same path —
// Phase 0's topology is built once at startup and never mutated afterward,
// so this is not yet exercised under concurrent removal.
class EntityRegistry {
public:
    EntityRegistry() = default;
    ~EntityRegistry();

    EntityRegistry(const EntityRegistry &) = delete;
    EntityRegistry &operator=(const EntityRegistry &) = delete;

    // Fails (returns false) if path is empty, already registered, its
    // parent (everything before the last '/') is not already registered
    // (orphan rejection — a path with no '/' is a root entity and needs no
    // parent), or path is nested deeper than kMaxEntityPathDepth segments.
    bool add_entity(const std::string &path, EntityType type,
                     const sovd_vtable_t *vtable = nullptr,
                     sovd_adapter_ctx *ctx = nullptr);

    const Entity *find(const std::string &path) const;
    std::vector<const Entity *> list_all() const;

    // Fails if path is unknown or still has children registered under it
    // (orphan rejection on the way out too).
    bool remove_entity(const std::string &path);

private:
    static std::string parent_of(const std::string &path);

    mutable std::mutex mtx_;
    std::unordered_map<std::string, Entity> entities_;
    std::vector<std::string> order_;
};

} // namespace sovd
