#pragma once

#include <stddef.h>

#if defined(_WIN32) && defined(IRIS_VSF_BUILD)
#define IRIS_VSF_API __declspec(dllexport)
#elif defined(_WIN32)
#define IRIS_VSF_API __declspec(dllimport)
#else
#define IRIS_VSF_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// Returns the ABI version of the Iris hard-subtitle scoring DLL.
IRIS_VSF_API int iris_vsf_abi_version(void);

/// Scores one packed RGB24 ROI. The result is normalized to [0, 1].
IRIS_VSF_API double iris_vsf_score_rgb(const unsigned char* rgb, int width, int height);

#ifdef __cplusplus
}
#endif
