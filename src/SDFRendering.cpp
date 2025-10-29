#include "SDFRendering.h"

bool SDFRendering::InitPipeLine()
{
	m_CommandList = GetDevice()->createCommandList();
	m_CommandList->open();
	m_Device = GetDevice();

	std::filesystem::path appShaderPath = app::GetDirectoryWithExecutable() / "shaders/SDFRendering" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());
	auto nativeFS = std::make_shared<vfs::NativeFileSystem>();

	engine::ShaderFactory shaderFactory(GetDevice(), nativeFS, appShaderPath);

	nvrhi::BindingLayoutDesc bindingLayoutDesc = nvrhi::BindingLayoutDesc()
		.setRegisterSpace(0)
		.setVisibility(nvrhi::ShaderType::All)
		.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
		.addItem(nvrhi::BindingLayoutItem::Sampler(0))
		.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));

	bindingLayoutDesc.visibility = nvrhi::ShaderType::Pixel;
	m_BindingLayout = m_Device->createBindingLayout(bindingLayoutDesc);

	m_VertexShader = shaderFactory.CreateShader("sdf_vs.hlsl", "VS", nullptr, nvrhi::ShaderType::Vertex);
	m_PixelShader = shaderFactory.CreateShader("sdf_ps.hlsl", "PS", nullptr, nvrhi::ShaderType::Pixel);
	
	if (!m_VertexShader || !m_PixelShader) {
		return false;
	}

	auto texture = textureCache->LoadTextureFromFile(
		"F:/图形学习/SDFRendering/SDFRendering/src/Texture/noise0.jpg",
		true, nullptr, m_CommandList
	);
	m_Texture = texture->texture;

	nvrhi::SamplerDesc samplerDesc;
	samplerDesc.setAllFilters(true); // 启用线性过滤（min/mag/mip）
	samplerDesc.addressU = nvrhi::SamplerAddressMode::Wrap;
	samplerDesc.addressV = nvrhi::SamplerAddressMode::Wrap;
	samplerDesc.addressW = nvrhi::SamplerAddressMode::Wrap;
	samplerDesc.mipBias = 0.0f;
	samplerDesc.maxAnisotropy = 1.0f;
	samplerDesc.borderColor = nvrhi::Color(0.0f);
	m_Sampler = GetDevice()->createSampler(samplerDesc);

	m_ConstantBuffer = m_Device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(RenderConstants)).
		setStructStride(sizeof(RenderConstants)).
		setInitialState(nvrhi::ResourceStates::ConstantBuffer).
		setIsConstantBuffer(true).
		setKeepInitialState(true).
		setIsVolatile(true).
		setDebugName("ConstantBuffer").
		setMaxVersions(16));

	

	Vertex vertices[] = {
	{ {-1, -1, 0},{0, 0}},
	{ {1, -1, 0}, {1, 0}},
	{ {-1, 1, 0}, {0, 1} },
	{ {1, 1, 0}, {1, 1}},
	};    //设置我们ps的计算范围

	nvrhi::BufferDesc vbDesc;
	vbDesc.byteSize = sizeof(Vertex) * 4;
	vbDesc.debugName = "VertexBuffer";
	vbDesc.isVertexBuffer = true;
	vbDesc.initialState = nvrhi::ResourceStates::CopyDest;;

	vertexBuffer = m_Device->createBuffer(vbDesc);
	m_CommandList->beginTrackingBufferState(vertexBuffer, nvrhi::ResourceStates::CopyDest);
	m_CommandList->writeBuffer(vertexBuffer, vertices, sizeof(vertices));
	m_CommandList->setPermanentBufferState(vertexBuffer, nvrhi::ResourceStates::VertexBuffer);


	uint indices[] = {0,2,1,2,3,1 };
	indicesBuffer = m_Device->createBuffer(
		nvrhi::BufferDesc()
		.setByteSize(sizeof(indices))
		.setDebugName("IndexBuffer")
		.setIsIndexBuffer(true)
		.setStructStride(sizeof(uint))
		.setInitialState(nvrhi::ResourceStates::CopyDest)
	);

	m_CommandList->beginTrackingBufferState(indicesBuffer, nvrhi::ResourceStates::CopyDest);
	m_CommandList->writeBuffer(indicesBuffer, indices, sizeof(indices));
	m_CommandList->setPermanentBufferState(indicesBuffer, nvrhi::ResourceStates::IndexBuffer);



	nvrhi::VertexAttributeDesc attributes[] = {
	nvrhi::VertexAttributeDesc()
		.setName("POSITION")
		.setFormat(nvrhi::Format::RGB32_FLOAT)
		.setOffset(0)
		.setElementStride(sizeof(Vertex)),
	nvrhi::VertexAttributeDesc()
		.setName("TEXCOORD")
		.setFormat(nvrhi::Format::RG32_FLOAT)
		.setOffset(sizeof(float3))
		.setElementStride(sizeof(Vertex)),
	};

	m_InputLayout = m_Device->createInputLayout(attributes, uint32_t(std::size(attributes)), m_VertexShader);

	m_CommandList->close();
	GetDevice()->executeCommandList(m_CommandList);

	return true;
}

