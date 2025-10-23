#include "SDFRendering.h"

bool SDFRendering::InitPipeLine()
{
	m_CommandList = GetDevice()->createCommandList();
	m_Device = GetDevice();

	std::filesystem::path appShaderPath = app::GetDirectoryWithExecutable() / "shaders/SDFRendering" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());
	auto nativeFS = std::make_shared<vfs::NativeFileSystem>();

	engine::ShaderFactory shaderFactory(GetDevice(), nativeFS, appShaderPath);

	nvrhi::BindingLayoutDesc bindingLayoutDesc = nvrhi::BindingLayoutDesc()
		.setRegisterSpace(0)
		.setVisibility(nvrhi::ShaderType::All)
		.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(float2)));

	bindingLayoutDesc.visibility = nvrhi::ShaderType::Pixel;
	m_BindingLayout = m_Device->createBindingLayout(bindingLayoutDesc);

	m_VertexShader = shaderFactory.CreateShader("sdf_vs.hlsl", "VS", nullptr, nvrhi::ShaderType::Vertex);
	m_PixelShader = shaderFactory.CreateShader("sdf_ps.hlsl", "PS", nullptr, nvrhi::ShaderType::Pixel);
	
	if (!m_VertexShader || !m_PixelShader) {
		return false;
	}


	nvrhi::VertexAttributeDesc attributes[] = {
	nvrhi::VertexAttributeDesc()
		.setName("POSITION")
		.setFormat(nvrhi::Format::RGB32_FLOAT)
		.setOffset(0)
		.setElementStride(sizeof(float3) + sizeof(float2)),
	nvrhi::VertexAttributeDesc()
		.setName("TEXCOORD")
		.setFormat(nvrhi::Format::RG32_FLOAT)
		.setOffset(sizeof(float3))
		.setElementStride(sizeof(float3) + sizeof(float2)),
	};

	m_InputLayout = m_Device->createInputLayout(attributes, uint32_t(std::size(attributes)), nullptr);

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

		m_GraphicsPipeline = m_Device->createGraphicsPipeline(pipelineDesc, framebuffer);

		nvrhi::BufferDesc vbDesc;
		vbDesc.byteSize = sizeof(Vertex) * vertices.size();
		vbDesc.debugName = "VertexBuffer";
		vbDesc.isVertexBuffer = true;
		vbDesc.initialState = nvrhi::ResourceStates::VertexBuffer;
		vbDesc.keepInitialState = true;
		std::vector<int> indices = { 0,1,2,2,1,3 };
		vertexBuffer = m_Device->createBuffer(vbDesc);
		m_CommandList->writeBuffer(vertexBuffer, vertices.data(), vertices.size() * sizeof(Vertex));

		indicesBuffer = m_Device->createBuffer(
			nvrhi::BufferDesc()
			.setByteSize(sizeof(int) * indices.size())
			.setDebugName("IndexBuffer")
			.setIsIndexBuffer(true)
			.setInitialState(nvrhi::ResourceStates::IndexBuffer)
			.setKeepInitialState(true)
		);

		m_CommandList->writeBuffer(indicesBuffer, indices.data(), indices.size() * sizeof(int));
	}



	nvrhi::BindingSetDesc bindingSetDesc;
	bindingSetDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(float2)));

	nvrhi::BindingSetHandle bindingSet = m_BindingSets.GetOrCreateBindingSet(bindingSetDesc, m_BindingLayout);

	nvrhi::GraphicsState state;
	state.pipeline = m_GraphicsPipeline;
	state.bindings = { bindingSet };
	state.framebuffer = framebuffer;
	state.viewport.addViewportAndScissorRect(framebuffer->getFramebufferInfo().getViewport());
	state.indexBuffer = nvrhi::IndexBufferBinding().setFormat(nvrhi::Format::R16_UINT);
	state.vertexBuffers.push_back(nvrhi::VertexBufferBinding());
	state.indexBuffer.buffer = indicesBuffer;
	state.vertexBuffers[0].buffer = vertexBuffer;

	m_CommandList->setGraphicsState(state);
	m_CommandList->setPushConstants(float2(float(framebuffer->getFramebufferInfo().width), float(framebuffer->getFramebufferInfo().height)), sizeof(float2));
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