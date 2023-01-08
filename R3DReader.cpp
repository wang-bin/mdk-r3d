/*
 * Copyright (c) 2023 WangBin <wbsecg1 at gmail.com>
 * r3d plugin for libmdk
 */
#if defined(__x86_64) || defined(__x86_64__) || defined(__amd64) || /*vc*/defined(_M_X64) || defined(_M_AMD64)
# define HAS_R3D 1
#elif defined(__i386) || defined(__i386__) || /*vc*/defined(_M_IX86)
# define HAS_R3D 1
#endif
#if (HAS_R3D+0)

#include "mdk/FrameReader.h"
#include "mdk/MediaInfo.h"
#include "mdk/VideoFrame.h"
#include "mdk/AudioFrame.h"
#include "base/Hash.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include "R3DSDK.h"
#include "R3DSDKDecoder.h"
#include "R3DCxxAbi.h"

using namespace std;

MDK_NS_BEGIN

class R3DReader final : public FrameReader
{
public:
    R3DReader();
    ~R3DReader() override {
        if (unload_fut_.valid())
            unload_fut_.wait();
        if (init_) {
            R3DSDK::FinalizeSdk(); // FIXME: crash
        }
    }

    const char* name() const override { return "R3D"; }
    bool isSupported(const std::string& url, MediaType type) const override;
    void setTimeout(int64_t value, TimeoutCallback cb) override {}
    bool load() override;
    bool unload() override;
    bool seekTo(int64_t msec, SeekFlag flag, int id) override;
    int64_t buffered(int64_t* bytes = nullptr, float* percent = nullptr) const override;

protected:
    void onPropertyChanged(const std::string& /*key*/, const std::string& /*value*/) override;
private:
    bool readAt(uint64_t index);
    void parseDecoderOptions();
    bool setupDecoder();
    void setupDecodeJobs();

    R3DSDK::R3DDecodeJob* getJob(size_t index);
    void onJobComplete(R3DSDK::R3DDecodeJob *job, R3DSDK::R3DStatus status);

    struct UserData {
        R3DReader* reader = nullptr;
        uint64_t index = 0;
        int seekId = 0;
        bool seekWaitFrame = true;
        VideoFrame frame;
    };

    bool init_ = false;
    future<void> unload_fut_;
    unique_ptr<R3DSDK::Clip> clip_;
    R3DSDK::R3DDecoder* dec_ = nullptr;
    mutex job_mtx_;
    vector<R3DSDK::R3DDecodeJob*> job_;
    vector<VideoFrame> frame_; // static frame pool, reduce mem allocation
    int frame_idx_ = 0; // current frame index in pool

    int gpu_ = OPTION_RED_OPENCL;
    PixelFormat format_ = PixelFormat::BGRX;
    int copy_ = 1; // copy gpu resources
    R3DSDK::VideoDecodeMode mode_ = R3DSDK::DECODE_FULL_RES_PREMIUM;
    uint32_t scaleToW_ = 0; // closest down scale to target width
    uint32_t scaleToH_ = 0;
    int64_t duration_ = 0;
    int64_t frames_ = 0;
    atomic<int> seeking_ = 0;
    atomic<uint64_t> index_ = 0; // for stepping frame forward/backward

    NativeVideoBufferPoolRef pool_;
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
    //case PixelType_10Bit_DPX_MethodB: return PixelFormat::;
    //case PixelType_12Bit_BGR_Interleaved: return PixelFormat::;
    case PixelType_8Bit_BGR_Interleaved: return PixelFormat::BGR24; // ?
    //case PixelType_HalfFloat_RGB_Interleaved: return PixelFormat::RGBF16;
    //case PixelType_HalfFloat_RGB_ACES_Int: return PixelFormat::RGBF16;
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
    case PixelFormat::BGRX:
    case PixelFormat::BGRA: return PixelType_8Bit_BGRA_Interleaved;
    case PixelFormat::BGR24: return PixelType_8Bit_BGR_Interleaved;
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
        return R3DSDK::DECODE_HALF_RES_GOOD;
    // DECODE_ROCKET_CUSTOM_RES for rocket?
    return R3DSDK::DECODE_FULL_RES_PREMIUM;
}

static uint32_t Scale(uint32_t x, R3DSDK::VideoDecodeMode mode)
{
    switch (mode) {
    case R3DSDK::DECODE_HALF_RES_GOOD: return x/2;
    case R3DSDK::DECODE_QUARTER_RES_GOOD: return x/4;
    case R3DSDK::DECODE_EIGHT_RES_GOOD: return x/8;
    case R3DSDK::DECODE_SIXTEENTH_RES_GOOD: return x/16;
    default: return x;
    }
}

R3DReader::R3DReader()
    : FrameReader()
{
    const auto ret = R3DSDK::InitializeSdk(".", OPTION_RED_DECODER);
    init_ = ret == R3DSDK::ISInitializeOK;
    if (ret != R3DSDK::ISInitializeOK) {
        clog << "R3D InitializeSdk error: " << ret << endl;
        return;
    }
    clog << R3DSDK::GetSdkVersion() << endl;
}

bool R3DReader::isSupported(const std::string& url, MediaType type) const
{
    if (url.empty())
        return true;
    auto dot = url.rfind('.');
    if (dot == string::npos)
        return true;
    string s = url.substr(dot + 1);
    transform(s.begin(), s.end(), s.begin(), [](unsigned char c){
        return std::tolower(c);
    });
    return s == "r3d";
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

    if (!setupDecoder())
        return false;

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

    MediaInfo info;
    to(info, clip_.get());
    info.video[0].codec.format = format_;
    clog << info << endl;
    duration_ = info.video[0].duration;
    frames_ = info.video[0].frames;

    changed(info); // may call seek for player.prepare(), duration_, frames_ and SetCallback() must be ready
    update(MediaStatus::Loaded);

    updateBufferingProgress(0);

    if (state() == State::Stopped) // start with pause
        update(State::Running);

    setupDecodeJobs();

    if (seeking_ == 0 && !readAt(0)) // prepare(pos) will seek in changed(MediaInfo)
        return false;

    return true;
}

