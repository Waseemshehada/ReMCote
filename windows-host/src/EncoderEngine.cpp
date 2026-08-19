#include "EncoderEngine.h"
#include "Logger.h"

// Diagnostic stub: EncoderEngine NVENC code temporarily excluded to isolate
// the MSVC compile error.  If the build passes with this stub, the root cause
// is in the NVENC EncoderEngine implementation.

namespace remcote {

bool EncoderEngine::Initialize(ID3D11Device* /*device*/, const EncoderConfig& config) {
    config_ = config;
    Logger::Warning("EncoderEngine: NVENC disabled in this diagnostic build");
    return false;
}

bool EncoderEngine::SubmitFrame(ID3D11Texture2D* /*frame*/, int64_t /*captureUs*/) {
    return false;
}

void EncoderEngine::SetBitrate(int /*kbps*/) {}

void EncoderEngine::Shutdown() {}

} // namespace remcote
