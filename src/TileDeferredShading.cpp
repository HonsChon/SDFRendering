#include "TileDeferredShading.h"


bool TileDeferredShading::LoadScene(std::shared_ptr<IFileSystem> fs, const std::filesystem::path& fileName)
{
    using namespace std::chrono;

    std::unique_ptr<engine::Scene> scene = std::make_unique<engine::Scene>(GetDevice(),
        *m_ShaderFactory, fs, m_TextureCache, nullptr, nullptr);

    auto startTime = high_resolution_clock::now();

    if (scene->Load(fileName))
    {
        m_Scene = std::move(scene);

        auto endTime = high_resolution_clock::now();
        auto duration = duration_cast<milliseconds>(endTime - startTime).count();
        log::info("Scene loading time: %llu ms", duration);

        return true;
    }
    return false;
}

void TileDeferredShading::SceneLoaded()
{  
    ApplicationBase::SceneLoaded();
    m_Scene->FinishedLoading(GetFrameIndex());
    ///添加光源
    for (auto light : m_Scene->GetSceneGraph()->GetLights())
    {
        if (light->GetLightType() == LightType_Directional)
        {
            m_SunLight = std::static_pointer_cast<DirectionalLight>(light);
            if (m_SunLight->irradiance <= 0.f)
                m_SunLight->irradiance = 1.f;
            break;
        }
    }

    if (!m_SunLight)
    {
        CreateLights();
    }

    auto cameras = m_Scene->GetSceneGraph()->GetCameras();
    if (!cameras.empty())
    {
        m_ui.ActiveSceneCamera = cameras[0];
    }
    else
    {
        m_ui.ActiveSceneCamera.reset();

        m_FirstPersonCamera.LookAt(
            float3(0.f, 1.8f, 0.f),
            float3(1.f, 1.8f, 0.f));

    }

    m_ThirdPersonCamera.SetRotation(dm::radians(135.f), dm::radians(20.f));
    PointThirdPersonCameraAt(m_Scene->GetSceneGraph()->GetRootNode());
    m_ui.UseThirdPersonCamera=true;
    CopyActiveCameraToFirstPerson();

    if (g_PrintSceneGraph)
        PrintSceneGraph(m_Scene->GetSceneGraph()->GetRootNode());
}

void TileDeferredShading::RenderScene(nvrhi::IFramebuffer* framebuffer)
{   
    int windowWidth, windowHeight;
    GetDeviceManager()->GetWindowDimensions(windowWidth, windowHeight);
    nvrhi::Viewport windowViewport = nvrhi::Viewport(float(windowWidth), float(windowHeight));
    nvrhi::Viewport renderViewport = windowViewport;

    uint width = windowWidth;
    uint height = windowHeight;

    m_CommandList->open();

    m_Scene->RefreshSceneGraph(GetFrameIndex());
    if (!m_RenderTargets)
    {
        m_RenderTargets = nullptr;   
        m_RenderTargets = std::make_unique<RenderTargets>(GetDevice(), uint2(width, height), uint(1));
        LoadScenePipelines(framebuffer);      
    }
    //设置视角
    SetupView();

    m_Scene->RefreshBuffers(m_CommandList, GetFrameIndex());
    nvrhi::ITexture* framebufferTexture = framebuffer->getDesc().colorAttachments[0].texture;
    m_CommandList->clearTextureFloat(framebufferTexture, nvrhi::AllSubresources, nvrhi::Color(0.f));
    m_RenderTargets->Clear(m_CommandList);
    std::vector<UINT32> levelChangedLight = {};
    std::vector<UINT32> lightPlacedSlots = {};

    m_SDFRendering->Render(framebuffer);

    //PopulateShadowPoolResetPass(levelChangedLight, lightPlacedSlots);
    //PopulateShadowGenerationPass(levelChangedLight, lightPlacedSlots);
    //PopulateShadowReconstructPass();
    //PopulateGBufferPass();
    //
    //PopulateLightCullingPass();
    ////PopulateDeferedLightingPass();
    //PopulateTiledDeferedLightingPass();
    //m_CommonPasses->BlitTexture(m_CommandList, framebuffer, m_RenderTargets->m_LDRBuffer, &m_BindingCache);
    // Done with this frame.
    m_CommandList->close();
    GetDevice()->executeCommandList(m_CommandList);
    std::swap(m_View, m_ViewPrevious);
    GetDeviceManager()->SetVsyncEnabled(m_ui.EnableVsync);
}


