// H264Decoder — Windows Media Foundation H.264 MFT decoder implementation.
// See H264Decoder.h for the public interface contract.

#include "H264Decoder.h"
#include "Logger.h"

#include <algorithm>
#include <cstring>
#include <string>

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <mfobjects.h>
#include <wmcodecdsp.h>   // CLSID_CMSH264DecoderMFT

// Link: mf.lib mfplat.lib mfuuid.lib wmcodecdspuuid.lib

namespace remcote {

// ─── helpers ─────────────────────────────────────────────────────────────────

namespace {

// Build a minimal MFMediaType for H.264 compressed input.
HRESULT MakeH264InputType(int width, int height, IMFMediaType** ppType) {
    Microsoft::WRL::ComPtr<IMFMediaType> t;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr)) return hr;
    hr = t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); if (FAILED(hr)) return hr;
    hr = t->SetGUID(MF_MT_SUBTYPE,    MFVideoFormat_H264); if (FAILED(hr)) return hr;
    hr = MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, width, height); if (FAILED(hr)) return hr;
    hr = MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, 60, 1);        if (FAILED(hr)) return hr;
    hr = MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1); if (FAILED(hr)) return hr;
    hr = t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); if (FAILED(hr)) return hr;
    *ppType = t.Detach();
    return S_OK;
}

// Build NV12 or RGB32 output type.
HRESULT MakeOutputType(const GUID& subtype, int width, int height, IMFMediaType** ppType) {
    Microsoft::WRL::ComPtr<IMFMediaType> t;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr)) return hr;
    hr = t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); if (FAILED(hr)) return hr;
    hr = t->SetGUID(MF_MT_SUBTYPE,    subtype);            if (FAILED(hr)) return hr;
    hr = MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, width, height); if (FAILED(hr)) return hr;
    hr = MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, 60, 1);        if (FAILED(hr)) return hr;
    hr = MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1); if (FAILED(hr)) return hr;
    hr = t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); if (FAILED(hr)) return hr;
    *ppType = t.Detach();
    return S_OK;
}

// Read actual decoded dimensions from an output media type.
void ReadDimensions(IMFMediaType* type, int& outWidth, int& outHeight) {
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
    if (w && h) {
        outWidth  = static_cast<int>(w);
        outHeight = static_cast<int>(h);
    }
}

} // namespace

// ─── Initialize ──────────────────────────────────────────────────────────────

bool H264Decoder::Initialize(int width, int height, FrameCallback callback) {
    std::lock_guard<std::mutex> lk(mutex_);

    if (mft_) {
        Logger::Warning("H264Decoder::Initialize called on already-initialized decoder; re-initializing");
        mft_.Reset();
        ready_ = false;
    }

    callback_ = std::move(callback);
    width_    = width;
    height_   = height;

    if (!mfStarted_) {
        HRESULT startup = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(startup)) {
            Logger::Errorf("H264Decoder: MFStartup failed: 0x%08X", startup);
            return false;
        }
        mfStarted_ = true;
    }

    // Co-create the system H.264 decoder MFT.
    HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT,
                                  nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&mft_));
    if (FAILED(hr)) {
        Logger::Errorf("H264Decoder: CoCreateInstance(CMSH264DecoderMFT) failed: 0x%08X", hr);
        MFShutdown();
        mfStarted_ = false;
        return false;
    }

    // Enumerate stream IDs (the SW MFT uses 0/0 by default).
    DWORD nIn = 0, nOut = 0;
    hr = mft_->GetStreamCount(&nIn, &nOut);
    if (FAILED(hr) || nIn == 0 || nOut == 0) {
        Logger::Error("H264Decoder: GetStreamCount failed or returned 0");
        mft_.Reset();
        MFShutdown();
        mfStarted_ = false;
        return false;
    }
    streamInId_  = 0;
    streamOutId_ = 0;

    if (!SetMediaTypes(width_, height_)) {
        mft_.Reset();
        MFShutdown();
        mfStarted_ = false;
        return false;
    }

    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    Logger::Infof("H264Decoder: initialized %dx%d", width_, height_);
    ready_ = true;
    return true;
}

// ─── SetMediaTypes ───────────────────────────────────────────────────────────

