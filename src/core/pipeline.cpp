#include "daisy/core/pipeline.hpp"

#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace daisy::core {

void Pipeline::add(std::shared_ptr<Operator> op) { ops_.push_back(std::move(op)); }

bool Pipeline::topo_sort(std::vector<std::shared_ptr<Operator>>& out) const {
    std::unordered_map<std::string, std::shared_ptr<Operator>> by_name;
    std::unordered_map<std::string, int> indegree;
    std::unordered_map<std::string, std::vector<std::string>> edges;

    for (const auto& op : ops_) {
        by_name[op->name()] = op;
        indegree[op->name()] = 0;
    }
    for (const auto& op : ops_) {
        for (const auto& dep : op->dependencies()) {
            const auto it = by_name.find(dep);
            if (it == by_name.end()) return false; // 依赖缺失
            edges[dep].push_back(op->name());
            ++indegree[op->name()];
        }
    }

    std::queue<std::string> ready;
    for (const auto& [name, deg] : indegree) {
        if (deg == 0) ready.push(name);
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
    if (order.size() != by_name.size()) return false; // 存在环

    out.clear();
    out.reserve(order.size());
    for (const auto& name : order) out.push_back(by_name[name]);
    return true;
}

bool Pipeline::run(PipelineContext& ctx) {
    std::vector<std::shared_ptr<Operator>> order;
    if (!topo_sort(order)) return false;
    for (auto& op : order) {
        if (ctx.on_progress) ctx.on_progress(op->name(), 0.0f);
        const bool ok = op->execute(ctx);
        if (ctx.on_progress) ctx.on_progress(op->name(), 1.0f);
        if (!ok) return false;
    }
    return true;
}

} // namespace daisy::core
