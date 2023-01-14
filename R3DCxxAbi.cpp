/*
 * Copyright (c) 2023 WangBin <wbsecg1 at gmail.com>
 * r3d plugin for libmdk
 */
#include "R3DCxxAbi.h"
#include "string.h"
using namespace std;

char* MetadataItemKey(const R3DSDK::Clip* clip, size_t index)
{
    return
#if defined(_MSC_VER)
        _strdup
#else
        strdup
#endif
            (clip->MetadataItemKey(index).data());

}

char* MetadataItemAsString(const R3DSDK::Clip* clip, size_t index)
{
    return
#if defined(_MSC_VER)
        _strdup
#else
        strdup
#endif
            (clip->MetadataItemAsString(index).data());
}

R3DSDK::R3DStatus SetupCudaCLDevices(R3DSDK::R3DDecoderOptions* opts, int type)
{
    auto status = R3DSDK::R3DStatus_Ok;
    if (type & OPTION_RED_OPENCL) {
        vector<R3DSDK::OpenCLDeviceInfo> devs;
		status = opts->GetOpenCLDeviceList(devs);
        if (status != R3DSDK::R3DStatus_Ok)
            return status;
        if (devs.empty())
            return R3DSDK::R3DStatus_NoGPUDeviceSpecified;
        for (const auto& i : devs) {
            status = opts->useDevice(i);
        }
    } else if (type & OPTION_RED_CUDA) {
        vector<R3DSDK::CudaDeviceInfo> devs;
		status = opts->GetCudaDeviceList(devs);
        if (status != R3DSDK::R3DStatus_Ok)
            return status;
        if (devs.empty())
            return R3DSDK::R3DStatus_NoGPUDeviceSpecified;
        for (const auto& i : devs) {
            status = opts->useDevice(i);
        }
    }
    return status;
}