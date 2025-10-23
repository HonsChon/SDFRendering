/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#include <string>
#include <vector>
#include <memory>
#include <chrono>

#include <donut/core/vfs/VFS.h>
#include <donut/core/log.h>
#include <donut/core/string_utils.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/ConsoleInterpreter.h>
#include <donut/engine/ConsoleObjects.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/Scene.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/TextureCache.h>
#include <donut/render/BloomPass.h>
#include <donut/render/CascadedShadowMap.h>
#include <donut/render/DeferredLightingPass.h>
#include <donut/render/DepthPass.h>
#include <donut/render/DrawStrategy.h>
#include <donut/render/ForwardShadingPass.h>
#include <donut/render/GBuffer.h>
#include <donut/render/GBufferFillPass.h>
#include <donut/render/LightProbeProcessingPass.h>
#include <donut/render/PixelReadbackPass.h>
#include <donut/render/SkyPass.h>
#include <donut/render/SsaoPass.h>
#include <donut/render/TemporalAntiAliasingPass.h>
#include <donut/render/ToneMappingPasses.h>
#include <donut/render/MipMapGenPass.h>
#include <donut/app/ApplicationBase.h>
#include <donut/app/UserInterfaceUtils.h>
#include <donut/app/Camera.h>
#include <donut/app/DeviceManager.h>
#include <donut/app/imgui_console.h>
#include <donut/app/imgui_renderer.h>
#include <nvrhi/utils.h>
#include <nvrhi/common/misc.h>

#include <wrl.h>

#include "LightCullingPass.h"
#include "LightingPass.h"
#include "ShadowPoolResetPass.h"
#include "ShadowMapGenerationPass.h"
#include "ShadowMapReconstruction.h"
#include "SDFRendering.h"

#ifdef DONUT_WITH_TASKFLOW
#include <taskflow/taskflow.hpp>
#endif

using namespace donut;
using namespace donut::math;
using namespace donut::app;
using namespace donut::vfs;
using namespace donut::engine;
using namespace donut::render;

static bool g_PrintSceneGraph = false;
static bool g_PrintFormats = false;

// Constants used by deferred shading. Ensure these values are matched with the shaders.
static const uint32_t DeferredShadingParam_MaxLightsPerTile = 1024; // If changed, make sure to also change the constant c_MaxLightsPerTile in lighting.hlsli
static const uint32_t DeferredShadingParam_TileWidth = 32;
static const uint32_t DeferredShadingParam_TileHeight = 32;

//Constants used by shadowpool
static const int ShadowPoolLevel = 7;

//Max number of lights in the scene
static const uint32_t MaxLightsInScene = 1024;


struct RenderTargets : public GBufferRenderTargets
{
    nvrhi::TextureHandle m_shadowMap;
    nvrhi::TextureHandle m_LDRBuffer;
    nvrhi::BufferHandle m_CulledLightsBuffer;
    nvrhi::BufferHandle m_ShadowSlotBuffer;
    nvrhi::BufferHandle m_LightPlacesSlotBuffer;
    nvrhi::BufferHandle m_LevelChangedLightIndexBuffer;

    nvrhi::BufferHandle m_LevelChangeLightBufferReadBack; 
    nvrhi::BufferHandle m_LightPlacedSlotBufferReadBack;

    nvrhi::BufferHandle m_FrameIndexBuffer;

    nvrhi::BufferHandle m_LightPlacedSlotBufferDraw;
    nvrhi::BufferHandle m_LightPlacedSlotFrameBufferIndexDraw;

    std::shared_ptr<DescriptorTableManager> m_DescriptorTableQuardTree;
    std::shared_ptr<DescriptorTableManager> m_DescriptorTableQuardCodeBook;

    int2 m_Size;

