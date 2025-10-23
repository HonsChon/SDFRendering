#include "LightingPass.h"

void LightingPass::Inputs::SetGBuffer(const GBufferRenderTargets& targets)
{
    depth = targets.Depth;
    gbufferNormals = targets.GBufferNormals;
    gbufferDiffuse = targets.GBufferDiffuse;
    gbufferSpecular = targets.GBufferSpecular;
    gbufferEmissive = targets.GBufferEmissive;
}

void LightingPass::Init(ShaderFactory& shaderFactory, uint2 framebufferSize, shared_ptr<Scene> scene)
{
    nvrhi::BindingLayoutDesc bindingLayoutDesc = nvrhi::BindingLayoutDesc()
        .setRegisterSpace(0)
        .setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(uint3)))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(1))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(2))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1))
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(2))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(3))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(4))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(5))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(6))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(7))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(8))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(9))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(10))
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(11));

    bindingLayoutDesc.visibility = nvrhi::ShaderType::Compute;
    m_BindingLayout = m_Device->createBindingLayout(bindingLayoutDesc);

    //shadowMapQuardTreeArray and shadowCodebookArray

    nvrhi::BindlessLayoutDesc quardCodeBookLayoutDesc;
    quardCodeBookLayoutDesc.visibility = nvrhi::ShaderType::All;
    quardCodeBookLayoutDesc.firstSlot = 0;
    quardCodeBookLayoutDesc.maxCapacity = (pow(4, maxShadowPoolLevel) - 1) / 3;
    quardCodeBookLayoutDesc.registerSpaces = {
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1),
    };
    nvrhi::BindingLayoutHandle quardCodeBookLayout = m_Device->createBindlessLayout(quardCodeBookLayoutDesc);

    nvrhi::BindlessLayoutDesc quardTreeLayoutDesc;
    quardTreeLayoutDesc.visibility = nvrhi::ShaderType::All;
    quardTreeLayoutDesc.firstSlot = 0;
    quardTreeLayoutDesc.maxCapacity = (pow(4, maxShadowPoolLevel) - 1) / 3;
    quardTreeLayoutDesc.registerSpaces = {
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2),
    };
    nvrhi::BindingLayoutHandle quardTreeLayout = m_Device->createBindlessLayout(quardTreeLayoutDesc);

    nvrhi::ComputePipelineDesc pipelineDesc;
    m_ComputeShader = shaderFactory.CreateShader("deferred_shading.hlsl", "CSMain", nullptr, nvrhi::ShaderType::Compute);
    pipelineDesc.CS = m_ComputeShader;
    pipelineDesc.bindingLayouts = { m_BindingLayout,quardTreeLayout, quardCodeBookLayout  };

    m_Pso = m_Device->createComputePipeline(pipelineDesc);
   
    m_Lights = scene->GetSceneGraph()->GetLights();
    lightConstantsArray.resize(m_Lights.size());
    
    shadowConstantsArray.resize(m_Lights.size());
    GetShadowConstantsBuffer(m_Lights);

    lightConstantsHandle = m_Device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(LightConstants) * m_Lights.size()).
        setCanHaveUAVs(true).
        setCanHaveTypedViews(true).
        setStructStride(sizeof(LightConstants)).
        setInitialState(nvrhi::ResourceStates::UnorderedAccess).
        setKeepInitialState(true).
        setDebugName("LightsData"));

    cameraConstantsHandle = m_Device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(PlanarViewConstants)).
        setStructStride(sizeof(PlanarViewConstants)).
        setInitialState(nvrhi::ResourceStates::ConstantBuffer).
        setKeepInitialState(true).
        setIsConstantBuffer(true).
        setIsVolatile(true).
        setDebugName("CameraData").
        setMaxVersions(16)
        );

    deferedParametersHandle = m_Device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(DeferedLightParameters)).
        setStructStride(sizeof(DeferedLightParameters)).
        setInitialState(nvrhi::ResourceStates::ConstantBuffer).
        setIsConstantBuffer(true).
        setKeepInitialState(true).
        setIsVolatile(true).
        setDebugName("DeferedParameterData").
        setMaxVersions(16));

    shadowConstantsHandle = m_Device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(ShadowConstants) * m_Lights.size()).
        setCanHaveUAVs(true).
        setCanHaveTypedViews(true).
        setStructStride(sizeof(ShadowConstants)).
        setInitialState(nvrhi::ResourceStates::ShaderResource).
        setKeepInitialState(true).
        setDebugName("shadowData"));
}

