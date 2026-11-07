#include "iris/asr/align.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <filesystem>
#include <limits>
#include <system_error>
#include <vector>

#include "iris/infra/log.hpp"
#include "iris/infra/paths.hpp"
#include "iris/infra/process.hpp"
#include "iris/tts/tts.hpp"

#if IRIS_HAS_CRISPASR
#include "crispasr_session.h"
#endif

namespace iris::asr {

namespace fs = std::filesystem;

namespace {

std::string temp_wav_path() {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return (fs::temp_directory_path() /
            ("iris-align-" + std::to_string(static_cast<long long>(ticks)) + ".wav"))
        .string();
}

} // namespace

AudioRange sentence_range(std::int64_t start_ms, std::int64_t end_ms, std::int64_t total_ms,
                          double pad_seconds) {
    AudioRange range;
    const double pad = std::max(0.0, pad_seconds);
    range.start = std::max(0.0, static_cast<double>(start_ms) / 1000.0 - pad);
    range.end = std::min(static_cast<double>(total_ms) / 1000.0,
                         static_cast<double>(end_ms) / 1000.0 + pad);
    if (range.end < range.start) range.end = range.start;
    return range;
}

void repair_collapsed_words(std::vector<model::Word>& words, double range_start,
                            double range_end) {
    std::size_t i = 0;
    while (i < words.size()) {
        if (std::isfinite(words[i].start) && std::isfinite(words[i].end) &&
            words[i].end > words[i].start) {
            ++i;
            continue;
        }
        const std::size_t begin = i;
        while (i < words.size() &&
               (!std::isfinite(words[i].start) || !std::isfinite(words[i].end) ||
                words[i].end <= words[i].start))
            ++i;
        const double left = begin == 0 ? range_start : words[begin - 1].end;
        const double next = i == words.size() ? range_end : words[i].start;
        const double right = std::max(next, left + 0.08 * static_cast<double>(i - begin));
        const double step = (right - left) / static_cast<double>(i - begin);
        for (std::size_t k = begin; k < i; ++k) {
            words[k].start = left + step * static_cast<double>(k - begin);
            words[k].end = left + step * static_cast<double>(k - begin + 1);
        }
    }
}

bool apply_word_timings_to_sentence(model::Sentence& sentence, std::int64_t total_ms) {
    double start = std::numeric_limits<double>::infinity();
    double end = 0.0;
    std::size_t valid_words = 0;
    for (const auto& word : sentence.words) {
        if (!std::isfinite(word.start) || !std::isfinite(word.end) || word.start < 0.0 ||
            word.end <= word.start) {
            continue;
        }
        ++valid_words;
        start = std::min(start, word.start);
        end = std::max(end, word.end);
    }
    if (!std::isfinite(start) || end <= start ||
        valid_words * 5 < sentence.words.size() * 3) {
        return false;
    }

    std::size_t text_characters = 0;
    for (const unsigned char ch : sentence.text) {
        if ((ch & 0xC0) != 0x80) ++text_characters;
    }
    if (end - start < std::max(0.1, static_cast<double>(text_characters) / 50.0)) return false;

    const auto start_ms = static_cast<std::int64_t>(std::llround(start * 1000.0));
    const auto end_ms = static_cast<std::int64_t>(std::llround(end * 1000.0));
    sentence.start_ms = std::clamp<std::int64_t>(start_ms, 0, total_ms);
    sentence.end_ms = std::clamp<std::int64_t>(end_ms, sentence.start_ms, total_ms);
    return sentence.end_ms > sentence.start_ms;
}

bool decode_media_16k(const std::string& media, std::vector<float>& pcm, std::string& error) {
    if (!fs::exists(infra::utf8_path(media))) {
        error = "媒体文件不存在：" + media;
        return false;
    }
    const std::string wav = temp_wav_path();
    const auto result = infra::run_process_split(
        {"ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", media, "-vn", "-ac", "1",
         "-ar", "16000", "-c:a", "pcm_s16le", wav});
    if (result.failed()) {
        error = "ffmpeg 启动失败：" + result.error();
        return false;
    }
    if (result.value().exit_code != 0) {
        error = "ffmpeg 转码失败：" + result.value().stderr_text;
        return false;
    }

    int rate = 0;
    std::string wav_error;
    bool ok = tts::read_wav(wav, pcm, rate, wav_error);
    std::error_code ec;
    fs::remove(infra::utf8_path(wav), ec);
    if (!ok) {
        error = "读取 16 kHz WAV 失败：" + wav_error;
        return false;
    }
    if (rate != 16000) {
        error = "音频采样率不是 16 kHz（" + std::to_string(rate) + "）";
        return false;
    }
    return true;
}

bool align_track(model::Track& track, const std::vector<float>& pcm, int sample_rate,
                 const AlignOptions& options, AlignStats& stats, std::string& error) {
    if (sample_rate <= 0) {
        error = "音频为空";
        return false;
    }
    stats.audio_seconds = static_cast<double>(pcm.size()) / static_cast<double>(sample_rate);
    const std::int64_t total_ms =
        static_cast<std::int64_t>(pcm.size()) * 1000 / static_cast<std::int64_t>(sample_rate);

    if (options.dry_run) {
        for (const auto& s : track.sentences) {
            if (s.comment || s.text.empty()) continue;
            ++stats.sentences;
        }
        return true;
    }

#if !IRIS_HAS_CRISPASR
    (void)pcm;
    (void)options;
    error = "当前构建没有 CrispASR 运行时，无法运行强制对齐";
    return false;
#else
    if (options.model.empty()) {
        error = "缺少对齐模型（--model qwen3-forced-aligner-0.6b-q4_k.gguf）";
        return false;
    }

    int done = 0;
    for (auto& s : track.sentences) {
        if (s.comment || s.text.empty()) continue;
        ++stats.sentences;

        const AudioRange range = sentence_range(s.start_ms, s.end_ms, total_ms, options.pad_seconds);
        int begin = static_cast<int>(range.start * sample_rate);
        int end = static_cast<int>(range.end * sample_rate);
        begin = std::clamp(begin, 0, static_cast<int>(pcm.size()));
        end = std::clamp(end, begin, static_cast<int>(pcm.size()));
        if (end <= begin) {
            ++stats.failed;
            continue;
        }
        const std::int64_t offset_cs =
            static_cast<std::int64_t>(std::llround(range.start * 100.0));

        crispasr_align_result* result = crispasr_align_words_abi(
            options.model.c_str(), s.text.c_str(), pcm.data() + begin, end - begin, offset_cs,
            options.threads);
        if (result == nullptr) {
            if (stats.aligned == 0 && stats.failed == 0) {
                error = "对齐失败：模型无法加载或音频无法对齐（" + options.model + "）";
                return false;
            }
            ++stats.failed;
            continue;
        }

        const int n = crispasr_align_result_n_words(result);
        std::vector<model::Word> words;
        words.reserve(static_cast<std::size_t>(std::max(n, 0)));
        for (int i = 0; i < n; ++i) {
            const char* text = crispasr_align_result_word_text(result, i);
            if (text == nullptr || *text == '\0') continue;
            model::Word w;
            w.text = text;
            w.start = static_cast<double>(crispasr_align_result_word_t0(result, i)) / 100.0;
            w.end = static_cast<double>(crispasr_align_result_word_t1(result, i)) / 100.0;
            w.score = 1.0;
            if (w.end < w.start) std::swap(w.start, w.end);
            words.push_back(std::move(w));
        }
        crispasr_align_result_free(result);

        if (words.empty()) {
            ++stats.failed;
        } else {
            repair_collapsed_words(words, range.start, range.end);
            stats.tokens += static_cast<int>(words.size());
            s.words = std::move(words);
            if (options.apply_sentence_timings && !apply_word_timings_to_sentence(s, total_ms)) {
                ++stats.failed;
                ++done;
                continue;
            }
            ++stats.aligned;
        }

        ++done;
        if (done % 25 == 0)
            spdlog::info("align: {}/{} 句（{} tok）", stats.aligned, stats.sentences, stats.tokens);
    }
    return true;
#endif
}

} // namespace iris::asr