bool TileDeferredShading::LoadScenePipelines(nvrhi::IFramebuffer* gBufferFramebuffer) 
{
    m_CommandList->clearBufferUInt(m_RenderTargets->m_LightPlacedSlotBufferDraw, 0xFFFFFFFF);
    m_CommandList->clearBufferUInt(m_RenderTargets->m_LevelChangeLightBufferReadBack, 0xFFFFFFFF);
    m_CommandList->clearBufferUInt(m_RenderTargets->m_LightPlacedSlotFrameBufferIndexDraw, 0xFFFFFFFF);
    std::filesystem::path appShaderPath = app::GetDirectoryWithExecutable() / "shaders/MultiLightShadow" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());
    auto nativeFS = std::make_shared<vfs::NativeFileSystem>();
    engine::ShaderFactory shaderFactory(GetDevice(), nativeFS, appShaderPath);
   
    //
	m_SDFRendering = std::make_unique<SDFRendering>(GetDevice());
	m_SDFRendering->InitPipeLine(gBufferFramebuffer,shaderFactory, m_CommandList);

    //shadowPoolReset pass
    /*ShadowPoolResetPass::Inputs shadowPoolResetPassInputs;
    shadowPoolResetPassInputs.lightPlacedSlotsBuffer = m_RenderTargets->m_LightPlacesSlotBuffer;
    shadowPoolResetPassInputs.shadowPoolSlotsBuffer = m_RenderTargets->m_ShadowSlotBuffer;
    shadowPoolResetPassInputs.levelChangedLightIndexBuffer = m_RenderTargets->m_LevelChangedLightIndexBuffer;
    shadowPoolResetPassInputs.maxShadowLevel = ShadowPoolLevel;
    shadowPoolResetPassInputs.maxLightCount = MaxLightsInScene;
    m_ShadowPoolResetPass = std::make_unique<ShadowPoolResetPass>(GetDevice());
    m_ShadowPoolResetPass->Init(m_CommandList,shaderFactory,m_Scene,shadowPoolResetPassInputs);*/

    //shadowGeneration pass
    //m_ShadowMapGenerationPass = std::make_unique<ShadowMapGenerationPass>(GetDevice(),  MaxLightsInScene, ShadowPoolLevel);
    //ShadowMapGenerationPass::Inputs shadowMapGenerationPassInputs;
    //shadowMapGenerationPassInputs.QuardTree = m_RenderTargets->m_DescriptorTableQuardTree;
    //shadowMapGenerationPassInputs.QuardCodeBook = m_RenderTargets->m_DescriptorTableQuardCodeBook;
    //m_ShadowMapGenerationPass->Init(*m_ShaderFactory,shaderFactory, m_Scene, m_CommonPasses, shadowMapGenerationPassInputs);

    ////shadowReconstruction pass
    //m_ShadowMapReconstructionPass = std::make_unique<ShadowMapReconstruction>(GetDevice());
    //m_ShadowMapReconstructionPass->Init(shaderFactory);

    //light-culling pass
    //m_LightCullingPass = std::make_unique<LightCullingPass>(GetDevice());
    //m_LightCullingPass->Init(shaderFactory, uint2(gBufferFramebuffer->getFramebufferInfo().width, gBufferFramebuffer->getFramebufferInfo().height),m_CommandList,m_Scene);
      
    //G-buffer fill pass
    //GBufferFillPass::CreateParameters GBufferParams;
    //GBufferParams.enableMotionVectors = true;

    //GBufferParams.stencilWriteMask = 0x01;
    //m_GBufferPass = std::make_unique<GBufferFillPass>(GetDevice(), m_CommonPasses);
    //m_GBufferPass->Init(*m_ShaderFactory, GBufferParams);
    //    
    ////deferred lighting pass
    //m_DeferredLightingPass = std::make_unique<DeferredLightingPass>(GetDevice(), m_CommonPasses);
    //m_DeferredLightingPass->Init(m_ShaderFactory);

    ////tile deferred lighting pass
    //m_TileLightingPass = std::make_unique<LightingPass>(GetDevice(), ShadowPoolLevel);
    //m_TileLightingPass->Init(shaderFactory, uint2(gBufferFramebuffer->getFramebufferInfo().width, gBufferFramebuffer->getFramebufferInfo().height),m_Scene);

    return true;
}

