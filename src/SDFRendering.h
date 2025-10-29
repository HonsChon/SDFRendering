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
#include <donut/engine/TextureCache.h>
#include <donut/engine/Scene.h>
#include <donut/engine/SceneGraph.h>
#include <wrl.h>
#include <donut/render/GBuffer.h>

#include <donut/app/ApplicationBase.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <nvrhi/utils.h>

using namespace donut;


using namespace::std;
using namespace::donut::engine;
using namespace donut::math;
using namespace donut::render;

#include <donut/shaders/light_cb.h>
#include <donut/shaders/view_cb.h>


class SDFRendering : public app::IRenderPass
{

public:

	SDFRendering(app::DeviceManager* deviceManager) :IRenderPass(deviceManager),m_BindingSets(deviceManager->GetDevice()) {
		auto fs = std::make_shared<donut::vfs::NativeFileSystem>();
		textureCache = std::make_shared<donut::engine::TextureCache>(deviceManager->GetDevice(),fs,nullptr);
	};
	struct Vertex
	{
		float3 position;
		float2 uv;
	};

	struct RenderConstants
	{
		float4 g_Time;
		float4 g_Resolution;
		int4 g_Switch;
		float2 g_Factor;
	};




	bool InitPipeLine();
	void Render(nvrhi::IFramebuffer* framebuffer) override;

	void BackBufferResizing() override
	{
		m_Pipeline = nullptr;
	}

	void Animate(float fElapsedTimeSeconds) override
	{
		GetDeviceManager()->SetInformativeWindowTitle("g_WindowTitle");
	}
protected:
	float delta = 0.0f;
	nvrhi::DeviceHandle m_Device;
	nvrhi::ShaderHandle m_VertexShader;
	nvrhi::ShaderHandle m_PixelShader;
	nvrhi::GraphicsPipelineHandle m_Pipeline;
	nvrhi::CommandListHandle m_CommandList;

	nvrhi::BufferHandle vertexBuffer;
	nvrhi::BufferHandle indicesBuffer;
	nvrhi::BufferHandle m_ConstantBuffer;
	nvrhi::SamplerHandle m_Sampler;
	nvrhi::TextureHandle m_Texture;

	nvrhi::InputLayoutHandle m_InputLayout;
	nvrhi::GraphicsPipelineHandle m_GraphicsPipeline;
	nvrhi::BindingLayoutHandle m_BindingLayout;
	BindingCache m_BindingSets;
	std::shared_ptr<donut::engine::TextureCache> textureCache;
};

