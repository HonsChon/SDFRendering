
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
#include <donut/render/GBuffer.h>
#include <donut/render/DepthPass.h>
#include <donut/render/PlanarShadowMap.h>
#include <donut/engine/SceneGraph.h>
#include <donut/render/DrawStrategy.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/render/CascadedShadowMap.h>
#include <nvrhi/utils.h>

using namespace::std;
using namespace::donut::engine;
using namespace donut::math;
using namespace donut::render;

class ShadowMapReconstruction
{
public:
    struct Inputs
    {
        std::shared_ptr<DescriptorTableManager> QuardTree = nullptr;
        std::shared_ptr<DescriptorTableManager> QuardCodeBook = nullptr;
        nvrhi::BufferHandle lightPlacedSlotsBuffer = nullptr;
        nvrhi::BufferHandle lightPlacedFrameIndex = nullptr;
    };

    struct ReconstructionConstants
    {
        uint32_t lightIndex;
    };

    int maxShadowPoolLevel = 7;

    nvrhi::ComputePipelineHandle m_ReconstructPipeline;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BufferHandle m_ConstantBuffer;
    nvrhi::TextureHandle m_TextureBuffer;

    std::shared_ptr<DescriptorTableManager> textureDescriptor;
    std::vector<nvrhi::TextureHandle> reconstructTextureHandleArray;

    ShadowMapReconstruction(nvrhi::IDevice* device) :m_Device(device), m_BindingSets(device) {}

    void InitReconstructTextureBuffers();
    bool Init(ShaderFactory& shaderFactory);
    void ReconstructShadowMap(nvrhi::ICommandList* commandList, Inputs input, shared_ptr<Scene> scene);

private:
    nvrhi::DeviceHandle m_Device;
    BindingCache m_BindingSets;
};

