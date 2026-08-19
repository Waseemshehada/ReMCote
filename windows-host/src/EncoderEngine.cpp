#include "EncoderEngine.h"

#include <cstdio>
#include <cstring>

// NVIDIA Video Codec SDK interface header (see build-windows.ps1 — fetched
// into third_party/nv-codec-headers).
#include <ffnvcodec/nvEncodeAPI.h>

namespace remcote {

// Convenience: cast the void* stored in the header back to the real type.
static NV_ENCODE_API_FUNCTION_LIST* Api(void* p) {
    return static_cast<NV_ENCODE_API_FUNCTION_LIST*>(p);
}

typedef NVENCSTATUS(NVENCAPI* PFN_NvEncodeAPICreateInstance)(NV_ENCODE_API_FUNCTION_LIST*);

bool EncoderEngine::Initialize(ID3D11Device* device, const EncoderConfig& config) {
    config_ = config;
    device->GetImmediateContext(context_.GetAddressOf());

    // nvEncodeAPI ships with the NVIDIA driver — load it dynamically.
    hModule_ = LoadLibraryA("nvEncodeAPI64.dll");
    if (!hModule_) {
        std::fprintf(stderr, "[NVENC] nvEncodeAPI64.dll not found. An NVIDIA GPU with a current driver is required.\n");
        return false;
    }
    auto createInstance = reinterpret_cast<PFN_NvEncodeAPICreateInstance>(
        GetProcAddress(static_cast<HMODULE>(hModule_), "NvEncodeAPICreateInstance"));
    if (!createInstance) return false;

    api_ = new NV_ENCODE_API_FUNCTION_LIST{};
    Api(api_)->version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (createInstance(Api(api_)) != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "[NVENC] NvEncodeAPICreateInstance failed\n");
        return false;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
    open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.device = device;
    open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open.apiVersion = NVENCAPI_VERSION;
    if (Api(api_)->nvEncOpenEncodeSessionEx(&open, &encoder_) != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "[NVENC] open session failed\n");
        return false;
    }

    // --- Low-latency preset configuration (spec §10) -----------------------
    NV_ENC_INITIALIZE_PARAMS init{};
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = NV_ENC_CODEC_H264_GUID;
    init.presetGUID = NV_ENC_PRESET_P1_GUID;              // fastest
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
    Api(api_)->nvEncGetEncodePresetConfigEx(encoder_, init.encodeGUID, init.presetGUID,
                                       init.tuningInfo, &presetCfg);
    NV_ENC_CONFIG encCfg = presetCfg.presetCfg;
    encCfg.gopLength = NVENC_INFINITE_GOPLENGTH;          // IDR only on demand
    encCfg.frameIntervalP = 1;                            // NO B-frames
    encCfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    encCfg.rcParams.averageBitRate = config.bitrateKbps * 1000;
    encCfg.rcParams.maxBitRate = config.maxBitrateKbps * 1000;
    encCfg.rcParams.vbvBufferSize = config.bitrateKbps * 1000 / config.fps; // ~1 frame VBV
    encCfg.rcParams.vbvInitialDelay = encCfg.rcParams.vbvBufferSize;
    encCfg.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    encCfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1; // resync-friendly
    encCfg.encodeCodecConfig.h264Config.sliceMode = 0;
    encCfg.encodeCodecConfig.h264Config.sliceModeData = 0;
    init.encodeConfig = &encCfg;

