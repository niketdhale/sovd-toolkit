#include "sovd/entity_registry.hpp"

#include <algorithm>

namespace sovd {

std::string entity_type_to_string(EntityType t) {
    switch (t) {
        case EntityType::Vehicle: return "vehicle";
        case EntityType::Area: return "area";
        case EntityType::Component: return "component";
    }
    return "unknown";
}

std::string EntityRegistry::parent_of(const std::string &path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) return "";
    return path.substr(0, pos);
}

EntityRegistry::~EntityRegistry() {
    for (auto &path : order_) {
        auto &e = entities_.at(path);
        if (e.vtable && e.vtable->destroy && e.adapter_ctx) {
            e.vtable->destroy(e.adapter_ctx);
        }
    }
}

bool EntityRegistry::add_entity(const std::string &path, EntityType type,
                                 const sovd_vtable_t *vtable, sovd_adapter_ctx *ctx) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (path.empty() || entities_.count(path)) return false;

    std::string parent = parent_of(path);
    if (!parent.empty() && !entities_.count(parent)) {
        return false; // orphan rejection
    }

    Entity e;
    e.path = path;
    e.type = type;
    e.parent = parent;
    e.vtable = vtable;
    e.adapter_ctx = ctx;

    entities_.emplace(path, std::move(e));
    order_.push_back(path);
    return true;
}

const Entity *EntityRegistry::find(const std::string &path) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = entities_.find(path);
    if (it == entities_.end()) return nullptr;
    return &it->second;
}

std::vector<const Entity *> EntityRegistry::list_all() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<const Entity *> out;
    out.reserve(order_.size());
    for (auto &p : order_) out.push_back(&entities_.at(p));
    return out;
}

bool EntityRegistry::remove_entity(const std::string &path) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = entities_.find(path);
    if (it == entities_.end()) return false;

    for (auto &kv : entities_) {
        if (kv.second.parent == path) return false; // orphan rejection
    }

    if (it->second.vtable && it->second.vtable->destroy && it->second.adapter_ctx) {
        it->second.vtable->destroy(it->second.adapter_ctx);
    }
    entities_.erase(it);
    order_.erase(std::remove(order_.begin(), order_.end(), path), order_.end());
    return true;
}

} // namespace sovd
