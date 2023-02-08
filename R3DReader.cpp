/*
 * Copyright (c) 2023 WangBin <wbsecg1 at gmail.com>
 * r3d plugin for libmdk
 */
#include "mdk/FrameReader.h"
#include "mdk/MediaInfo.h"
#include "mdk/VideoFrame.h"
#include "mdk/AudioFrame.h"
#include "base/ByteArray.h"
//#include "base/fmt.h"
#include "base/Hash.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string_view>
#include <thread>
#include "R3DSDK.h"
#include "R3DSDKDecoder.h"
#include "R3DCxxAbi.h"
#include "Debayer.h"
#if (__APPLE__ + 0) || (__linux__ + 0)
#include <sys/resource.h>
#endif

// TODO: 1 job + frame pool(mutex based)
// TODO: safe queue

using namespace std;

MDK_NS_BEGIN

class R3DReader final : public FrameReader
{
public:
    enum Decompress {
        R3D,
        Async,
        Gpu,
        Cpu,
    };

    R3DReader();
    ~R3DReader() override {
        if (output_thread_.joinable())
            output_thread_.join();
        if (init_) {
            //R3DSDK::FinalizeSdk(); // FIXME: crash
        }
    }

    const char* name() const override { return "R3D"; }
    void setTimeout(int64_t value, TimeoutCallback cb) override {}
    bool load() override;
    bool unload() override;
    bool seekTo(int64_t msec, SeekFlag flag, int id) override;
    int64_t buffered(int64_t* bytes = nullptr, float* percent = nullptr) const override;

protected:
    void onPropertyChanged(const std::string& /*key*/, const std::string& /*value*/) override;
private:
    bool readAt(uint64_t index, int seekId = -1, SeekFlag flag = SeekFlag::Default);
    void parseDecoderOptions();
    bool setupDecoder();
    void setupDecodeJobs();

    R3DSDK::R3DDecodeJob* getJob(size_t index);
    void onJobComplete(R3DSDK::R3DDecodeJob *job, R3DSDK::R3DStatus status);

    R3DSDK::AsyncDecompressJob* getDecompressJob(size_t index);
    void onJobComplete(R3DSDK::AsyncDecompressJob *job, R3DSDK::DecodeStatus status);

    void outputLoop();

    struct UserData {
        R3DReader* reader = nullptr;
        uint64_t index = 0;
        int seekId = 0;
        bool seekWaitFrame = true;
        VideoFrame frame;
        size_t decompressIndex = 0;
        void* debayerJob = nullptr;
        R3DSDK::VideoDecodeJob* swJob = nullptr;
    };

    R3DSDK::VideoDecodeJob* getVideoDecodeJob(size_t index, UserData* data) {
        frame_idx_ = (frame_idx_+1) % (int)sw_job_.size();
        data->index = index;
        data->frame = frame_[frame_idx_];
        return &sw_job_[frame_idx_];
    }

    void process(const UserData& data);

    void push(const UserData& data) {
        unique_lock lock(output_mtx_);
        outputs_.push(data);
        output_cv_.notify_one();
    }

    bool pop(UserData& data) {
        unique_lock lock(output_mtx_);
        if (!output_running_) // unload
            return false;
        if (outputs_.empty())
            output_cv_.wait(lock);
        if (outputs_.empty())
            return false;
        data = outputs_.front();
        outputs_.pop();
        return true;
    }

    bool init_ = false;
    mutex job_mtx_;
    unique_ptr<R3DSDK::Clip> clip_;
    R3DSDK::R3DDecoder* dec_ = nullptr;
    vector<R3DSDK::R3DDecodeJob*> job_;
    vector<VideoFrame> frame_; // static frame pool, reduce mem allocation
    int frame_idx_ = 0; // current frame index in pool

    bool copy_ = false; // try 0-copy when possible(async/gpu decoder, not R3DDecoder)
    int gpu_ = OPTION_RED_CUDA|OPTION_RED_OPENCL|OPTION_RED_METAL;
    Decompress decompress_ = Decompress::R3D; // if not R3D, gpu_ will be set to a supported value
    PixelFormat format_ = PixelFormat::BGRX;
    R3DSDK::ImagePipeline ipp_ = R3DSDK::Full_Graded;
    R3DSDK::VideoDecodeMode mode_ = R3DSDK::DECODE_FULL_RES_PREMIUM;
    uint32_t scaleToW_ = 0; // closest down scale to target width
    uint32_t scaleToH_ = 0;
    int64_t duration_ = 0;
    int64_t frames_ = 0;
    atomic<int> seeking_ = 0;
    atomic<uint64_t> index_ = 0; // for stepping frame forward/backward
    R3DSDK::ImageProcessingSettings ipsettings_;