    RenderTargets(nvrhi::IDevice* device, uint2 size, dm::uint sampleCount)
        : m_Size(size)
    {
        GBufferRenderTargets::Init(device, size, sampleCount, false,true);
        nvrhi::TextureDesc desc;
        desc.width = size.x;
        desc.height = size.y;
        desc.keepInitialState = true;

        // LDR buffer
        desc.format = nvrhi::Format::RGBA8_UNORM;
        desc.isRenderTarget = false;
        desc.isUAV = true;
        desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        desc.debugName = "LDRBuffer";
        m_LDRBuffer = device->createTexture(desc);

        //LightCullingBuffer
        uint2 framebufferSize = uint2(size.x, size.y);
        const uint32_t tileCount = LightCullingPass::GetLightTileCount(framebufferSize.x, framebufferSize.y);
        nvrhi::BufferDesc bufferDesc;
        bufferDesc.byteSize = tileCount * DeferredShadingParam_MaxLightsPerTile * sizeof(UINT32);
        bufferDesc.structStride = sizeof(UINT32);
        bufferDesc.canHaveUAVs = true;
        bufferDesc.debugName = "CulledLights";
        bufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        bufferDesc.keepInitialState = true;
        m_CulledLightsBuffer = device->createBuffer(bufferDesc);

        //ShadowSlotBuffer
        nvrhi::BufferDesc shadowPoolSlotBufferDesc;
        shadowPoolSlotBufferDesc.byteSize =  ((pow(4,ShadowPoolLevel)-1)/(3)+ShadowPoolLevel)*sizeof(UINT32);
        shadowPoolSlotBufferDesc.structStride = sizeof(UINT32);
        shadowPoolSlotBufferDesc.canHaveUAVs = true;
        shadowPoolSlotBufferDesc.debugName = "ShadowSlotBuffer";
        shadowPoolSlotBufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        shadowPoolSlotBufferDesc.keepInitialState = true;
        m_ShadowSlotBuffer = device->createBuffer(shadowPoolSlotBufferDesc);

        //LightPlacesSlotBuffer
        nvrhi::BufferDesc lightPlacesSlotBufferDesc;
        lightPlacesSlotBufferDesc.byteSize = MaxLightsInScene*sizeof(UINT32);
        lightPlacesSlotBufferDesc.structStride = sizeof(UINT32);
        lightPlacesSlotBufferDesc.canHaveUAVs = true;
        lightPlacesSlotBufferDesc.debugName = "LightPlacesSlotBuffer";
        lightPlacesSlotBufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        lightPlacesSlotBufferDesc.keepInitialState = true;
        m_LightPlacesSlotBuffer = device->createBuffer(lightPlacesSlotBufferDesc); 

        //LevelChangedLightIndexBuffer
        nvrhi::BufferDesc levelChangedLightIndexBufferDesc;
        levelChangedLightIndexBufferDesc.byteSize = MaxLightsInScene * sizeof(UINT32);
        levelChangedLightIndexBufferDesc.structStride = sizeof(UINT32);
        levelChangedLightIndexBufferDesc.canHaveUAVs = true;
        levelChangedLightIndexBufferDesc.debugName = "LevelChangedLightIndexBuffer";
        levelChangedLightIndexBufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        levelChangedLightIndexBufferDesc.keepInitialState = true;
        m_LevelChangedLightIndexBuffer = device->createBuffer(levelChangedLightIndexBufferDesc);

        //lightPlacedSlotBufferReadBack
        nvrhi::BufferDesc lightPlacedSlotBufferReadBackDesc;
        lightPlacedSlotBufferReadBackDesc.byteSize = MaxLightsInScene * sizeof(UINT32);
        lightPlacedSlotBufferReadBackDesc.structStride = sizeof(UINT32);
        lightPlacedSlotBufferReadBackDesc.canHaveUAVs = true;
        lightPlacedSlotBufferReadBackDesc.debugName = "lightPlacedSlotBufferReadBack";
        lightPlacedSlotBufferReadBackDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        lightPlacedSlotBufferReadBackDesc.keepInitialState = true;
        m_LightPlacedSlotBufferReadBack = device->createBuffer(lightPlacedSlotBufferReadBackDesc);

        //levelChangedLightIndexBufferReadBack
        nvrhi::BufferDesc levelChangedLightIndexBufferReadBackDesc;
        levelChangedLightIndexBufferReadBackDesc.byteSize = MaxLightsInScene * sizeof(UINT32);
        levelChangedLightIndexBufferReadBackDesc.structStride = sizeof(UINT32);
        levelChangedLightIndexBufferReadBackDesc.canHaveUAVs = true;
        levelChangedLightIndexBufferReadBackDesc.debugName = "lightPlacedSlotBufferReadBack";
        levelChangedLightIndexBufferReadBackDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        levelChangedLightIndexBufferReadBackDesc.keepInitialState = true;
        m_LevelChangeLightBufferReadBack = device->createBuffer(levelChangedLightIndexBufferReadBackDesc);

        //LightPlacesSlotBufferDraw
        nvrhi::BufferDesc lightPlacesSlotBufferDrawDesc;
        lightPlacesSlotBufferDrawDesc.byteSize = MaxLightsInScene * sizeof(UINT32);
        lightPlacesSlotBufferDrawDesc.structStride = sizeof(UINT32);
        lightPlacesSlotBufferDrawDesc.canHaveUAVs = true;
        lightPlacesSlotBufferDrawDesc.debugName = "LightPlacesSlotBufferDraw";
        lightPlacesSlotBufferDrawDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        lightPlacesSlotBufferDrawDesc.keepInitialState = true;
        m_LightPlacedSlotBufferDraw = device->createBuffer(lightPlacesSlotBufferDrawDesc);

        //LightPlacesSlotFrameIndexDraw
        nvrhi::BufferDesc lightPlacedSlotFrameBufferIndexDrawDesc;
        lightPlacedSlotFrameBufferIndexDrawDesc.byteSize = MaxLightsInScene * sizeof(UINT32);
        lightPlacedSlotFrameBufferIndexDrawDesc.structStride = sizeof(UINT32);
        lightPlacedSlotFrameBufferIndexDrawDesc.canHaveUAVs = true;
        lightPlacedSlotFrameBufferIndexDrawDesc.debugName = "LightPlacesSlotFrameIndexDraw";
        lightPlacedSlotFrameBufferIndexDrawDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        lightPlacedSlotFrameBufferIndexDrawDesc.keepInitialState = true;
        m_LightPlacedSlotFrameBufferIndexDraw = device->createBuffer(lightPlacedSlotFrameBufferIndexDrawDesc);

        //shadowMapQuardTreeArray and shadowCodebokkArray
        nvrhi::BindlessLayoutDesc quardTreeLayout;
        quardTreeLayout.visibility = nvrhi::ShaderType::All;
        quardTreeLayout.firstSlot = 0;
        quardTreeLayout.maxCapacity = (pow(4,ShadowPoolLevel)-1)/3;
        quardTreeLayout.registerSpaces = {
            nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1),
        };

