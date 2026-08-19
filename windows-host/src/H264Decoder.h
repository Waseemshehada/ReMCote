#pragma once
// H264Decoder — Windows Media Foundation H.264 MFT software/hardware decoder.
// Accepts Annex-B access units (start-code prefixed), decodes via the system
// MFT (CLSID_CMSH264DecoderMFT), and delivers decoded frames as BGRA to the
// caller's FrameCallback.  Thread-safe: Decode() and Reset()/Shutdown() are
// serialised by an internal mutex.

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wrl/client.h>

namespace remcote {

class H264Decoder {
public:
    // Decoded frame delivered on the caller's Decode() thread (or internally
    // from ProcessOutput).  bgra holds width*height*4 bytes in B8G8R8A8 order.
    // stride is always width*4 for the converted BGRA output.
    using FrameCallback = std::function<void(
        std::vector<uint8_t> bgra, int width, int height, int stride)>;

    // Initialize the MFT for the given stream dimensions.
    // width/height are hints; the MFT may override them on the first keyframe.
    bool Initialize(int width, int height, FrameCallback callback);

    // Submit one complete Annex-B access unit (may contain one or more NALUs).
    // timestamp100ns is the presentation timestamp in 100-nanosecond units.
    // Returns false only on a fatal error; MF_E_TRANSFORM_STREAM_CHANGE is
    // handled internally and is not propagated as a failure.
    bool Decode(const uint8_t* annexB, size_t size, int64_t timestamp100ns);

    // Drain any buffered frames and reset decoder state (e.g. after a seek or
    // resolution change).  The MFT is flushed but kept alive for reuse.
    void Reset();

    // Release all MFT resources.  After Shutdown() the object must not be used.
    void Shutdown();

private:
    // Build and apply input/output media types.
    bool SetMediaTypes(int width, int height);

    // Drain the MFT output queue; delivers frames via callback_.
    // Returns false only on a fatal error.
    bool DrainOutput();

    // Convert a single NV12 sample to BGRA and invoke callback_.
    void ConvertAndDeliver(IMFSample* sample);

    // Convert NV12 planes to BGRA.  dst stride is width*4.
    static void Nv12ToBgra(const uint8_t* y, int yStride,
                            const uint8_t* uv, int uvStride,
                            uint8_t* dst, int width, int height);

    // Convert RGB32 (RGBX / X8R8G8B8) to BGRA in-place (just fill alpha = 0xFF).
    static void Rgb32ToBgra(const uint8_t* src, int srcStride,
                             uint8_t* dst, int width, int height);

    Microsoft::WRL::ComPtr<IMFTransform> mft_;
    FrameCallback callback_;
    int width_  = 0;
    int height_ = 0;
    DWORD streamInId_  = 0;
    DWORD streamOutId_ = 0;
    bool  ready_       = false; // true after first successful SetMediaTypes
    bool  mfStarted_   = false;
    std::mutex mutex_;
};

} // namespace remcote