nvrhi::ShaderHandle LightingPass::CreateComputeShader(ShaderFactory& shaderFactory)
{
    m_ComputeShader = shaderFactory.CreateShader("deferred_shading.hlsl", "CSMain", nullptr, nvrhi::ShaderType::Compute);
    return m_ComputeShader;
}

void LightingPass::GetLightsBuffer(const std::vector<std::shared_ptr<Light>> lights)
{
    for (int i = 0; i < lights.size();i++) {
        LightConstants lightConstants;        
        lights[i]->FillLightConstants(lightConstants);
        lightConstantsArray[i]=lightConstants;
    }
}

void LightingPass::GetCameraBuffer(const IView& view)
{
    view.FillPlanarViewConstants(planarViewConstants);
}

void LightingPass::GetShadowConstantsBuffer(const std::vector<std::shared_ptr<Light>> lights)
{
    for (int i = 0; i < lights.size(); i++) {
        ShadowConstants shadowConstants;
        shadowConstants.matWorldToUvzwShadow = CalculateLightsViewProj(lights[i]);
        if (lights[i]->GetLightType() == 2) {
            SpotLight* spotLight = dynamic_cast<SpotLight*>(lights[i].get());
            shadowConstants.shadowFalloffDistance = spotLight->range;            //farPlane
        }
        shadowConstantsArray[i] = shadowConstants;
    }
}

float4x4 LightingPass::CalculateLightsViewProj(shared_ptr<Light> light)
{
    daffine3 viewToWorld = light->GetNode()->GetLocalToWorldTransform();
    viewToWorld = dm::scaling(dm::double3(1.0, 1.0, -1.0)) * viewToWorld;
    affine3 worldToView = affine3(inverse(viewToWorld));

    // 将 affine3 转换为 float4x4
    float4x4 viewMatrix = float4x4(
        float4(worldToView.m_linear[0], 0.0f),  // 第一行 + w=0
        float4(worldToView.m_linear[1], 0.0f),  // 第二行 + w=0  
        float4(worldToView.m_linear[2], 0.0f),  // 第三行 + w=0
        float4(worldToView.m_translation, 1.0f) // 平移向量 + w=1
    );

    if (light->GetLightType() == 2) {
        SpotLight* spotLight = dynamic_cast<SpotLight*>(light.get());
        float outerAngle = spotLight->outerAngle;
        float range = spotLight->range;
        float nearPlane = 0.1f;
        float farPlane = range;
        float aspectRatio = 1.f;

        float yScale = 1.0f / tan(outerAngle);
        float xScale = yScale / aspectRatio;

        float4x4 projMatrix = float4x4(
            float4(xScale, 0.0f, 0.0f, 0.0f),
            float4(0.0f, yScale, 0.0f, 0.0f),
            float4(0.0f, 0.0f, farPlane / (farPlane - nearPlane), 1.0f),
            float4(0.0f, 0.0f, -nearPlane * farPlane / (farPlane - nearPlane), 0.0f)
        );

        // 计算 viewProj 矩阵
        float4x4 viewProjMatrix = viewMatrix * projMatrix;

        return viewProjMatrix;
    } 
}