    vector<R3DSDK::AsyncDecompressJob*> decompress_job_;
    vector<ByteArray> decompress_buf_;
    unique_ptr<R3DSDK::AsyncDecoder> async_dec_;
    unique_ptr<R3DSDK::GpuDecoder> gpu_dec_;
    GpuDebayer::Ptr debayer_;

    vector<R3DSDK::VideoDecodeJob> sw_job_;

// need a thread to process output frames. if do it in decode job complete callback, may have dead lock when range loop starts
    atomic<bool> output_running_ = false;
    thread output_thread_;
    queue<UserData> outputs_;
    condition_variable output_cv_;
    mutex output_mtx_;
};


void to(MediaInfo& info, const R3DSDK::Clip* clip)
{
    info.format = "r3d";

    info.streams = clip->VideoTrackCount();
    if (info.streams <= 0)
        return;

    const auto m = clip->MetadataCount();
    for (size_t i = 0; i < m; ++i) {
        auto key = MetadataItemKey(clip, i);
        auto val = MetadataItemAsString(clip, i);
        info.metadata.emplace(key, val);
        free(key);
        free(val);
    }

    VideoCodecParameters vcp;
    vcp.codec = "r3d";
    vcp.width = clip->Width();
    vcp.height = clip->Height();
    vcp.frame_rate = clip->VideoAudioFramerate();
    VideoStreamInfo vsi;
    vsi.index = 0;
    vsi.frames = clip->VideoFrameCount();
    vsi.duration = vsi.frames * (1000.0 / vcp.frame_rate);
    vsi.codec = vcp;
    info.video.reserve(clip->VideoTrackCount());
    info.video.push_back(vsi);
    info.duration = vsi.duration;

    if (clip->AudioChannelCount() == 0)
        return;
    info.streams++;
    AudioCodecParameters acp;
    AudioStreamInfo asi;
    asi.index = 1;
    acp.codec = "pcm";
    acp.format = AudioFormat::SampleFormat::S24; // clip->MetadataItemAsInt(RMD_SAMPLE_SIZE) is always 24
    acp.channels = clip->AudioChannelCount();
    acp.channel_layout = (uint64_t)clip->MetadataItemAsInt(R3DSDK::RMD_CHANNEL_MASK);
    acp.sample_rate = clip->MetadataItemAsInt(R3DSDK::RMD_SAMPLERATE); // always 48000

    asi.frames = clip->AudioSampleCount();
    asi.duration = asi.frames * 1000 / acp.sample_rate;
    asi.codec = acp;
    info.audio.reserve(1);
    info.audio.push_back(asi);

    info.duration = std::max<int64_t>(info.duration, asi.duration);
}

PixelFormat to(R3DSDK::VideoPixelType fmt)
{
    using namespace R3DSDK;
    switch (fmt) {
    case PixelType_16Bit_RGB_Planar: return PixelFormat::RGBP16;
    case PixelType_16Bit_RGB_Interleaved: return PixelFormat::RGB48; // "rgb48le" NOT RECOMMENDED! 3 channel formats are not directly supported by gpu
    case PixelType_8Bit_BGRA_Interleaved: return PixelFormat::BGRX;
    //case PixelType_10Bit_DPX_MethodB: return PixelFormat::X2RGB10;
    //case PixelType_12Bit_BGR_Interleaved: return PixelFormat::;
    case PixelType_8Bit_BGR_Interleaved: return PixelFormat::BGR24; // ?
    case PixelType_HalfFloat_RGB_Interleaved: return PixelFormat::RGBF16LE;
    case PixelType_HalfFloat_RGB_ACES_Int: return PixelFormat::RGBF16;
    default:
        return PixelFormat::Unknown;
    }
}

R3DSDK::VideoPixelType from(PixelFormat fmt)
{
    using namespace R3DSDK;
    switch (fmt) {
    case PixelFormat::RGBP16: return PixelType_16Bit_RGB_Planar;
    case PixelFormat::RGB48: return PixelType_16Bit_RGB_Interleaved; // NOT RECOMMENDED! 3 channel formats are not directly supported by gpu
    //case PixelFormat::X2RGB10: return PixelType_10Bit_DPX_MethodB;
    case PixelFormat::BGRX:
    case PixelFormat::BGRA: return PixelType_8Bit_BGRA_Interleaved;
    case PixelFormat::BGR24: return PixelType_8Bit_BGR_Interleaved;
    case PixelFormat::RGBF16LE: return PixelType_HalfFloat_RGB_Interleaved;
    default:
        return PixelType_8Bit_BGRA_Interleaved;
    }
}