void TileDeferredShading::PopulateGBufferPass() 
{
    GBufferFillPass::Context gbufferContext;
    std::shared_ptr<InstancedOpaqueDrawStrategy> m_OpaqueDrawStrategy = std::make_shared<InstancedOpaqueDrawStrategy>();
    RenderCompositeView(m_CommandList,
        m_View.get(), m_ViewPrevious.get(),
        *m_RenderTargets->GBufferFramebuffer,
        m_Scene->GetSceneGraph()->GetRootNode(),
        *m_OpaqueDrawStrategy,
        *m_GBufferPass,
        gbufferContext,
        "GBufferFill"
        );
}

void TileDeferredShading::PopulateLightCullingPass() 
{
    LightCullingPass::Inputs lightCullingInputs;
    m_CommandList->clearBufferUInt(m_RenderTargets->m_CulledLightsBuffer, -1);
    lightCullingInputs.m_DepthTexture = m_RenderTargets->Depth;
    lightCullingInputs.m_CulledLightsBuffer = m_RenderTargets->m_CulledLightsBuffer;
    lightCullingInputs.m_Size = m_RenderTargets->m_Size;
    m_LightCullingPass->ComputeLightCulling(m_CommandList, lightCullingInputs,m_View,m_Scene);
}

void TileDeferredShading::PopulateDeferedLightingPass()
{
    m_ui.SkyParams.brightness = 0;
    float3 m_AmbientTop = m_ui.AmbientIntensity * m_ui.SkyParams.skyColor * m_ui.SkyParams.brightness;
    float3 m_AmbientBottom = m_ui.AmbientIntensity * m_ui.SkyParams.groundColor * m_ui.SkyParams.brightness;
    DeferredLightingPass::Inputs deferredInputs;
    deferredInputs.SetGBuffer(*m_RenderTargets);
    deferredInputs.ambientOcclusion =  nullptr;
    deferredInputs.ambientColorTop = m_AmbientTop;
    deferredInputs.ambientColorBottom = m_AmbientBottom;
    deferredInputs.lights = &m_Scene->GetSceneGraph()->GetLights();
    deferredInputs.lightProbes =  nullptr;
    deferredInputs.output = m_RenderTargets->m_LDRBuffer;

    m_DeferredLightingPass->Render(m_CommandList, *m_View, deferredInputs);
}

void TileDeferredShading::PopulateTiledDeferedLightingPass()
{
    LightingPass::Inputs deferredInputs;
    deferredInputs.SetGBuffer(*m_RenderTargets);
    deferredInputs.m_Size = m_RenderTargets->m_Size;
    deferredInputs.lights = m_Scene->GetSceneGraph()->GetLights();
    deferredInputs.output = m_RenderTargets->m_LDRBuffer;
    deferredInputs.culledLights = m_RenderTargets->m_CulledLightsBuffer;
    deferredInputs.ambientColorBottom = 0.0f;
    deferredInputs.ambientColorTop = 0.7f;
    deferredInputs.QuardTree = m_RenderTargets->m_DescriptorTableQuardTree;
    deferredInputs.QuardCodeBook = m_RenderTargets->m_DescriptorTableQuardCodeBook;
    deferredInputs.lightsPlacedSlot = m_RenderTargets->m_LightPlacedSlotBufferDraw;
    deferredInputs.frameIndexConstant = m_RenderTargets->m_FrameIndexBuffer;
    deferredInputs.lightPlacedFrameIndex = m_RenderTargets->m_LightPlacedSlotFrameBufferIndexDraw;
    m_TileLightingPass->Render(m_CommandList, *m_View, deferredInputs);
  
}

