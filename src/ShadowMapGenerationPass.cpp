#include "ShadowMapGenerationPass.h"
#include <fstream>
void ShadowMapGenerationPass::InitDescriptorTableResource()
{
    int curSize = frameSizeMax;
    int num = 1;
    int size = (pow(4, maxShadowPoolLevel) - 1) / (3);
    shadowQuardTreeCodeBookHandleArray.resize(size * readBackBufferCount);
    shadowQuardTreeHandleArray.resize(size * readBackBufferCount);
    shadowTemplateQuardTreeHandleArray.resize(size * readBackBufferCount);
    shadowTemplateNodeNumPerLevelHandleArray.resize(size * readBackBufferCount);
    int index = 0;
    for (int frame = 0; frame < readBackBufferCount; frame++) {
        curSize = frameSizeMax;
        num = 1;
        for (int i = 0; i < maxShadowPoolLevel; ++i) {
            for (int j = 0; j < num; j++) {
                nvrhi::BufferDesc shadowTemplateQuardTreeBufferDesc;
                shadowTemplateQuardTreeBufferDesc.byteSize = (curSize / 32) * (curSize / 32) * 342 * sizeof(TemplateQuadTreeNode);
                shadowTemplateQuardTreeBufferDesc.structStride = sizeof(TemplateQuadTreeNode);
                shadowTemplateQuardTreeBufferDesc.canHaveUAVs = true;
                shadowTemplateQuardTreeBufferDesc.debugName = "TemplateQuardTree_" + to_string(index);
                shadowTemplateQuardTreeBufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
                shadowTemplateQuardTreeBufferDesc.keepInitialState = true;
                shadowTemplateQuardTreeHandleArray[index] = m_Device->createBuffer(shadowTemplateQuardTreeBufferDesc);
                m_DescriptorTableTemplateQuardTree->CreateDescriptor(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, shadowTemplateQuardTreeHandleArray[index]));

                nvrhi::BufferDesc shadowTemplateNodeNumPerLevelBufferDesc;
                shadowTemplateNodeNumPerLevelBufferDesc.byteSize = 65 * 65 * sizeof(uint);
                shadowTemplateNodeNumPerLevelBufferDesc.structStride = sizeof(uint);
                shadowTemplateNodeNumPerLevelBufferDesc.canHaveUAVs = true;
                shadowTemplateNodeNumPerLevelBufferDesc.debugName = "TemplateNodeNumPerLevel_" + to_string(index);
                shadowTemplateNodeNumPerLevelBufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
                shadowTemplateNodeNumPerLevelBufferDesc.keepInitialState = true;
                shadowTemplateNodeNumPerLevelHandleArray[index] = m_Device->createBuffer(shadowTemplateNodeNumPerLevelBufferDesc);
                m_DescriptorTableTemplateNodeNumPerLevel->CreateDescriptor(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, shadowTemplateNodeNumPerLevelHandleArray[index]));

                nvrhi::BufferDesc shadowQuardCodeBookDesc;
                shadowQuardCodeBookDesc.byteSize = sizeof(CodeBookEntry);
                shadowQuardCodeBookDesc.structStride = sizeof(CodeBookEntry);
                shadowQuardCodeBookDesc.canHaveUAVs = true;
                shadowQuardCodeBookDesc.debugName = "QuardCodeBook_" + to_string(index);
                shadowQuardCodeBookDesc.initialState = nvrhi::ResourceStates::ShaderResource;
                shadowQuardCodeBookDesc.keepInitialState = true;
                shadowQuardTreeCodeBookHandleArray[index] = m_Device->createBuffer(shadowQuardCodeBookDesc);
                m_DescriptorTableQuardCodeBook->CreateDescriptor(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, shadowQuardTreeCodeBookHandleArray[index]));

                nvrhi::BufferDesc shadowQuardTreeBufferDesc;
                shadowQuardTreeBufferDesc.byteSize = sizeof(QuadTreeNode);
                shadowQuardTreeBufferDesc.structStride = sizeof(QuadTreeNode);
                shadowQuardTreeBufferDesc.canHaveUAVs = true;
                shadowQuardTreeBufferDesc.debugName = "QuardTree_" + to_string(index);
                shadowQuardTreeBufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
                shadowQuardTreeBufferDesc.keepInitialState = true;
                shadowQuardTreeHandleArray[index] = m_Device->createBuffer(shadowQuardTreeBufferDesc);
                m_DescriptorTableQuardTree->CreateDescriptor(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, shadowQuardTreeHandleArray[index]));

                index++;
            }
            curSize = curSize / 2;
            num *= 4;
        }
    }
    
}

