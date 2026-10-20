#pragma once

#include <string>
#include <vector>

#include "iris/core/context.hpp"

namespace iris::core {

/// 操作符：管线中的一个独立、可替换阶段（对应 C# Operator 抽象基类）。
/// 每个操作符声明名称、依赖的操作符（DAG 边）和 execute 行为。
class Operator {
public:
    virtual ~Operator() = default;

    /// 操作符名称（必须唯一；同时写入 IR provenance）。
    [[nodiscard]] virtual std::string name() const = 0;

    /// 依赖的操作符名列表；执行前必须已完成。
    [[nodiscard]] virtual std::vector<std::string> dependencies() const { return {}; }

    /// 执行本阶段。返回是否成功。
    virtual bool execute(PipelineContext& ctx) = 0;
};

} // namespace iris::core
