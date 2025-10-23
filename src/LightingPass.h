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
#include <donut/engine/SceneGraph.h>
#include <wrl.h>
#include <donut/render/GBuffer.h>


using namespace::std;
using namespace::donut::engine;
using namespace donut::math;
using namespace donut::render;
#include <donut/shaders/light_cb.h>
#include <donut/shaders/view_cb.h>

class LightingPass
{
public:
    struct Inputs
    {
        nvrhi::ITexture* depth = nullptr;
        nvrhi::ITexture* gbufferNormals = nullptr;
        nvrhi::ITexture* gbufferDiffuse = nullptr;
        nvrhi::ITexture* gbufferSpecular = nullptr;
        nvrhi::ITexture* gbufferEmissive = nullptr;
        nvrhi::ITexture* indirectDiffuse = nullptr;
        nvrhi::ITexture* indirectSpecular = nullptr;
        nvrhi::ITexture* shadowChannels = nullptr;
        nvrhi::ITexture* ambientOcclusion = nullptr;
        nvrhi::ITexture* output = nullptr;

        nvrhi::BufferHandle culledLights = nullptr;
        nvrhi::BufferHandle lightsPlacedSlot = nullptr;
        nvrhi::BufferHandle frameIndexConstant = nullptr;
        nvrhi::BufferHandle lightPlacedFrameIndex = nullptr;

        std::vector<std::shared_ptr<Light>> lights;

        std::shared_ptr<DescriptorTableManager> QuardTree = nullptr;
        std::shared_ptr<DescriptorTableManager> QuardCodeBook = nullptr;

        dm::float3 ambientColorTop = 0.f;
        dm::float3 ambientColorBottom = 0.f;
        int2 m_Size;
        // Fills the GBuffer-related textures (depth, normals, etc.) from the provided structure.
        void SetGBuffer(const GBufferRenderTargets& targets);
    };

    struct DeferedLightParameters
    {
        float4      ambientColorTop;
        float4      ambientColorBottom;
        int2       viewportSizeXY;
    };

    int maxLightNum;
    int maxShadowPoolLevel;
    int frameSizeMax = 2048;

    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::ComputePipelineHandle m_Pso;
    BindingCache m_BindingSets;

    nvrhi::ShaderHandle m_ComputeShader;

    std::vector<shared_ptr<Light>> m_Lights;
    std::vector<LightConstants> lightConstantsArray;
    std::vector<ShadowConstants> shadowConstantsArray;

    PlanarViewConstants planarViewConstants;
    nvrhi::BufferHandle lightConstantsHandle;
    nvrhi::BufferHandle cameraConstantsHandle;
    nvrhi::BufferHandle deferedParametersHandle;
    nvrhi::BufferHandle shadowConstantsHandle;

    static const uint32_t DeferredShadingParam_MaxLightsPerTile = 64; // If changed, make sure to also change the constant c_MaxLightsPerTile in lighting.hlsli
    static const uint32_t DeferredShadingParam_TileWidth = 32;
    static const uint32_t DeferredShadingParam_TileHeight = 32;

    LightingPass(nvrhi::IDevice* device, int ShadowPoolSize) :m_Device(device), m_BindingSets(device), maxShadowPoolLevel(ShadowPoolSize){}
    void Init(ShaderFactory& shaderFactory, uint2 framebufferSize, shared_ptr<Scene> scene);
    nvrhi::ShaderHandle CreateComputeShader(ShaderFactory& shaderFactory);

    static inline uint32_t GetLightTileCountX(uint32_t viewportWidth) { return (viewportWidth + DeferredShadingParam_TileWidth - 1) / DeferredShadingParam_TileWidth; };
    static inline uint32_t GetLightTileCountY(uint32_t viewportHeight) { return (viewportHeight + DeferredShadingParam_TileHeight - 1) / DeferredShadingParam_TileHeight; };
    static inline uint32_t GetLightTileCount(uint32_t viewportWidth, uint32_t viewportHeight) { return GetLightTileCountX(viewportWidth) * GetLightTileCountY(viewportHeight); };

    void GetLightsBuffer(const std::vector<std::shared_ptr<Light>> lights);
    void GetCameraBuffer(const IView& view);
    void GetShadowConstantsBuffer(const std::vector<std::shared_ptr<Light>> lights);

    float4x4 CalculateLightsViewProj(shared_ptr<Light> light);

    void Render(
        nvrhi::ICommandList* commandList,
        const IView& view,
        const Inputs& inputs);

protected:
    nvrhi::DeviceHandle m_Device;
};