bool H264Decoder::SetMediaTypes(int width, int height) {
    // Input type: H.264 Annex-B.
    Microsoft::WRL::ComPtr<IMFMediaType> inType;
    HRESULT hr = MakeH264InputType(width, height, &inType);
    if (FAILED(hr)) {
        Logger::Errorf("H264Decoder: MakeH264InputType failed: 0x%08X", hr);
        return false;
    }
    hr = mft_->SetInputType(streamInId_, inType.Get(), 0);
    if (FAILED(hr)) {
        Logger::Errorf("H264Decoder: SetInputType failed: 0x%08X", hr);
        return false;
    }

    // Try NV12 first (most efficient); fall back to RGB32.
    Microsoft::WRL::ComPtr<IMFMediaType> outType;
    hr = MakeOutputType(MFVideoFormat_NV12, width, height, &outType);
    if (SUCCEEDED(hr)) {
        hr = mft_->SetOutputType(streamOutId_, outType.Get(), 0);
    }
    if (FAILED(hr)) {
        Logger::Warning("H264Decoder: NV12 output type rejected, trying RGB32");
        outType.Reset();
        hr = MakeOutputType(MFVideoFormat_RGB32, width, height, &outType);
        if (SUCCEEDED(hr))
            hr = mft_->SetOutputType(streamOutId_, outType.Get(), 0);
    }
    if (FAILED(hr)) {
        Logger::Errorf("H264Decoder: SetOutputType failed: 0x%08X", hr);
        return false;
    }

    return true;
}

// ─── Decode ──────────────────────────────────────────────────────────────────

bool H264Decoder::Decode(const uint8_t* annexB, size_t size, int64_t timestamp100ns) {
    if (!annexB || size == 0) return true;

    std::lock_guard<std::mutex> lk(mutex_);
    if (!mft_ || !ready_) {
        Logger::Warning("H264Decoder::Decode called before Initialize or after Shutdown");
        return false;
    }

    // ── Wrap the Annex-B bytes in an MF media buffer ──────────────────────────
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), &buf);
    if (FAILED(hr)) {
        Logger::Errorf("H264Decoder: MFCreateMemoryBuffer failed: 0x%08X", hr);
        return false;
    }
    {
        BYTE* p = nullptr;
        DWORD maxLen = 0, curLen = 0;
        hr = buf->Lock(&p, &maxLen, &curLen);
        if (FAILED(hr)) return false;
        std::memcpy(p, annexB, size);
        buf->Unlock();
        buf->SetCurrentLength(static_cast<DWORD>(size));
    }

    Microsoft::WRL::ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) return false;
    sample->AddBuffer(buf.Get());
    sample->SetSampleTime(timestamp100ns);
    sample->SetSampleDuration(166667); // ~1/60 s in 100ns units

    // ── Feed to MFT ──────────────────────────────────────────────────────────
    hr = mft_->ProcessInput(streamInId_, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        // The MFT wants us to drain output before accepting more input.
        if (!DrainOutput()) return false;
        hr = mft_->ProcessInput(streamInId_, sample.Get(), 0);
    }
    if (FAILED(hr)) {
        Logger::Errorf("H264Decoder: ProcessInput failed: 0x%08X", hr);
        return false;
    }

    return DrainOutput();
}

// ─── DrainOutput ─────────────────────────────────────────────────────────────