void ShadowMapGenerationPass::InitSlotToLevel()
{
    UINT32 curIndex = maxShadowPoolLevel;
    int levelLength = 1;
    for (UINT32 i = 0; i < maxShadowPoolLevel; i++) {
        int k = 0;
        for (int j = 0; j < levelLength; j++) {
            slot2Level[curIndex] = i;
            curIndex++;
        }
        levelLength *= 4;
    }
}

void ShadowMapGenerationPass::Init(ShaderFactory& shaderFactory_Donut, ShaderFactory& shaderFactory, shared_ptr<Scene> scene, std::shared_ptr<CommonRenderPasses> commonPass, Inputs inputs)
{
    writeBufferIndex = 0;
    readBufferIndex = 0;
    m_ReadBackBuffers.resize(readBackBufferCount);

    for (int i = 0; i < readBackBufferCount; i++) {
        m_ReadBackBuffers[i].cpuCodeBookSizesBuffer = m_Device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(UINT32) * (pow(4, maxShadowPoolLevel) - 1) / 3).
            setStructStride(sizeof(UINT32)).
            setInitialState(nvrhi::ResourceStates::ShaderResource).
            setKeepInitialState(false).
            setCanHaveUAVs(false).
            setCpuAccess(nvrhi::CpuAccessMode::Read).
            setDebugName("ShadowMapGen_cpuCodeBookSizes_" + to_string(i)));

        m_ReadBackBuffers[i].cpuQuardTreeSizesBuffer = m_Device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(UINT32) * (pow(4, maxShadowPoolLevel) - 1) / 3).
            setStructStride(sizeof(UINT32)).
            setInitialState(nvrhi::ResourceStates::ShaderResource).
            setKeepInitialState(false).
            setCanHaveUAVs(false).
            setCpuAccess(nvrhi::CpuAccessMode::Read).
            setDebugName("ShadowMapGen_cpuQuardTreeSizes" + to_string(i)));

        m_ReadBackBuffers[i].gpuLightPlacedSlotsBuffer = m_Device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(UINT32) * maxLightNum).
            setStructStride(sizeof(UINT32)).
            setInitialState(nvrhi::ResourceStates::ShaderResource).
            setKeepInitialState(true).
            setCanHaveUAVs(true).
            setDebugName("ShadowMapGen_gpuLightPlacedSLots_" + to_string(i)));

        m_ReadBackBuffers[i].gpuLevelChangedLightIndexBuffer = m_Device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(UINT32) * maxLightNum).
            setStructStride(sizeof(UINT32)).
            setInitialState(nvrhi::ResourceStates::ShaderResource).
            setKeepInitialState(true).
            setCanHaveUAVs(true).
            setDebugName("ShadowMapGen_gpuLightSlotChangedIndex_" + to_string(i)));

        m_ReadBackBuffers[i].constantBuffer = m_Device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(UINT32)).
            setStructStride(sizeof(UINT32)).
            setInitialState(nvrhi::ResourceStates::ShaderResource).
            setKeepInitialState(true).
            setCanHaveUAVs(true).
            setDebugName("ShadowMapGen_FrameIndex" + to_string(i)));

        m_ReadBackBuffers[i].writeEventQuery = m_Device->createEventQuery();
        m_ReadBackBuffers[i].readEventQuery = m_Device->createEventQuery();
        m_Device->resetEventQuery(m_ReadBackBuffers[i].writeEventQuery);
        m_Device->resetEventQuery(m_ReadBackBuffers[i].readEventQuery);
        m_ReadBackBuffers[i].frameInUse = false;

    }
        
    InitSlotToLevel();
    m_Lights = scene->GetSceneGraph()->GetLights();
     
    DepthPass::CreateParameters shadowDepthParams;
    shadowDepthParams.slopeScaledDepthBias = 5.f;
    shadowDepthParams.depthBias = 150;
    shadowDepthParams.depthBiasClamp = 0.2f;
    shadowDepthParams.useInputAssembler = TRUE;
    m_ShadowDepthPass = std::make_shared<DepthPass>(m_Device, commonPass);
    m_ShadowDepthPass->Init(shaderFactory_Donut, shadowDepthParams);

    m_ShadowMap = make_shared<PlanarShadowMap>(m_Device, 2048, nvrhi::Format::D24S8);;
    m_ShadowFramebuffer = std::make_shared<FramebufferFactory>(m_Device);

    nvrhi::BindingLayoutDesc bindingLayoutDesc = nvrhi::BindingLayoutDesc();
    bindingLayoutDesc.visibility = nvrhi::ShaderType::Compute;
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(4));
    m_TileBasedCompBindingLayout = m_Device->createBindingLayout(bindingLayoutDesc);

    nvrhi::BindingLayoutDesc compBindingLayoutDesc = nvrhi::BindingLayoutDesc();
    compBindingLayoutDesc.visibility = nvrhi::ShaderType::Compute;
    compBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(4));
    m_CompressionBindingLayout = m_Device->createBindingLayout(compBindingLayoutDesc);

    //shadowMapQuardTreeArray and shadowCodebookArray
    nvrhi::BindlessLayoutDesc quardCodeBookLayoutDesc;
    quardCodeBookLayoutDesc.visibility = nvrhi::ShaderType::All;
    quardCodeBookLayoutDesc.firstSlot = 0;
    quardCodeBookLayoutDesc.maxCapacity = (pow(4, maxShadowPoolLevel) - 1) / 3;
    quardCodeBookLayoutDesc.registerSpaces = {
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1),
    };
    quardCodeBookLayout = m_Device->createBindlessLayout(quardCodeBookLayoutDesc);

    nvrhi::BindlessLayoutDesc quardTreeLayoutDesc;
    quardTreeLayoutDesc.visibility = nvrhi::ShaderType::All;
    quardTreeLayoutDesc.firstSlot = 0;
    quardTreeLayoutDesc.maxCapacity = (pow(4, maxShadowPoolLevel) - 1) / 3;
    quardTreeLayoutDesc.registerSpaces = {
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2),
    };
    quardTreeLayout = m_Device->createBindlessLayout(quardTreeLayoutDesc);

    nvrhi::BindlessLayoutDesc templateQuardTreeLayoutDesc;
    templateQuardTreeLayoutDesc.visibility = nvrhi::ShaderType::All;
    templateQuardTreeLayoutDesc.firstSlot = 0;
    templateQuardTreeLayoutDesc.maxCapacity = (pow(4, maxShadowPoolLevel) - 1) / 3;
    templateQuardTreeLayoutDesc.registerSpaces = {
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3),
    };
    templateQuardTreeLayout = m_Device->createBindlessLayout(templateQuardTreeLayoutDesc);

    nvrhi::BindlessLayoutDesc templateQuardTreeLevelNumLayoutDesc;
    templateQuardTreeLevelNumLayoutDesc.visibility = nvrhi::ShaderType::All;
    templateQuardTreeLevelNumLayoutDesc.firstSlot = 0;
    templateQuardTreeLevelNumLayoutDesc.maxCapacity = (pow(4, maxShadowPoolLevel) - 1) / 3;
    templateQuardTreeLevelNumLayoutDesc.registerSpaces = {
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(4),
    };
    templateNodeNumPerLevelLayout = m_Device->createBindlessLayout(templateQuardTreeLevelNumLayoutDesc);
    
    m_DescriptorTableQuardTree = inputs.QuardTree;
    m_DescriptorTableQuardCodeBook = inputs.QuardCodeBook;
    m_DescriptorTableTemplateQuardTree = std::make_shared<DescriptorTableManager>(m_Device, templateQuardTreeLayout);
    m_DescriptorTableTemplateNodeNumPerLevel = std::make_shared<DescriptorTableManager>(m_Device, templateNodeNumPerLevelLayout);

    InitDescriptorTableResource();

    //创建CompressionParams常量buffer
    nvrhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(CompressionParams);
    bufferDesc.maxVersions = 16;
    bufferDesc.isConstantBuffer = true;
    bufferDesc.isVolatile = true;
    bufferDesc.debugName = "CompressionParams";
    bufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    bufferDesc.keepInitialState = true;
    m_ConstantBuffer = m_Device->createBuffer(bufferDesc);

    m_QuardTreeCounter = m_Device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(uint)* (pow(4, maxShadowPoolLevel) - 1) / 3).
        setStructStride(sizeof(uint)).
        setInitialState(nvrhi::ResourceStates::ShaderResource).
        setKeepInitialState(true).
        setCanHaveUAVs(true).
        setDebugName("quardTreeCounter"));

    m_CodeBookCounter = m_Device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(uint) * (pow(4, maxShadowPoolLevel) - 1) / 3).
        setStructStride(sizeof(uint)).
        setInitialState(nvrhi::ResourceStates::ShaderResource).
        setKeepInitialState(true).
        setCanHaveUAVs(true).
        setDebugName("codeBookCounter"));

    m_CodeBookHashIndexBuffer = m_Device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(uint) * 4096 * 4096).
        setStructStride(sizeof(uint)).
        setInitialState(nvrhi::ResourceStates::ShaderResource).
        setKeepInitialState(true).
        setCanHaveUAVs(true).
        setDebugName("codeBookHashIndex"));

    m_CodeBookHashBookEntryBuffer = m_Device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(TemplateCodeBookEntry) * 4096 * 4096).
        setStructStride(sizeof(uint)).
        setInitialState(nvrhi::ResourceStates::ShaderResource).
        setKeepInitialState(true).
        setCanHaveUAVs(true).
        setDebugName("codeBookHashEntry"));

    m_TileBasedQuardTreeBuildCS = shaderFactory.CreateShader("tile_based_shadow_compression_cs.hlsl", "CSMain", nullptr, nvrhi::ShaderType::Compute);
    m_CompressionQuaardTreeCS = shaderFactory.CreateShader("shadow_compression.hlsl", "CSMain", nullptr, nvrhi::ShaderType::Compute);

    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = m_TileBasedQuardTreeBuildCS;
    pipelineDesc.bindingLayouts = {
        m_TileBasedCompBindingLayout, 
        templateQuardTreeLayout,
        templateNodeNumPerLevelLayout, 
    
    };
    m_TileBasedQuardTreeGen_Pso = m_Device->createComputePipeline(pipelineDesc);

    nvrhi::ComputePipelineDesc compPipelineDesc;
    compPipelineDesc.CS = m_CompressionQuaardTreeCS;
    compPipelineDesc.bindingLayouts = { 
        m_CompressionBindingLayout,
        templateQuardTreeLayout,
        templateNodeNumPerLevelLayout,
        quardCodeBookLayout, 
        quardTreeLayout,
    };
    m_QuardTreeGeneration_Pso = m_Device->createComputePipeline(compPipelineDesc);
    
}