        nvrhi::BufferDesc frameIndexBufferDesc;
        frameIndexBufferDesc.byteSize = sizeof(int);
        frameIndexBufferDesc.structStride = sizeof(int);
        frameIndexBufferDesc.debugName = "Rendertarget_FrameIndexBuffer";
        frameIndexBufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        frameIndexBufferDesc.keepInitialState = true;
        frameIndexBufferDesc.canHaveUAVs = true;
        m_FrameIndexBuffer = device->createBuffer(frameIndexBufferDesc);

        nvrhi::BindingLayoutHandle m_QuardTreeLayout;
        m_QuardTreeLayout = device->createBindlessLayout(quardTreeLayout);
        m_DescriptorTableQuardTree = std::make_shared<DescriptorTableManager>(device,m_QuardTreeLayout);

        nvrhi::BindlessLayoutDesc quardCodeBookLayout;
        quardCodeBookLayout.visibility = nvrhi::ShaderType::All;
        quardCodeBookLayout.firstSlot = 0;
        quardCodeBookLayout.maxCapacity = (pow(4, ShadowPoolLevel) - 1) / 3;
        quardCodeBookLayout.registerSpaces = {
            nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2),
        };
        nvrhi::BindingLayoutHandle m_CodeBookLayout;
        m_CodeBookLayout = device->createBindlessLayout(quardTreeLayout);
        m_DescriptorTableQuardCodeBook = std::make_shared<DescriptorTableManager>(device, m_CodeBookLayout);
    }

    bool IsUpdateRequired(int2 size) const { return donut::math::any(m_Size != size); }

    void Clear(nvrhi::ICommandList* commandList) override
    {
        GBufferRenderTargets::Clear(commandList);

        commandList->clearTextureFloat(m_LDRBuffer, nvrhi::AllSubresources, nvrhi::Color(0.f));
    }
};