bool H264Decoder::DrainOutput() {
    // Called with mutex_ already held.
    for (;;) {
        // Allocate an output sample.  The SW MFT allocates its own buffers
        // (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES flag) or we must provide one;
        // query to find out.
        MFT_OUTPUT_STREAM_INFO si{};
        HRESULT hr = mft_->GetOutputStreamInfo(streamOutId_, &si);
        if (FAILED(hr)) {
            Logger::Errorf("H264Decoder: GetOutputStreamInfo failed: 0x%08X", hr);
            return false;
        }

        MFT_OUTPUT_DATA_BUFFER outBuf{};
        outBuf.dwStreamID = streamOutId_;

        bool mftProvidesBuffer =
            (si.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;

        Microsoft::WRL::ComPtr<IMFSample>      outSample;
        Microsoft::WRL::ComPtr<IMFMediaBuffer> outMemBuf;

        if (!mftProvidesBuffer) {
            // We must supply the sample and buffer.
            hr = MFCreateSample(&outSample);
            if (FAILED(hr)) return false;
            DWORD bufSize = si.cbSize ? si.cbSize : static_cast<DWORD>(width_ * height_ * 4);
            hr = MFCreateMemoryBuffer(bufSize, &outMemBuf);
            if (FAILED(hr)) return false;
            outSample->AddBuffer(outMemBuf.Get());
            outBuf.pSample = outSample.Get();
        }

        DWORD status = 0;
        hr = mft_->ProcessOutput(0, 1, &outBuf, &status);

        // Release the event (MFT may set it).
        if (outBuf.pEvents) {
            outBuf.pEvents->Release();
            outBuf.pEvents = nullptr;
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            // No more output available at this time — normal.
            return true;
        }

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // Resolution or format change: re-negotiate the output type.
            Logger::Info("H264Decoder: MF_E_TRANSFORM_STREAM_CHANGE — re-negotiating output type");

            // Release any sample the MFT may have set before returning this error.
            if (mftProvidesBuffer && outBuf.pSample) {
                outBuf.pSample->Release();
                outBuf.pSample = nullptr;
            }

            // The MFT clears the output type; re-read the new preferred type.
            Microsoft::WRL::ComPtr<IMFMediaType> newType;
            bool outputTypeSet = false;
            for (DWORD i = 0; ; ++i) {
                newType.Reset();
                HRESULT hrAvail = mft_->GetOutputAvailableType(streamOutId_, i, &newType);
                if (FAILED(hrAvail)) break;
                GUID sub = GUID_NULL;
                newType->GetGUID(MF_MT_SUBTYPE, &sub);
                if (sub == MFVideoFormat_NV12 || sub == MFVideoFormat_RGB32) {
                    ReadDimensions(newType.Get(), width_, height_);
                    HRESULT hrSet = mft_->SetOutputType(streamOutId_, newType.Get(), 0);
                    if (SUCCEEDED(hrSet)) {
                        Logger::Infof("H264Decoder: stream change -> %dx%d", width_, height_);
                        outputTypeSet = true;
                        break;
                    }
                }
            }
            if (!outputTypeSet) {
                Logger::Error(
                    "H264Decoder: stream changed but no supported output type "
                    "could be selected");
                return false;
            }
            // Continue looping to flush any pending output with new type.
            continue;
        }

        if (FAILED(hr)) {
            Logger::Errorf("H264Decoder: ProcessOutput failed: 0x%08X", hr);
            return false;
        }

        // We have a decoded frame.
        IMFSample* deliverSample = mftProvidesBuffer ? outBuf.pSample : outSample.Get();
        if (deliverSample) {
            ConvertAndDeliver(deliverSample);
        }
        // If MFT allocated the sample, release it.
        if (mftProvidesBuffer && outBuf.pSample) {
            outBuf.pSample->Release();
            outBuf.pSample = nullptr;
        }
    }
}

// ─── ConvertAndDeliver ───────────────────────────────────────────────────────

void H264Decoder::ConvertAndDeliver(IMFSample* sample) {
    // Determine the current output subtype.
    Microsoft::WRL::ComPtr<IMFMediaType> outType;
    HRESULT hr = mft_->GetOutputCurrentType(streamOutId_, &outType);
    GUID subtype = GUID_NULL;
    if (SUCCEEDED(hr)) outType->GetGUID(MF_MT_SUBTYPE, &subtype);

    // Read up-to-date dimensions.
    int w = width_, h = height_;
    if (SUCCEEDED(hr)) ReadDimensions(outType.Get(), w, h);
    if (w <= 0 || h <= 0) return;

    // Get a contiguous buffer from the sample.
    Microsoft::WRL::ComPtr<IMFMediaBuffer> contiguous;
    hr = sample->ConvertToContiguousBuffer(&contiguous);
    if (FAILED(hr)) {
        Logger::Warningf("H264Decoder: ConvertToContiguousBuffer failed: 0x%08X", hr);
        return;
    }

    // Try IMF2DBuffer first to get the real row stride.
    BYTE* ptr     = nullptr;
    LONG  pitch2d = 0;
    DWORD curLen  = 0;
    bool  used2d  = false;

    Microsoft::WRL::ComPtr<IMF2DBuffer> buf2d;
    if (SUCCEEDED(contiguous.As(&buf2d))) {
        if (SUCCEEDED(buf2d->Lock2D(&ptr, &pitch2d))) {
            used2d = true;
        }
    }
    if (!used2d) {
        DWORD maxLen2 = 0;
        hr = contiguous->Lock(&ptr, &maxLen2, &curLen);
        if (FAILED(hr)) return;
    }

    std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);

    if (subtype == MFVideoFormat_NV12) {
        int yStride;
        if (used2d) {
            yStride = static_cast<int>(pitch2d);
        } else {
            // Derive from buffer size: total = stride * (h + h/2).
            yStride = (h > 0) ? static_cast<int>(curLen) / (h + h / 2) : w;
            if (yStride < w) yStride = w;
        }
        const uint8_t* yPlane  = ptr;
        const uint8_t* uvPlane = ptr + yStride * h;
        Nv12ToBgra(yPlane, yStride, uvPlane, yStride, bgra.data(), w, h);
    } else {
        // RGB32/RGBX stored as B8G8R8X8 on little-endian Windows.
        int srcStride;
        if (used2d) {
            srcStride = static_cast<int>(pitch2d);
        } else {
            srcStride = (h > 0) ? static_cast<int>(curLen) / h : w * 4;
            if (srcStride < w * 4) srcStride = w * 4;
        }
        Rgb32ToBgra(ptr, srcStride, bgra.data(), w, h);
    }

    if (used2d) {
        buf2d->Unlock2D();
    } else {
        contiguous->Unlock();
    }

    if (callback_) {
        callback_(std::move(bgra), w, h, w * 4);
    }
}

