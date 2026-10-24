#include "iris/controller/commands.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "iris/catalog/models.hpp"
#include "iris/infra/i18n.hpp"
#include "iris/infra/log.hpp"
#include "iris/infra/process.hpp"

namespace iris::controller {

namespace fs = std::filesystem;

namespace {

std::string human_size(std::int64_t bytes) {
    if (bytes <= 0) return "-";
    char buf[32];
    if (bytes >= 1024LL * 1024 * 1024) {
        std::snprintf(buf, sizeof buf, "%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
    } else {
        std::snprintf(buf, sizeof buf, "%.0f MB", static_cast<double>(bytes) / (1024.0 * 1024));
    }
    return buf;
}

/// curl 下载到临时文件，再原子改名；已存在且非空则跳过。
bool download_one(const std::string& url, const std::string& target, std::string& error) {
    std::error_code ec;
    if (fs::exists(target, ec) && fs::file_size(target, ec) > 0) {
        spdlog::info("models: 已存在，跳过 {}", target);
        return true;
    }
    fs::create_directories(fs::path(target).parent_path(), ec);

    const std::string tmp = target + ".part";
    const auto r = infra::run_process({"curl", "-L", "--fail", "--retry", "3", "--silent",
                                       "--show-error", "-o", tmp, url});
    if (!r.ok() || !r.value().success()) {
        std::cerr << _("models: download failed: ", "models: 下载失败：") << url << '\n';
        if (r.ok()) std::cerr << r.value().stderr_text << '\n';
        std::error_code ignore;
        fs::remove(tmp, ignore);
        error = "下载失败";
        return false;
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        error = "无法写入 " + target;
        return false;
    }
    spdlog::info("models: 已下载 {}（{}）", target,
                 human_size(static_cast<std::int64_t>(fs::file_size(target, ec))));
    return true;
}

} // namespace

int run_models_list(const std::string& models_root) {
    for (const auto& category : catalog::categories()) {
        std::cout << "[" << category << "]\n";
        for (const auto* entry : catalog::by_category(category)) {
            const bool ok = catalog::installed(*entry, models_root);
            std::cout << "  " << (ok ? "✓" : "·") << " " << entry->key << "  " << entry->name
                      << "  (" << human_size(entry->bytes) << ")"
                      << (entry->files.empty() ? _("  [manual]", "  [需手动准备]") : "") << '\n';
        }
    }
    return 0;
}

int run_models_install(const std::vector<std::string>& keys, const std::string& models_root,
                       const std::string& mirror, bool all, const std::string& category,
                       bool dry_run) {
    std::vector<const catalog::ModelEntry*> targets;

    if (all) {
        for (const auto& entry : catalog::all()) targets.push_back(&entry);
    } else if (!category.empty()) {
        targets = catalog::by_category(category);
        if (targets.empty()) {
            std::cerr << _("models: unknown category: ", "models: 未知类别：") << category << '\n';
            return 1;
        }
    } else {
        if (keys.empty()) {
            std::cerr << _("models: give model keys, --all, or --category\n",
                           "models: 请给出模型 key，或使用 --all / --category\n");
            return 1;
        }
        for (const auto& key : keys) {
            const auto* entry = catalog::find(key);
            if (entry == nullptr) {
                std::cerr << _("models: unknown model: ", "models: 未知模型：") << key << '\n';
                return 1;
            }
            targets.push_back(entry);
        }
    }

    int installed_count = 0;
    int skipped_manual = 0;
    for (const auto* entry : targets) {
        const bool downloadable =
            std::any_of(entry->files.begin(), entry->files.end(),
                        [](const catalog::ModelFile& f) { return !f.url.empty(); });
        if (entry->files.empty() || !downloadable) {
            ++skipped_manual;
            if (catalog::installed(*entry, models_root)) {
                std::cout << _("ok: ", "已就绪：") << entry->key << _(" (manual)", "（手动准备）") << '\n';
            } else {
                std::cout << _("skip (manual): ", "跳过（需手动准备）：") << entry->key << " — "
                          << entry->note << '\n';
            }
            continue;
        }
        if (catalog::installed(*entry, models_root)) {
            std::cout << _("ok: ", "已就绪：") << entry->key << '\n';
            ++installed_count;
            continue;
        }
        if (dry_run) {
            std::cout << _("would install: ", "将安装：") << entry->key << " ("
                      << human_size(entry->bytes) << ") → " << catalog::target_dir(*entry, models_root)
                      << '\n';
            continue;
        }

        std::cout << _("installing: ", "安装：") << entry->key << " (" << human_size(entry->bytes)
                  << ")\n";
        bool ok = true;
        std::string error;
        for (const auto& file : entry->files) {
            const std::string url = catalog::rewrite_url(file.url, mirror);
            const std::string target = catalog::target_path(*entry, file, models_root);
            if (!download_one(url, target, error)) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            std::cerr << _("models: failed: ", "models: 失败：") << entry->key << '\n';
            return 1;
        }
        ++installed_count;
    }

    std::cout << _("models: ready ", "models: 就绪 ") << installed_count << _(" model(s)", " 个模型")
              << (skipped_manual > 0 ? _(", manual ", "，需手动 ") + std::to_string(skipped_manual)
                                     : std::string())
              << '\n';
    return 0;
}

} // namespace iris::controller
