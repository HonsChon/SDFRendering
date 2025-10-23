#include "ShadowPoolResetPass.h"
#include <fstream>

void ShadowPoolResetPass::InitBuffers(nvrhi::ICommandList* commandList, Inputs& inputs)
{
    int shadowPoolSlotsNum = (pow(4, inputs.maxShadowLevel) - 1) / (3) + inputs.maxShadowLevel;
    std::vector<uint32_t> initShadowPoolData((pow(4, inputs.maxShadowLevel) - 1) / (3) + inputs.maxShadowLevel);
    int slotsNumCurLevel = 1;
    int levelAddNum = 0;
    for (int i = 0; i < inputs.maxShadowLevel; i++) {
        initShadowPoolData[i] = inputs.maxShadowLevel + levelAddNum;
        levelAddNum += slotsNumCurLevel;
        slotsNumCurLevel *= 4;
    }
    slotsNumCurLevel = 1;
    for (int i = 0; i < inputs.maxShadowLevel; i++) {
        for (int j = 0; j < slotsNumCurLevel; j++) {
            if (j == slotsNumCurLevel - 1)
                initShadowPoolData[initShadowPoolData[i] + j] = 0xFFFFFFFF;
            else
                initShadowPoolData[initShadowPoolData[i] + j] = initShadowPoolData[i] + j + 1;
        }
        slotsNumCurLevel *= 4;
    }

    std::vector<uint32_t> lightPlaceSlots(inputs.maxLightCount);
    for (int i = 0; i < inputs.maxLightCount; i++)
        lightPlaceSlots[i] = 0xFFFFFFFF;

    commandList->writeBuffer(inputs.shadowPoolSlotsBuffer, initShadowPoolData.data(), shadowPoolSlotsNum * sizeof(UINT32));
    commandList->writeBuffer(inputs.lightPlacedSlotsBuffer, lightPlaceSlots.data(), inputs.maxLightCount * sizeof(UINT32));
    commandList->clearBufferUInt(inputs.levelChangedLightIndexBuffer, 0xFFFFFFFF);
}

