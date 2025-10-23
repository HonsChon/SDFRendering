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
#include <unordered_set>

using namespace::std;
using namespace::donut::engine;
using namespace donut::math;
using namespace donut::render;



class ShadowMapGenerationPass
{
public:
    struct Inputs 
    {
        std::shared_ptr<DescriptorTableManager> QuardTree = nullptr;
        std::shared_ptr<DescriptorTableManager> QuardCodeBook = nullptr;

        nvrhi::BufferHandle lightPlacedSLotsBuffer = nullptr;
        nvrhi::BufferHandle levelChangedLightsBuffer = nullptr;
        nvrhi::BufferHandle frameIndexConstant = nullptr;
        nvrhi::BufferHandle lightPlacedFrameIndex = nullptr;

        nvrhi::BufferHandle lightPlacedSlotBufferDraw = nullptr;
    };
    struct CompressionParams 
    {
        uint2 shadowMapSize;
        uint2 tileCount;
        uint maxQuadTreeLevel;
        uint lightIndex;
        float compressionErrorThreshold;
        int frameIndex;
        float nearPlane;
        float farPlane;
    };

    // CodeBook��Ŀ
    struct CodeBookEntry
    {
        uint2 parames;
    };

    // �Ĳ����ڵ�
    struct TemplateQuadTreeNode
    {
        uint level; // �ڵ�㼶
        int isLeaf; // �Ƿ�ΪҶ�ӽڵ�  
        uint firstChildOrCodeBook; // Ҷ�ӽڵ㣺CodeBook��������Ҷ�ӽڵ㣺��һ���ӽڵ�����
        int compressionType; // ѹ������
        float4 params;
    };

    struct TemplateCodeBookEntry
    {
        int compressionType; // ѹ������
        float4 params; // ƽ��ģʽ��(nx, ny, nz, d) | ���ģʽ��(depth0, depth1, depth2, depth3)
    };

    struct QuadTreeNode {
        uint firstChildOrIndex;
    };

    struct BufferReadBack {
        nvrhi::BufferHandle cpuCodeBookSizesBuffer;
        nvrhi::BufferHandle cpuQuardTreeSizesBuffer;
        nvrhi::BufferHandle gpuLightPlacedSlotsBuffer;
        nvrhi::BufferHandle gpuLevelChangedLightIndexBuffer;
        nvrhi::BufferHandle constantBuffer;

        nvrhi::EventQueryHandle writeEventQuery;
        nvrhi::EventQueryHandle readEventQuery;

        std::vector<UINT32> lightPlacedSlots;
        std::vector<UINT32> levelChangedLightIndex;
        CompressionParams compressionParams;
        int shadowMapSize;
        int frameIndex;

        bool frameInUse;
    };

    int maxLightNum;
    int maxShadowPoolLevel;
    int frameSizeMax = 2048;
    int readBackBufferCount = 3;
    int writeBufferIndex;
    int readBufferIndex;

    bool isInitial = true;
    bool isFirstround = true;

    unordered_map<UINT32, UINT32> slot2Level;

    std::vector<BufferReadBack> m_ReadBackBuffers;

    std::shared_ptr<DepthPass>          m_ShadowDepthPass;
    std::shared_ptr<PlanarShadowMap>  m_ShadowMap;
    std::shared_ptr<FramebufferFactory> m_ShadowFramebuffer;

    std::vector<nvrhi::BufferHandle> shadowQuardTreeHandleArray;
    std::vector<nvrhi::BufferHandle> shadowQuardTreeCodeBookHandleArray;
    std::vector<nvrhi::BufferHandle> shadowTemplateQuardTreeHandleArray;
    std::vector<nvrhi::BufferHandle> shadowTemplateNodeNumPerLevelHandleArray;

    nvrhi::BindingLayoutHandle m_TileBasedCompBindingLayout;
    nvrhi::BindingLayoutHandle templateQuardTreeLayout;       
    nvrhi::BindingLayoutHandle templateNodeNumPerLevelLayout;
    nvrhi::BindingLayoutHandle quardCodeBookLayout;
    nvrhi::BindingLayoutHandle quardTreeLayout;
    nvrhi::BindingLayoutHandle m_CompressionBindingLayout;

    nvrhi::ComputePipelineHandle m_TileBasedQuardTreeGen_Pso;
    nvrhi::ComputePipelineHandle m_QuardTreeGeneration_Pso;
    nvrhi::ShaderHandle m_TileBasedQuardTreeBuildCS;
    nvrhi::ShaderHandle m_CompressionQuaardTreeCS;

    nvrhi::BufferHandle m_CodeBookCounter;
    nvrhi::BufferHandle m_QuardTreeCounter;
    nvrhi::BufferHandle m_ConstantBuffer;
    nvrhi::BufferHandle m_CodeBookHashIndexBuffer;
    nvrhi::BufferHandle m_CodeBookHashBookEntryBuffer;



    BindingCache m_BindingSets;

    std::vector<std::shared_ptr<Light>> m_Lights;

    std::shared_ptr<DescriptorTableManager> m_DescriptorTableQuardTree;
    std::shared_ptr<DescriptorTableManager> m_DescriptorTableQuardCodeBook;
    std::shared_ptr<DescriptorTableManager> m_DescriptorTableTemplateQuardTree;
    std::shared_ptr<DescriptorTableManager> m_DescriptorTableTemplateNodeNumPerLevel;

    ShadowMapGenerationPass(nvrhi::IDevice* device,  int maxNum, int maxLevel) :m_Device(device), m_BindingSets(device), maxLightNum(maxNum), maxShadowPoolLevel(maxLevel) {}
    void InitDescriptorTableResource();
    void InitSlotToLevel();
    void Init(ShaderFactory& shaderFactory_Donut, ShaderFactory& shaderFactory_Ours, shared_ptr<Scene> scene, std::shared_ptr<CommonRenderPasses> commonPass, Inputs inputs);
    void Render(nvrhi::ICommandList* commandList, std::shared_ptr<Scene> scene, std::shared_ptr<IView> view, Inputs inputs, std::vector<UINT32>& levelChangedLight, std::vector<UINT32>& lightPlacedSlots);
protected:
	nvrhi::DeviceHandle m_Device;
};