bool R3DReader::unload()
{
    update(MediaStatus::Unloaded);
    if (!clip_) {
        update(State::Stopped);
        return false;
    }

    for (auto j : job_) {
        R3DSDK::R3DDecoder::ReleaseDecodeJob(j);
    }
    job_.clear();
    if (dec_)
        R3DSDK::R3DDecoder::ReleaseDecoder(dec_);
    dec_ = nullptr;
    clip_.reset();
    frames_ = 0;
    update(State::Stopped);
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
    clog << seeking_ << " Seek to index: " << index << " from " << index_<< endl;
    updateBufferingProgress(0);

    auto job = getJob(index);
    if (!job)
        return false;
    auto data = (UserData*)job->privateData;
    data->seekId = id;
    //data->seekWaitFrame = !test_flag(flag & SeekFlag::IOCompleteCallback); // FIXME:
    const auto status = dec_->decode(job);
    if (status != R3DSDK::R3DStatus_Ok) {
        clog << "decode error: " << status << endl;
        delete data;
        job->privateData = nullptr;
        return false;
    }

    return true;
}

int64_t R3DReader::buffered(int64_t* bytes, float* percent) const
{
    return 0;
}

bool R3DReader::readAt(uint64_t index)
{
    if (!test_flag(mediaStatus(), MediaStatus::Loaded))
        return false;
    if (!clip_)
        return false;

    auto job = getJob(index);
    if (!job)
        return false;
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
    R3DSDK::R3DDecoderOptions *options = nullptr;
	R3DSDK::R3DStatus status = R3DSDK::R3DDecoderOptions::CreateOptions(&options);
    if (status != R3DSDK::R3DStatus_Ok) {
        clog << "R3DDecoderOptions::CreateOptions error: " << status << endl;
        return false;
    }
    // TODO:
	options->setMemoryPoolSize(1024);           // 1024+
	options->setGPUMemoryPoolSize(1024);        // 1024+
	options->setGPUConcurrentFrameCount(1);     // 1~3
	//options->setScratchFolder("");            //empty string disables scratch folder. c++ abi
	options->setDecompressionThreadCount(0);    //cores - 1 is good if you are a gui based app.
	options->setConcurrentImageCount(0);        //threads to process images/manage state of image processing.
    //options->useRRXAsync(true);

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
    frame_.resize(simultaneousJobs);
    for (int i = 0; i < simultaneousJobs; ++i) {
		R3DSDK::R3DDecodeJob *job = nullptr;
		R3DSDK::R3DDecoder::CreateDecodeJob(&job);
        job->clip = clip_.get();
        job->mode = mode_;
        job->pixelType = from(format_);

        VideoFrame frame(scaleToW_, scaleToH_, format_);
        frame.setBuffers(nullptr); // requires 16bytes aligned. already 64bytes aligned
        frame_[i] = frame;
        job->bytesPerRow = frame.buffer()->stride();
        job->outputBuffer = frame.buffer()->data();
        job->outputBufferSize = frame.format().bytesPerFrame(scaleToW_, scaleToH_);
        job->privateData = nullptr;
        job->videoFrameNo = 0;
        job->videoTrackNo = 0;
        job->imageProcessingSettings = new R3DSDK::ImageProcessingSettings();
		clip_->GetDefaultImageProcessingSettings(*(job->imageProcessingSettings));
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
    updateBufferingProgress(100);

    auto index = data->index;
    auto seekId = data->seekId;
    auto seekWaitFrame = data->seekWaitFrame;
    auto frame = data->frame;
    delete data;
    job->privateData = nullptr;

    if (index == frames_ - 1) {
        update(MediaStatus::Loaded|MediaStatus::End); // Options::ContinueAtEnd
    }

    index_ = index; // update index_ before seekComplete because pending seek may be executed in seekCompleted
    if (seekId > 0 && seekWaitFrame) {
        seeking_--;
        if (seeking_ > 0 && seekId == 0) {
            seekComplete(duration_ * index / frames_, seekId); // may create a new seek
            clog << "onJobComplete drop @" << index << endl;
            return;
        }
        seekComplete(duration_ * index / frames_, seekId); // may create a new seek
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
            unload_fut_ = async(launch::async, [=]{
                unload();
                });
        }
        return;
    }
    // frameAvailable() will wait in pause state, and return when seeking, do not read the next index
    if (accepted && seeking_ == 0 && state() == State::Running && test_flag(mediaStatus() & MediaStatus::Loaded)) // seeking_ > 0: new seek created by seekComplete when continuously seeking
        readAt(index + 1);
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
        } else if ("metal" == val) {
        } else if ("opencl" == val) {
        } else if ("cuda" == val) {
        } else {
        }
    }
        return;
    case "copy"_svh:
        copy_ = stoi(val);
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
                mode_ = R3DSDK::DECODE_HALF_RES_GOOD;
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
    }
}


void register_framereader_r3d() {
    FrameReader::registerOnce("r3d", []{return new R3DReader();});
}
MDK_NS_END

extern "C" MDK_API int mdk_plugin_load() {
    using namespace MDK_NS;
    register_framereader_r3d();
    return abiVersion();
}
#endif // (HAS_R3D+0)