static R3DSDK::VideoDecodeMode GetScaleMode(uint32_t w, uint32_t h, uint32_t W, uint32_t H)
{
    const auto scaleX = w > 0 ? (float)W / (float)w : 1.0f;
    const auto scaleY = h > 0 ? (float)H / (float)h : 1.0f;
    const auto scale = std::max<float>(scaleX, scaleY);
    if (scale >= 12.0f) // (16+8)/2
        return R3DSDK::DECODE_SIXTEENTH_RES_GOOD;
    if (scale >= 6.0f) // (8+4)/2
        return R3DSDK::DECODE_EIGHT_RES_GOOD;
    if (scale >= 3.0f) // (4+2)/2
        return R3DSDK::DECODE_QUARTER_RES_GOOD;
    if (scale >= 1.5f) // (2+1)/2
        return R3DSDK::DECODE_HALF_RES_PREMIUM;
    // DECODE_ROCKET_CUSTOM_RES for rocket?
    return R3DSDK::DECODE_FULL_RES_PREMIUM;
}

static uint32_t Scale(uint32_t x, R3DSDK::VideoDecodeMode mode)
{
    switch (mode) {
    case R3DSDK::DECODE_HALF_RES_PREMIUM:
    case R3DSDK::DECODE_HALF_RES_GOOD: return x/2;
    case R3DSDK::DECODE_QUARTER_RES_GOOD: return x/4;
    case R3DSDK::DECODE_EIGHT_RES_GOOD: return x/8;
    case R3DSDK::DECODE_SIXTEENTH_RES_GOOD: return x/16;
    default: return x;
    }
}

static auto init_sdk()
{
#if (__APPLE__ + 0) || (__linux__ + 0)
    // Increase open file limit, because R3D decoder opens /dev/urandom
    // per each thread, and this crashes on Macbook M1 Max
    struct rlimit limit;
    if (::getrlimit(RLIMIT_NOFILE, &limit) == 0) {
        if (limit.rlim_cur < 4096) {
            limit.rlim_cur = 4096;
            if (limit.rlim_max < 4096)
                limit.rlim_max = 4096;
            if (::setrlimit(RLIMIT_NOFILE, &limit) != 0) {
                clog << "Failed to set RLIMIT_NOFILE to 4096!" << endl;
            }
        }
    }
#endif

    string sdk_dir = "."; // TODO: default is Framework/Plugins dir, dll dir, so dir
    const auto v = GetGlobalOption("R3DSDK_DIR");
    if (const auto s = get_if<string>(&v))
        sdk_dir = *s;
    else if (const auto s = get_if<const char*>(&v))
        sdk_dir = *s;
    if (const auto s = getenv("R3DSDK_DIR"))
        sdk_dir = s;
    // InitializeSdk again w/o FinalizeSdk will crash. So init as much components(via flags) as possible only once and then all possible features are available
    // a flag will try to load corresponding runtime library, for example OPTION_RED_DECODER is REDDecoder.dylib/REDDecoder-x64.dll
    int flags = OPTION_RED_DECODER|OPTION_RED_OPENCL|OPTION_RED_CUDA; // doc says DECODER can not combine with OPENCL/CUDA, but seems ok in my tests
#if (__APPLE__ + 0)
    flags |= OPTION_RED_METAL;
#endif
    auto ret = R3DSDK::InitializeSdk(sdk_dir.data(), flags); // TODO: depends on decoder option, fallback to default if failed
    if (ret == R3DSDK::ISRedCudaLibraryNotFound) {
        flags &= ~OPTION_RED_CUDA;
        ret = R3DSDK::InitializeSdk(sdk_dir.data(), flags);
    }
    if (ret == R3DSDK::ISRedOpenCLLibraryNotFound) {
        flags &= ~OPTION_RED_OPENCL;
        ret = R3DSDK::InitializeSdk(sdk_dir.data(), flags);
    }
    return ret;
}

R3DReader::R3DReader()
    : FrameReader()
{
    static const auto ret = init_sdk();
    init_ = ret == R3DSDK::ISInitializeOK;
    if (ret != R3DSDK::ISInitializeOK) {
        clog << "R3D InitializeSdk error: " << ret << endl;
        return;
    }
    clog << R3DSDK::GetSdkVersion() << endl;
}

