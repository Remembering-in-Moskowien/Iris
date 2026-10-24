#include "iris/controller/commands.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <vector>

#include "iris/catalog/models.hpp"
#include "iris/infra/i18n.hpp"
#include "iris/infra/log.hpp"
#include "iris/infra/process.hpp"
#include "iris/ui/console.hpp"
#include <nlohmann/json.hpp>

namespace iris::controller {

namespace fs = std::filesystem;

namespace {

/// curl 下载到临时文件，再原子改名；已存在且非空则跳过。
/// 下载期间轮询 .part 大小刷新进度条（速度/ETA 由 ui::ProgressBar 计算）。
bool download_one(const std::string& url, const std::string& target, std::string& error) {
    std::error_code ec;
    if (fs::exists(target, ec) && fs::file_size(target, ec) > 0) {
        ui::note(_("already present: ", "已存在：") + target);
        return true;
    }
    fs::create_directories(fs::path(target).parent_path(), ec);

    const std::string tmp = target + ".part";
    std::error_code ignore;
    fs::remove(tmp, ignore);

    auto fut = std::async(std::launch::async, [&] {
        return infra::run_process({"curl", "-L", "--fail", "--retry", "3", "--silent",
                                   "--show-error", "-o", tmp, url});
    });

    ui::ProgressBar bar(fs::path(target).filename().string());
    bar.begin(0);
    while (fut.wait_for(std::chrono::milliseconds(150)) != std::future_status::ready) {
        std::error_code size_ec;
        const auto sz = fs::file_size(tmp, size_ec);
        if (!size_ec) bar.update(static_cast<std::int64_t>(sz));
    }
    bar.finish();
    const auto r = fut.get();

    if (!r.ok() || !r.value().success()) {
        fs::remove(tmp, ignore);
        error = "下载失败：" + url;
        if (r.ok() && !r.value().stderr_text.empty()) error += "\n    " + r.value().stderr_text;
        return false;
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        error = "无法写入 " + target;
        return false;
    }
    const auto size = static_cast<std::int64_t>(fs::file_size(target, ec));
    ui::success(ui::dim(target) + "  " + ui::green(ui::human_size(size)));
    return true;
}

} // namespace

int run_models_list(const std::string& models_root) {
    if (ui::json_mode()) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& entry : catalog::all()) {
            arr.push_back({{"key", entry.key},
                           {"category", entry.category},
                           {"name", entry.name},
                           {"bytes", entry.bytes},
                           {"installed", catalog::installed(entry, models_root)}});
        }
        ui::json_out(nlohmann::json({{"ok", true},
                                     {"command", "models list"},
                                     {"modelsDir", models_root},
                                     {"models", arr}})
                         .dump(2));
        return 0;
    }
    ui::banner("iris models", _("installed models under ", "模型目录 ") + models_root);
    for (const auto& category : catalog::categories()) {
        ui::section(category);
        std::vector<std::vector<std::string>> rows;
        for (const auto* entry : catalog::by_category(category)) {
            const bool ok = catalog::installed(*entry, models_root);
            const bool manual =
                std::none_of(entry->files.begin(), entry->files.end(),
                             [](const catalog::ModelFile& f) { return !f.url.empty(); });
            rows.push_back({ok ? ui::green(ui::symbols().check) : ui::dim(ui::symbols().ring),
                            entry->key, entry->name, ui::human_size(entry->bytes),
                            ok ? _("ready", "就绪")
                               : (manual ? _("manual", "需手动") : _("missing", "缺失"))});
        }
        ui::table({_("", ""), _("key", "标识"), _("name", "名称"), _("size", "体积"),
                   _("status", "状态")},
                  rows);
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
            ui::failure(_("unknown category: ", "未知类别：") + category);
            return 1;
        }
    } else {
        if (keys.empty()) {
            ui::failure(_("give model keys, or use --all / --category",
                          "请给出模型 key，或使用 --all / --category"));
            return 1;
        }
        for (const auto& key : keys) {
            const auto* entry = catalog::find(key);
            if (entry == nullptr) {
                ui::failure(_("unknown model: ", "未知模型：") + key);
                return 1;
            }
            targets.push_back(entry);
        }
    }

    if (!mirror.empty()) ui::note(_("mirror: ", "镜像：") + mirror);
    ui::banner("iris models install", _("target: ", "目标目录：") + models_root + _(", ", "，") +
                                        std::to_string(targets.size()) + _(" model(s)", " 个模型"));

    int ready = 0;
    int manual = 0;
    for (const auto* entry : targets) {
        const bool downloadable =
            std::any_of(entry->files.begin(), entry->files.end(),
                        [](const catalog::ModelFile& f) { return !f.url.empty(); });
        if (entry->files.empty() || !downloadable) {
            ++manual;
            if (catalog::installed(*entry, models_root)) {
                ui::success(entry->key + _(" (manual, present)", "（手动准备，已就绪）"));
            } else {
                ui::warning(entry->key + _(" needs manual preparation — ", " 需手动准备 —— ") +
                            entry->note);
            }
            continue;
        }
        if (catalog::installed(*entry, models_root)) {
            ui::success(entry->key + _(" already installed", " 已安装"));
            ++ready;
            continue;
        }
        if (dry_run) {
            ui::step(entry->key + "  " + ui::human_size(entry->bytes) + "  →  " +
                     catalog::target_dir(*entry, models_root));
            continue;
        }

        ui::step(entry->key + "  " + ui::dim(ui::human_size(entry->bytes)));
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
            ui::failure(entry->key + ": " + error);
            return 1;
        }
        ++ready;
    }

    ui::finish(true, std::to_string(ready) + _(" model(s) ready", " 个模型就绪") +
                         (manual > 0 ? _(", ", "，") + std::to_string(manual) +
                                           _(" manual", " 个需手动")
                                     : std::string()));
    return 0;
}

} // namespace iris::controller
