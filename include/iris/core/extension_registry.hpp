#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace iris::core {

/// Type-safe registry for statically linked extensions and their command-line aliases.
template <typename Extension>
class ExtensionRegistry {
public:
    struct Entry {
        std::string id;
        std::vector<std::string> aliases;
        Extension extension;
    };

    bool register_extension(std::string id, std::vector<std::string> aliases,
                            Extension extension, std::string& error) {
        error.clear();
        if (id.empty()) {
            error = "extension id must not be empty";
            return false;
        }

        std::vector<std::string_view> names;
        names.reserve(aliases.size() + 1);
        names.push_back(id);
        for (const auto& alias : aliases) {
            if (alias.empty()) {
                error = "extension alias must not be empty";
                return false;
            }
            names.push_back(alias);
        }

        for (std::size_t i = 0; i < names.size(); ++i) {
            for (std::size_t j = 0; j < i; ++j) {
                if (names[i] == names[j]) {
                    error = "duplicate extension id or alias: " + std::string(names[i]);
                    return false;
                }
            }
            if (find(names[i]) != nullptr) {
                error = "extension id or alias already registered: " +
                        std::string(names[i]);
                return false;
            }
        }

        entries_.push_back({std::move(id), std::move(aliases), std::move(extension)});
        return true;
    }

    [[nodiscard]] const Entry* find(std::string_view id_or_alias) const {
        const auto it = std::find_if(entries_.begin(), entries_.end(),
                                     [&](const Entry& entry) {
                                         return entry.id == id_or_alias ||
                                                std::find(entry.aliases.begin(),
                                                          entry.aliases.end(),
                                                          id_or_alias) != entry.aliases.end();
                                     });
        return it == entries_.end() ? nullptr : &*it;
    }

    [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }

private:
    std::vector<Entry> entries_;
};

} // namespace iris::core