bool R3DReader::load()
{
    if (!init_)
        return false;
    parseDecoderOptions();

    clip_ = make_unique<R3DSDK::Clip>(url().data());
    if (clip_->Status() != R3DSDK::LoadStatus::LSClipLoaded) {
        clog << "Load error: " << clip_->Status();
        return false;
    }

    if (!setupDecoder()) {
        clog << "R3D will use software decoder" << endl;
    }

    if (output_thread_.joinable())
        output_thread_.join();
    output_thread_ = thread([=]{
        outputLoop();
    });

    MediaEvent e{};
    e.category = "decoder.video";
    e.detail = "r3d";
    dispatchEvent(e);

 // TODO: change scale when playing
    if (scaleToW_ > 0 || scaleToH_ > 0) {
        mode_ = GetScaleMode(scaleToW_, scaleToH_, clip_->Width(), clip_->Height());
    }
    scaleToW_ = Scale(clip_->Width(), mode_);
    scaleToH_ = Scale(clip_->Height(), mode_);
    clip_->GetDefaultImageProcessingSettings(ipsettings_);
    ipsettings_.ImagePipelineMode = ipp_;
    ipsettings_.HdrPeakNits = 1000;
    ipsettings_.CdlEnabled = true;
    ipsettings_.OutputToneMap = R3DSDK::ToneMap_None;
    //clog << fmt::to_string("clip ImageProcessingSettings: ImagePipelineMode=%d, ExposureAdjust=%f, CdlSaturation=%f, CdlEnabled:%d, OutputToneMap=%d, HdrPeakNits=%u"
    //    , ipsettings_.ImagePipelineMode, ipsettings_.ExposureAdjust, ipsettings_.CdlSaturation, ipsettings_.CdlEnabled, ipsettings_.OutputToneMap, ipsettings_.HdrPeakNits) << endl;

    MediaInfo info;
    to(info, clip_.get());
    info.video[0].codec.format = format_;
    clog << info << endl;
    duration_ = info.video[0].duration;
    frames_ = info.video[0].frames;

// parameters are ready, prepare jobs here for seeking+decoding in changed(info)
    setupDecodeJobs();

    changed(info); // may call seek for player.prepare(), duration_, frames_ and SetCallback() must be ready
    update(MediaStatus::Loaded);

    updateBufferingProgress(0);

    if (state() == State::Stopped) // start with pause
        update(State::Running);

    if (seeking_ == 0 && !readAt(0)) // prepare(pos) will seek in changed(MediaInfo)
        return false;

    return true;
}

bool R3DReader::unload()
{
    {
        scoped_lock lock(output_mtx_);
        output_running_ = false;
        output_cv_.notify_one();
    }

    lock_guard lock(job_mtx_);
    update(MediaStatus::Unloaded);
    if (!clip_) {
        update(State::Stopped);
        return false;
    }
    for (auto& job : decompress_job_) {
        job->AbortDecode = true;
    }
    if (async_dec_) {
        async_dec_->Close();
        async_dec_.reset();
    }
    if (gpu_dec_) {
        gpu_dec_->Close();
        gpu_dec_.reset();
    }
    for (auto& job : decompress_job_) {
        delete job;
        job = nullptr;
    }
    decompress_job_.clear();
    debayer_.reset();

    if (dec_)
        R3DSDK::R3DDecoder::ReleaseDecoder(dec_); // FIXME: may block here
    dec_ = nullptr;

    for (auto j : job_) {
        R3DSDK::R3DDecoder::ReleaseDecodeJob(j);
    }
    job_.clear();
    sw_job_.clear();
    clip_.reset();
    frames_ = 0;
    update(State::Stopped);
    { // onJobComplete() after output thread finished
        unique_lock lock(output_mtx_);
        outputs_ = {};
    }
    return true;
}

bool R3DReader::seekTo(int64_t msec, SeekFlag flag, int id)
{
    if (!clip_)
        return false;
    // TODO: cancel running decodeProcessJob
    // TODO: seekCompelete if error later
    if (msec > duration_) // msec can be INT64_MAX, avoid overflow
        msec = duration_;
    const auto dt = (duration_ + frames_ - 1) / frames_;
    auto index = std::min<uint64_t>(frames_ * (msec + dt) / duration_, frames_ - 1);
    if (test_flag(flag, SeekFlag::FromNow|SeekFlag::Frame)) {
        if (msec == 0) {
            seekComplete(duration_ * index_ / frames_, id);
            return true;
        }
        index = (uint64_t)clamp<int64_t>((int64_t)index_ + msec, 0, frames_ - 1);
    }
    seeking_++;
    clog << seeking_ << " Seek to index: " << index << " from " << index_ << " #" << this_thread::get_id()<< endl;
    updateBufferingProgress(0);

    return readAt(index, id, flag);
}

