#include "iris/controller/commands.hpp"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "iris/asr/asr.hpp"
#include "iris/catalog/models.hpp"
#include "iris/infra/i18n.hpp"
#include "iris/infra/process.hpp"
#include "iris/model/config.hpp"
#include "iris/tts/tts.hpp"
#include "iris/ui/console.hpp"

namespace iris::controller {

int run_doctor() {
    ui::banner("iris doctor", std::string("版本 ") + kVersion + " · 环境与模型自检");

    // ---- 外部工具 ----
    ui::section(_("External tools", "外部工具"));
    const auto ffmpeg = infra::run_process({"ffmpeg", "-version"});
    const bool ffmpeg_ok = ffmpeg.ok() && ffmpeg.value().success();
    if (ffmpeg_ok) {
        ui::success(_("ffmpeg available (media conversion)", "ffmpeg 可用（媒体转换）"));
    } else {
        ui::failure(_("ffmpeg missing — media conversion will fail",
                      "ffmpeg 缺失 —— 媒体转换不可用"));
    }

    // ---- ASR ----
    ui::section(_("ASR backends (in-process)", "ASR 后端（进程内）"));
    constexpr std::pair<const char*, asr::AsrBackend> kBackends[] = {
        {"whisper", asr::AsrBackend::WhisperGgml},
        {"qwen3-asr", asr::AsrBackend::Qwen3AsrGgml},
        {"whisper-ct2", asr::AsrBackend::WhisperCt2},
        {"whisper-onnx", asr::AsrBackend::WhisperOnnx},
        {"sensevoice-onnx", asr::AsrBackend::SenseVoiceOnnx},
        {"paraformer-onnx", asr::AsrBackend::ParaformerOnnx},
    };
    std::vector<std::vector<std::string>> asr_rows;
    for (const auto& [name, backend] : kBackends) {
        const bool ok = asr::asr_backend_available(backend);
        asr_rows.push_back({ok ? ui::symbols().check : ui::symbols().ring, name,
                            ok ? _("ready", "就绪") : _("not compiled", "未编译")});
    }
    ui::table({_("", ""), _("engine", "引擎"), _("status", "状态")}, asr_rows);

    // ---- TTS ----
    ui::section(_("TTS backends (in-process)", "TTS 后端（进程内）"));
    constexpr std::pair<const char*, tts::TtsEngine> kTtsEngines[] = {
        {"qwen3-tts", tts::TtsEngine::Qwen3Tts},
        {"indextts", tts::TtsEngine::IndexTts},
    };
    std::vector<std::vector<std::string>> tts_rows;
    for (const auto& [name, engine] : kTtsEngines) {
        const bool ok = tts::tts_engine_available(engine);
        tts_rows.push_back({ok ? ui::symbols().check : ui::symbols().ring, name,
                            ok ? _("ready", "就绪") : _("not compiled", "未编译")});
    }
    ui::table({_("", ""), _("engine", "引擎"), _("status", "状态")}, tts_rows);

    // ---- 配置 ----
    ui::section(_("Configuration", "配置"));
    const auto& cfg = model::active_config();
    ui::kv_table({{_("config file", "配置文件"), model::active_config_path()},
                  {_("tools dir", "工具目录"), cfg.tools_dir},
                  {_("models dir", "模型目录"), cfg.models_dir},
                  {_("mirror", "镜像"),
                   cfg.hf_mirror.empty() ? _("(none)", "（无）") : cfg.hf_mirror},
                  {_("download proxy", "下载代理"),
                   cfg.download_proxy.empty() ? _("(none)", "（无）") : cfg.download_proxy}});

    // ---- 模型 ----
    ui::section(_("Models", "模型"));
    int total_ok = 0;
    int total = 0;
    std::vector<std::vector<std::string>> rows;
    for (const auto& category : catalog::categories()) {
        int ok = 0;
        int count = 0;
        std::string miss;
        for (const auto* entry : catalog::by_category(category)) {
            ++count;
            ++total;
            if (catalog::installed(*entry, cfg.models_dir)) {
                ++ok;
                ++total_ok;
            } else if (!entry->files.empty()) {
                miss += (miss.empty() ? "" : ", ") + entry->key;
            }
        }
        const bool all = ok == count;
        rows.push_back({all ? ui::symbols().check : ui::symbols().dot, category,
                        std::to_string(ok) + "/" + std::to_string(count), miss});
    }
    ui::table({_("", ""), _("category", "类别"), _("installed", "已安装"), _("missing", "缺失")}, rows);

    std::cout << '\n';
    if (total_ok == total) {
        ui::success(_("all models present", "全部模型已就绪"));
    } else {
        ui::note(_("install with: iris models install --all    (or --category <name>)",
                   "安装：iris models install --all    （或 --category <类别>）"));
    }
    ui::finish(true, std::to_string(total_ok) + "/" + std::to_string(total) +
                         _(" models ready", " 个模型就绪"));
    return 0;
}

} // namespace iris::controller
