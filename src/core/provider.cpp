#include "daisy/core/provider.hpp"

#include <algorithm>

namespace daisy::core {

void ProviderRegistry::register_provider(ProviderInfo info) {
    // 同名同域覆盖；否则追加。
    const auto it = std::find_if(providers_.begin(), providers_.end(),
                                 [&](const ProviderInfo& p) {
                                     return p.name == info.name && p.domain == info.domain;
                                 });
    if (it != providers_.end()) {
        *it = std::move(info);
    } else {
        providers_.push_back(std::move(info));
    }
}

std::optional<ProviderInfo> ProviderRegistry::find(const std::string& domain,
                                                   const std::string& name) const {
    const auto it = std::find_if(providers_.begin(), providers_.end(),
                                 [&](const ProviderInfo& p) {
                                     return p.name == name && p.domain == domain;
                                 });
    if (it == providers_.end()) return std::nullopt;
    return *it;
}

std::vector<ProviderInfo> ProviderRegistry::all() const { return providers_; }

} // namespace daisy::core
