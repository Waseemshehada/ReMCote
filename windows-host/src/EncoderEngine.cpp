#include "EncoderEngine.h"
#include "Logger.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

// NVIDIA Video Codec SDK interface header (see build-windows.ps1 — fetched
// into third_party/nv-codec-headers).
#include <ffnvcodec/nvEncodeAPI.h>

namespace remcote {

namespace {

NV_ENCODE_API_FUNCTION_LIST* Api(void* p) {
    return static_cast<NV_ENCODE_API_FUNCTION_LIST*>(p);
}

using PFN_NvEncodeAPICreateInstance =
    NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);

const char* StatusName(NVENCSTATUS status) {
    switch (status) {
    case NV_ENC_SUCCESS: return "NV_ENC_SUCCESS";
    case NV_ENC_ERR_NO_ENCODE_DEVICE: return "NV_ENC_ERR_NO_ENCODE_DEVICE";
    case NV_ENC_ERR_UNSUPPORTED_DEVICE: return "NV_ENC_ERR_UNSUPPORTED_DEVICE";
    case NV_ENC_ERR_INVALID_ENCODERDEVICE: return "NV_ENC_ERR_INVALID_ENCODERDEVICE";
    case NV_ENC_ERR_INVALID_DEVICE: return "NV_ENC_ERR_INVALID_DEVICE";
    case NV_ENC_ERR_DEVICE_NOT_EXIST: return "NV_ENC_ERR_DEVICE_NOT_EXIST";
    case NV_ENC_ERR_INVALID_PTR: return "NV_ENC_ERR_INVALID_PTR";
    case NV_ENC_ERR_INVALID_EVENT: return "NV_ENC_ERR_INVALID_EVENT";
    case NV_ENC_ERR_INVALID_PARAM: return "NV_ENC_ERR_INVALID_PARAM";
    case NV_ENC_ERR_INVALID_CALL: return "NV_ENC_ERR_INVALID_CALL";
    case NV_ENC_ERR_OUT_OF_MEMORY: return "NV_ENC_ERR_OUT_OF_MEMORY";
    case NV_ENC_ERR_UNSUPPORTED_PARAM: return "NV_ENC_ERR_UNSUPPORTED_PARAM";
    default: return "NV_ENC_ERR_UNKNOWN";
    }
}

} // namespace

