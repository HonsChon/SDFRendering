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
#include <donut/engine/Scene.h>
#include <donut/engine/SceneGraph.h>
#include <wrl.h>
#include <donut/render/GBuffer.h>


using namespace::std;
using namespace::donut::engine;
using namespace donut::math;
using namespace donut::render;
#include <donut/shaders/light_cb.h>
#include <donut/shaders/view_cb.h>


class SDFRendering
{

public:

	struct Vertex
	{
		float3 position;
		float2 uv;
	};

	std::vector<Vertex> vertices =
	{
		{ { -1, -1, 0 },{ 0, 0 }},
		{ {  1, -1, 0 }, { 1, 0 }},
		{ {  -1, 1, 0 }, {0, 1 }},
		{ { 1, 1, 0}, {1, 1}},
	};    //设置我们ps的计算范围

	SDFRendering(nvrhi::IDevice* device) :m_Device(device), m_BindingSets(device) {

	}

	void InitPipeLine(nvrhi::IFramebuffer* framebuffer, ShaderFactory& shaderFactory, nvrhi::CommandListHandle commandlist);
	void Render(nvrhi::IFramebuffer* framebuffer);
protected:
	nvrhi::DeviceHandle m_Device;
	nvrhi::ShaderHandle m_VertexShader;
	nvrhi::ShaderHandle m_PixelShader;
	nvrhi::GraphicsPipelineHandle m_Pipeline;
	nvrhi::CommandListHandle m_CommandList;
	nvrhi::BufferHandle vertexBuffer;
	nvrhi::BufferHandle indicesBuffer;
	nvrhi::InputLayoutHandle m_InputLayout;
	nvrhi::GraphicsPipelineHandle m_GraphicsPipeline;
	nvrhi::BindingLayoutHandle m_BindingLayout;
	BindingCache m_BindingSets;
};