int64_t R3DReader::buffered(int64_t* bytes, float* percent) const
{
    return 0;
}

bool R3DReader::readAt(uint64_t index, int seekId, SeekFlag flag)
{
    if (!clip_)
        return false;

    if (async_dec_ || gpu_dec_) {
        auto job = getDecompressJob(index);
        if (!job)
            return false;
        if (seekId > 0) {
            auto data = (UserData*)job->PrivateData;
            data->seekId = seekId;
            //data->seekWaitFrame = !test_flag(flag & SeekFlag::IOCompleteCallback); // FIXME:
        }
        R3DSDK::DecodeStatus status = R3DSDK::DSDecodeOK;
        if (async_dec_) {
            status = async_dec_->DecodeForGpuSdk(*job);
        } else if (gpu_dec_) {
            status = gpu_dec_->DecodeForGpuSdk(*job);
        }
        if (status != R3DSDK::DSDecodeOK) {
            clog << "decompress error: " << status << endl;
            delete (UserData*)job->PrivateData;
            job->PrivateData = nullptr;
            return false;
        }
        return true;
    }
    if (!dec_) {
        UserData data{};
        data.swJob = getVideoDecodeJob(index, &data);
        if (seekId > 0) {
            data.seekId = seekId;
            data.seekWaitFrame = !test_flag(flag & SeekFlag::IOCompleteCallback);
            if (!data.seekWaitFrame) { // seek in frameAvailable() and will wait seek finish, dead wait
                seeking_--;
                seekComplete(duration_ * index / frames_, seekId);
            }
            unique_lock lock(output_mtx_);
            outputs_ = {};
        }
        push(data);
        return true;
    }
    auto job = getJob(index);
    if (!job)
        return false;
    if (seekId > 0) {
        auto data = (UserData*)job->privateData;
        data->seekId = seekId;
        //data->seekWaitFrame = !test_flag(flag & SeekFlag::IOCompleteCallback); // FIXME:
    }
    const auto status = dec_->decode(job);
    if (status != R3DSDK::R3DStatus_Ok) {
        clog << "decode error: " << status << endl;
        delete (UserData*)job->privateData;
        job->privateData = nullptr;
        return false;
    }
    return true;
}

void R3DReader::parseDecoderOptions()
{
    // decoder: name:key1=val1:key2=val2
    for (const auto& i : decoders(MediaType::Video)) {
        if (auto colon = i.find(':'); colon != string::npos) {
            if (string_view(i).substr(0, colon) == name()) {
                parse(i.data() + colon);
                return;
            }
        }
    }
}

bool R3DReader::setupDecoder()
{
    if (decompress_ == Decompress::Cpu || gpu_ == OPTION_RED_NONE)
        return true;
    if (decompress_ == Decompress::R3D) {
        if (gpu_ & OPTION_RED_METAL)
            gpu_ &= ~OPTION_RED_METAL;
#if (__APPLE__ + 0)
        if (gpu_ & OPTION_RED_CUDA)
            gpu_ &= ~OPTION_RED_CUDA;
#endif
    }

    if (decompress_ != Decompress::R3D) {
        debayer_ = GpuDebayer::create(gpu_);
    }
    if (decompress_ == Decompress::Gpu) {
        if (auto ret = R3DSDK::GpuDecoder::DecodeSupportedForClip(*clip_.get()); ret != R3DSDK::DSDecodeOK) {
            clog << ret << " R3DSDK::GpuDecoder does not support current clip, fallback to AsyncDecoder" << endl;
            decompress_ = Decompress::Async;
        } else {
            gpu_dec_ = make_unique<R3DSDK::GpuDecoder>();
            gpu_dec_->Open();
            return true;
        }
    }
    if (decompress_ == Decompress::Async) {
        async_dec_ = make_unique<R3DSDK::AsyncDecoder>();
        async_dec_->Open();
        return true;
    }

    if (gpu_ == OPTION_RED_NONE)
        gpu_ = OPTION_RED_OPENCL;

    R3DSDK::R3DDecoderOptions *options = nullptr;
    R3DSDK::R3DStatus status = R3DSDK::R3DDecoderOptions::CreateOptions(&options);
    if (status != R3DSDK::R3DStatus_Ok) {
        clog << "R3DDecoderOptions::CreateOptions error: " << status << endl;
        return false;
    }
    // TODO:
    options->setMemoryPoolSize(4096);           // 1024+
    options->setGPUMemoryPoolSize(4096);        // 1024+
    options->setGPUConcurrentFrameCount(1);     // 1~3
    //options->setScratchFolder("");            //empty string disables scratch folder. c++ abi
    options->setDecompressionThreadCount(0);    //cores - 1 is good if you are a gui based app.
    options->setConcurrentImageCount(0);        //threads to process images/manage state of image processing.
    options->useRRXAsync(true);

    status = SetupCudaCLDevices(options, gpu_);
    if (status != R3DSDK::R3DStatus_Ok) {
        clog << "Setup Cuda/OpenCL Devices error: " << status << endl;
        return false;
    }

    status = R3DSDK::R3DDecoder::CreateDecoder(options, &dec_);

    R3DSDK::R3DDecoderOptions::ReleaseOptions(options);
    if (status != R3DSDK::R3DStatus_Ok) {
        clog << "R3DDecoder::CreateDecoder error: " << status << endl;
        return false;
    }

    return true;
}

