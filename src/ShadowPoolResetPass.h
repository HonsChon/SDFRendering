#pragma once
#include <donut/engine/View.h>
#include <donut/engine/SceneTypes.h>
#include <donut/core/vfs/VFS.h>
//#include <donut/core/log.h>
//#include <donut/core/string_utils.h>
#include <donut/render/GeometryPasses.h>
#include <memory>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/BindingCache.h>
#include <donut/engine/Scene.h>
#include <wrl.h>

using namespace::std;
using namespace::donut::engine;
using namespace donut::math;




class ShadowPoolResetPass
{
public:
    struct LightBuffer   
    {
        //boundingbox
        float radius;
        float3 center;
    };

    struct Inputs
    {
        nvrhi::BufferHandle shadowPoolSlotsBuffer;
        nvrhi::BufferHandle lightPlacedSlotsBuffer;
        nvrhi::BufferHandle levelChangedLightIndexBuffer;

        nvrhi::BufferHandle levelChangeLightBufferReadBackOut;
        nvrhi::BufferHandle lightPlacedSlotBufferReadBackOut;

        int maxShadowLevel;
        int maxLightCount;
        int2 viewportSizeXY;
    };

    struct CameraBuffer {
        float4x4 viewMatrix;
        float4x4 projMatrix;
        float4x4 viewProj;
        float4x4 ProjInverse;
        float4x4 viewInverse;
        float nearPlaneDistance;
        int2 viewportSizeXY;
    };

    struct BufferReadBack {
        nvrhi::BufferHandle cpuLightPlacedSlotsBuffer;
        nvrhi::BufferHandle cpuLevelChangedLightIndexBuffer;
        nvrhi::BufferHandle cpuShadowPoolSlotsBuffer;
        nvrhi::EventQueryHandle m_EventQuery;

        CameraBuffer m_CameraBuffer;
        std::vector<LightBuffer> m_LightsBuffer;

        nvrhi::BufferHandle gpuLightPlacedSlotsBuffer;
        nvrhi::BufferHandle gpuLevelChangedLightIndexBuffer;

        bool frameInUse = false;
    };

    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::ComputePipelineHandle m_Pso;
    nvrhi::BufferHandle m_LightsBufferHandle;
    nvrhi::BufferHandle m_CameraBufferHandle;

    std::vector<BufferReadBack> m_BufferReadBack;

    int writeFrameIndex;
    int readFrameIndex;
    int readBackBufferCount = 3;

    BindingCache m_BindingSets;



    ShadowPoolResetPass(nvrhi::IDevice* device) :m_Device(device), m_BindingSets(device) {}
    void InitBuffers(nvrhi::ICommandList* commandList, Inputs& inputs);
    void Init(nvrhi::ICommandList* commandList, ShaderFactory& shaderFactory, shared_ptr<Scene> scene, Inputs inputs);
    void UpdateLightConstants(std::shared_ptr<Scene> scene, int frameIndex);
    void UpdateCameraConstants(std::shared_ptr<IView> view, int frameIndex);
    void ComputeShadowPoolSlot(nvrhi::ICommandList* commandList, shared_ptr<IView> view, shared_ptr<Scene> scene, Inputs& inputs, std::vector<UINT32>& levelChangedLight, std::vector<UINT32>& lightPlacedSlots);

protected:
    nvrhi::DeviceHandle m_Device;
    nvrhi::ShaderHandle m_ComputeShader;
};

