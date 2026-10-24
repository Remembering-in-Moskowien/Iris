#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "iris/model/ir_document.hpp"
#include "iris/tts/gguf.hpp"
#include "iris/tts/kernels.hpp"
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
    for (float v : norm.data) mean += v;
    mean /= static_cast<double>(norm.data.size());
    REQUIRE(mean > 0.5); // RMSNorm 权重初始化为 1 附近
    REQUIRE(mean < 1.5);

    // Q8_0 注意力投影：[1024(进), 2048(出)] → 16 heads × 128
    iris::tts::Tensor q;
    REQUIRE(weights.load("talker.blk.0.attn_q.weight", q, error));
    REQUIRE(q.dims.size() == 2);
    REQUIRE(q.cols() == 1024);
    REQUIRE(q.rows() == 2048);
    REQUIRE(q.data.size() == 2048u * 1024u);
    double energy = 0.0;
    for (float v : q.data) energy += static_cast<double>(v) * v;
    REQUIRE(energy > 0.0); // 不是全零

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
