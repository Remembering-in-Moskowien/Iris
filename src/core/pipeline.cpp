#include "iris/core/pipeline.hpp"

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace iris::core {

void Pipeline::add(std::shared_ptr<Operator> op) { ops_.push_back(std::move(op)); }

bool Pipeline::topo_sort(std::vector<std::shared_ptr<Operator>>& out,
                         std::string* error) const {
    if (error) error->clear();
    std::unordered_map<std::string, std::shared_ptr<Operator>> by_name;
    std::unordered_map<std::string, int> indegree;
    std::unordered_map<std::string, std::vector<std::string>> edges;

    for (const auto& op : ops_) {
        if (!op) {
            if (error) *error = "pipeline contains a null operator";
            return false;
        }
        const std::string name = op->name();
        if (name.empty()) {
            if (error) *error = "pipeline operator name must not be empty";
            return false;
        }
        if (!by_name.emplace(name, op).second) {
            if (error) *error = "duplicate pipeline operator: " + name;
            return false;
        }
        indegree.emplace(name, 0);
    }
    for (const auto& op : ops_) {
        for (const auto& dep : op->dependencies()) {
            const auto it = by_name.find(dep);
            if (it == by_name.end()) {
                if (error) *error = "missing pipeline dependency '" + dep +
                                    "' for operator '" + op->name() + "'";
                return false;
            }
            edges[dep].push_back(op->name());
            ++indegree[op->name()];
        }
    }

    std::queue<std::string> ready;
    for (const auto& op : ops_) {
        const auto& name = op->name();
        if (indegree.at(name) == 0) ready.push(name);
    }

    std::vector<std::string> order;
    order.reserve(by_name.size());
    while (!ready.empty()) {
        const std::string n = ready.front();
        ready.pop();
        order.push_back(n);
        for (const auto& m : edges[n]) {
            if (--indegree[m] == 0) ready.push(m);
        }
    }
    if (order.size() != by_name.size()) {
        if (error) *error = "pipeline contains a dependency cycle";
        return false;
    }

    out.clear();
    out.reserve(order.size());
    for (const auto& name : order) out.push_back(by_name[name]);
    return true;
}

bool Pipeline::run(PipelineContext& ctx, std::string* error) {
    std::vector<std::shared_ptr<Operator>> order;
    if (!topo_sort(order, error)) return false;
    for (auto& op : order) {
        if (ctx.on_progress) ctx.on_progress(op->name(), 0.0f);
        const bool ok = op->execute(ctx);
        if (ctx.on_progress) ctx.on_progress(op->name(), 1.0f);
        if (!ok) {
            if (error) *error = "pipeline operator failed: " + op->name();
            return false;
        }
    }
    return true;
}

} // namespace iris::core