// ─── NV12 → BGRA conversion ──────────────────────────────────────────────────

void H264Decoder::Nv12ToBgra(const uint8_t* y,  int yStride,
                               const uint8_t* uv, int uvStride,
                               uint8_t* dst,
                               int width, int height)
{
    for (int row = 0; row < height; ++row) {
        const uint8_t* yRow  = y  + row * yStride;
        const uint8_t* uvRow = uv + (row / 2) * uvStride;
        uint8_t* dstRow = dst + row * width * 4;

        for (int col = 0; col < width; ++col) {
            int Y  = static_cast<int>(yRow[col]);
            int U  = static_cast<int>(uvRow[(col & ~1)]) - 128;
            int V  = static_cast<int>(uvRow[(col & ~1) + 1]) - 128;

            // BT.601 limited-range coefficients (integer approximation).
            int C = (Y - 16) * 298;
            int R = (C           + 409 * V + 128) >> 8;
            int G = (C - 100 * U - 208 * V + 128) >> 8;
            int B = (C + 516 * U             + 128) >> 8;

            dstRow[col * 4 + 0] = static_cast<uint8_t>(std::clamp(B, 0, 255));
            dstRow[col * 4 + 1] = static_cast<uint8_t>(std::clamp(G, 0, 255));
            dstRow[col * 4 + 2] = static_cast<uint8_t>(std::clamp(R, 0, 255));
            dstRow[col * 4 + 3] = 255;
        }
    }
}

// ─── RGB32 → BGRA conversion ─────────────────────────────────────────────────

void H264Decoder::Rgb32ToBgra(const uint8_t* src, int srcStride,
                               uint8_t* dst,
                               int width, int height)
{
    // MFVideoFormat_RGB32 on Windows is stored as B8G8R8X8 in memory (little-
    // endian), so R and B are already in the right positions for BGRA; we just
    // need to write alpha = 0xFF.
    for (int row = 0; row < height; ++row) {
        const uint8_t* s = src + row * srcStride;
        uint8_t*       d = dst + row * width * 4;
        for (int col = 0; col < width; ++col) {
            d[col * 4 + 0] = s[col * 4 + 0]; // B
            d[col * 4 + 1] = s[col * 4 + 1]; // G
            d[col * 4 + 2] = s[col * 4 + 2]; // R
            d[col * 4 + 3] = 255;             // A
        }
    }
}

// ─── Reset ───────────────────────────────────────────────────────────────────

void H264Decoder::Reset() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!mft_) return;
    HRESULT hr = mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    if (FAILED(hr)) {
        Logger::Warningf("H264Decoder: flush failed: 0x%08X", hr);
    }
    Logger::Info("H264Decoder: reset (flushed)");
}

// ─── Shutdown ────────────────────────────────────────────────────────────────

void H264Decoder::Shutdown() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (mft_) {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        mft_.Reset();
    }
    ready_    = false;
    callback_ = nullptr;
    if (mfStarted_) {
        MFShutdown();
        mfStarted_ = false;
    }
    Logger::Info("H264Decoder: shut down");
}

} // namespace remcote