void ShadowPoolResetPass :: Init(nvrhi::ICommandList* commandList, ShaderFactory& shaderFactory, shared_ptr<Scene> scene, Inputs inputs)
{
 
    m_BufferReadBack.resize(readBackBufferCount);

    writeFrameIndex = 0;
    readFrameIndex = 0;

    nvrhi::BindingLayoutDesc bindingLayoutDesc = nvrhi::BindingLayoutDesc()
        .setRegisterSpace(0)
        .setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(uint)))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(1))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3));

    bindingLayoutDesc.visibility = nvrhi::ShaderType::Compute;
    m_BindingLayout = m_Device->createBindingLayout(bindingLayoutDesc);
    nvrhi::ComputePipelineDesc pipelineDesc;
    m_ComputeShader = shaderFactory.CreateShader("shadowpool_reset.hlsl", "CSMain", nullptr, nvrhi::ShaderType::Compute);
    pipelineDesc.CS = m_ComputeShader;
    pipelineDesc.bindingLayouts = { m_BindingLayout };
    m_Pso = m_Device->createComputePipeline(pipelineDesc);
 
    InitBuffers(commandList,inputs);

    //创建camera常量buffer
    nvrhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(CameraBuffer);
    bufferDesc.maxVersions = 16;
    bufferDesc.isConstantBuffer = true;
    bufferDesc.isVolatile = true;
    bufferDesc.debugName = "CameraConstants";
    bufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    bufferDesc.keepInitialState = true;
    m_CameraBufferHandle = m_Device->createBuffer(bufferDesc);

    //创建灯光常量buffer
    
    
    const uint64_t lightDataBufferSize = scene->GetSceneGraph()->GetLights().size() * sizeof(LightBuffer);
    m_LightsBufferHandle = m_Device->createBuffer(
        nvrhi::BufferDesc().setByteSize(lightDataBufferSize).
        setCanHaveUAVs(true).
        setCanHaveTypedViews(true).
        setStructStride(sizeof(LightBuffer)).
        setInitialState(nvrhi::ResourceStates::ShaderResource).
        setKeepInitialState(true).
        setDebugName("LightsData"));

    for (int i = 0; i < readBackBufferCount; i++) {
        m_BufferReadBack[i].cpuLightPlacedSlotsBuffer = m_Device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(UINT32) * inputs.maxLightCount).
            setStructStride(sizeof(UINT32)).
            setInitialState(nvrhi::ResourceStates::ShaderResource).
            setKeepInitialState(false).
            setCanHaveUAVs(false).
            setCpuAccess(nvrhi::CpuAccessMode::Read).
            setDebugName("shadowPoolReset_cpuLightPlacedSLots_" + to_string(i)));

        m_BufferReadBack[i].cpuLevelChangedLightIndexBuffer = m_Device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(UINT32) * inputs.maxLightCount).
            setStructStride(sizeof(UINT32)).
            setInitialState(nvrhi::ResourceStates::ShaderResource).
            setKeepInitialState(false).
            setCanHaveUAVs(false).
            setCpuAccess(nvrhi::CpuAccessMode::Read).
            setDebugName("shadowPoolReset_cpuLightSlotChangedIndex" + to_string(i)));

        m_BufferReadBack[i].cpuShadowPoolSlotsBuffer = m_Device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(UINT32) * ((pow(4, inputs.maxShadowLevel) - 1) / (3) + inputs.maxShadowLevel) * sizeof(UINT32)).
            setStructStride(sizeof(UINT32)).
            setInitialState(nvrhi::ResourceStates::ShaderResource).
            setKeepInitialState(false).
            setCanHaveUAVs(false).
            setCpuAccess(nvrhi::CpuAccessMode::Read).
            setDebugName("shadowPoolReset_cpuShadowPoolSlot" + to_string(i)));

        m_BufferReadBack[i].gpuLightPlacedSlotsBuffer = m_Device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(UINT32) * inputs.maxLightCount).
            setStructStride(sizeof(UINT32)).
            setInitialState(nvrhi::ResourceStates::ShaderResource).
            setKeepInitialState(true).
            setCanHaveUAVs(true).
            setDebugName("shadowPoolReset_gpuLightPlacedSLots_" + to_string(i)));

        m_BufferReadBack[i].gpuLevelChangedLightIndexBuffer = m_Device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(UINT32) * inputs.maxLightCount).
            setStructStride(sizeof(UINT32)).
            setInitialState(nvrhi::ResourceStates::ShaderResource).
            setKeepInitialState(true).
            setCanHaveUAVs(true).
            setDebugName("shadowPoolReset_gpuLightSlotChangedIndex_" + to_string(i)));

        m_BufferReadBack[i].m_LightsBuffer.resize(scene->GetSceneGraph()->GetLights().size());

        m_BufferReadBack[i].m_EventQuery = m_Device->createEventQuery();

        m_BufferReadBack[i].frameInUse = false;
    }


        
}

void ShadowPoolResetPass::UpdateLightConstants(std::shared_ptr<Scene> scene, int frameIndex)
{
    auto lights = scene->GetSceneGraph()->GetLights();
    for (int i = 0; i < lights.size(); i++) {
        LightBuffer lightBuffer;

        if (lights[i]->GetLightType() == LightType_Spot) {
            SpotLight* spotLight = dynamic_cast<SpotLight*>(lights[i].get());
            lightBuffer.radius = spotLight->range / (2 * cos(dm::radians(spotLight->outerAngle / 2)));
            lightBuffer.center = float3(spotLight->GetPosition()) + lightBuffer.radius * float3(spotLight->GetDirection());
            //lightBuffer.center = float3(spotLight->GetPosition());
        }
        else if (lights[i]->GetLightType() == LightType_Point) {
            PointLight* pointLight = dynamic_cast<PointLight*>(lights[i].get());
            lightBuffer.radius = pointLight->range;
            lightBuffer.center = float3(pointLight->GetPosition());
        }
        m_BufferReadBack[frameIndex].m_LightsBuffer[i]=lightBuffer;
    }
}