bool EncoderEngine::Initialize(
    ID3D11Device* device,
    const EncoderConfig& config) {
    Shutdown();
    config_ = config;
    if (config_.sourceWidth <= 0) config_.sourceWidth = config_.width;
    if (config_.sourceHeight <= 0) config_.sourceHeight = config_.height;
    device->GetImmediateContext(context_.GetAddressOf());

    hModule_ = LoadLibraryA("nvEncodeAPI64.dll");
    if (!hModule_) {
        Logger::Error(
            "NVENC driver DLL was not found; an NVIDIA GPU with a current "
            "driver is required");
        return false;
    }

    auto createInstance =
        reinterpret_cast<PFN_NvEncodeAPICreateInstance>(
            GetProcAddress(
                static_cast<HMODULE>(hModule_),
                "NvEncodeAPICreateInstance"));
    if (!createInstance) {
        Logger::Error(
            "NVENC API entry point NvEncodeAPICreateInstance is unavailable");
        Shutdown();
        return false;
    }

    api_ = new NV_ENCODE_API_FUNCTION_LIST{};
    Api(api_)->version = NV_ENCODE_API_FUNCTION_LIST_VER;
    const NVENCSTATUS instanceStatus = createInstance(Api(api_));
    if (instanceStatus != NV_ENC_SUCCESS) {
        Logger::Errorf(
            "NVENC API instance creation failed (%s, status %d)",
            StatusName(instanceStatus), instanceStatus);
        Shutdown();
        return false;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
    open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.device = device;
    open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open.apiVersion = NVENCAPI_VERSION;
    const NVENCSTATUS openStatus =
        Api(api_)->nvEncOpenEncodeSessionEx(&open, &encoder_);
    if (openStatus != NV_ENC_SUCCESS) {
        Logger::Errorf(
            "NVENC encoder session open failed (%s, status %d)",
            StatusName(openStatus), openStatus);
        Shutdown();
        return false;
    }

    NV_ENC_INITIALIZE_PARAMS init{};
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = NV_ENC_CODEC_H264_GUID;
    init.presetGUID = NV_ENC_PRESET_P1_GUID;
    init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    init.encodeWidth = config.width;
    init.encodeHeight = config.height;
    init.darWidth = config.width;
    init.darHeight = config.height;
    init.frameRateNum = config.fps;
    init.frameRateDen = 1;
    init.enablePTD = 1;

    NV_ENC_PRESET_CONFIG presetCfg{};
    presetCfg.version = NV_ENC_PRESET_CONFIG_VER;
    presetCfg.presetCfg.version = NV_ENC_CONFIG_VER;
    const NVENCSTATUS presetStatus =
        Api(api_)->nvEncGetEncodePresetConfigEx(
            encoder_,
            init.encodeGUID,
            init.presetGUID,
            init.tuningInfo,
            &presetCfg);
    if (presetStatus != NV_ENC_SUCCESS) {
        Logger::Errorf(
            "NVENC preset lookup failed (%s, status %d)",
            StatusName(presetStatus), presetStatus);
        Shutdown();
        return false;
    }

    NV_ENC_CONFIG encCfg = presetCfg.presetCfg;
    // The viewer SDP advertises constrained baseline (42e01f). Keep the
    // encoder profile compatible with that negotiated payload type.
    encCfg.profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
    encCfg.gopLength = NVENC_INFINITE_GOPLENGTH;
    encCfg.frameIntervalP = 1;
    encCfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    encCfg.rcParams.averageBitRate = config.bitrateKbps * 1000;
    encCfg.rcParams.maxBitRate = config.maxBitrateKbps * 1000;
    encCfg.rcParams.vbvBufferSize =
        config.bitrateKbps * 1000 / std::max(config.fps, 1);
    encCfg.rcParams.vbvInitialDelay = encCfg.rcParams.vbvBufferSize;
    encCfg.encodeCodecConfig.h264Config.idrPeriod =
        NVENC_INFINITE_GOPLENGTH;
    encCfg.encodeCodecConfig.h264Config.level = NV_ENC_LEVEL_H264_42;
    encCfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    encCfg.encodeCodecConfig.h264Config.sliceMode = 0;
    encCfg.encodeCodecConfig.h264Config.sliceModeData = 0;
    init.encodeConfig = &encCfg;

    const NVENCSTATUS initializeStatus =
        Api(api_)->nvEncInitializeEncoder(encoder_, &init);
    if (initializeStatus != NV_ENC_SUCCESS) {
        Logger::Errorf(
            "NVENC encoder initialization failed (%s, status %d)",
            StatusName(initializeStatus), initializeStatus);
        Shutdown();
        return false;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = config.width;
    td.Height = config.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    const HRESULT textureResult =
        device->CreateTexture2D(&td, nullptr, inputTexture_.GetAddressOf());
    if (FAILED(textureResult)) {
        Logger::Errorf(
            "NVENC input texture creation failed (0x%08lx)",
            textureResult);
        Shutdown();
        return false;
    }

    NV_ENC_REGISTER_RESOURCE reg{};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.resourceToRegister = inputTexture_.Get();
    reg.width = config.width;
    reg.height = config.height;
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    const NVENCSTATUS registerStatus =
        Api(api_)->nvEncRegisterResource(encoder_, &reg);
    if (registerStatus != NV_ENC_SUCCESS) {
        Logger::Errorf(
            "NVENC input-resource registration failed (status %d)",
            registerStatus);
        Shutdown();
        return false;
    }
    inputResource_ = reg.registeredResource;

    const bool needsScaling =
        config_.sourceWidth != config_.width ||
        config_.sourceHeight != config_.height;
    if (needsScaling) {
        HRESULT hr = device->QueryInterface(
            IID_PPV_ARGS(&videoDevice_));
        if (SUCCEEDED(hr)) {
            hr = context_.As(&videoContext_);
        }
        if (FAILED(hr)) {
            Logger::Errorf(
                "NVENC GPU scaler initialization failed (0x%08lx)", hr);
            Shutdown();
            return false;
        }

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputWidth = config_.sourceWidth;
        content.InputHeight = config_.sourceHeight;
        content.OutputWidth = config_.width;
        content.OutputHeight = config_.height;
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        hr = videoDevice_->CreateVideoProcessorEnumerator(
            &content, videoEnumerator_.GetAddressOf());
        if (SUCCEEDED(hr)) {
            hr = videoDevice_->CreateVideoProcessor(
                videoEnumerator_.Get(), 0, videoProcessor_.GetAddressOf());
        }
        if (FAILED(hr)) {
            Logger::Errorf(
                "NVENC GPU scaler processor creation failed (0x%08lx)", hr);
            Shutdown();
            return false;
        }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output{};
        output.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        output.Texture2D.MipSlice = 0;
        hr = videoDevice_->CreateVideoProcessorOutputView(
            inputTexture_.Get(),
            videoEnumerator_.Get(),
            &output,
            videoOutputView_.GetAddressOf());
        if (FAILED(hr)) {
            Logger::Errorf(
                "NVENC GPU scaler output view creation failed (0x%08lx)", hr);
            Shutdown();
            return false;
        }
        Logger::Infof(
            "NVENC GPU scaler ready: %dx%d -> %dx%d",
            config_.sourceWidth,
            config_.sourceHeight,
            config_.width,
            config_.height);
    }

    NV_ENC_CREATE_BITSTREAM_BUFFER bs{};
    bs.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    const NVENCSTATUS bitstreamStatus =
        Api(api_)->nvEncCreateBitstreamBuffer(encoder_, &bs);
    if (bitstreamStatus != NV_ENC_SUCCESS) {
        Logger::Errorf(
            "NVENC bitstream-buffer creation failed (status %d)",
            bitstreamStatus);
        Shutdown();
        return false;
    }
    bitstreamBuffer_ = bs.bitstreamBuffer;
    busy_ = false;
    forceIdr_ = true;

    Logger::Infof(
        "NVENC initialized: %dx%d @ %d fps, %d kbps CBR, baseline, "
        "ultra-low latency",
        config.width,
        config.height,
        config.fps,
        config.bitrateKbps);
    return true;
}

bool EncoderEngine::SubmitFrame(
    ID3D11Texture2D* frame,
    int64_t captureUs) {
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true)) return false;
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    if (!encoder_ || !api_ || !context_ || !inputTexture_) {
        busy_ = false;
        return false;
    }

    const int64_t encodeStart = NowUs();
    if (videoProcessor_) {
        if (videoInputTexture_.Get() != frame) {
            D3D11_TEXTURE2D_DESC source{};
            frame->GetDesc(&source);
            if (static_cast<int>(source.Width) != config_.sourceWidth ||
                static_cast<int>(source.Height) != config_.sourceHeight) {
                Logger::WarningRateLimited(
                    "nvenc-scaler-source-size",
                    "Desktop size changed; waiting for the capture pipeline to reinitialize");
                busy_ = false;
                return false;
            }
            videoInputView_.Reset();
            videoInputTexture_ = frame;
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input{};
            input.FourCC = 0;
            input.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            input.Texture2D.MipSlice = 0;
            input.Texture2D.ArraySlice = 0;
            const HRESULT inputViewResult =
                videoDevice_->CreateVideoProcessorInputView(
                    videoInputTexture_.Get(),
                    videoEnumerator_.Get(),
                    &input,
                    videoInputView_.GetAddressOf());
            if (FAILED(inputViewResult)) {
                Logger::WarningRateLimited(
                    "nvenc-scaler-input-view",
                    "NVENC GPU scaler could not create an input view");
                videoInputTexture_.Reset();
                busy_ = false;
                return false;
            }
        }

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = videoInputView_.Get();
        const HRESULT scaleResult = videoContext_->VideoProcessorBlt(
            videoProcessor_.Get(),
            videoOutputView_.Get(),
            0,
            1,
            &stream);
        if (FAILED(scaleResult)) {
            Logger::WarningRateLimited(
                "nvenc-gpu-scale-frame",
                "NVENC GPU scaler could not resize a desktop frame");
            busy_ = false;
            return false;
        }
    } else {
        context_->CopyResource(inputTexture_.Get(), frame);
    }

    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = inputResource_;
    if (Api(api_)->nvEncMapInputResource(encoder_, &map) !=
        NV_ENC_SUCCESS) {
        Logger::WarningRateLimited(
            "nvenc-map-frame",
            "NVENC could not map an input frame; dropping until it recovers");
        busy_ = false;
        return false;
    }

    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputBuffer = map.mappedResource;
    pic.bufferFmt = map.mappedBufferFmt;
    pic.inputWidth = config_.width;
    pic.inputHeight = config_.height;
    pic.outputBitstream = bitstreamBuffer_;
    pic.inputTimeStamp = static_cast<uint64_t>(captureUs);
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    if (forceIdr_.exchange(false)) {
        pic.encodePicFlags =
            NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }

    const NVENCSTATUS encodeStatus =
        Api(api_)->nvEncEncodePicture(encoder_, &pic);
    if (encodeStatus != NV_ENC_SUCCESS) {
        Logger::WarningRateLimited(
            "nvenc-encode-frame",
            "NVENC could not encode frames; dropping until it recovers");
        Api(api_)->nvEncUnmapInputResource(
            encoder_, map.mappedResource);
        busy_ = false;
        return false;
    }

    NV_ENC_LOCK_BITSTREAM lock{};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = bitstreamBuffer_;
    if (Api(api_)->nvEncLockBitstream(encoder_, &lock) ==
        NV_ENC_SUCCESS) {
        if (onOutput_) {
            EncodedFrame out;
            out.data =
                static_cast<const uint8_t*>(lock.bitstreamBufferPtr);
            out.size = lock.bitstreamSizeInBytes;
            out.keyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR;
            out.captureUs = captureUs;
            out.encodeDurationUs = NowUs() - encodeStart;
            onOutput_(out);
        }
        Api(api_)->nvEncUnlockBitstream(encoder_, bitstreamBuffer_);
    }

    Api(api_)->nvEncUnmapInputResource(encoder_, map.mappedResource);
    busy_ = false;
    return true;
}