    if (Api(api_)->nvEncInitializeEncoder(encoder_, &init) != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "[NVENC] initialize failed\n");
        return false;
    }

    // Reusable input texture registered with NVENC (desktop frames are copied
    // GPU-to-GPU into this texture by the capture engine's device context).
    D3D11_TEXTURE2D_DESC td{};
    td.Width = config.width;
    td.Height = config.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(device->CreateTexture2D(&td, nullptr, inputTexture_.GetAddressOf()))) return false;

    NV_ENC_REGISTER_RESOURCE reg{};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.resourceToRegister = inputTexture_.Get();
    reg.width = config.width;
    reg.height = config.height;
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    if (Api(api_)->nvEncRegisterResource(encoder_, &reg) != NV_ENC_SUCCESS) return false;
    inputResource_ = reg.registeredResource;

    NV_ENC_CREATE_BITSTREAM_BUFFER bs{};
    bs.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    if (Api(api_)->nvEncCreateBitstreamBuffer(encoder_, &bs) != NV_ENC_SUCCESS) return false;
    bitstreamBuffer_ = bs.bitstreamBuffer;

    std::printf("[NVENC] initialized %dx%d @ %d fps, %d kbps CBR, ultra-low-latency\n",
                config.width, config.height, config.fps, config.bitrateKbps);
    return true;
}

bool EncoderEngine::SubmitFrame(ID3D11Texture2D* frame, int64_t captureUs) {
    // Queue depth 1: if an encode is in flight, DROP this frame (spec §9/§10).
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true)) return false;

    const int64_t encodeStart = NowUs();
    context_->CopyResource(inputTexture_.Get(), frame);

    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = inputResource_;
    if (Api(api_)->nvEncMapInputResource(encoder_, &map) != NV_ENC_SUCCESS) {
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
        pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }

    // Synchronous encode: low-latency presets emit one output per input.
    NVENCSTATUS st = Api(api_)->nvEncEncodePicture(encoder_, &pic);
    if (st != NV_ENC_SUCCESS) {
        Api(api_)->nvEncUnmapInputResource(encoder_, map.mappedResource);
        busy_ = false;
        return false;
    }

    NV_ENC_LOCK_BITSTREAM lock{};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = bitstreamBuffer_;
    if (Api(api_)->nvEncLockBitstream(encoder_, &lock) == NV_ENC_SUCCESS) {
        if (onOutput_) {
            EncodedFrame out;
            out.data = static_cast<const uint8_t*>(lock.bitstreamBufferPtr);
            out.size = lock.bitstreamSizeInBytes;
            out.keyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR;
            out.captureUs = captureUs;
            out.encodeDurationUs = NowUs() - encodeStart;
            onOutput_(out); // hand straight to WebRTC — no intermediate queue
        }
        Api(api_)->nvEncUnlockBitstream(encoder_, bitstreamBuffer_);
    }
    Api(api_)->nvEncUnmapInputResource(encoder_, map.mappedResource);
    busy_ = false;
    return true;
}

void EncoderEngine::SetBitrate(int kbps) {
    std::lock_guard<std::mutex> guard(reconfigureMutex_);
    if (!encoder_ || !api_) return;
    NV_ENC_RECONFIGURE_PARAMS re{};
    re.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    // Fetch current params, adjust rate control only, keep the stream alive.
    NV_ENC_CONFIG cfg{};
    cfg.version = NV_ENC_CONFIG_VER;
    re.reInitEncodeParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    re.reInitEncodeParams.encodeConfig = &cfg;
    // Minimal reconfigure: bitrate ceiling change without IDR storm.
    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    cfg.rcParams.averageBitRate = kbps * 1000;
    cfg.rcParams.maxBitRate = kbps * 2000;
    re.resetEncoder = 0;
    re.forceIDR = 0;
    Api(api_)->nvEncReconfigureEncoder(encoder_, &re);
    config_.bitrateKbps = kbps;
    std::printf("[NVENC] bitrate reconfigured to %d kbps\n", kbps);
}

void EncoderEngine::Shutdown() {
    if (!api_ || !encoder_) return;
    if (bitstreamBuffer_) Api(api_)->nvEncDestroyBitstreamBuffer(encoder_, bitstreamBuffer_);
    if (inputResource_) Api(api_)->nvEncUnregisterResource(encoder_, inputResource_);
    Api(api_)->nvEncDestroyEncoder(encoder_);
    encoder_ = nullptr;
    delete static_cast<NV_ENCODE_API_FUNCTION_LIST*>(api_);  // void* must be cast before delete
    api_ = nullptr;
    if (hModule_) FreeLibrary(static_cast<HMODULE>(hModule_));
}

} // namespace remcote
