#include "ShadowMapReconstruction.h"


void ShadowMapReconstruction::InitReconstructTextureBuffers()
{
    int curSize = 2048;
    int num = 1;
    int size = (pow(4, maxShadowPoolLevel) - 1) / (3);
    int index = 0;
    reconstructTextureHandleArray.resize(size);
    for (int i = 0; i < maxShadowPoolLevel; i++)
    {
        for (int j = 0; j < num; j++) {
            // 创建重构的阴影贴图纹理
            nvrhi::TextureDesc reconstructedDesc;
            reconstructedDesc.width = curSize;
            reconstructedDesc.height = curSize;
            reconstructedDesc.depth = 1;
            reconstructedDesc.arraySize = 1;
            reconstructedDesc.mipLevels = 1;
            reconstructedDesc.format = nvrhi::Format::R32_FLOAT;  // 单通道浮点深度
            reconstructedDesc.dimension = nvrhi::TextureDimension::Texture2D;
            reconstructedDesc.isUAV = true;  // 重要：设置为UAV
            reconstructedDesc.keepInitialState = false;
            reconstructedDesc.debugName = "ReconstructedShadowMap_" + to_string(index);
            reconstructedDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;

            reconstructTextureHandleArray[index] = m_Device->createTexture(reconstructedDesc);

            textureDescriptor->CreateDescriptor(nvrhi::BindingSetItem::Texture_UAV(0, reconstructTextureHandleArray[index]));
            index++;
        }
        curSize = curSize / 2;
        num *= 4;
    }
}

bool ShadowMapReconstruction::Init(ShaderFactory& shaderFactory)
{
    // 创建常量缓冲区
    nvrhi::BufferDesc cbDesc;
    cbDesc.byteSize = sizeof(ReconstructionConstants);
    cbDesc.isConstantBuffer = true;
    cbDesc.isVolatile = true;
    cbDesc.debugName = "ShadowMapReconstructionCB";
    cbDesc.maxVersions = 16;
    m_ConstantBuffer = m_Device->createBuffer(cbDesc);

    // 创建绑定布局
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1),
         nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2)
    };
    m_BindingLayout = m_Device->createBindingLayout(layoutDesc);

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

    nvrhi::BindlessLayoutDesc reconstructTextureLayoutDesc;
    reconstructTextureLayoutDesc.visibility = nvrhi::ShaderType::All;
    reconstructTextureLayoutDesc.firstSlot = 0;
    reconstructTextureLayoutDesc.maxCapacity = (pow(4, maxShadowPoolLevel) - 1) / 3;
    reconstructTextureLayoutDesc.registerSpaces = {
        nvrhi::BindingLayoutItem::Texture_UAV(3),
    };
    nvrhi::BindingLayoutHandle reconstructTextureLayout = m_Device->createBindlessLayout(reconstructTextureLayoutDesc);

    textureDescriptor = std::make_shared<DescriptorTableManager>(m_Device, reconstructTextureLayout);
    InitReconstructTextureBuffers();

    // 创建计算管线
    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.bindingLayouts = { m_BindingLayout,quardTreeLayout,quardCodeBookLayout,reconstructTextureLayout };
    pipelineDesc.CS = shaderFactory.CreateShader("shadow_reconstruction.hlsl", "CSMain", nullptr, nvrhi::ShaderType::Compute);
    m_ReconstructPipeline = m_Device->createComputePipeline(pipelineDesc);

    return true;
}

void ShadowMapReconstruction::ReconstructShadowMap(nvrhi::ICommandList* commandList, Inputs inputs, shared_ptr<Scene> scene)
{
    commandList->beginMarker("Reconstruction");
    auto lights = scene->GetSceneGraph()->GetLights();
    for (int i = 0; i < lights.size(); i++) {
        ReconstructionConstants reconstructionConstants;
        reconstructionConstants.lightIndex = i;
        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, m_ConstantBuffer))
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, inputs.lightPlacedSlotsBuffer))
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(2, inputs.lightPlacedFrameIndex));
        commandList->writeBuffer(m_ConstantBuffer, &reconstructionConstants, sizeof(ReconstructionConstants));
        nvrhi::BindingSetHandle bindingSet = m_BindingSets.GetOrCreateBindingSet(bindingSetDesc, m_BindingLayout);
        nvrhi::ComputeState state;
        state.pipeline = m_ReconstructPipeline;
        state.bindings = { bindingSet, inputs.QuardTree->GetDescriptorTable(), inputs.QuardCodeBook->GetDescriptorTable(), textureDescriptor->GetDescriptorTable() };
        commandList->setComputeState(state);

        commandList->dispatch(64, 64, 1);  
    }
    commandList->endMarker();
}