void TileDeferredShading::PopulateShadowPoolResetPass(std::vector<UINT32>& levelChangedLight, std::vector<UINT32>& lightPlacedSlots)
{
    ShadowPoolResetPass::Inputs shadowPoolResetPassInputs;
    shadowPoolResetPassInputs.lightPlacedSlotsBuffer = m_RenderTargets->m_LightPlacesSlotBuffer;
    shadowPoolResetPassInputs.shadowPoolSlotsBuffer = m_RenderTargets->m_ShadowSlotBuffer;
    shadowPoolResetPassInputs.levelChangedLightIndexBuffer = m_RenderTargets->m_LevelChangedLightIndexBuffer;
    shadowPoolResetPassInputs.maxShadowLevel = ShadowPoolLevel;
    shadowPoolResetPassInputs.maxLightCount = MaxLightsInScene;
    shadowPoolResetPassInputs.viewportSizeXY = m_RenderTargets->m_Size;
    shadowPoolResetPassInputs.lightPlacedSlotBufferReadBackOut = m_RenderTargets->m_LightPlacedSlotBufferReadBack;
    shadowPoolResetPassInputs.levelChangeLightBufferReadBackOut = m_RenderTargets->m_LevelChangeLightBufferReadBack;

    m_ShadowPoolResetPass->ComputeShadowPoolSlot(m_CommandList, m_View, m_Scene,shadowPoolResetPassInputs,levelChangedLight,lightPlacedSlots);

}

void TileDeferredShading::PopulateShadowGenerationPass(std::vector<UINT32>& levelChangedLight, std::vector<UINT32>& lightPlacedSlots)
{
    ShadowMapGenerationPass::Inputs inputs;
    inputs.lightPlacedSLotsBuffer = m_RenderTargets->m_LightPlacedSlotBufferReadBack;
    inputs.levelChangedLightsBuffer = m_RenderTargets->m_LevelChangeLightBufferReadBack;
    inputs.lightPlacedSlotBufferDraw = m_RenderTargets->m_LightPlacedSlotBufferDraw;
    inputs.frameIndexConstant = m_RenderTargets->m_FrameIndexBuffer;
    inputs.lightPlacedFrameIndex = m_RenderTargets->m_LightPlacedSlotFrameBufferIndexDraw;
    m_ShadowMapGenerationPass->Render(m_CommandList, m_Scene, m_View, inputs, levelChangedLight, lightPlacedSlots);
}

void TileDeferredShading::PopulateShadowReconstructPass()
{
    ShadowMapReconstruction::Inputs shadowMapReconstructionInputs;
    shadowMapReconstructionInputs.QuardTree = m_RenderTargets->m_DescriptorTableQuardTree;
    shadowMapReconstructionInputs.QuardCodeBook = m_RenderTargets->m_DescriptorTableQuardCodeBook;
    shadowMapReconstructionInputs.lightPlacedSlotsBuffer = m_RenderTargets->m_LightPlacedSlotBufferDraw;
    shadowMapReconstructionInputs.lightPlacedFrameIndex = m_RenderTargets->m_FrameIndexBuffer;
    m_ShadowMapReconstructionPass->ReconstructShadowMap(m_CommandList, shadowMapReconstructionInputs, m_Scene);
}

