/*
 * Copyright (c) 2023 WangBin <wbsecg1 at gmail.com>
 * r3d plugin for libmdk
 */
#include "Debayer.h"
#include "R3DSDKMetal.h"
#include "mdk/VideoBuffer.h"
#include "mdk/VideoFormat.h"
#include "mdk/VideoFrame.h"
#include "mdk/global.h"
#include <memory>
#include <iostream>

using namespace std;
MDK_NS_BEGIN

static R3DSDK::EXT_METAL_API gApi{}; // TODO: pool

class MetalDebayer final : public GpuDebayer, protected R3DSDK::REDMetal
{
public:
    MetalDebayer() : R3DSDK::REDMetal(gApi) {
        dev_ = MTLCreateSystemDefaultDevice();
        cmdQ_ = [dev_ newCommandQueue];
        int err = 0;
        if (auto st = checkCompatibility(cmdQ_, err); st != R3DSDK::REDMetal::Status_Ok) {
            clog << "checkCompatibility failed: " << st << " error: " << err << endl;
        }
    }

    void* createJob(const void* hostMemInput, size_t hostMemSize, int width, int height, R3DSDK::VideoDecodeMode mode, R3DSDK::VideoPixelType pix, R3DSDK::ImageProcessingSettings* ips) override;
    void releaseJob(void* job) override {
        auto mtljob = (R3DSDK::DebayerMetalJob*)job;
        mtljob->raw_device_mem = nil;
        mtljob->output_device_image = nil;
        releaseDebayerJob(mtljob);
    }

    GpuDebayer::Status submit(void* job) override {
        int err;
        return (GpuDebayer::Status)processAsync(cmdQ_, (R3DSDK::DebayerMetalJob*)job, err);
    }

    VideoFrame wait(void* job) override;

    GpuDebayer::Status flush() override { return GpuDebayer::Status_Ok; }

private:
    id<MTLDevice> dev_;
    id<MTLCommandQueue> cmdQ_;
};

void* MetalDebayer::createJob(const void *hostMemInput, size_t hostMemSize, int width, int height, R3DSDK::VideoDecodeMode mode, R3DSDK::VideoPixelType pix, R3DSDK::ImageProcessingSettings *ips)
{
    auto job = createDebayerJob();
    job->raw_host_mem = (void*)hostMemInput;
    job->mode = mode;
    job->pixelType = pix;
    job->imageProcessingSettings = ips;
    job->batchMode = false;
    // TODO: reuse mtl buffer, texture
    auto buf = [dev_ newBufferWithBytes:hostMemInput length:hostMemSize options:MTLResourceStorageModeShared];
    //[buf didModifyRange:NSMakeRange(0, hostMemSize)]; // if managed storage
    job->raw_device_mem = buf;
    // seems BGRA does not work
    auto fmt = MTLPixelFormatBGRA8Unorm;
    if (pix == R3DSDK::PixelType_16Bit_RGB_Interleaved) {
        fmt = MTLPixelFormatRGBA16Uint;
    }
    auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:width height:height mipmapped:false];
    desc.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
    job->output_device_image = [dev_ newTextureWithDescriptor:desc];
    job->output_device_mem_size = R3DSDK::DebayerMetalJob::ResultFrameSize(*job);
    return job;
}

PixelFormat format(MTLPixelFormat mfmt)
{
    switch (mfmt) {
    case MTLPixelFormatBGRA8Unorm: return PixelFormat::BGRA;
    case MTLPixelFormatRGBA8Unorm: return PixelFormat::RGBA;
    case MTLPixelFormatRGBA16Uint: return PixelFormat::RGBA64;
    default: return PixelFormat::Unknown;
    }
}

VideoFrame MetalDebayer::wait(void* job)
{
    auto mtljob = (R3DSDK::DebayerMetalJob*)job;
    mtljob->completeAsync();
    auto tex = mtljob->output_device_image;
    VideoFrame frame((int)tex.width, (int)tex.height, format(tex.pixelFormat));
    frame.setBuffers(nullptr);
    [tex getBytes:frame.buffer(0)->data() bytesPerRow:frame.buffer()->stride() fromRegion:MTLRegionMake2D(0, 0, frame.width(), frame.height()) mipmapLevel:0];
    return frame;
}

GpuDebayer::Ptr CreateMTLDebayer() {
    return make_unique<MetalDebayer>();
}
MDK_NS_END