enum class AntiAliasingMode
{
    NONE,
    TEMPORAL,
    MSAA_2X,
    MSAA_4X,
    MSAA_8X
};

struct UIData
{
    bool                                ShowUI = true;
    bool                                ShowConsole = false;
    bool                                UseDeferredShading = true;
    bool                                Stereo = false;
    bool                                EnableSsao = true;
    SsaoParameters                      SsaoParams;
    ToneMappingParameters               ToneMappingParams;
    TemporalAntiAliasingParameters      TemporalAntiAliasingParams;
    SkyParameters                       SkyParams;
    enum AntiAliasingMode               AntiAliasingMode = AntiAliasingMode::TEMPORAL;
    enum TemporalAntiAliasingJitter     TemporalAntiAliasingJitter = TemporalAntiAliasingJitter::MSAA;
    bool                                EnableVsync = true;
    bool                                ShaderReoladRequested = false;
    bool                                EnableProceduralSky = true;
    bool                                EnableBloom = true;
    float                               BloomSigma = 32.f;
    float                               BloomAlpha = 0.05f;
    bool                                EnableTranslucency = true;
    bool                                EnableMaterialEvents = false;
    bool                                EnableShadows = true;
    float                               AmbientIntensity = 1.0f;
    bool                                EnableLightProbe = true;
    float                               LightProbeDiffuseScale = 1.f;
    float                               LightProbeSpecularScale = 1.f;
    float                               CsmExponent = 4.f;
    bool                                DisplayShadowMap = false;
    bool                                UseThirdPersonCamera = false;
    bool                                EnableAnimations = false;
    bool                                TestMipMapGen = false;
    std::shared_ptr<Material>           SelectedMaterial;
    std::shared_ptr<SceneGraphNode>     SelectedNode;
    std::string                         ScreenshotFileName;
    std::shared_ptr<SceneCamera>        ActiveSceneCamera;
};

class TileDeferredShading : public ApplicationBase {

private:
    enum class ScenePass
    {
        ShadowLevelRefresh,
        ShadowMapRefresh,
        GBufferFill,
        LightCulling,
        DeferredShading,
        COUNT
    };

    std::shared_ptr<RootFileSystem>     m_RootFs;
    std::shared_ptr<NativeFileSystem>   m_NativeFs;

    std::unique_ptr<RenderTargets>      m_RenderTargets;
    nvrhi::InputLayoutHandle            m_InputLayout;
    nvrhi::BindingLayoutHandle          m_BindingLayout;
    nvrhi::BindingSetHandle             m_BindingSets[(int)ScenePass::COUNT];

    std::shared_ptr<Scene>              m_Scene;
    std::shared_ptr<IView>              m_View;
    std::shared_ptr<IView>              m_ViewPrevious;

    std::shared_ptr<ShaderFactory>      m_ShaderFactory;
    nvrhi::CommandListHandle            m_CommandList;

    BindingCache                        m_BindingCache;