void R3DReader::setupDecodeJobs()
{
    const int simultaneousJobs = 8; // frame queue size in renderer is 4
    if (async_dec_ || gpu_dec_) {
        decompress_buf_.resize(simultaneousJobs);
        for (int i = 0; i < simultaneousJobs; ++i) {
            auto job = new R3DSDK::AsyncDecompressJob();
            job->Clip = clip_.get();
            job->Mode = mode_;
            job->VideoFrameNo = 0;
            job->VideoTrackNo = 0;
            job->AbortDecode = false;
            size_t outSize = 0;
            if (decompress_ == Decompress::Async) {
                outSize = R3DSDK::AsyncDecoder::GetSizeBufferNeeded(*job);
            } else {
                outSize = R3DSDK::GpuDecoder::GetSizeBufferNeeded(*job);
            }
            if (outSize == 0) {
                clog << "Failed to get decompress job output buffer size" << endl;
                return;
            }
            decompress_buf_[i] = ByteArray(outSize);
            job->OutputBuffer = decompress_buf_[i].data();
            job->OutputBufferSize = outSize;
            job->Callback = [](R3DSDK::AsyncDecompressJob* job, R3DSDK::DecodeStatus decodeStatus) {
                auto data = (UserData*)job->PrivateData;
                data->reader->onJobComplete(job, decodeStatus);
            };
            decompress_job_.push_back(job);
        }
        return;
    }

    frame_.resize(simultaneousJobs);
    for (int i = 0; i < simultaneousJobs; ++i) {
        VideoFrame frame(scaleToW_, scaleToH_, format_);
        frame.setBuffers(nullptr); // requires 16bytes aligned. already 64bytes aligned
        frame_[i] = frame;
    }
    if (!dec_) {
        for (int i = 0; i < simultaneousJobs; ++i) {
            R3DSDK::VideoDecodeJob job{};
            job.Mode = mode_;
            job.PixelType = from(format_);
            const auto& frame = frame_[i];
            job.OutputBuffer = frame.buffer()->data();
            job.BytesPerRow = frame.buffer()->stride();
            job.OutputBufferSize = frame.format().bytesPerFrame(scaleToW_, scaleToH_);
            job.ImageProcessing = &ipsettings_;
            sw_job_.push_back(std::move(job));
        }
        return;
    }
    for (int i = 0; i < simultaneousJobs; ++i) {
        R3DSDK::R3DDecodeJob *job = nullptr;
        R3DSDK::R3DDecoder::CreateDecodeJob(&job);
        job->clip = clip_.get();
        job->mode = mode_;
        job->pixelType = from(format_);
        const auto& frame = frame_[i];
        job->bytesPerRow = frame.buffer()->stride();
        job->outputBuffer = frame.buffer()->data();
        job->outputBufferSize = frame.format().bytesPerFrame(scaleToW_, scaleToH_);
        job->privateData = nullptr;
        job->videoFrameNo = 0;
        job->videoTrackNo = 0;
        job->imageProcessingSettings = &ipsettings_;
        job->callback = [](R3DSDK::R3DDecodeJob *job, R3DSDK::R3DStatus status) {
            auto data = (UserData*)job->privateData;
            data->reader->onJobComplete(job, status);
        };
        job_.push_back(job);
    }
}