void LightingPass::Render(nvrhi::ICommandList* commandList, const IView& view, const Inputs& inputs)
{
    GetLightsBuffer(inputs.lights);
    GetCameraBuffer(view);
    GetShadowConstantsBuffer(inputs.lights);
    DeferedLightParameters deferedLightParameters;
    deferedLightParameters.ambientColorTop = float4(inputs.ambientColorTop,1);
    deferedLightParameters.ambientColorBottom = float4(inputs.ambientColorBottom, 1);
    deferedLightParameters.viewportSizeXY = inputs.m_Size;

    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.bindings = {
          nvrhi::BindingSetItem::PushConstants(0, sizeof(uint3)),
          nvrhi::BindingSetItem::ConstantBuffer(1, cameraConstantsHandle),
          nvrhi::BindingSetItem::ConstantBuffer(2, deferedParametersHandle),
          nvrhi::BindingSetItem::StructuredBuffer_SRV(0, inputs.culledLights),
          nvrhi::BindingSetItem::StructuredBuffer_SRV(1, lightConstantsHandle),
          nvrhi::BindingSetItem::Texture_SRV(2, inputs.depth),
          nvrhi::BindingSetItem::Texture_SRV(3, inputs.gbufferDiffuse, nvrhi::Format::UNKNOWN),
          nvrhi::BindingSetItem::Texture_SRV(4, inputs.gbufferSpecular, nvrhi::Format::UNKNOWN),
          nvrhi::BindingSetItem::Texture_SRV(5, inputs.gbufferNormals, nvrhi::Format::UNKNOWN),
          nvrhi::BindingSetItem::Texture_SRV(6, inputs.gbufferEmissive, nvrhi::Format::UNKNOWN),
          nvrhi::BindingSetItem::StructuredBuffer_SRV(7, shadowConstantsHandle),
          nvrhi::BindingSetItem::StructuredBuffer_UAV(8, inputs.lightsPlacedSlot),
          nvrhi::BindingSetItem::StructuredBuffer_UAV(9, inputs.frameIndexConstant),
          nvrhi::BindingSetItem::StructuredBuffer_UAV(10, inputs.lightPlacedFrameIndex),
          nvrhi::BindingSetItem::Texture_UAV(0,inputs.output),
    };

    nvrhi::BindingSetHandle bindingSet = m_BindingSets.GetOrCreateBindingSet(bindingSetDesc, m_BindingLayout);

    commandList->beginMarker("Tile Defered Shading");
    
    commandList->writeBuffer(lightConstantsHandle, lightConstantsArray.data(), lightConstantsArray.size() * sizeof(LightConstants));
    commandList->writeBuffer(deferedParametersHandle, &deferedLightParameters, sizeof(DeferedLightParameters));
    commandList->writeBuffer(cameraConstantsHandle, &planarViewConstants, sizeof(PlanarViewConstants));
    commandList->writeBuffer(shadowConstantsHandle, shadowConstantsArray.data(), shadowConstantsArray.size() * sizeof(ShadowConstants));
    
    nvrhi::ComputeState state;
    state.pipeline = m_Pso;
    state.bindings = { bindingSet, inputs.QuardTree->GetDescriptorTable(), inputs.QuardCodeBook->GetDescriptorTable()};
    commandList->setComputeState(state);

    const uint32_t tilesX = GetLightTileCountX(inputs.m_Size.x);
    const uint32_t tilesY = GetLightTileCountY(inputs.m_Size.y);
    const uint32_t rootConstants[3] = { tilesX, tilesY, m_Lights.size()};
    commandList->setPushConstants(rootConstants, sizeof(rootConstants));

    // Dispatch enough thread groups to cover the entire viewport.
    {

        const int threadsX = 32;
        const int threadsY = 32;
        commandList->dispatch((inputs.m_Size.x + (threadsX - 1)) / threadsX, (inputs.m_Size.y + (threadsY - 1)) / threadsY, 1);
    }
    commandList->endMarker();
}