    std::shared_ptr<DirectionalLight>   m_SunLight;
    FirstPersonCamera                   m_FirstPersonCamera;
    ThirdPersonCamera                   m_ThirdPersonCamera;

    std::unique_ptr<ShadowPoolResetPass> m_ShadowPoolResetPass;
    std::unique_ptr<ShadowMapGenerationPass> m_ShadowMapGenerationPass;
    std::unique_ptr<ShadowMapReconstruction> m_ShadowMapReconstructionPass;
    std::unique_ptr<GBufferFillPass>    m_GBufferPass;
    std::unique_ptr<LightCullingPass>   m_LightCullingPass;
    //std::unique_ptr<MaterialIDPass>     m_MaterialIDPass;
    std::unique_ptr<DeferredLightingPass> m_DeferredLightingPass;
    std::unique_ptr<LightingPass>       m_TileLightingPass;
    std::unique_ptr<SkyPass>            m_SkyPass;

    std::unique_ptr<SDFRendering>       m_SDFRendering;

    // Pipeline state objects.
    nvrhi::ComputePipelineHandle        m_AnimateObjectsPSO;
    nvrhi::ComputePipelineHandle        m_AnimateLightsPSO;
    nvrhi::GraphicsPipelineHandle       m_GBufferFillPSO;
    nvrhi::ComputePipelineHandle        m_CullLightsPSO;
    nvrhi::ComputePipelineHandle        m_ShadePSO;

    // Resources.
    nvrhi::BufferHandle                 m_ConstantBuffer;
    nvrhi::BufferHandle                 m_LightsBuffer;
    nvrhi::BufferHandle                 m_CulledLightsBuffer;

    nvrhi::BufferHandle                 m_NullSRVBuffer;
    nvrhi::BufferHandle                 m_NullUAVBuffer;
    nvrhi::TextureHandle                m_NullSRVTexture;
    nvrhi::TextureHandle                m_NullUAVTexture;

    UIData&                             m_ui;

    // Timing.
    static const uint32_t QueuedFramesCount = 10;
    nvrhi::TimerQueryHandle m_FrameTimers[QueuedFramesCount];
    nvrhi::TimerQueryHandle m_ShadingTimers[QueuedFramesCount];
    int m_NextTimerToUse = 0;
    float m_TimeInSeconds = 0.0f;
    float m_TimeDiffThisFrame = 0.0f;
    bool m_ForceResetAnimation = true;

    // Utility functions.
    static inline bool HRSuccess(HRESULT hr) { assert(SUCCEEDED(hr)); return SUCCEEDED(hr); }
    static inline D3D12_SHADER_BYTECODE getShaderLibD3D12Bytecode(const nvrhi::ShaderLibraryHandle& shaderLib)
    {
        D3D12_SHADER_BYTECODE bc = {};
        shaderLib->getBytecode(&bc.pShaderBytecode, &bc.BytecodeLength);
        return bc;
    };
   
    static inline float4x4 lookToD3DStyle(const float3& eyePosition, const float3& focusPosition, const float3& upDirection)
    {
        float3 eyeDirection = focusPosition - eyePosition;
        float3 negEyePosition = -eyePosition;
        float3 z = normalize(eyeDirection);
        float3 x = normalize(cross(upDirection, z));
        float3 y = cross(z, x);

        float4x4 m;
        m.row0 = float4(x, dot(x, negEyePosition));
        m.row1 = float4(y, dot(y, negEyePosition));
        m.row2 = float4(z, dot(z, negEyePosition));
        m.row3 = float4(0, 0, 0, 1);
        return transpose(m);
    }

public:
   
    TileDeferredShading(DeviceManager* deviceManager, UIData& ui) :
        ApplicationBase(deviceManager),  m_ui(ui) ,m_BindingCache(deviceManager->GetDevice())
    {
        m_RootFs = std::make_shared<RootFileSystem>();
        m_NativeFs = std::make_shared<NativeFileSystem>();

        std::filesystem::path mediaDir = app::GetDirectoryWithExecutable().parent_path() / "media";
        std::filesystem::path frameworkShaderDir = app::GetDirectoryWithExecutable() / "shaders/framework" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());

