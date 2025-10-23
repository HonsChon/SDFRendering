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
class LightCullingPass
{
public:
    struct LightBuffer    //将来要传入GPU的数据，目前先按照示例的hlsl中的结构写死，后续可以根据需要改动
    {
        ////light
        //int lightType = 0;
        //float3 position = 0;
        //float3 target = 0;
        //float3 targetOffset = 0;
        //float3 color = 0;
        //float innerAngle = 0;
        //float outerAngle = 0;

        //boundingbox
        float radius;
        float3 center;
        
    };

    struct CameraBuffer {
        float4x4 viewMatrix;
        float4x4 ProjInverse;
        int2 viewportSizeXY;
        // Constant buffers are 256-byte aligned. Add padding in the struct to allow multiple buffers
        // to be array-indexed.
        float padding[32] = {};
    };

    struct Inputs
    {
        nvrhi::TextureHandle m_DepthTexture = nullptr;
        nvrhi::BufferHandle m_CulledLightsBuffer = nullptr;
        int2 m_Size;
       
    };

    nvrhi::BufferHandle                 m_CulledLightsBuffer;
    nvrhi::BufferHandle                 m_ConstantBuffer;
    nvrhi::BufferHandle                 m_LightsBuffer;

    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::ComputePipelineHandle m_Pso;
    BindingCache m_BindingSets;

    std::vector<LightBuffer>            m_Lights;
    CameraBuffer                        m_Camera;
    // Constants used by deferred shading. Ensure these values are matched with the shaders.
    static const uint32_t DeferredShadingParam_MaxLightsPerTile = 4; // If changed, make sure to also change the constant c_MaxLightsPerTile in lighting.hlsli
    static const uint32_t DeferredShadingParam_TileWidth = 32;
    static const uint32_t DeferredShadingParam_TileHeight = 32;

	LightCullingPass(nvrhi::IDevice* device):m_Device(device),m_BindingSets(device) {
        
	}

    static inline uint32_t GetLightTileCountX(uint32_t viewportWidth) { return (viewportWidth + DeferredShadingParam_TileWidth - 1) / DeferredShadingParam_TileWidth; };
    static inline uint32_t GetLightTileCountY(uint32_t viewportHeight) { return (viewportHeight + DeferredShadingParam_TileHeight - 1) / DeferredShadingParam_TileHeight; };
    static inline uint32_t GetLightTileCount(uint32_t viewportWidth, uint32_t viewportHeight) { return GetLightTileCountX(viewportWidth) * GetLightTileCountY(viewportHeight); };

	nvrhi::ShaderHandle& CreateComputeShader(ShaderFactory& shaderFactory);
    void UpdateLightConstants(std::shared_ptr<Scene> scene);
    void UpdateCameraConstants(std::shared_ptr<IView> view);
	void ComputeLightCulling(nvrhi::ICommandList* commandList,Inputs inputs, shared_ptr<IView> view, std::shared_ptr<Scene> scene);
    void Init(ShaderFactory& shaderFactory, uint2 framebufferSize, nvrhi::ICommandList* commandList, shared_ptr<Scene> scene);

protected:
	nvrhi::DeviceHandle m_Device;
	nvrhi::ShaderHandle m_ComputeShader;
};
