#include "LightCullingPass.h"


void LightCullingPass::Init(ShaderFactory& shaderFactory,uint2 framebufferSize, nvrhi::ICommandList* commandList, shared_ptr<Scene> scene) {    
    nvrhi::BindingLayoutDesc bindingLayoutDesc = nvrhi::BindingLayoutDesc()
        .setRegisterSpace(0)
        .setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(int3)))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(1))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0));

        bindingLayoutDesc.visibility = nvrhi::ShaderType::Compute;
        m_BindingLayout = m_Device->createBindingLayout(bindingLayoutDesc);
        nvrhi::ComputePipelineDesc pipelineDesc;
        CreateComputeShader(shaderFactory);
        pipelineDesc.CS = m_ComputeShader;
        pipelineDesc.bindingLayouts = { m_BindingLayout };

        m_Pso = m_Device->createComputePipeline(pipelineDesc);
        
        //创建camera常量buffer
        nvrhi::BufferDesc bufferDesc;
        bufferDesc.byteSize = sizeof(CameraBuffer);
        bufferDesc.maxVersions = 16;
        bufferDesc.isConstantBuffer = true;
        bufferDesc.isVolatile = true;
        bufferDesc.debugName = "SceneConstants";
        bufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        bufferDesc.keepInitialState = true;
        m_ConstantBuffer = m_Device->createBuffer(bufferDesc);
       
        //创建灯光常量buffer
        m_Lights.resize(scene->GetSceneGraph()->GetLights().size());
        UpdateLightConstants(scene);
        const uint64_t lightDataBufferSize = m_Lights.size() * sizeof(LightBuffer);
        m_LightsBuffer = m_Device->createBuffer(
            nvrhi::BufferDesc().setByteSize(lightDataBufferSize).
            setCanHaveUAVs(true).
            setCanHaveTypedViews(true).
            setStructStride(sizeof(LightBuffer)).
            setInitialState(nvrhi::ResourceStates::ShaderResource).
            setKeepInitialState(true).
            setDebugName("LightsData"));
             
}

nvrhi::ShaderHandle& LightCullingPass::CreateComputeShader(ShaderFactory& shaderFactory)
{
    m_ComputeShader = shaderFactory.CreateShader("light_culling.hlsl", "CSMain", nullptr, nvrhi::ShaderType::Compute);
    return m_ComputeShader;
}

void LightCullingPass::UpdateLightConstants(std::shared_ptr<Scene> scene)
{
    int index = 0;
    for (auto light : scene->GetSceneGraph()->GetLights()) {
        LightBuffer lightBuffer;
        
        if (light->GetLightType() == LightType_Spot) {
            SpotLight* spotLight = dynamic_cast<SpotLight*>(light.get());
            lightBuffer.radius = spotLight->range/(2*cos(dm::radians(spotLight->outerAngle)));
            lightBuffer.center = float3(spotLight->GetPosition())+ lightBuffer.radius*float3(spotLight->GetDirection());
            //lightBuffer.center = float3(spotLight->GetPosition());
        }
        else if (light->GetLightType() == LightType_Point) {
            PointLight* pointLight = dynamic_cast<PointLight*>(light.get());
            lightBuffer.radius = pointLight->range;
            lightBuffer.center = float3(pointLight->GetPosition());
        }
        m_Lights[index]=lightBuffer;
        index++;
    }
}

void LightCullingPass::UpdateCameraConstants(std::shared_ptr<IView> view)
{
    m_Camera.viewMatrix = affineToHomogeneous(view->GetViewMatrix());
    m_Camera.ProjInverse = view->GetInverseProjectionMatrix(true); 
}

void LightCullingPass::ComputeLightCulling(nvrhi::ICommandList* commandList,Inputs inputs, shared_ptr<IView> view, std::shared_ptr<Scene> scene)
{
    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(uint3)))
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, m_ConstantBuffer))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0, inputs.m_DepthTexture))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_LightsBuffer))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, inputs.m_CulledLightsBuffer));
   
    nvrhi::BindingSetHandle bindingSet = m_BindingSets.GetOrCreateBindingSet(bindingSetDesc, m_BindingLayout);
    commandList->beginMarker("Light Culling");

    UpdateLightConstants(scene);
    UpdateCameraConstants(view);
    m_Camera.viewportSizeXY = inputs.m_Size;
    const uint64_t lightDataBufferSize = m_Lights.size() * sizeof(LightBuffer);
    commandList->writeBuffer(m_ConstantBuffer, &m_Camera, sizeof(CameraBuffer));
    commandList->writeBuffer(m_LightsBuffer, m_Lights.data(), lightDataBufferSize);
    // Light culling compute shader.
    nvrhi::ComputeState state;
    state.pipeline = m_Pso;
    state.bindings = { bindingSet };
    commandList->setComputeState(state);

    const uint32_t tilesX = GetLightTileCountX(inputs.m_Size.x);
    const uint32_t tilesY = GetLightTileCountY(inputs.m_Size.y);
    const uint32_t rootConstants[3] = { tilesX, tilesY, m_Lights.size()};
    commandList->setPushConstants(rootConstants, sizeof(rootConstants));

    // Dispatch enough thread groups to cover all screen tiles.
    commandList->dispatch(tilesX, tilesY);

    commandList->endMarker();
}