void SDFRendering::Render(nvrhi::IFramebuffer* framebuffer) {

	m_CommandList->open();
	nvrhi::utils::ClearColorAttachment(m_CommandList, framebuffer, 0, nvrhi::Color(0.f));

	if (!m_GraphicsPipeline) {
		nvrhi::GraphicsPipelineDesc pipelineDesc;
		pipelineDesc.inputLayout = m_InputLayout;
		pipelineDesc.bindingLayouts = { m_BindingLayout };
		pipelineDesc.VS = m_VertexShader;
		pipelineDesc.PS = m_PixelShader;
		pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
		pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;

		m_GraphicsPipeline = m_Device->createGraphicsPipeline(pipelineDesc, framebuffer);

		
	}

	RenderConstants renderConstants;
	renderConstants.g_Time = float4(delta, 0, 0, 0);
	renderConstants.g_Resolution = float4((float)framebuffer->getFramebufferInfo().width, (float)framebuffer->getFramebufferInfo().height, 0, 0);
	renderConstants.g_Switch = int4(1, 1, 1, 1);
	renderConstants.g_Factor = float2(256.0f, 20.0f);
	delta = delta + 0.001f;
	m_CommandList->writeBuffer(m_ConstantBuffer, &renderConstants, sizeof(RenderConstants));

	nvrhi::BindingSetDesc bindingSetDesc;
	bindingSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, m_ConstantBuffer))
		.addItem(nvrhi::BindingSetItem::Sampler(0,m_Sampler))
		.addItem(nvrhi::BindingSetItem::Texture_SRV(0,m_Texture));

	nvrhi::BindingSetHandle bindingSet = m_BindingSets.GetOrCreateBindingSet(bindingSetDesc, m_BindingLayout);

	nvrhi::GraphicsState state;
	state.pipeline = m_GraphicsPipeline;
	state.bindings = { bindingSet };
	state.framebuffer = framebuffer;
	state.viewport.addViewportAndScissorRect(framebuffer->getFramebufferInfo().getViewport());
	state.indexBuffer = nvrhi::IndexBufferBinding().setFormat(nvrhi::Format::R32_UINT);
	state.vertexBuffers.push_back(nvrhi::VertexBufferBinding());
	state.indexBuffer.buffer = indicesBuffer;
	state.vertexBuffers[0].buffer = vertexBuffer;

	m_CommandList->setGraphicsState(state);



	m_CommandList->drawIndexed(nvrhi::DrawArguments().setVertexCount(6));

	m_CommandList->close();
	GetDevice()->executeCommandList(m_CommandList);

}




#ifdef WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
#else
int main(int __argc, const char** __argv)
#endif
{
	nvrhi::GraphicsAPI api = app::GetGraphicsAPIFromCommandLine(__argc, __argv);
	app::DeviceManager* deviceManager = app::DeviceManager::Create(api);

	app::DeviceCreationParameters deviceParams;
#ifdef _DEBUG
	deviceParams.enableDebugRuntime = true;
	deviceParams.enableNvrhiValidationLayer = true;
#endif

	if (!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, "SDFRendering"))
	{
		log::fatal("Cannot initialize a graphics device with the requested parameters");
		return 1;
	}

	{
		SDFRendering example(deviceManager);
		if (example.InitPipeLine())
		{
			deviceManager->AddRenderPassToBack(&example);
			deviceManager->RunMessageLoop();
			deviceManager->RemoveRenderPass(&example);
		}
	}

	deviceManager->Shutdown();

	delete deviceManager;

	return 0;
}