        m_RootFs->mount("/media", mediaDir);
        m_RootFs->mount("/shaders/donut", frameworkShaderDir);

      /*  std::filesystem::path m_SceneDir = "F:/NetEase/MultipleLightShadow/test_resource/bistro";
        
        std::vector<std::string>  m_SceneFilesAvailable = FindScenes(*m_NativeFs, m_SceneDir);
        
        if (m_SceneFilesAvailable.empty())
        {
            log::fatal("No scene file found in media folder '%s'\n"
                "Please make sure that folder contains valid scene files.", m_SceneDir.generic_string().c_str());
        } */
        m_TextureCache = std::make_shared<TextureCache>(GetDevice(), m_NativeFs, nullptr);

        m_ShaderFactory = std::make_shared<ShaderFactory>(GetDevice(), m_RootFs, "/shaders");
        m_CommonPasses = std::make_shared<CommonRenderPasses>(GetDevice(), m_ShaderFactory);

        m_CommandList = GetDevice()->createCommandList();

        SetAsynchronousLoadingEnabled(true);

     //   BeginLoadingScene(m_NativeFs, "F:/NetEase/MultipleLightShadow/test_resource/bistro/bistro.gltf");
    }

    virtual bool LoadScene(std::shared_ptr<IFileSystem> fs, const std::filesystem::path& fileName) override;
    virtual void SceneLoaded() override;
    virtual void RenderScene(nvrhi::IFramebuffer* framebuffer) override;
    virtual bool KeyboardUpdate(int key, int scancode, int action, int mods) override;
    virtual bool MousePosUpdate(double xpos, double ypos) override;
    virtual bool MouseButtonUpdate(int button, int action, int mods) override;
    virtual bool MouseScrollUpdate(double xoffset, double yoffset) override;
    void PointThirdPersonCameraAt(const std::shared_ptr<SceneGraphNode>& node);
    virtual void Animate(float fElapsedTimeSeconds) override;

    bool LoadScenePipelines(nvrhi::IFramebuffer* gBufferFramebuffer);
    void PopulateGBufferPass();
    void PopulateLightCullingPass();
    void PopulateDeferedLightingPass();
    void PopulateTiledDeferedLightingPass();
    void PopulateShadowPoolResetPass(std::vector<UINT32>& levelChangedLight, std::vector<UINT32>& lightPlacedSlots);
    void PopulateShadowGenerationPass(std::vector<UINT32>& levelChangedLight, std::vector<UINT32>& lightPlacedSlots);
    void PopulateShadowReconstructPass();
    bool SetupView();
    void CreateLights();
    std::shared_ptr<ShaderFactory> GetShaderFactory();

    std::shared_ptr<vfs::IFileSystem> GetRootFs() const;
    std::shared_ptr<Scene> GetScene();
    BaseCamera& GetActiveCamera() const;
    void CopyActiveCameraToFirstPerson();
};



class UIRenderer : public ImGui_Renderer
{
private:
    std::shared_ptr<TileDeferredShading> m_app;

    std::shared_ptr<app::RegisteredFont> m_FontOpenSans;
    std::shared_ptr<app::RegisteredFont> m_FontDroidMono;

    std::unique_ptr<ImGui_Console> m_console;
    std::shared_ptr<engine::Light> m_SelectedLight;