bool TileDeferredShading::SetupView()
{
    float2 renderTargetSize = float2(m_RenderTargets->GetSize());

    float2 pixelOffset = float2(0.f);

    std::shared_ptr<StereoPlanarView> stereoView = std::dynamic_pointer_cast<StereoPlanarView, IView>(m_View);
    std::shared_ptr<PlanarView> planarView = std::dynamic_pointer_cast<PlanarView, IView>(m_View);

    dm::affine3 viewMatrix;
    float verticalFov = dm::radians(60.f);
    float zNear = 0.01f;
    if (m_ui.ActiveSceneCamera)
    {
        auto perspectiveCamera = std::dynamic_pointer_cast<PerspectiveCamera,SceneCamera>(m_ui.ActiveSceneCamera);
        if (perspectiveCamera)
        {
            zNear = perspectiveCamera->zNear;
            verticalFov = perspectiveCamera->verticalFov;
        }

        viewMatrix = m_ui.ActiveSceneCamera->GetWorldToViewMatrix();
    }
    else
    {
        viewMatrix = GetActiveCamera().GetWorldToViewMatrix();
    }

    bool topologyChanged = false;

    if (!planarView)
    {
        m_View = planarView = std::make_shared<PlanarView>();
        m_ViewPrevious = std::make_shared<PlanarView>();
        topologyChanged = true;
    }

    float4x4 projection = perspProjD3DStyleReverse(verticalFov, renderTargetSize.x / renderTargetSize.y, zNear);

    planarView->SetViewport(nvrhi::Viewport(renderTargetSize.x, renderTargetSize.y));
    planarView->SetPixelOffset(pixelOffset);

    planarView->SetMatrices(viewMatrix, projection);
    planarView->UpdateCache();

    m_ThirdPersonCamera.SetView(*planarView);

    if (topologyChanged)
    {
        *std::static_pointer_cast<PlanarView>(m_ViewPrevious) = *std::static_pointer_cast<PlanarView>(m_View);
    }

    return topologyChanged;
}

void TileDeferredShading::CreateLights()
{
    std::vector<double3> positions = {
        double3(-17,2,10),
        double3(5.063, 20.983, 22.166),
        double3(-13,2,18),
        double3(-5, 2, 12),
        double3(-4.79, 2, 17.32),
        double3(-4.79, 2, 25.106),
        double3(-19.49, 2, 14.263),
        double3(-12.57, 2, 4.649),
        double3(-12.57, 2, -2.163),
        double3(-7.594,2,-2.163),
        double3(-7.594,2,-11.50),
        double3(-0.489, 2, -11.50),
        double3(-0.489, 2, -19.92),
        double3(4.513, 2, -19.92),
        double3(4.513, 2, -31.66),
        double3(-20.57, 2, 4.556),
        double3(-30.01, 2, 5.29),
        double3(-30.01, 2, -2.754),
        double3(-37.01, 2, -2.754),
        double3(-37.01, 2, -7.962),
        double3(-30.59, 2, -6.075),
        double3(-31.64, 2, -13.96),
        double3(1.301, 2, 23.532),
        double3(1.301, 2, 26.821),
        double3(8.891, 2, 26.821),
        double3(8.891, 2, 30.689),
        double3(13.285, 2, 25.808),
        double3(13.285, 2, 32.545),
        double3(16.827, 2, 29.369),
        double3(16.827, 2, 35.078),
        double3(21.268, 2, 29.242),
        double3(20.736, 2, 42.426),
        double3(26.506, 2, 47.659),
        double3(32.937, 2, 50.781),
        double3(40.244, 2, 47.728),       
    };
    for (int i = 0; i < positions.size(); i++) {
        shared_ptr<SpotLight> spotLight = std::make_shared<SpotLight>();

        auto node = std::make_shared<SceneGraphNode>();
        node->SetLeaf(spotLight);
        spotLight->SetDirection(dm::double3(0.0, -1.0, 0));
        spotLight->SetName("SpotLight"+char(i));
        spotLight->innerAngle = 30.0f;
        spotLight->outerAngle = 89.9f;
        spotLight->range = 10;
        
        spotLight->intensity = 100.0;
        spotLight->SetPosition(positions[i]);
        m_Scene->GetSceneGraph()->Attach(m_Scene->GetSceneGraph()->GetRootNode(), node);
    }
}

std::shared_ptr<ShaderFactory> TileDeferredShading::GetShaderFactory()
{
    return m_ShaderFactory;
}