void ShadowPoolResetPass::UpdateCameraConstants(std::shared_ptr<IView> view, int frameIndex)
{
    m_BufferReadBack[frameIndex].m_CameraBuffer.viewMatrix =affineToHomogeneous(view->GetViewMatrix());
    m_BufferReadBack[frameIndex].m_CameraBuffer.projMatrix = view->GetProjectionMatrix();
    m_BufferReadBack[frameIndex].m_CameraBuffer.viewProj = view->GetViewProjectionMatrix();
    m_BufferReadBack[frameIndex].m_CameraBuffer.ProjInverse = view->GetInverseProjectionMatrix(true);
    m_BufferReadBack[frameIndex].m_CameraBuffer.viewInverse = affineToHomogeneous(view->GetInverseViewMatrix());
    //m_CameraBuffer.nearPlaneDistance = view->GetViewFrustum().planes[frustum::NEAR_PLANE].distance;
    m_BufferReadBack[frameIndex].m_CameraBuffer.nearPlaneDistance = 0.01f;
}

void ShadowPoolResetPass::ComputeShadowPoolSlot(nvrhi::ICommandList* commandList, shared_ptr<IView> view, shared_ptr<Scene> scene ,Inputs& inputs, std::vector<UINT32>& levelChangedLight, std::vector<UINT32>& lightPlacedSlots)
{
    if (m_BufferReadBack[writeFrameIndex].frameInUse == false) 
    {
        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(uint)))
            .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, m_CameraBufferHandle))
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_LightsBufferHandle))
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, inputs.shadowPoolSlotsBuffer))
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, inputs.lightPlacedSlotsBuffer))
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(2, inputs.levelChangedLightIndexBuffer))
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(3, inputs.lightPlacedSlotBufferReadBackOut));

        UpdateCameraConstants(view, writeFrameIndex);
        m_BufferReadBack[writeFrameIndex].m_CameraBuffer.viewportSizeXY = inputs.viewportSizeXY;
        UpdateLightConstants(scene, writeFrameIndex);
        nvrhi::BindingSetHandle bindingSet = m_BindingSets.GetOrCreateBindingSet(bindingSetDesc, m_BindingLayout);

        commandList->beginMarker("ShadowPoolReset");

        commandList->writeBuffer(m_CameraBufferHandle, &m_BufferReadBack[writeFrameIndex].m_CameraBuffer, sizeof(CameraBuffer));
        commandList->writeBuffer(m_LightsBufferHandle, m_BufferReadBack[writeFrameIndex].m_LightsBuffer.data(), m_BufferReadBack[writeFrameIndex].m_LightsBuffer.size() * sizeof(LightBuffer));
        commandList->clearBufferUInt(inputs.levelChangedLightIndexBuffer, 0xFFFFFFFF);

        // ShdowpoolReset compute shader.
        nvrhi::ComputeState state;
        state.pipeline = m_Pso;
        state.bindings = { bindingSet };
        commandList->setComputeState(state);
        const uint32_t rootConstants[1] = { m_BufferReadBack[writeFrameIndex].m_LightsBuffer.size() };
        commandList->setPushConstants(rootConstants, sizeof(rootConstants));
        commandList->dispatch(1, 1, 1);
        commandList->endMarker();

        m_BufferReadBack[writeFrameIndex].frameInUse = true;

        commandList->copyBuffer(m_BufferReadBack[writeFrameIndex].cpuLightPlacedSlotsBuffer, 0, inputs.lightPlacedSlotsBuffer, 0, inputs.lightPlacedSlotsBuffer->getDesc().byteSize);
        commandList->copyBuffer(m_BufferReadBack[writeFrameIndex].cpuLevelChangedLightIndexBuffer, 0, inputs.levelChangedLightIndexBuffer, 0, inputs.levelChangedLightIndexBuffer->getDesc().byteSize);
        commandList->copyBuffer(m_BufferReadBack[writeFrameIndex].cpuShadowPoolSlotsBuffer, 0, inputs.shadowPoolSlotsBuffer, 0, inputs.shadowPoolSlotsBuffer->getDesc().byteSize);
        commandList->copyBuffer(m_BufferReadBack[writeFrameIndex].gpuLightPlacedSlotsBuffer, 0, inputs.lightPlacedSlotsBuffer, 0, inputs.lightPlacedSlotsBuffer->getDesc().byteSize);
        commandList->copyBuffer(m_BufferReadBack[writeFrameIndex].gpuLevelChangedLightIndexBuffer, 0, inputs.levelChangedLightIndexBuffer, 0, inputs.levelChangedLightIndexBuffer->getDesc().byteSize);
       
        commandList->close();
        m_Device->executeCommandList(commandList);
        m_Device->setEventQuery(m_BufferReadBack[writeFrameIndex].m_EventQuery, nvrhi::CommandQueue::Graphics);
        commandList->open();
        writeFrameIndex = (writeFrameIndex + 1) % readBackBufferCount;
    }

    if (m_BufferReadBack[readFrameIndex].frameInUse == true && m_Device->pollEventQuery(m_BufferReadBack[readFrameIndex].m_EventQuery)==true) {
        void* cpuLightPlacedSLots = m_Device->mapBuffer(m_BufferReadBack[readFrameIndex].cpuLightPlacedSlotsBuffer, nvrhi::CpuAccessMode::Read);
        void* cpuLevelChangedLightIndex = m_Device->mapBuffer(m_BufferReadBack[readFrameIndex].cpuLevelChangedLightIndexBuffer, nvrhi::CpuAccessMode::Read);
        void* cpuShadowPoolSlots = m_Device->mapBuffer(m_BufferReadBack[readFrameIndex].cpuShadowPoolSlotsBuffer, nvrhi::CpuAccessMode::Read);
        std::vector<UINT32> shadowPoolSlotsVector;
        if (cpuLightPlacedSLots) {
            int pos = 0;
            auto array = static_cast<UINT32*>(cpuLightPlacedSLots);
            while (pos < scene->GetSceneGraph()->GetLights().size()) {
                lightPlacedSlots.push_back(array[pos]);
                pos++;
            }
        }
        if (cpuLevelChangedLightIndex) {
            int pos = 0;
            auto array = static_cast<UINT32*>(cpuLevelChangedLightIndex);
            while (array[pos] != 0xFFFFFFFF) {
                levelChangedLight.push_back(array[pos]);
                pos++;
            }   
        }
        
        if (cpuShadowPoolSlots) {
            int pos = 0;
            auto array = static_cast<UINT32*>(cpuShadowPoolSlots);
            while (pos < ((pow(4, inputs.maxShadowLevel) - 1) / (3) + inputs.maxShadowLevel)) {
                shadowPoolSlotsVector.push_back(array[pos]);
                pos++;
            }
        }
        

        m_Device->unmapBuffer(m_BufferReadBack[readFrameIndex].cpuLightPlacedSlotsBuffer);
        m_Device->unmapBuffer(m_BufferReadBack[readFrameIndex].cpuLevelChangedLightIndexBuffer);
        m_Device->unmapBuffer(m_BufferReadBack[readFrameIndex].cpuShadowPoolSlotsBuffer);
        
      /*  commandList->copyBuffer(inputs.lightPlacedSlotBufferReadBackOut, 0, m_BufferReadBack[readFrameIndex].gpuLightPlacedSlotsBuffer, 0, m_BufferReadBack[readFrameIndex].gpuLightPlacedSlotsBuffer->getDesc().byteSize);
        commandList->copyBuffer(inputs.levelChangeLightBufferReadBackOut, 0, m_BufferReadBack[readFrameIndex].gpuLevelChangedLightIndexBuffer, 0, m_BufferReadBack[readFrameIndex].gpuLevelChangedLightIndexBuffer->getDesc().byteSize);*/

        m_BufferReadBack[readFrameIndex].frameInUse = false;
        m_Device->resetEventQuery(m_BufferReadBack[readFrameIndex].m_EventQuery);
        readFrameIndex = (readFrameIndex + 1) % readBackBufferCount;
    }
    
}