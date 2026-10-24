#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "iris/asr/diarize.hpp"
#include "iris/asr/onnx_funasr.hpp"
#include "iris/catalog/models.hpp"
#include "iris/server/api.hpp"
#include "iris/server/http_server.hpp"
#include "iris/ocr/rapid_ocr.hpp"
#include "iris/quality/report.hpp"
#include "iris/ops/dub_tts.hpp"
#include "iris/ops/sentence_split.hpp"
#include "iris/subtitle/combine.hpp"
#include "iris/subtitle/parser.hpp"
#include "iris/text/cleanup.hpp"
#include "iris/text/sat.hpp"
#include "iris/model/ir_document.hpp"
#include "iris/mt/translator.hpp"
#include "iris/tts/gguf.hpp"
#include "iris/tts/kernels.hpp"
#include "iris/tts/talker.hpp"
#include "iris/tts/tensor.hpp"
#include "iris/tts/timestretch.hpp"
#include "iris/tts/tts.hpp"

using iris::tts::TtsEngine;

namespace {

std::string temp_wav(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

} // namespace

TEST_CASE("TTS engine names and aliases") {
    TtsEngine e{};
    REQUIRE(iris::tts::parse_tts_engine("qwen3-tts", e));
    REQUIRE(e == TtsEngine::Qwen3Tts);
    REQUIRE(iris::tts::parse_tts_engine("qwen3tts", e));
    REQUIRE(e == TtsEngine::Qwen3Tts);
    REQUIRE(iris::tts::parse_tts_engine("qwen3", e));
    REQUIRE(e == TtsEngine::Qwen3Tts);
    REQUIRE(iris::tts::parse_tts_engine("indextts", e));
    REQUIRE(e == TtsEngine::IndexTts);
    REQUIRE(iris::tts::parse_tts_engine("index-tts", e));
    REQUIRE(e == TtsEngine::IndexTts);

    REQUIRE_FALSE(iris::tts::parse_tts_engine("whisper", e));
    REQUIRE_FALSE(iris::tts::parse_tts_engine("", e));

    REQUIRE(std::string(iris::tts::tts_engine_name(TtsEngine::Qwen3Tts)) == "qwen3-tts");
    REQUIRE(std::string(iris::tts::tts_engine_name(TtsEngine::IndexTts)) == "indextts");
}

TEST_CASE("WAV write/read round-trip") {
    const std::string path = temp_wav("iris_tts_roundtrip.wav");
    std::vector<float> pcm(1600);
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 16000.0f);
    }

    std::string error;
    REQUIRE(iris::tts::write_wav(path, pcm, 16000, error));

    std::vector<float> back;
    int rate = 0;
    REQUIRE(iris::tts::read_wav(path, back, rate, error));
    REQUIRE(rate == 16000);
    REQUIRE(back.size() == pcm.size());
    // 16-bit 量化误差上限约 1/32768。
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        REQUIRE(std::abs(back[i] - pcm[i]) < 1e-3f);
    }
    std::filesystem::remove(path);
}

TEST_CASE("WAV read rejects non-WAV input") {
    const std::string path = temp_wav("iris_tts_bogus.wav");
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        std::fputs("not a wav file at all", f);
        std::fclose(f);
    }
    std::vector<float> pcm;
    int rate = 0;
    std::string error;
    REQUIRE_FALSE(iris::tts::read_wav(path, pcm, rate, error));
    REQUIRE_FALSE(error.empty());
    std::filesystem::remove(path);
}

TEST_CASE("trim_silence strips edge silence but keeps internal pauses") {
    constexpr int kRate = 1000; // 1 采样 = 1 ms，便于算长度
    std::vector<float> pcm(1000, 0.0f);
    for (int i = 200; i < 300; ++i) pcm[i] = 0.5f; // 第一段语音
    for (int i = 500; i < 600; ++i) pcm[i] = 0.5f; // 第二段语音（300..500 是句内停顿）

    iris::tts::trim_silence(pcm, kRate, 0.003f, 20);

    // 保留 180..620：两端各 20ms 余量，中间 200ms 停顿原样保留
    REQUIRE(pcm.size() == 440);
    REQUIRE(std::abs(pcm.front()) < 1e-6f);
    REQUIRE(std::abs(pcm.back()) < 1e-6f);

    // 整段静音 → 清空
    std::vector<float> silent(500, 0.0f);
    iris::tts::trim_silence(silent, kRate);
    REQUIRE(silent.empty());
}

