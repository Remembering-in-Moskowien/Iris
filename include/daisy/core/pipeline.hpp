#pragma once

#include <memory>
#include <string>
#include <vector>

#include "daisy/core/context.hpp"
#include "daisy/core/operator.hpp"

namespace daisy::core {

/// DAG 管线：注册操作符 → 拓扑排序 → 按依赖顺序执行。
/// 对应 C# 版 Workflow/Pipeline 的 DAG 执行器。
class Pipeline {
public:
    void add(std::shared_ptr<Operator> op);

    /// 按依赖拓扑执行全部操作符；任一失败立即停止。
    /// 返回是否全部成功。
    bool run(PipelineContext& ctx);

private:
    /// Kahn 拓扑排序；存在环或缺失依赖返回 false。
    bool topo_sort(std::vector<std::shared_ptr<Operator>>& out) const;

    std::vector<std::shared_ptr<Operator>> ops_;
};

} // namespace daisy::core