std::shared_ptr<vfs::IFileSystem> TileDeferredShading:: GetRootFs() const
{
    return m_RootFs;
}

std::shared_ptr<Scene> TileDeferredShading:: GetScene()
{
    return m_Scene;
}

BaseCamera& TileDeferredShading::GetActiveCamera() const
{
    return m_ui.UseThirdPersonCamera ? (BaseCamera&)m_ThirdPersonCamera : (BaseCamera&)m_FirstPersonCamera;
}

void TileDeferredShading::CopyActiveCameraToFirstPerson()
{
    if (m_ui.ActiveSceneCamera)
    {
        dm::affine3 viewToWorld = m_ui.ActiveSceneCamera->GetViewToWorldMatrix();
        dm::float3 cameraPos = viewToWorld.m_translation;
        m_FirstPersonCamera.LookAt(cameraPos, cameraPos + viewToWorld.m_linear.row2, viewToWorld.m_linear.row1);
    }
    else if (m_ui.UseThirdPersonCamera)
    {
        m_FirstPersonCamera.LookAt(m_ThirdPersonCamera.GetPosition(), m_ThirdPersonCamera.GetPosition() + m_ThirdPersonCamera.GetDir(), m_ThirdPersonCamera.GetUp());
    }
}


bool TileDeferredShading::KeyboardUpdate(int key, int scancode, int action, int mods)
{
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
    {
        m_ui.ShowUI = !m_ui.ShowUI;
        return true;
    }

    if (key == GLFW_KEY_GRAVE_ACCENT && action == GLFW_PRESS)
    {
        m_ui.ShowConsole = !m_ui.ShowConsole;
        return true;
    }

    if (key == GLFW_KEY_SPACE && action == GLFW_PRESS)
    {
        m_ui.EnableAnimations = !m_ui.EnableAnimations;
        return true;
    }

    if (key == GLFW_KEY_T && action == GLFW_PRESS)
    {
        CopyActiveCameraToFirstPerson();
        if (m_ui.ActiveSceneCamera)
        {
            m_ui.UseThirdPersonCamera = false;
            m_ui.ActiveSceneCamera = nullptr;
        }
        else
        {
            m_ui.UseThirdPersonCamera = !m_ui.UseThirdPersonCamera;
        }
        return true;
    }

    if (!m_ui.ActiveSceneCamera)
        GetActiveCamera().KeyboardUpdate(key, scancode, action, mods);
    return true;
}

bool TileDeferredShading::MousePosUpdate(double xpos, double ypos)
{
    if (!m_ui.ActiveSceneCamera)
        GetActiveCamera().MousePosUpdate(xpos, ypos);

    return true;
}

bool TileDeferredShading::MouseButtonUpdate(int button, int action, int mods)
{
    if (!m_ui.ActiveSceneCamera)
        GetActiveCamera().MouseButtonUpdate(button, action, mods);

    return true;
}

bool TileDeferredShading::MouseScrollUpdate(double xoffset, double yoffset)
{
    if (!m_ui.ActiveSceneCamera)
        GetActiveCamera().MouseScrollUpdate(xoffset, yoffset);

    return true;
}

void TileDeferredShading::PointThirdPersonCameraAt(const std::shared_ptr<SceneGraphNode>& node)
{
    dm::box3 bounds = node->GetGlobalBoundingBox();
    m_ThirdPersonCamera.SetTargetPosition(bounds.center());
    float radius = length(bounds.diagonal()) * 0.5f;
    float distance = radius / sinf(dm::radians(60.0f * 0.5f));
    m_ThirdPersonCamera.SetDistance(distance);
    m_ThirdPersonCamera.Animate(0.f);
}

void TileDeferredShading::Animate(float fElapsedTimeSeconds)
{
    if (!m_ui.ActiveSceneCamera)
        GetActiveCamera().Animate(fElapsedTimeSeconds);
}



