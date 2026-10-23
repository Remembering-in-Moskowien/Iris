#pragma once

#include <string>

namespace iris::controller {

/// 鐗堟湰鍙凤紙Model/IR generator.version 涓?--version 鍏辩敤锛夈€?/// CI 閫氳繃 IRIS_BUILD_NUMBER 娉ㄥ叆鏋勫缓鍙凤紙release tag = v{run_number}锛夛紱鏈湴榛樿 0.1.0銆?#ifndef IRIS_VERSION_STRING
#define IRIS_VERSION_STRING "0.1.0"
#endif
inline constexpr const char* kVersion = IRIS_VERSION_STRING;

// MVC Controller 灞傦細姣忎釜瀛愬懡浠や竴涓鐞嗗櫒锛岃礋璐ｇ紪鎺?Model 涓?Service锛坥ps锛夛紝
// 骞舵妸缁撴灉浜ょ粰 View锛堟覆鏌?鎺у埗鍙帮級銆傝繑鍥炶繘绋嬮€€鍑虹爜锛? 鎴愬姛锛岄潪 0 澶辫触锛夈€?
/// asr锛氬獟浣?鈫?IR锛坒fmpeg_convert 鈫?audio_preprocess 鈫?transcribe 鈫?sentence_split锛夈€?int run_asr(const std::string& input, const std::string& language, const std::string& engine,
            const std::string& model, const std::string& output);

/// build锛欼R 鈫?ASS/SRT/TXT锛圴iew 灞傛覆鏌擄級銆?int run_build(const std::string& ir_path, const std::string& format, const std::string& output);

/// validate锛氭牎楠?IR 鏂囦欢銆?int run_validate(const std::string& ir_path);

/// doctor锛氱幆澧冭瘖鏂紙澶栭儴濯掍綋宸ュ叿 + 鍐呭祵 ASR 鍚庣鍙敤鎬э級銆?int run_doctor();

/// models list锛氬垪鍑哄凡娉ㄥ唽鎻愪緵鑰呫€?int run_models_list();

} // namespace iris::controller