    UIData& m_ui;
    nvrhi::CommandListHandle m_CommandList;

public:
    UIRenderer(DeviceManager* deviceManager, std::shared_ptr<TileDeferredShading> app, UIData& ui)
        : ImGui_Renderer(deviceManager)
        , m_app(app)
        , m_ui(ui)
    {
        m_CommandList = GetDevice()->createCommandList();

        m_FontOpenSans = CreateFontFromFile(*(app->GetRootFs()), "/media/fonts/OpenSans/OpenSans-Regular.ttf", 17.f);
        m_FontDroidMono = CreateFontFromFile(*(app->GetRootFs()), "/media/fonts/DroidSans/DroidSans-Mono.ttf", 14.f);

        ImGui_Console::Options opts;
        opts.font = m_FontDroidMono;
        auto interpreter = std::make_shared<console::Interpreter>();
        // m_console = std::make_unique<ImGui_Console>(interpreter,opts);

        ImGui::GetIO().IniFilename = nullptr;
    }

protected:
    virtual void buildUI(void) override
    {
        if (!m_ui.ShowUI)
            return;

        const auto& io = ImGui::GetIO();

        int width, height;
        GetDeviceManager()->GetWindowDimensions(width, height);

        if (m_app->IsSceneLoading())
        {
            BeginFullScreenWindow();
            ImGui::PushFont(m_FontOpenSans->GetScaledFont());

            char messageBuffer[256];
            const auto& stats = Scene::GetLoadingStats();
            snprintf(messageBuffer, std::size(messageBuffer), "Loading scene");
            DrawScreenCenteredText(messageBuffer);

            ImGui::PopFont();
            EndFullScreenWindow();

            return;
        }

        ImGui::PushFont(m_FontOpenSans->GetScaledFont());

        if (m_ui.ShowConsole && m_console)
        {
            m_console->Render(&m_ui.ShowConsole);
        }

        float const fontSize = ImGui::GetFontSize();

        ImGui::SetNextWindowPos(ImVec2(fontSize * 0.6f, fontSize * 0.6f), 0);
        ImGui::Begin("Settings", 0, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("Renderer: %s", GetDeviceManager()->GetRendererString());
        double frameTime = GetDeviceManager()->GetAverageFrameTimeSeconds();
        if (frameTime > 0.0)
            ImGui::Text("%.3f ms/frame (%.1f FPS)", frameTime * 1e3, 1.0 / frameTime);

        const std::string currentScene = "scene";
        
        if (ImGui::Button("Reload Shaders"))
            m_ui.ShaderReoladRequested = true;

        ImGui::Checkbox("VSync", &m_ui.EnableVsync);
        ImGui::Checkbox("Deferred Shading", &m_ui.UseDeferredShading);
        if (m_ui.AntiAliasingMode >= AntiAliasingMode::MSAA_2X)
            m_ui.UseDeferredShading = false; // Deferred shading doesn't work with MSAA
        /*ImGui::Checkbox("Stereo", &m_ui.Stereo);
        ImGui::Checkbox("Animations", &m_ui.EnableAnimations);*/

        if (ImGui::BeginCombo("Camera (T)", m_ui.ActiveSceneCamera ? m_ui.ActiveSceneCamera->GetName().c_str()
            : m_ui.UseThirdPersonCamera ? "Third-Person" : "First-Person"))
        {
            if (ImGui::Selectable("First-Person", !m_ui.ActiveSceneCamera && !m_ui.UseThirdPersonCamera))
            {
                m_ui.ActiveSceneCamera.reset();
                m_ui.UseThirdPersonCamera = false;
            }
            if (ImGui::Selectable("Third-Person", !m_ui.ActiveSceneCamera && m_ui.UseThirdPersonCamera))
            {
                m_ui.ActiveSceneCamera.reset();
                m_ui.UseThirdPersonCamera = true;
                m_app->CopyActiveCameraToFirstPerson();
            }
            for (const auto& camera : m_app->GetScene()->GetSceneGraph()->GetCameras())
            {
                if (ImGui::Selectable(camera->GetName().c_str(), m_ui.ActiveSceneCamera == camera))
                {
                    m_ui.ActiveSceneCamera = camera;
                    m_app->CopyActiveCameraToFirstPerson();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Combo("AA Mode", (int*)&m_ui.AntiAliasingMode, "None\0TemporalAA\0MSAA 2x\0MSAA 4x\0MSAA 8x\0");
        ImGui::Combo("TAA Camera Jitter", (int*)&m_ui.TemporalAntiAliasingJitter, "MSAA\0Halton\0R2\0White Noise\0");

        ImGui::SliderFloat("Ambient Intensity", &m_ui.AmbientIntensity, 0.f, 1.f);

        /*ImGui::Checkbox("Enable Light Probe", &m_ui.EnableLightProbe);
        if (m_ui.EnableLightProbe && ImGui::CollapsingHeader("Light Probe"))
        {
            ImGui::DragFloat("Diffuse Scale", &m_ui.LightProbeDiffuseScale, 0.01f, 0.0f, 10.0f);
            ImGui::DragFloat("Specular Scale", &m_ui.LightProbeSpecularScale, 0.01f, 0.0f, 10.0f);
        }*/

        ImGui::Checkbox("Enable Procedural Sky", &m_ui.EnableProceduralSky);
        if (m_ui.EnableProceduralSky && ImGui::CollapsingHeader("Sky Parameters"))
        {
            ImGui::SliderFloat("Brightness", &m_ui.SkyParams.brightness, 0.f, 1.f);
            ImGui::SliderFloat("Glow Size", &m_ui.SkyParams.glowSize, 0.f, 90.f);
            ImGui::SliderFloat("Glow Sharpness", &m_ui.SkyParams.glowSharpness, 1.f, 10.f);
            ImGui::SliderFloat("Glow Intensity", &m_ui.SkyParams.glowIntensity, 0.f, 1.f);
            ImGui::SliderFloat("Horizon Size", &m_ui.SkyParams.horizonSize, 0.f, 90.f);
        }
        /*ImGui::Checkbox("Enable SSAO", &m_ui.EnableSsao);
        ImGui::Checkbox("Enable Bloom", &m_ui.EnableBloom);
        ImGui::DragFloat("Bloom Sigma", &m_ui.BloomSigma, 0.01f, 0.1f, 100.f);
        ImGui::DragFloat("Bloom Alpha", &m_ui.BloomAlpha, 0.01f, 0.01f, 1.0f);*/
        ImGui::Checkbox("Enable Shadows", &m_ui.EnableShadows);
        /*ImGui::Checkbox("Enable Translucency", &m_ui.EnableTranslucency);

        ImGui::Separator();
        ImGui::Checkbox("Temporal AA Clamping", &m_ui.TemporalAntiAliasingParams.enableHistoryClamping);
        ImGui::Checkbox("Material Events", &m_ui.EnableMaterialEvents);*/
        ImGui::Separator();

     //   const auto& lights = m_app->GetScene()->GetSceneGraph()->GetLights();

     

        ImGui::TextUnformatted("Render Light Probe: ");
        uint32_t probeIndex = 1;
        
        ImGui::Separator();

        ImGui::Checkbox("Display Shadow Map", &m_ui.DisplayShadowMap);

        ImGui::End();

        auto material = m_ui.SelectedMaterial;
        if (material)
        {
            ImGui::SetNextWindowPos(ImVec2(float(width) - fontSize * 0.6f, fontSize * 0.6f), 0, ImVec2(1.f, 0.f));
            ImGui::Begin("Material Editor");
            ImGui::Text("Material %d: %s", material->materialID, material->name.c_str());

            MaterialDomain previousDomain = material->domain;
            material->dirty = donut::app::MaterialEditor(material.get(), true);

            if (previousDomain != material->domain)
                m_app->GetScene()->GetSceneGraph()->GetRootNode()->InvalidateContent();

            ImGui::End();
        }

        if (m_ui.AntiAliasingMode != AntiAliasingMode::NONE && m_ui.AntiAliasingMode != AntiAliasingMode::TEMPORAL)
            m_ui.UseDeferredShading = false;

        if (!m_ui.UseDeferredShading)
            m_ui.EnableSsao = false;

        ImGui::PopFont();
    }
};