// AgilitySDK version used with this sample. Incorrect values here will prevent use of experimental features.
extern "C" { __declspec(dllexport) extern const uint32_t D3D12SDKVersion = D3D12_SDK_VERSION; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = u8".\\D3D12\\"; }

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    nvrhi::GraphicsAPI api = app::GetGraphicsAPIFromCommandLine(__argc, __argv);
#else //  _WIN32
int main(int __argc, const char* const* __argv)
{
    nvrhi::GraphicsAPI api = nvrhi::GraphicsAPI::VULKAN;
#endif //  _WIN32


    DeviceCreationParameters deviceParams;

    // deviceParams.adapter = VrSystem::GetRequiredAdapter();
    deviceParams.backBufferWidth = 1920;
    deviceParams.backBufferHeight = 1080;
    deviceParams.swapChainSampleCount = 1;
    deviceParams.swapChainBufferCount = 3;
    deviceParams.startFullscreen = false;
    deviceParams.vsyncEnabled = true;
    deviceParams.enablePerMonitorDPI = true;
    deviceParams.supportExplicitDisplayScaling = true;

  
    deviceParams.enableDebugRuntime = true;        // 基础调试层
    deviceParams.enableGPUValidation = false;       // GPU端验证
    deviceParams.enableNvrhiValidationLayer = false; // NVRHI验证
   /* deviceParams.maxFramesInFlight = 10;*/

    std::string sceneName;

    DeviceManager* deviceManager = DeviceManager::Create(api);
    const char* apiString = nvrhi::utils::GraphicsAPIToString(deviceManager->GetGraphicsAPI());

    std::string windowTitle = "Donut Feature Demo (" + std::string(apiString) + ")";

    if (!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, windowTitle.c_str()))
    {
        log::error("Cannot initialize a %s graphics device with the requested parameters", apiString);
        return 1;
    }

    if (g_PrintFormats)
    {
        for (uint32_t format = 0; format < (uint32_t)nvrhi::Format::COUNT; format++)
        {
            auto support = deviceManager->GetDevice()->queryFormatSupport((nvrhi::Format)format);
            const auto& formatInfo = nvrhi::getFormatInfo((nvrhi::Format)format);

            char features[13];
            features[0] = (support & nvrhi::FormatSupport::Buffer) != 0 ? 'B' : '.';
            features[1] = (support & nvrhi::FormatSupport::IndexBuffer) != 0 ? 'I' : '.';
            features[2] = (support & nvrhi::FormatSupport::VertexBuffer) != 0 ? 'V' : '.';
            features[3] = (support & nvrhi::FormatSupport::Texture) != 0 ? 'T' : '.';
            features[4] = (support & nvrhi::FormatSupport::DepthStencil) != 0 ? 'D' : '.';
            features[5] = (support & nvrhi::FormatSupport::RenderTarget) != 0 ? 'R' : '.';
            features[6] = (support & nvrhi::FormatSupport::Blendable) != 0 ? 'b' : '.';
            features[7] = (support & nvrhi::FormatSupport::ShaderLoad) != 0 ? 'L' : '.';
            features[8] = (support & nvrhi::FormatSupport::ShaderSample) != 0 ? 'S' : '.';
            features[9] = (support & nvrhi::FormatSupport::ShaderUavLoad) != 0 ? 'l' : '.';
            features[10] = (support & nvrhi::FormatSupport::ShaderUavStore) != 0 ? 's' : '.';
            features[11] = (support & nvrhi::FormatSupport::ShaderAtomic) != 0 ? 'A' : '.';
            features[12] = 0;

            log::info("%17s: %s", formatInfo.name, features);
        }
    }

    {
       
        UIData uiData;
        std::shared_ptr<TileDeferredShading> demo = std::make_shared<TileDeferredShading>(deviceManager, uiData);
        std::shared_ptr<UIRenderer> gui = std::make_shared<UIRenderer>(deviceManager, demo, uiData);

        gui->Init(demo->GetShaderFactory());

        deviceManager->AddRenderPassToBack(demo.get());
        deviceManager->AddRenderPassToBack(gui.get());

        deviceManager->RunMessageLoop();
    }

    deviceManager->Shutdown();
    delete deviceManager;

    return 0;
}