void EncoderEngine::SetBitrate(int kbps) {
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    if (!encoder_ || !api_) return;
    if (kbps <= 0) return;
    const int targetKbps = std::min(kbps, config_.maxBitrateKbps);
    if (targetKbps != kbps) {
        Logger::Infof(
            "NVENC bitrate request capped at %d kbps for the negotiated H.264 level",
            targetKbps);
    }

    NV_ENC_RECONFIGURE_PARAMS re{};
    re.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    NV_ENC_CONFIG cfg{};
    cfg.version = NV_ENC_CONFIG_VER;
    re.reInitEncodeParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    re.reInitEncodeParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
    re.reInitEncodeParams.presetGUID = NV_ENC_PRESET_P1_GUID;
    re.reInitEncodeParams.tuningInfo =
        NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    re.reInitEncodeParams.encodeWidth = config_.width;
    re.reInitEncodeParams.encodeHeight = config_.height;
    re.reInitEncodeParams.darWidth = config_.width;
    re.reInitEncodeParams.darHeight = config_.height;
    re.reInitEncodeParams.frameRateNum = config_.fps;
    re.reInitEncodeParams.frameRateDen = 1;
    re.reInitEncodeParams.enablePTD = 1;
    re.reInitEncodeParams.encodeConfig = &cfg;
    cfg.profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
    cfg.gopLength = NVENC_INFINITE_GOPLENGTH;
    cfg.frameIntervalP = 1;
    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    cfg.rcParams.averageBitRate = targetKbps * 1000;
    cfg.rcParams.maxBitRate = config_.maxBitrateKbps * 1000;
    cfg.rcParams.vbvBufferSize =
        targetKbps * 1000 / std::max(config_.fps, 1);
    cfg.rcParams.vbvInitialDelay = cfg.rcParams.vbvBufferSize;
    cfg.encodeCodecConfig.h264Config.idrPeriod =
        NVENC_INFINITE_GOPLENGTH;
    cfg.encodeCodecConfig.h264Config.level = NV_ENC_LEVEL_H264_42;
    cfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    re.resetEncoder = 0;
    re.forceIDR = 0;
    const NVENCSTATUS status =
        Api(api_)->nvEncReconfigureEncoder(encoder_, &re);
    if (status != NV_ENC_SUCCESS) {
        Logger::Warningf(
            "NVENC bitrate reconfigure failed (status %d)", status);
        return;
    }
    config_.bitrateKbps = targetKbps;
    Logger::Infof("NVENC bitrate reconfigured to %d kbps", targetKbps);
}

void EncoderEngine::Shutdown() {
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    busy_ = false;
    if (api_ && encoder_) {
        if (bitstreamBuffer_) {
            Api(api_)->nvEncDestroyBitstreamBuffer(
                encoder_, bitstreamBuffer_);
        }
        if (inputResource_) {
            Api(api_)->nvEncUnregisterResource(
                encoder_, inputResource_);
        }
        Api(api_)->nvEncDestroyEncoder(encoder_);
    }

    encoder_ = nullptr;
    inputResource_ = nullptr;
    bitstreamBuffer_ = nullptr;
    videoInputView_.Reset();
    videoOutputView_.Reset();
    videoInputTexture_.Reset();
    videoProcessor_.Reset();
    videoEnumerator_.Reset();
    videoContext_.Reset();
    videoDevice_.Reset();
    inputTexture_.Reset();
    context_.Reset();

    if (api_) {
        delete static_cast<NV_ENCODE_API_FUNCTION_LIST*>(api_);
        api_ = nullptr;
    }
    if (hModule_) {
        FreeLibrary(static_cast<HMODULE>(hModule_));
        hModule_ = nullptr;
    }
}

} // namespace remcote