R3DSDK::R3DDecodeJob* R3DReader::getJob(size_t index)
{
    for (size_t i = 0; i < job_.size(); ++i) {
        auto n = (i + frame_idx_) % job_.size();
        auto j = job_[n];
        if (!j->privateData) {
            j->videoFrameNo = index;
            auto data = new UserData();
            data->reader = this;
            data->index = index;
            data->frame = frame_[n];
            j->privateData = data;
            frame_idx_++;
            return j;
        }
    }
    return nullptr;
}

void R3DReader::onJobComplete(R3DSDK::R3DDecodeJob *job, R3DSDK::R3DStatus status)
{
    auto data = (UserData*)job->privateData;
    if (status != R3DSDK::R3DStatus_Ok) {

    }

    updateBufferingProgress(100);

    const auto index = data->index;
    const auto seekId = data->seekId;
    const auto seekWaitFrame = data->seekWaitFrame;
    if (index == frames_ - 1) {
        update(MediaStatus::Loaded|MediaStatus::End); // Options::ContinueAtEnd
    }

    index_ = index; // update index_ before seekComplete because pending seek may be executed in seekCompleted
    if (seekId > 0 && seekWaitFrame) {
        seeking_--;
        seekComplete(duration_ * index / frames_, seekId); // may create a new seek
    }

    push(*data);

    delete data;
    job->privateData = nullptr;

}

R3DSDK::AsyncDecompressJob* R3DReader::getDecompressJob(size_t index)
{
    for (size_t i = 0; i < decompress_job_.size(); ++i) {
        auto n = (i + frame_idx_) % decompress_job_.size();
        auto j = decompress_job_[n];
        if (!j->PrivateData) {
            j->VideoFrameNo = index;
            auto data = new UserData();
            data->reader = this;
            data->index = index;
            data->decompressIndex = n;
            j->PrivateData = data;
            frame_idx_++;
            return j;
        }
    }
    return nullptr;
}

void R3DReader::onJobComplete(R3DSDK::AsyncDecompressJob *job, R3DSDK::DecodeStatus status)
{
    auto data = (UserData*)job->PrivateData;
    const auto index = data->index;
    const auto seekId = data->seekId;
    const auto seekWaitFrame = data->seekWaitFrame;
    const auto bufIdx = data->decompressIndex;
    job->PrivateData = nullptr; // TODO: when debayer done
    if (status != R3DSDK::DSDecodeOK) { // abort by user
        clog << "Decompress error: " << status << endl;
        delete data;
        return;
    }
    if (index == frames_ - 1) {
        update(MediaStatus::Loaded|MediaStatus::End); // Options::ContinueAtEnd
    }

    index_ = index; // update index_ before seekComplete because pending seek may be executed in seekCompleted
    if (seekId > 0 && seekWaitFrame) {
        seeking_--;
        seekComplete(duration_ * index / frames_, seekId); // may create a new seek
    }

    auto debayerJob = debayer_->createJob(decompress_buf_[bufIdx].constData(), decompress_buf_[bufIdx].size(), scaleToW_, scaleToH_, mode_, from(format_), &ipsettings_);
    if (!debayerJob) {
        clog << "Failed to create a debayer job" << endl;
        delete data;
        return;
    }
    debayer_->submit(debayerJob);

    data->debayerJob = debayerJob;
    push(*data);

    delete data;

    //readAt(index + 1); // TODO:
}

