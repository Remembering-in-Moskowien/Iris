#include "iris/infra/quiet.hpp"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace iris::infra {

namespace {

bool verbose() {
    const char* v = std::getenv("IRIS_VERBOSE");
    return v != nullptr && *v != '\0' && *v != '0';
}

} // namespace

StderrSilencer::StderrSilencer() {
    if (verbose()) return;
#ifdef _WIN32
    saved_fd_ = _dup(2);
    if (saved_fd_ < 0) return;
    FILE* nul = nullptr;
    if (freopen_s(&nul, "NUL", "w", stderr) != 0 || nul == nullptr) {
        _close(saved_fd_);
        saved_fd_ = -1;
        return;
    }
#else
    saved_fd_ = ::dup(2);
    if (saved_fd_ < 0) return;
    if (!std::freopen("/dev/null", "w", stderr)) {
        ::close(saved_fd_);
        saved_fd_ = -1;
        return;
    }
#endif
    active_ = true;
}

StderrSilencer::~StderrSilencer() {
    if (!active_ || saved_fd_ < 0) return;
    std::fflush(stderr);
#ifdef _WIN32
    _dup2(saved_fd_, 2);
    _close(saved_fd_);
#else
    ::dup2(saved_fd_, 2);
    ::close(saved_fd_);
#endif
    saved_fd_ = -1;
    active_ = false;
}

} // namespace iris::infra