TEST_CASE("WSOLA stretch keeps duration and waveform sane") {
    constexpr int kRate = 24000;
    std::vector<float> pcm(kRate); // 1 s / 440 Hz
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / kRate);
    }

    std::vector<float> out;
    std::string error;

    // 1.0x 原样返回
    REQUIRE(iris::tts::wsola_stretch(pcm, kRate, 1.0f, out, error));
    REQUIRE(out.size() == pcm.size());

    // 1.3x：时长按比例缩短
    REQUIRE(iris::tts::wsola_stretch(pcm, kRate, 1.3f, out, error));
    REQUIRE(std::abs(static_cast<long long>(out.size()) -
                     static_cast<long long>(pcm.size() / 1.3f)) <= 2);

    // 输出必须是有限值、无长串静音（权重空洞会被上一样本填住）
    std::size_t zeros = 0;
    for (float v : out) {
        REQUIRE(std::isfinite(v));
        REQUIRE(std::abs(v) <= 1.0f);
        if (std::abs(v) < 1e-6f) ++zeros;
    }
    REQUIRE(zeros < out.size() / 50);

    // 非法采样率要报错
    REQUIRE_FALSE(iris::tts::wsola_stretch(pcm, 0, 1.3f, out, error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("GGUF parser reads the qwen3-tts talker weights", "[gguf]") {
    const std::string path = "models/qwen3-tts/qwen3-tts-12hz-0.6b-base-q8_0.gguf";
    if (!std::filesystem::exists(path)) {
        WARN("跳过：本地没有 " << path);
        return;
    }
    iris::tts::gguf::File f;
    std::string error;
    REQUIRE(iris::tts::gguf::load(path, f, error));
    REQUIRE(error.empty());
    REQUIRE(f.version >= 2);
    REQUIRE(f.get_string("general.architecture") == "qwen3tts");
    REQUIRE(f.tensor_count == f.tensors.size());
    REQUIRE(f.tensor_count > 100);
    // 权重要能定位到 talker 主体（自研运行时按名前缀取张量）。
    REQUIRE_FALSE(f.find_tensors("talker").empty());
    REQUIRE(std::string(iris::tts::gguf::type_name(8)) == "Q8_0");
}

TEST_CASE("half and Q8_0 dequant match hand-computed values", "[tensor]") {
    // IEEE 半精度：1.0 / 0.5 / -2.0
    REQUIRE(iris::tts::f16_to_f32(0x3C00) == 1.0f);
    REQUIRE(iris::tts::f16_to_f32(0x3800) == 0.5f);
    REQUIRE(iris::tts::f16_to_f32(0xC000) == -2.0f);
    REQUIRE(iris::tts::f16_to_f32(0x0000) == 0.0f);

    // 块大小与字节数要和 ggml 对齐
    REQUIRE(iris::tts::gguf::block_elems(8) == 32);
    REQUIRE(iris::tts::gguf::block_bytes(8) == 34);
    REQUIRE(iris::tts::gguf::block_elems(12) == 256);
    REQUIRE(iris::tts::gguf::block_bytes(12) == 144);

    // Q8_0 块：f16 尺度 0.5 + 32 个 int8
    std::vector<std::uint8_t> block(34, 0);
    const std::uint16_t scale = 0x3800; // 0.5
    std::memcpy(block.data(), &scale, 2);
    for (int i = 0; i < 32; ++i) {
        block[2 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(static_cast<std::int8_t>(i));
    }
    std::vector<float> out;
    std::string error;
    REQUIRE(iris::tts::dequantize(block.data(), block.size(), 8, 32, out, error));
    REQUIRE(out.size() == 32);
    for (int i = 0; i < 32; ++i) REQUIRE(out[static_cast<std::size_t>(i)] == 0.5f * i);

    // 截断的数据必须报错
    REQUIRE_FALSE(iris::tts::dequantize(block.data(), 10, 8, 32, out, error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("WeightFile loads real talker tensors", "[tensor]") {
    const std::string path = "models/qwen3-tts/qwen3-tts-12hz-0.6b-base-q8_0.gguf";
    if (!std::filesystem::exists(path)) {
        WARN("跳过：本地没有 " << path);
        return;
    }
    iris::tts::WeightFile weights;
    std::string error;
    REQUIRE(weights.open(path, error));

    // F32 RMSNorm 权重：[1024]
    iris::tts::Tensor norm;
    REQUIRE(weights.load("talker.blk.0.attn_norm.weight", norm, error));
    REQUIRE(norm.dims.size() == 1);
    REQUIRE(norm.cols() == 1024);
    REQUIRE(norm.data.size() == 1024);
    double mean = 0.0;
    double energy = 0.0;
    for (float v : norm.data) {
        mean += v;
        energy += static_cast<double>(v) * v;
    }
    mean /= static_cast<double>(norm.data.size());
    // 训练后的 RMSNorm 权重不是恒等，但应当是非零、量级合理的一维权重。
    REQUIRE(std::abs(mean) > 1e-3);
    REQUIRE(std::abs(mean) < 5.0);
    REQUIRE(energy > 0.0);

    // Q8_0 注意力投影：[1024(进), 2048(出)] → 16 heads × 128
    iris::tts::Tensor q;
    REQUIRE(weights.load("talker.blk.0.attn_q.weight", q, error));
    REQUIRE(q.dims.size() == 2);
    REQUIRE(q.cols() == 1024);
    REQUIRE(q.rows() == 2048);
    REQUIRE(q.data.size() == 2048u * 1024u);
    double q_energy = 0.0;
    for (float v : q.data) q_energy += static_cast<double>(v) * v;
    REQUIRE(q_energy > 0.0); // 不是全零

    // 不存在的张量要报错
    iris::tts::Tensor missing;
    REQUIRE_FALSE(weights.load("talker.blk.999.attn_q.weight", missing, error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("runtime kernels: matvec / rmsnorm / softmax / rope", "[kernels]") {
    SECTION("matvec") {
        // 2×3 行主序：[[1,2,3],[4,5,6]] · [1,1,1] = [6,15]
        const float w[] = {1, 2, 3, 4, 5, 6};
        const float x[] = {1, 1, 1};
        float y[2] = {0, 0};
        iris::tts::matvec(w, 2, 3, x, y);
        REQUIRE(y[0] == 6.0f);
        REQUIRE(y[1] == 15.0f);
    }

    SECTION("rmsnorm normalises to unit RMS") {
        const float x[] = {3.0f, 4.0f, 0.0f, 0.0f}; // RMS = sqrt(25/4) = 2.5
        const float weight[] = {1, 1, 1, 1};
        float out[4] = {};
        iris::tts::rmsnorm(out, x, weight, 4, 1e-6f);
        REQUIRE(std::abs(out[0] - 1.2f) < 1e-5f);
        REQUIRE(std::abs(out[1] - 1.6f) < 1e-5f);

        // 权重缩放
        const float weight2[] = {2, 2, 2, 2};
        float out2[4] = {};
        iris::tts::rmsnorm(out2, x, weight2, 4, 1e-6f);
        REQUIRE(std::abs(out2[0] - 2.4f) < 1e-5f);
    }

    SECTION("softmax sums to one") {
        float v[] = {1.0f, 2.0f, 3.0f};
        iris::tts::softmax_inplace(v, 3);
        float sum = 0.0f;
        for (float p : v) sum += p;
        REQUIRE(std::abs(sum - 1.0f) < 1e-6f);
        REQUIRE(v[2] > v[1]);
        REQUIRE(v[1] > v[0]);
    }

    SECTION("rope preserves each head's norm") {
        std::vector<float> vec(2 * 8);
        for (std::size_t i = 0; i < vec.size(); ++i) vec[i] = 0.1f * static_cast<float>(i + 1);
        auto before = vec;

        iris::tts::rope_interleaved(vec.data(), 2, 8, 13, 1e6f);

        for (int h = 0; h < 2; ++h) {
            double n0 = 0.0, n1 = 0.0;
            for (int i = 0; i < 8; ++i) {
                n0 += static_cast<double>(before[static_cast<std::size_t>(h * 8 + i)]) * before[static_cast<std::size_t>(h * 8 + i)];
                n1 += static_cast<double>(vec[static_cast<std::size_t>(h * 8 + i)]) * vec[static_cast<std::size_t>(h * 8 + i)];
            }
            REQUIRE(std::abs(n0 - n1) < 1e-4);
        }

        // pos = 0 时恒等
        auto identity = before;
        iris::tts::rope_interleaved(identity.data(), 2, 8, 0, 1e6f);
        for (std::size_t i = 0; i < identity.size(); ++i) {
            REQUIRE(std::abs(identity[i] - before[i]) < 1e-6f);
        }
    }
}

TEST_CASE("talker: incremental decode matches full prefill", "[talker]") {
    const std::string path = "models/qwen3-tts/qwen3-tts-12hz-0.6b-base-q8_0.gguf";
    if (!std::filesystem::exists(path)) {
        WARN("跳过：本地没有 " << path);
        return;
    }

    // 只加载 2 层、跳过 151936×2048 的文本词表，保证测试内存/时间可控。
    iris::tts::Talker talker;
    std::string error;
    REQUIRE(talker.load(path, error, /*max_layers=*/2, /*with_text_embedding=*/false));
    REQUIRE(talker.n_loaded_layers() == 2);
    REQUIRE(talker.config().d_model == 1024);
    REQUIRE(talker.config().n_heads == 16);
    REQUIRE(talker.config().n_kv_heads == 8);
    REQUIRE(talker.config().mrope == std::array<int, 3>{24, 20, 20});

    // 5 个音频 token，位置连续（三路位置相同 → 退化为普通 RoPE）
    std::vector<iris::tts::Talker::Item> items;
    for (int i = 0; i < 5; ++i) {
        iris::tts::Talker::Item it;
        it.is_text = false;
        it.id = 100 + i;
        it.pos = {i, i, i};
        items.push_back(it);
    }

    std::vector<float> prefill_logits;
    REQUIRE(talker.forward(items, prefill_logits, error));
    REQUIRE(prefill_logits.size() == static_cast<std::size_t>(talker.config().vocab));
    for (float v : prefill_logits) REQUIRE(std::isfinite(v));

    // 逐 token 增量解码：最后一步的 logits 必须和一次性预填充一致（KV cache 自洽）
    talker.reset();
    REQUIRE(talker.seq_len() == 0);
    std::vector<float> step_logits;
    for (const auto& it : items) {
        REQUIRE(talker.forward({it}, step_logits, error));
    }
    REQUIRE(talker.seq_len() == items.size());

    double max_diff = 0.0;
    for (std::size_t i = 0; i < prefill_logits.size(); ++i) {
        max_diff = std::max(max_diff,
                            std::abs(static_cast<double>(prefill_logits[i]) - step_logits[i]));
    }
    REQUIRE(max_diff < 1e-3);

    // 越界 token 要报错
    iris::tts::Talker::Item bad;
    bad.is_text = false;
    bad.id = talker.config().vocab + 5;
    REQUIRE_FALSE(talker.forward({bad}, step_logits, error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("talker: mRoPE sections split the rotation pairs", "[talker]") {
    // 三路位置不同时，同一向量的旋转结果必须与单路位置不同
    std::vector<float> a(8, 0.5f);
    std::vector<float> b = a;
    const int pos[3] = {0, 5, 11};
    const int sections[3] = {2, 1, 1}; // head_dim 8 → 4 对

    iris::tts::rope_mrope(a.data(), 1, 8, pos, sections, 1e4f);
    const int pos_same[3] = {0, 0, 0};
    iris::tts::rope_mrope(b.data(), 1, 8, pos_same, sections, 1e4f);

    // 第 0、1 对（i=0,1）落在第一段，用 pos[0]=0 → 与全 0 相同；
    // 第 2 对（i=2）落在第二段（pos 5）→ 一定不同。
    REQUIRE(std::abs(a[0] - b[0]) < 1e-6f);
    REQUIRE(std::abs(a[1] - b[1]) < 1e-6f);
    REQUIRE(std::abs(a[2] - b[2]) < 1e-6f);
    REQUIRE(std::abs(a[3] - b[3]) < 1e-6f);
    REQUIRE(std::abs(a[4] - b[4]) > 1e-6f);
}

TEST_CASE("translate: language codes and Chinese output normalisation", "[mt]") {
    REQUIRE(iris::mt::normalize_language("en-US") == "en");
    REQUIRE(iris::mt::normalize_language("zh-CN") == "zh");
    REQUIRE(iris::mt::normalize_language("cmn") == "zh");
    REQUIRE(iris::mt::normalize_language("EN") == "en");
    REQUIRE(iris::mt::normalize_language("ja") == "ja");

    // 半角标点 → 全角；汉字旁的空格去掉；数字间的小数点保留
    REQUIRE(iris::mt::normalize_output("你好,世界.", "zh") == "你好，世界。");
    REQUIRE(iris::mt::normalize_output("我 爱 北京", "zh") == "我爱北京");
    REQUIRE(iris::mt::normalize_output("增长 3.5 个百分点", "zh") == "增长3.5个百分点");
    REQUIRE(iris::mt::normalize_output("什么?", "zh") == "什么？");

    // 非中文目标语言：只折叠空格，标点保持原样
    REQUIRE(iris::mt::normalize_output("Hello,   world.", "en") == "Hello, world.");
}

TEST_CASE("diarization: midpoint speaker assignment", "[diarize]") {
    using iris::asr::SpeakerSpan;
    using iris::asr::speaker_at;

    const std::vector<SpeakerSpan> spans = {{0, 1000, 0}, {1000, 2000, 1}, {2000, 3000, 0}};
    REQUIRE(speaker_at(0, spans) == 0);
    REQUIRE(speaker_at(999, spans) == 0);
    REQUIRE(speaker_at(1000, spans) == 1);
    REQUIRE(speaker_at(2999, spans) == 0);
    REQUIRE(speaker_at(3000, spans) == 0); // 空隙取最近一段
    REQUIRE(speaker_at(-50, spans) == 0);
    REQUIRE(speaker_at(500, {}) == -1);

    REQUIRE(iris::asr::speaker_label(0) == "SPEAKER_0");
    REQUIRE(iris::asr::speaker_label(3) == "SPEAKER_3");
    REQUIRE(iris::asr::speaker_label(-1) == "SPEAKER_?");
}

TEST_CASE("funasr: fbank / LFR / CTC helpers", "[funasr]") {
    // 1 s 空白音频：snip_edges=false → (16000 + 80) / 160 = 100 帧 × 80 维
    std::vector<float> pcm(16000, 0.0f);
    const auto fbank = iris::asr::detail::kaldi_fbank80(pcm);
    REQUIRE(fbank.size() == 100u * 80u);

    // LFR(7,6)：(100 + 6 - 1) / 6 = 17 帧 × 560 维
    const auto features = iris::asr::detail::lfr(fbank, 7, 6);
    REQUIRE(features.size() == 17u * 560u);

    // CMVN 参数为空时原样返回
    auto copy = features;
    iris::asr::detail::apply_cmvn(copy, {}, {});
    REQUIRE(copy == features);

    // CTC 文本拼接：SenseVoice 走标签过滤 + ▁ 转空格；Paraformer 走 @@ 续接
    const std::vector<std::string> sv = {"<blank>", "你", "好", "▁world", "<|zh|>", "<|NEUTRAL|>"};
    REQUIRE(iris::asr::detail::ctc_greedy_text({1, 2}, sv, true) == "你好");
    REQUIRE(iris::asr::detail::ctc_greedy_text({3}, sv, true) == "world");
    REQUIRE(iris::asr::detail::ctc_greedy_text({1, 4, 5}, sv, true) == "你");
    REQUIRE(iris::asr::detail::ctc_greedy_text({}, sv, true).empty());

    const std::vector<std::string> pf = {"<blank>", "a@@", "b", "你", "</s>"};
    REQUIRE(iris::asr::detail::ctc_greedy_text({1, 2}, pf, false) == "ab");
    REQUIRE(iris::asr::detail::ctc_greedy_text({3, 4}, pf, false) == "你");
}

TEST_CASE("sentence split keeps UTF-8 characters intact", "[split]") {
    iris::model::IrDocument doc;
    doc.config.max_sentence_length = 5;

    iris::model::Sentence s;
    s.start_ms = 0;
    s.end_ms = 1000;
    s.text = "一二三四五六七八";
    iris::model::Track t;
    t.type = "transcribe";
    t.sentences.push_back(s);
    doc.state.tracks.push_back(t);

    iris::core::PipelineContext ctx;
    ctx.doc = &doc;
    iris::ops::SentenceSplit op;
    REQUIRE(op.execute(ctx));

    const auto& out = doc.state.tracks[0].sentences;
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].text == "一二三四五");
    REQUIRE(out[1].text == "六七八");
    // 每段都是合法 UTF-8（汉字 3 字节，长度应为 3 的倍数，且没有替换字符）
    for (const auto& sentence : out) {
        REQUIRE(sentence.text.size() % 3 == 0);
        REQUIRE(sentence.text.find("\xEF\xBF\xBD") == std::string::npos);
    }
}

TEST_CASE("correction: text cleanup and timing repair", "[correct]") {
    using iris::text::CleanupReport;
    using iris::text::clean_sentences;
    using iris::text::clean_text;

    // 空白 + 重复标点 + 半角标点
    REQUIRE(clean_text("  所以 我的 同胞们 ，，不要问  ") == "所以我的同胞们，不要问");
    REQUIRE(clean_text("你好。。。") == "你好。");
    REQUIRE(clean_text("等等！！？") == "等等？"); // 句末标点优先
    // 数字里的逗号/冒号保持半角
    REQUIRE(clean_text("1,000 和 12:30") == "1,000 和 12:30");

    // 时间码修正 / 重叠消解 / 空句丢弃
    std::vector<iris::model::Sentence> sents;
    {
        iris::model::Sentence s;
        s.start_ms = 0;
        s.end_ms = 1000;
        s.text = "  你好  ";
        sents.push_back(s);
        s.start_ms = 900; // 与上一句重叠
        s.end_ms = 2000;
        s.text = "重叠";
        sents.push_back(s);
        s.start_ms = 3000;
        s.end_ms = 3000; // 零长度
        s.text = "零长度";
        sents.push_back(s);
        s.start_ms = 4000;
        s.end_ms = 4100;
        s.text = "   "; // 清理后为空
        sents.push_back(s);
    }

    CleanupReport report;
    clean_sentences(sents, report);
    REQUIRE(sents.size() == 3);
    REQUIRE(sents[0].text == "你好");
    REQUIRE(sents[1].start_ms == 1000);
    REQUIRE(sents[2].end_ms == 3001);
    REQUIRE(report.dropped == 1);
    REQUIRE(report.timing_fixed == 1);
    REQUIRE(report.overlap_fixed == 1);
}

TEST_CASE("subtitle import: SRT/ASS parsing, dedupe and bilingual alignment", "[subtitle]") {
    using iris::subtitle::Cue;

    std::string error;
    std::vector<Cue> cues;
    const std::string srt =
        "1\n00:00:00,000 --> 00:00:02,500\nHello\n\n"
        "2\n00:00:02,500 --> 00:00:05,000\nWorld\nline two\n";
    REQUIRE(iris::subtitle::parse_srt(srt, cues, error));
    REQUIRE(cues.size() == 2);
    REQUIRE(cues[0].start_ms == 0);
    REQUIRE(cues[0].end_ms == 2500);
    REQUIRE(cues[0].text == "Hello");
    REQUIRE(cues[1].text == "World line two");

    const std::string ass =
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:02.50,Default,,0,0,0,,{\\b1}你好，世界\n";
    std::vector<Cue> acues;
    REQUIRE(iris::subtitle::parse_ass(ass, acues, error));
    REQUIRE(acues.size() == 1);
    REQUIRE(acues[0].end_ms == 2500);
    REQUIRE(acues[0].text == "你好，世界"); // 覆盖标签已剥掉

    // 去重：时间相近且文本相同只留一条
    std::vector<Cue> dup = {{0, 1000, "a"}, {0, 1000, "a"}, {1000, 2000, "b"}};
    iris::subtitle::CombineStats stats;
    REQUIRE(iris::subtitle::merge_cues(dup, 500, stats, error));
    REQUIRE(dup.size() == 2);
    REQUIRE(stats.duplicates == 1);

    // 双语对齐：按最大时间重叠配对
    std::vector<Cue> primary = {{0, 2000, "hello"}};
    const std::vector<Cue> second = {{0, 1900, "你好"}};
    iris::subtitle::align_translations(primary, second, 0.1, stats);
    REQUIRE(primary[0].translation == "你好");

    // 重叠过少则不对齐
    std::vector<Cue> loner = {{0, 1000, "x"}};
    const std::vector<Cue> far = {{5000, 6000, "y"}};
    iris::subtitle::align_translations(loner, far, 0.1, stats);
    REQUIRE(loner[0].translation.empty());
}

TEST_CASE("sat: XLM-R tokenizer and boundary mapping", "[sat]") {
    // 边界 → 切分偏移（纯函数，无模型）
    std::vector<iris::text::SatToken> tokens = {
        {"\xE2\x96\x81Hello", 1, 0, 5},
        {"\xE2\x96\x81world", 2, 6, 11},
        {"\xE2\x96\x81" "again", 3, 12, 17},
    };
    std::vector<float> probs = {0.1f, 0.9f, 0.1f};
    auto points = iris::text::SatSegmenter::boundaries_to_offsets(tokens, probs, 0.5f);
    REQUIRE(points.size() == 1);
    REQUIRE(points[0] == 12);

    // 全部低于阈值时不切
    probs = {0.1f, 0.2f, 0.1f};
    REQUIRE(iris::text::SatSegmenter::boundaries_to_offsets(tokens, probs, 0.5f).empty());

    // 分词器（需要本机有 XLM-R tokenizer.json，缺失则跳过）
    const std::string path = "models/sat/tokenizer.json";
    if (!std::filesystem::exists(path)) {
        SUCCEED("sat tokenizer not installed, skipping");
        return;
    }
    iris::text::SatTokenizer tokenizer;
    std::string error;
    REQUIRE(tokenizer.load(path, error));
    REQUIRE(tokenizer.vocab_size() > 250000u);

    const auto encoded = tokenizer.encode("Hello world. This is a test.");
    REQUIRE(!encoded.empty());
    // 首 token 一定带 metaspace 前缀，且从 0 开始
    REQUIRE(encoded.front().start == 0);
    REQUIRE(encoded.front().piece.rfind("\xE2\x96\x81", 0) == 0);
    for (const auto& t : encoded) {
        REQUIRE(t.id >= 0);
        REQUIRE(t.id != tokenizer.unk_id()); // 常见英文不应出现 unk
        REQUIRE(t.end >= t.start);
    }
}

TEST_CASE("serve: http request parsing helpers", "[serve]") {
    iris::server::HttpRequest req;
    std::string error;
    REQUIRE(iris::server::parse_request_head(
        "POST /jobs?x=1&y=a%20b HTTP/1.1\r\nContent-Length: 5\r\nContent-Type: application/json",
        req, error));
    REQUIRE(req.method == "POST");
    REQUIRE(req.path == "/jobs");
    REQUIRE(req.query.at("x") == "1");
    REQUIRE(req.query.at("y") == "a b");
    REQUIRE(req.headers.at("content-length") == "5");
    REQUIRE(req.headers.at("content-type") == "application/json");

    REQUIRE(iris::server::url_decode("a%2Fb+c") == "a/b c");
    REQUIRE(iris::server::url_decode("%E4%BD%A0") == "你");
    REQUIRE(iris::server::url_decode("100%") == "100%"); // 残缺百分号原样保留

    REQUIRE(iris::server::path_param("/jobs/{id}", "/jobs/job7") == "job7");
    REQUIRE(iris::server::path_param("/jobs/{id}", "/jobs/").empty());
    REQUIRE(iris::server::path_param("/jobs/{id}", "/models").empty());

    REQUIRE(iris::server::json_escape("a\"b\n") == "a\\\"b\\n");
    REQUIRE_FALSE(iris::server::parse_request_head("garbage", req, error));

    // 白名单：不含 serve 自身与未知命令
    const auto& allowed = iris::server::JobManager::allowed_commands();
    REQUIRE(std::find(allowed.begin(), allowed.end(), "asr") != allowed.end());
    REQUIRE(std::find(allowed.begin(), allowed.end(), "serve") == allowed.end());
}

TEST_CASE("catalog: model registry integrity and helpers", "[catalog]") {
    using namespace iris::catalog;

    std::vector<std::string> keys;
    for (const auto& entry : all()) {
        // key 唯一
        REQUIRE(std::find(keys.begin(), keys.end(), entry.key) == keys.end());
        keys.push_back(entry.key);
        REQUIRE(!entry.category.empty());
        REQUIRE(!entry.name.empty());
        // 可下载条目必须给 https 地址；手动条目允许空地址
        for (const auto& file : entry.files) {
            REQUIRE(!file.filename.empty());
            if (!file.url.empty()) REQUIRE(file.url.rfind("https://", 0) == 0);
        }
    }
    REQUIRE(keys.size() >= 15);

    // 查找
    REQUIRE(find("ppocrv5") != nullptr);
    REQUIRE(find("nope") == nullptr);
    REQUIRE(by_category("ocr").size() == 1);
    REQUIRE(!categories().empty());

    // 镜像重写
    REQUIRE(rewrite_url("https://huggingface.co/a/b/resolve/main/x.gguf", "https://hf-mirror.com") ==
            "https://hf-mirror.com/a/b/resolve/main/x.gguf");
    REQUIRE(rewrite_url("https://huggingface.co/a/b", "https://hf-mirror.com/") ==
            "https://hf-mirror.com/a/b");
    REQUIRE(rewrite_url("https://example.com/x", "https://hf-mirror.com") == "https://example.com/x");
    REQUIRE(rewrite_url("https://huggingface.co/a/b", "") == "https://huggingface.co/a/b");

    // 落盘路径与安装检测
    const auto* entry = find("ppocrv5");
    REQUIRE(entry != nullptr);
    REQUIRE(target_dir(*entry, "models") ==
            (std::filesystem::path("models") / "ocr").string());
    REQUIRE(target_path(*entry, entry->files.front(), "models") ==
            (std::filesystem::path("models") / "ocr" / entry->files.front().filename).string());

    const auto tmp = std::filesystem::temp_directory_path() / "iris_catalog_test";
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp / "ocr", ec);
    REQUIRE_FALSE(installed(*entry, tmp.string()));
    for (const auto& file : entry->files) {
        std::ofstream f(target_path(*entry, file, tmp.string()));
        f << "x";
    }
    REQUIRE(installed(*entry, tmp.string()));
    std::filesystem::remove_all(tmp, ec);
}

TEST_CASE("quality: metrics, issue detection and HTML escaping", "[quality]") {
    iris::model::IrDocument doc;
    doc.config.language = "zh";
    doc.generator.input_file = "in.mp4";
    doc.generator.command = "asr";

    iris::model::Track track;
    track.type = "transcribe";
    track.language = "zh";

    auto add = [&](std::int64_t s, std::int64_t e, const std::string& text,
                   const std::string& speaker = "") {
        iris::model::Sentence sent;
        sent.start_ms = s;
        sent.end_ms = e;
        sent.text = text;
        if (!speaker.empty()) sent.speaker = speaker;
        track.sentences.push_back(sent);
    };

    add(0, 1000, "一二三四五六七八九十一二三四五六七八九十"); // 20 字 / 1 s → 20 CPS
    add(500, 2000, "重叠的一句话");                          // 与上一条重叠 500ms
    add(2000, 3000, "重叠的一句话");                          // 重复文本
    add(3000, 3000, "");                                     // 空字幕
    add(3000, 4000, "<b>html</b> & 符号", "SPEAKER_1");
    doc.state.tracks.push_back(track);

    const auto report = iris::quality::analyze(doc);
    REQUIRE(report.tracks.size() == 1);
    const auto& m = report.tracks[0];
    REQUIRE(m.cues == 5);
    REQUIRE(m.overlaps == 1);
    REQUIRE(m.duplicates == 1);
    REQUIRE(m.empty_cues == 1);
    REQUIRE(m.fast_cues == 2); // 第一条 20 CPS；最后一条 16 字符 / 1 s = 16 CPS
    REQUIRE(m.speakers.size() == 1);
    REQUIRE(m.speakers.at("SPEAKER_1") == 1);
    REQUIRE(report.errors == 2);   // overlap + empty
    REQUIRE(report.warnings == 3); // fast x2 + duplicate

    // CPS 上限按语言区分
    REQUIRE(iris::quality::max_cps_for("zh") == 12.0);
    REQUIRE(iris::quality::max_cps_for("en") == 20.0);

    // HTML 转义：文本里的标签必须被转义
    const std::string html = iris::quality::to_html(doc, report);
    REQUIRE(html.find("<b>html</b>") == std::string::npos);
    REQUIRE(html.find("&lt;b&gt;html&lt;/b&gt;") != std::string::npos);
}

TEST_CASE("ocr: detection boxes and CTC decoding", "[ocr]") {
    // 概率图里放一个矩形文本块 → 一个框
    const int w = 20;
    const int h = 10;
    std::vector<float> prob(static_cast<std::size_t>(w) * h, 0.0f);
    for (int y = 2; y <= 6; ++y) {
        for (int x = 5; x <= 10; ++x) prob[static_cast<std::size_t>(y) * w + x] = 0.9f;
    }
    const auto boxes = iris::ocr::detail::boxes_from_mask(prob, w, h, 0.3f, 0.5f);
    REQUIRE(boxes.size() == 1);
    REQUIRE(boxes[0].x == 5);
    REQUIRE(boxes[0].y == 2);
    REQUIRE(boxes[0].w == 6);
    REQUIRE(boxes[0].h == 5);
    REQUIRE(boxes[0].score > 0.8f);

    // 低置信度块被过滤
    std::fill(prob.begin(), prob.end(), 0.35f);
    REQUIRE(iris::ocr::detail::boxes_from_mask(prob, w, h, 0.3f, 0.5f).empty());

    // unclip 外扩：d = area*ratio/perimeter = 30*1.5/22 ≈ 2.05 → pad 2
    std::vector<iris::ocr::detail::Box> one;
    iris::ocr::detail::Box b;
    b.x = 5;
    b.y = 2;
    b.w = 6;
    b.h = 5;
    one.push_back(b);
    const auto grown = iris::ocr::detail::expand_boxes(one, 1.5f, 100, 100);
    REQUIRE(grown.size() == 1);
    REQUIRE(grown[0].x == 3);
    REQUIRE(grown[0].y == 0);
    REQUIRE(grown[0].w == 10);
    REQUIRE(grown[0].h == 9);

    // CTC：去 blank、去连续重复（0=blank，1/2 为字典项）
    const std::vector<std::string> dict = {"A", "B"};
    const int steps = 6;
    const int classes = 3;
    std::vector<float> logits(static_cast<std::size_t>(steps) * classes, 0.0f);
    const int ids[steps] = {1, 1, 0, 2, 0, 1};
    for (int t = 0; t < steps; ++t) {
        logits[static_cast<std::size_t>(t) * classes + ids[t]] = 1.0f;
    }
    float score = 0.0f;
    REQUIRE(iris::ocr::detail::ctc_decode(logits, steps, classes, dict, &score) == "ABA");
    REQUIRE(score > 0.0f);
}

TEST_CASE("dub: per-speaker voice selection", "[dub]") {
    std::vector<iris::ops::SpeakerVoice> voices;
    iris::ops::SpeakerVoice a;
    a.speaker = "SPEAKER_1";
    a.ref_wav = "a.wav";
    iris::ops::SpeakerVoice b;
    b.speaker = "SPEAKER_2";
    b.ref_wav = "b.wav";
    iris::ops::SpeakerVoice def;
    def.speaker = "default";
    def.ref_wav = "default.wav";
    voices = {a, b, def};

    // 精确匹配
    REQUIRE(iris::ops::find_voice("SPEAKER_2", voices) == &voices[1]);
    // 未知说话人 → default
    REQUIRE(iris::ops::find_voice("SPEAKER_9", voices) == &voices[2]);
    // 无标签（没有做说话人分割）→ 落 default
    REQUIRE(iris::ops::find_voice("", voices) == &voices[2]);
    // 没有配置 → nullptr
    REQUIRE(iris::ops::find_voice("SPEAKER_1", {}) == nullptr);

    // 没有 default 条目时，未知说话人不映射
    std::vector<iris::ops::SpeakerVoice> only_two = {a, b};
    REQUIRE(iris::ops::find_voice("SPEAKER_3", only_two) == nullptr);
}

TEST_CASE("dub timeline mix places cues at their offsets") {
    std::vector<float> timeline;
    const std::vector<float> cue(4, 0.5f);

    iris::tts::mix_at(timeline, cue, 8);
    REQUIRE(timeline.size() == 12);
    for (std::size_t i = 0; i < 8; ++i) REQUIRE(timeline[i] == 0.0f);
    for (std::size_t i = 8; i < 12; ++i) REQUIRE(timeline[i] == 0.5f);

    // 叠加是纯相加（不限幅），峰值交给调用方归一
    const std::vector<float> loud(2, 0.9f);
    iris::tts::mix_at(timeline, loud, 0);
    REQUIRE(timeline[0] == 0.9f);
    iris::tts::mix_at(timeline, loud, 0);
    REQUIRE(timeline[0] == 1.8f);

    // 峰值检测
    REQUIRE(iris::tts::peak_abs(timeline) == 1.8f);
    REQUIRE(iris::tts::peak_abs({}) == 0.0f);
    REQUIRE(iris::tts::peak_abs({-0.4f, 0.2f}) == 0.4f);

    // 空源不改动目标
    const std::size_t before = timeline.size();
    iris::tts::mix_at(timeline, {}, 100);
    REQUIRE(timeline.size() == before);
}

TEST_CASE("IR state keeps the dub product") {
    iris::model::IrDocument doc;
    doc.state.dubbed_audio_path = "out/dub.wav";
    doc.state.is_dubbed = true;

    const auto back = iris::model::IrDocument::from_json(doc.to_json());
    REQUIRE(back.state.dubbed_audio_path.has_value());
    REQUIRE(*back.state.dubbed_audio_path == "out/dub.wav");
    REQUIRE(back.state.is_dubbed);
}