void ShadowMapGenerationPass::Render(nvrhi::ICommandList* commandList, std::shared_ptr<Scene> scene, std::shared_ptr<IView> view, Inputs inputs, std::vector<UINT32>& levelChangedLight, std::vector<UINT32>& lightPlacedSlots)
{   
    commandList->beginMarker("ShadowMapGeneration");
    std::shared_ptr<InstancedOpaqueDrawStrategy> m_OpaqueDrawStrategy = std::make_shared<InstancedOpaqueDrawStrategy>();
    
    if (levelChangedLight.size() != 0&& m_ReadBackBuffers[writeBufferIndex].frameInUse==false) 
    {
        m_ReadBackBuffers[writeBufferIndex].frameInUse = true;
        m_ReadBackBuffers[writeBufferIndex].lightPlacedSlots.swap(lightPlacedSlots);
        //m_ReadBackBuffers[writeBufferIndex].levelChangedLightIndex.swap(levelChangedLight);
        m_ReadBackBuffers[writeBufferIndex].levelChangedLightIndex.clear();
        m_ReadBackBuffers[writeBufferIndex].frameIndex = writeBufferIndex;

		int lastWriteBufferIndex = writeBufferIndex == 0 ? readBackBufferCount - 1 : writeBufferIndex - 1;
        for (int i = 0; i < m_ReadBackBuffers[writeBufferIndex].lightPlacedSlots.size(); i++) {
            if (!isInitial&&m_ReadBackBuffers[writeBufferIndex].lightPlacedSlots[i] != 0xFFFFFFFF && m_ReadBackBuffers[writeBufferIndex].lightPlacedSlots[i] != m_ReadBackBuffers[lastWriteBufferIndex].lightPlacedSlots[i]) {
				m_ReadBackBuffers[writeBufferIndex].levelChangedLightIndex.push_back(i);
            }
        }

        commandList->clearBufferUInt(m_ReadBackBuffers[writeBufferIndex].gpuLightPlacedSlotsBuffer, 0xFFFFFFFF);
        commandList->clearBufferUInt(m_ReadBackBuffers[writeBufferIndex].gpuLevelChangedLightIndexBuffer, 0xFFFFFFFF);
        commandList->writeBuffer(m_ReadBackBuffers[writeBufferIndex].gpuLightPlacedSlotsBuffer, m_ReadBackBuffers[writeBufferIndex].lightPlacedSlots.data(), sizeof(UINT32) * m_ReadBackBuffers[writeBufferIndex].lightPlacedSlots.size());
        commandList->writeBuffer(m_ReadBackBuffers[writeBufferIndex].gpuLevelChangedLightIndexBuffer, m_ReadBackBuffers[writeBufferIndex].levelChangedLightIndex.data(), sizeof(UINT32) * m_ReadBackBuffers[writeBufferIndex].levelChangedLightIndex.size());
        commandList->writeBuffer(m_ReadBackBuffers[writeBufferIndex].constantBuffer, &m_ReadBackBuffers[writeBufferIndex].frameIndex, sizeof(int));

        for (auto lightIndex : m_ReadBackBuffers[writeBufferIndex].levelChangedLightIndex) {
            auto light = m_Lights[int(lightIndex)];
            m_ReadBackBuffers[writeBufferIndex].shadowMapSize = 2048 / pow(2, slot2Level[m_ReadBackBuffers[writeBufferIndex].lightPlacedSlots[lightIndex]]);

            nvrhi::TextureDesc desc;
            desc.width = m_ReadBackBuffers[writeBufferIndex].shadowMapSize;
            desc.height = m_ReadBackBuffers[writeBufferIndex].shadowMapSize;
            desc.sampleCount = 1;
            desc.isRenderTarget = true;
            desc.isTypeless = true;
            desc.format = nvrhi::Format::D24S8;
            desc.debugName = "ShadowMap_" + to_string(lightIndex);
            desc.useClearValue = true;
            desc.clearValue = nvrhi::Color(1.f);
            desc.initialState = nvrhi::ResourceStates::ShaderResource;
            desc.keepInitialState = true;
            desc.dimension = nvrhi::TextureDimension::Texture2DArray;
            nvrhi::TextureHandle m_ShadowMapTexture = m_Device->createTexture(desc);

            if (light->GetLightType() == 2) {
                light->shadowMap = m_ShadowMap;
                SpotLight* spotLight = dynamic_cast<SpotLight*>(light.get());

                m_ShadowMap->SetupSpotLightView(*spotLight);
                m_ShadowMap->UpdateShadowMapSize(dm::float2(m_ReadBackBuffers[writeBufferIndex].shadowMapSize, m_ReadBackBuffers[writeBufferIndex].shadowMapSize));
                const nvrhi::FormatInfo& depthFormatInfo = nvrhi::getFormatInfo(m_ShadowMapTexture->getDesc().format);
                commandList->clearDepthStencilTexture(m_ShadowMapTexture, nvrhi::AllSubresources, true, 1.f, depthFormatInfo.hasStencil, 0);
                m_ShadowFramebuffer->UpdateDepthTarget(m_ShadowMapTexture);

                DepthPass::Context context;        //Context中包括系数

                RenderCompositeView(commandList,
                    &m_ShadowMap->GetView(), nullptr,
                    *m_ShadowFramebuffer,
                    scene->GetSceneGraph()->GetRootNode(),
                    *m_OpaqueDrawStrategy,
                    *m_ShadowDepthPass,
                    context,
                    "ShadowMap",
                    false);
                int offset = writeBufferIndex * (pow(4, maxShadowPoolLevel) - 1) / 3;
                commandList->clearBufferUInt(shadowTemplateNodeNumPerLevelHandleArray[m_ReadBackBuffers[writeBufferIndex].lightPlacedSlots[lightIndex] - maxShadowPoolLevel + offset], 0);
                commandList->clearBufferUInt(m_CodeBookHashIndexBuffer, 0xFFFFFFFF - 1);
                commandList->clearBufferUInt(m_CodeBookHashBookEntryBuffer, 0xFFFFFFFF);
                
                m_ReadBackBuffers[writeBufferIndex].compressionParams.lightIndex = lightIndex;
                m_ReadBackBuffers[writeBufferIndex].compressionParams.shadowMapSize = uint2(m_ReadBackBuffers[writeBufferIndex].shadowMapSize, m_ReadBackBuffers[writeBufferIndex].shadowMapSize);
                m_ReadBackBuffers[writeBufferIndex].compressionParams.tileCount = uint2(m_ReadBackBuffers[writeBufferIndex].shadowMapSize / 32, m_ReadBackBuffers[writeBufferIndex].shadowMapSize / 32);
                m_ReadBackBuffers[writeBufferIndex].compressionParams.maxQuadTreeLevel = 7;
                m_ReadBackBuffers[writeBufferIndex].compressionParams.compressionErrorThreshold = 0.005f;
                m_ReadBackBuffers[writeBufferIndex].compressionParams.frameIndex = writeBufferIndex;
                m_ReadBackBuffers[writeBufferIndex].compressionParams.nearPlane = 0.1f;
                m_ReadBackBuffers[writeBufferIndex].compressionParams.farPlane = spotLight->range;

                nvrhi::BindingSetDesc bindingSetDesc;
                bindingSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, m_ConstantBuffer))
                    .addItem(nvrhi::BindingSetItem::Texture_SRV(0, m_ShadowMapTexture))
                    .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_CodeBookHashIndexBuffer))
                    .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, m_CodeBookCounter))
                    .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(2, m_QuardTreeCounter))
                    .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(3, m_ReadBackBuffers[writeBufferIndex].gpuLightPlacedSlotsBuffer))
                    .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(4, m_CodeBookHashBookEntryBuffer));
                nvrhi::BindingSetHandle bindingSet = m_BindingSets.GetOrCreateBindingSet(bindingSetDesc, m_TileBasedCompBindingLayout);
                commandList->writeBuffer(m_ConstantBuffer, &m_ReadBackBuffers[writeBufferIndex].compressionParams, sizeof(CompressionParams));

                // ShdowpoolReset compute shader.
                nvrhi::ComputeState state;
                state.pipeline = m_TileBasedQuardTreeGen_Pso;
                state.bindings = { 
                    bindingSet,
                    m_DescriptorTableTemplateQuardTree->GetDescriptorTable(),
                    m_DescriptorTableTemplateNodeNumPerLevel->GetDescriptorTable(),                            
                };
                commandList->setComputeState(state);
                commandList->dispatch(m_ReadBackBuffers[writeBufferIndex].shadowMapSize / 32, m_ReadBackBuffers[writeBufferIndex].shadowMapSize / 32, 1);
            }
        }

        commandList->copyBuffer(m_ReadBackBuffers[writeBufferIndex].cpuCodeBookSizesBuffer, 0, m_CodeBookCounter, 0, m_CodeBookCounter->getDesc().byteSize);
        commandList->copyBuffer(m_ReadBackBuffers[writeBufferIndex].cpuQuardTreeSizesBuffer, 0, m_QuardTreeCounter, 0, m_QuardTreeCounter->getDesc().byteSize);
        
        commandList->close();
       
        m_Device->executeCommandList(commandList);
        m_Device->setEventQuery(m_ReadBackBuffers[writeBufferIndex].writeEventQuery, nvrhi::CommandQueue::Graphics);
        //m_Device->waitForIdle();

        writeBufferIndex = (writeBufferIndex + 1) % readBackBufferCount;
        commandList->open();
    }
    

    if (m_ReadBackBuffers[readBufferIndex].frameInUse == true && m_Device->pollEventQuery(m_ReadBackBuffers[readBufferIndex].writeEventQuery) && (isFirstround==true || m_Device->pollEventQuery(m_ReadBackBuffers[(readBufferIndex+1)%readBackBufferCount].readEventQuery)))
    {
        int lastReadBufferIndex = readBufferIndex == 0 ? readBackBufferCount - 1 : readBufferIndex - 1;
        int offset = readBufferIndex * (pow(4, maxShadowPoolLevel) - 1) / 3;
        int offsetLast = lastReadBufferIndex * (pow(4, maxShadowPoolLevel) - 1) / 3;
        unordered_set<int> lightChangedSet;
        for (auto light : m_ReadBackBuffers[readBufferIndex].levelChangedLightIndex) {
            lightChangedSet.insert(light);
        }

        void* codeBookSizesData = m_Device->mapBuffer(m_ReadBackBuffers[readBufferIndex].cpuCodeBookSizesBuffer, nvrhi::CpuAccessMode::Read);
        void* quardTreeSizesData = m_Device->mapBuffer(m_ReadBackBuffers[readBufferIndex].cpuQuardTreeSizesBuffer, nvrhi::CpuAccessMode::Read);

        if (codeBookSizesData) {
            auto array = static_cast<uint*>(codeBookSizesData);
            for (int i = 0; i < m_ReadBackBuffers[readBufferIndex].lightPlacedSlots.size(); i++) {
                if (m_ReadBackBuffers[readBufferIndex].lightPlacedSlots[i] == 0xFFFFFFFF)
                    continue;
                
                
                nvrhi::BufferDesc shadowQuardCodeBookDesc;
                int byteSize = 0;
                if (!lightChangedSet.count(i)) {
                    if (shadowQuardTreeCodeBookHandleArray[m_ReadBackBuffers[readBufferIndex].lightPlacedSlots[i] - maxShadowPoolLevel + offset]->getDesc().byteSize == shadowQuardTreeCodeBookHandleArray[m_ReadBackBuffers[readBufferIndex].lightPlacedSlots[i] - maxShadowPoolLevel + offsetLast]->getDesc().byteSize)
                        continue;
                    byteSize = shadowQuardTreeCodeBookHandleArray[m_ReadBackBuffers[readBufferIndex].lightPlacedSlots[i] - maxShadowPoolLevel + offsetLast]->getDesc().byteSize;

                }
                else {
                    byteSize = array[m_ReadBackBuffers[readBufferIndex].lightPlacedSlots[i] - maxShadowPoolLevel] > 0 ? array[m_ReadBackBuffers[readBufferIndex].lightPlacedSlots[i] - maxShadowPoolLevel] * sizeof(CodeBookEntry) : sizeof(CodeBookEntry);
                }
                m_DescriptorTableQuardCodeBook->ReleaseDescriptor(m_ReadBackBuffers[readBufferIndex].lightPlacedSlots[i] - maxShadowPoolLevel + offset);
                shadowQuardCodeBookDesc.byteSize = byteSize;
                shadowQuardCodeBookDesc.structStride = sizeof(CodeBookEntry);
                shadowQuardCodeBookDesc.canHaveUAVs = true;
                shadowQuardCodeBookDesc.debugName = "QuardCodeBook_" + to_string(m_ReadBackBuffers[readBufferIndex].lightPlacedSlots[i] - maxShadowPoolLevel + offset);
                shadowQuardCodeBookDesc.initialState = nvrhi::ResourceStates::ShaderResource;
                shadowQuardCodeBookDesc.keepInitialState = true;
                shadowQuardTreeCodeBookHandleArray[m_ReadBackBuffers[readBufferIndex].lightPlacedSlots[i] - maxShadowPoolLevel + offset] = m_Device->createBuffer(shadowQuardCodeBookDesc);
                m_DescriptorTableQuardCodeBook->CreateDescriptor(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, shadowQuardTreeCodeBookHandleArray[m_ReadBackBuffers[readBufferIndex].lightPlacedSlots[i] - maxShadowPoolLevel + offset]));
            }  
        }
        
        if (quardTreeSizesData) {
            auto array = static_cast<uint*>(quardTreeSizesData);
            for (int i = 0; i < m_ReadBackBuffers[readBufferIndex].lightPlacedSlots.size(); i++)
            {
                if (m_ReadBackBuffers[readBufferIndex].lightPlacedSlots[i] == 0xFFFFFFFF)
                    continue;

                uint tileCount = pow(((2048 / 32) / pow(2, slot2Level[m_ReadBackBuffers[readBufferIndex].lightPlacedSlots[i]])), 2);          

                int byteSize = 0;
                if (!lightChangedSet.count(i)) {
                    if (shadowQuardTreeHandleArray[m_ReadBackBuffers[readBufferIndex].lightPlacedSlots[i] - maxShadowPoolLevel + offset]->getDesc().byteSize == shadowQuardTreeHandleArray[m_ReadBackBuffers[readBufferIndex].lightPlacedSlots[i] - maxShadowPoolLevel + offsetLast]->getDesc().byteSize)
                        continue;
                    byteSize = shadowQuardTreeHandleArray[m_ReadBackBuffers[readBufferIndex].lightPlacedSlots[i] - maxShadowPoolLevel + offsetLast]->getDesc().byteSize;
                }
                else {
                    byteSize = (array[m_ReadBackBuffers[readBufferIndex].lightPlacedSlots[i] - maxShadowPoolLevel] + tileCount) * sizeof(QuadTreeNode);
                }

                m_DescriptorTableQuardTree->ReleaseDescriptor(m_ReadBackBuffers[readBufferIndex].lightPlacedSlots[i] - maxShadowPoolLevel + offset);
                nvrhi::BufferDesc shadowQuardTreeBufferDesc;
                shadowQuardTreeBufferDesc.byteSize = byteSize;
                shadowQuardTreeBufferDesc.structStride = sizeof(QuadTreeNode);
                shadowQuardTreeBufferDesc.canHaveUAVs = true;
                shadowQuardTreeBufferDesc.debugName = "QuardTree_" + to_string(m_ReadBackBuffers[readBufferIndex].lightPlacedSlots[i] - maxShadowPoolLevel + offset);
                shadowQuardTreeBufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
                shadowQuardTreeBufferDesc.keepInitialState = true;
                shadowQuardTreeHandleArray[m_ReadBackBuffers[readBufferIndex].lightPlacedSlots[i] - maxShadowPoolLevel + offset] = m_Device->createBuffer(shadowQuardTreeBufferDesc);
                m_DescriptorTableQuardTree->CreateDescriptor(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, shadowQuardTreeHandleArray[m_ReadBackBuffers[readBufferIndex].lightPlacedSlots[i] - maxShadowPoolLevel + offset]));
            }      
        }

        m_Device->unmapBuffer(m_ReadBackBuffers[readBufferIndex].cpuCodeBookSizesBuffer);
        m_Device->unmapBuffer(m_ReadBackBuffers[readBufferIndex].cpuQuardTreeSizesBuffer);

        for (int i = 0; i < m_ReadBackBuffers[readBufferIndex].lightPlacedSlots.size(); i++) {
            if (isInitial) {
                isInitial = false;
                break;
            }
            if (lightChangedSet.count(i) == 1 || m_ReadBackBuffers[readBufferIndex].lightPlacedSlots[i] == 0xFFFFFFFF)
                continue;
            int slot = m_ReadBackBuffers[readBufferIndex].lightPlacedSlots[i];


            commandList->copyBuffer(shadowQuardTreeHandleArray[slot - maxShadowPoolLevel + offset], 0, shadowQuardTreeHandleArray[slot - maxShadowPoolLevel + offsetLast], 0, shadowQuardTreeHandleArray[slot - maxShadowPoolLevel + offsetLast]->getDesc().byteSize);
            commandList->copyBuffer(shadowQuardTreeCodeBookHandleArray[slot - maxShadowPoolLevel + offset], 0, shadowQuardTreeCodeBookHandleArray[slot - maxShadowPoolLevel + offsetLast], 0, shadowQuardTreeCodeBookHandleArray[slot - maxShadowPoolLevel + offsetLast]->getDesc().byteSize);
        }
        
        nvrhi::BindingSetDesc compressionBindingSetDesc;
      
        compressionBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(2, m_ReadBackBuffers[readBufferIndex].gpuLightPlacedSlotsBuffer))
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(3, m_ReadBackBuffers[readBufferIndex].gpuLevelChangedLightIndexBuffer))
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(4, m_ReadBackBuffers[readBufferIndex].constantBuffer));
        nvrhi::BindingSetHandle bindingSet = m_BindingSets.GetOrCreateBindingSet(compressionBindingSetDesc, m_CompressionBindingLayout);

        nvrhi::ComputeState state;
        state.pipeline = m_QuardTreeGeneration_Pso;
        state.bindings = { 
            bindingSet,
            m_DescriptorTableTemplateQuardTree->GetDescriptorTable(), 
            m_DescriptorTableTemplateNodeNumPerLevel->GetDescriptorTable(),
            m_DescriptorTableQuardCodeBook->GetDescriptorTable(),
            m_DescriptorTableQuardTree->GetDescriptorTable() };
        commandList->setComputeState(state);

        commandList->dispatch(128, 1, 1);

        commandList->copyBuffer(inputs.lightPlacedSlotBufferDraw, 0, m_ReadBackBuffers[readBufferIndex].gpuLightPlacedSlotsBuffer, 0, m_ReadBackBuffers[readBufferIndex].gpuLightPlacedSlotsBuffer->getDesc().byteSize);
        commandList->copyBuffer(inputs.frameIndexConstant, 0, m_ReadBackBuffers[readBufferIndex].constantBuffer, 0, m_ReadBackBuffers[readBufferIndex].constantBuffer->getDesc().byteSize);

        commandList->close();

        m_Device->resetEventQuery(m_ReadBackBuffers[readBufferIndex].writeEventQuery);
        if(!isFirstround)
            m_Device->resetEventQuery(m_ReadBackBuffers[(readBufferIndex + 1) % readBackBufferCount].readEventQuery);
        
        m_Device->executeCommandList(commandList);

        m_Device->setEventQuery(m_ReadBackBuffers[readBufferIndex].readEventQuery, nvrhi::CommandQueue::Graphics);
        
        if (isFirstround && readBufferIndex == readBackBufferCount - 1) {
            isFirstround = false;
        }

        m_ReadBackBuffers[readBufferIndex].frameInUse = false;
        readBufferIndex = (readBufferIndex + 1) % readBackBufferCount;
        commandList->open();
    }

    commandList->endMarker();
}