void R3DReader::process(const UserData& data)
{
    const auto index = data.index;
    const auto seekId = data.seekId;
    const auto seekWaitFrame = data.seekWaitFrame;
    VideoFrame frame;
    if (data.debayerJob) {
        lock_guard lock(job_mtx_); // debayer_ reset in unload() after wait done
        frame = debayer_->wait(data.debayerJob, copy_);
        debayer_->releaseJob(data.debayerJob);
    } else if (data.swJob) {
        lock_guard lock(job_mtx_);
        if (!clip_)
            return;;
        clip_->DecodeVideoFrame(data.index, *data.swJob);
        frame = data.frame;
        if (seekId > 0 && seekWaitFrame) {
            seeking_--;
            seekComplete(duration_ * index / frames_, seekId); // may create a new seek
        }
    } else {
        frame = data.frame;
    }

    if (seekId == 0 && seeking_ > 0 && seekWaitFrame) { // ?
        clog << "R3D decoded frame drop index@" << index << endl;
        return;
    }

    frame.setTimestamp(double(duration_ * index / frames_) / 1000.0);
    frame.setDuration((double)duration_/(double)frames_ / 1000.0);
    if (seekId > 0) {
        frameAvailable(VideoFrame(frame.format()).setTimestamp(frame.timestamp()));
    }
    bool accepted = frameAvailable(frame); // false: out of loop range and begin a new loop
    if (index == frames_ - 1 && seeking_ == 0 && accepted) {
        accepted = frameAvailable(VideoFrame().setTimestamp(TimestampEOS));
        if (accepted && !test_flag(options() & Options::ContinueAtEnd)) {
            unload();
        }
        return;
    }
    if (decompress_ != Decompress::R3D && decompress_ != Decompress::Cpu) {
        //return;
    }
    // frameAvailable() will wait in pause state, and return when seeking, do not read the next index
    if (accepted && seeking_ == 0 && state() == State::Running && test_flag(mediaStatus() & MediaStatus::Loaded)) // seeking_ > 0: new seek created by seekComplete when continuously seeking
        readAt(index + 1);
}

void R3DReader::outputLoop()
{
    output_running_ = true;
    while (output_running_) {
        UserData data;
        if (!pop(data))
            continue;
        process(data);
    }
    unique_lock lock(output_mtx_);
    outputs_ = {};
    clog << "R3D finish output loop" << endl;
}

void R3DReader::onPropertyChanged(const std::string& key, const std::string& val)
{
    const auto k = detail::fnv1a_32(key);
    switch (k) {
    case "format"_svh:
        format_ = VideoFormat::fromName(val.data());
        return;
    case "gpu"_svh: {
        if ("auto"sv == val) { // metal > cuda > opencl > cpu
            gpu_ = OPTION_RED_CUDA|OPTION_RED_OPENCL|OPTION_RED_METAL;
        } else if ("metal" == val) {
            gpu_ = OPTION_RED_METAL;
        } else if ("opencl" == val) {
            gpu_ = OPTION_RED_OPENCL;
        } else if ("cuda" == val) {
            gpu_ = OPTION_RED_CUDA;
        } else {
            gpu_ = OPTION_RED_NONE;
        }
    }
        return;
    case "copy"_svh:
        copy_ = stoi(val) > 0;
        return;
    case "scale"_svh: // 1/2, 1/4, 1/8, 1/16
    case "size"_svh: { // widthxheight or width(height=width)
        if (val.find('x') != string::npos) { // closest scale to target resolution
            char* s = nullptr;
            scaleToW_ = strtoul(val.data(), &s, 10);
            if (s && s[0] == 'x')
                scaleToH_ = strtoul(s + 1, nullptr, 10);
        } else if (val.find('/') != string::npos) { // closest scale to target resolution
            char* s = nullptr;
            auto x = strtoul(val.data(), &s, 10);
            if (s && s[0] == '/')
                x = strtoul(s + 1, nullptr, 10);
            if (x == 2)
                mode_ = R3DSDK::DECODE_HALF_RES_PREMIUM;
            else if (x == 4)
                mode_ = R3DSDK::DECODE_QUARTER_RES_GOOD;
            else if (x == 8)
                mode_ = R3DSDK::DECODE_EIGHT_RES_GOOD;
            else if (x == 16)
                mode_ = R3DSDK::DECODE_SIXTEENTH_RES_GOOD;
        } else {
            scaleToW_ = strtoul(val.data(), nullptr, 10);
            scaleToH_ = scaleToW_;
        }
    }
        return;
    case "ipp"_svh:
    case "image_pipeline"_svh: {
        if (val.find("primary") != string::npos)
            ipp_ = R3DSDK::Primary_Development_Only;
        else
            ipp_ = R3DSDK::Full_Graded;
    }
        return;
    case "decompress"_svh: {
        // gpu(4GB gpu vram, fallback to async/r3d if not supported DecodeSupportedForClip), async, r3d, rocket
        if (val == "async")
            decompress_ = Decompress::Async;
        else if (val == "gpu")
            decompress_ = Decompress::Gpu;
        else if (val == "cpu")
            decompress_ = Decompress::Cpu;
        else
            decompress_ = Decompress::R3D;
    }
        return;
    }
}


void register_framereader_r3d() {
    FrameReader::registerOnce("R3D", []{return new R3DReader();});
}
MDK_NS_END

extern "C" MDK_API int mdk_plugin_load() {
    using namespace MDK_NS;
    register_framereader_r3d();
    return abiVersion();
}
