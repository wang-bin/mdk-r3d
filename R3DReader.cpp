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
#include <iostream>
#include <memory>
#include <string_view>
#include "R3DSDK.h"

using namespace std;

MDK_NS_BEGIN

class R3DReader final : public FrameReader
{
public:
    R3DReader();
    ~R3DReader() override {
        if (init_) {
            R3DSDK::FinalizeSdk();
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
    void setDecoderOption(const char* key, const char* val);


    bool init_ = false;
    unique_ptr<R3DSDK::Clip> clip_;
    PixelFormat format_ = PixelFormat::BGRA;
    int copy_ = 1; // copy gpu resources
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
    case PixelType_8Bit_BGRA_Interleaved: return PixelFormat::BGRA;
    //case PixelType_10Bit_DPX_MethodB: return PixelFormat::;
    //case PixelType_12Bit_BGR_Interleaved: return PixelFormat::;
    case PixelType_8Bit_BGR_Interleaved: return PixelFormat::BGR24;
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
    case PixelFormat::BGRA: return PixelType_8Bit_BGRA_Interleaved;
    case PixelFormat::BGR24: return PixelType_8Bit_BGR_Interleaved;
    default:
        return PixelType_8Bit_BGRA_Interleaved;
    }
}


R3DReader::R3DReader()
    : FrameReader()
{
    const auto ret = R3DSDK::InitializeSdk(".", OPTION_RED_OPENCL);
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

    MediaEvent e{};
    e.category = "decoder.video";
    e.detail = "r3d";
    dispatchEvent(e);

    if (scaleToW_ > 0 || scaleToH_ > 0) {

    }

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