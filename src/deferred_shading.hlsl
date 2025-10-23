/*
* Copyright (c) 2014-2024, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#pragma pack_matrix(row_major)
#include <donut/shaders/gbuffer.hlsli>
#include <donut/shaders/view_cb.h>
#include <donut/shaders/lighting.hlsli>

#define MAX_LIGHTS_PER_TILE 1024
#define SHADOW_POOL_LEVEL 7


// These are root 32-bit values
cbuffer InlineConstants : register(b0)
{
    uint g_LightTilesX, g_LightTilesY;
    uint g_LightCount;
};
cbuffer SceneConstantBuffer : register(b1)
{
    PlanarViewConstants view;
};

cbuffer DeferedLightParameter : register(b2)
{
    float4 ambientColorTop;
    float4 ambientColorBottom;
    int2 viewportSizeXY;
};

struct QuadTreeNode
{
    uint firstChildOrIndex;
};

struct CodeBookEntry
{
    uint2 params;
};



StructuredBuffer<uint> t_CulledLightsData : register(t0);
StructuredBuffer<LightConstants> t_LightsData : register(t1);
RWTexture2D<float4> u_LDRBuffer : register(u0);
Texture2D<float> t_DepthBuffer : register(t2);
Texture2D t_GBufferDiffuse : register(t3);
Texture2D t_GBufferSpecular: register(t4);
Texture2D t_GBufferNormal : register(t5);
Texture2D t_GBufferEmissive : register(t6);

StructuredBuffer<ShadowConstants> t_shadowContants : register(t7);
RWStructuredBuffer<uint> u_LightPlaceSlotRW : register(u8);
RWStructuredBuffer<uint> frameIndexBuffer : register(u9);
RWStructuredBuffer<uint> u_LightPlaceSlotFrameIndexRW : register(u10);
RWStructuredBuffer<CodeBookEntry> CodeBook[] : register(u0, space1);
RWStructuredBuffer<QuadTreeNode> QuardTree[] : register(u0, space2);


//返回该光源对应槽位索引
int GetResourceIndex(uint lightIndex)
{
    return u_LightPlaceSlotRW[lightIndex] - SHADOW_POOL_LEVEL;
}

int GetLightLevel(uint lightIndex)
{
    int SlotToLevel[SHADOW_POOL_LEVEL] = { 7, 8, 12, 28, 92, 348, 1372 };
    int slotIndex = u_LightPlaceSlotRW[lightIndex];
    int curLevel = SHADOW_POOL_LEVEL - 1;
    for (int i = 0; i < SHADOW_POOL_LEVEL; i++)
    {
        if (slotIndex < SlotToLevel[i])
        {
            curLevel = i - 1;
            break;
        }
    }
    return curLevel;
}

// 根据压缩类型评估深度值
float EvaluateCompressedDepth(CodeBookEntry entry, float2 ndcPos, int indexInTile, uint compressionType)
{
    if (compressionType == 1) // 平面模式
    {
        float zoffset = asfloat(entry.params.y);
        float ddx = f16tof32((entry.params.x >> 16) & 0xFFFF);
        float ddy = f16tof32(entry.params.x & 0xFFFF);
        return ddx * ndcPos.x + ddy * ndcPos.y + zoffset;
    }
    else if (compressionType == 2) // 深度模式
    {
        uint packedMinMax = entry.params.y;
        float leafMin = f16tof32(packedMinMax & 0xFFFF); // 低16位是min
        float leafMax = f16tof32((packedMinMax >> 16) & 0xFFFF); // 高16位是max
        
        
        uint packedOffsets = entry.params.x;
        uint4 quantized = uint4(
        (packedOffsets >> 24) & 0xFF, // depth0: 最高8位
        (packedOffsets >> 16) & 0xFF, // depth1: 次高8位  
        (packedOffsets >> 8) & 0xFF, // depth2: 次低8位
        (packedOffsets) & 0xFF // depth3: 最低8位
        );
    
        float4 normalizedOffsets = float4(quantized) / 255.0f;
    
        float depthRange = max(leafMax - leafMin, 1e-4f);
        float4 depthValues = leafMin + normalizedOffsets * depthRange;
    
        return depthValues[indexInTile];
    }
    
    return compressionType;
}

// 从压缩数据中解压阴影深度
float DecompressShadowDepth(uint lightIndex, uint resourceIndex, float2 uv, float3 ndcSpace)
{
    int level = GetLightLevel(lightIndex);
    const uint shadowMapSize = 2048 / pow(2,level);
    const uint tileSize = 32;
    const uint tilesPerRow = shadowMapSize / tileSize; 
    const uint tilesPerCol = shadowMapSize / tileSize;
    uint tileNum = tilesPerRow * tilesPerCol;
    
    // 将UV转换为像素坐标
    uint2 pixelCoord = uint2(uv * shadowMapSize);
    
    // 计算tile坐标
    uint2 tileCoord = pixelCoord / tileSize;
    uint tileIndex = tileCoord.y * tilesPerRow + tileCoord.x;
    
    // 边界检查
    if (tileIndex >= tileNum) // 超出tile范围
        return 1.0;
    
    // 获取tile对应的四叉树根节点索引
    QuadTreeNode tileNode = QuardTree[resourceIndex][tileIndex];
    
    // 如果tile节点就是叶子节点（整个tile使用同一压缩）
    uint nodeType = asuint((tileNode.firstChildOrIndex & 0xC0000000u) >> 30);
    uint nodeIndex = asuint(tileNode.firstChildOrIndex & 0x3FFFFFFFu);
    if (nodeType!=0)
    {
        CodeBookEntry entry = CodeBook[resourceIndex][nodeIndex];
        
        return EvaluateCompressedDepth(entry, ndcSpace.xy, 0, nodeType); // 使用tile中心
    }
    
    // 计算在tile内的相对坐标 [0, tileSize-1]
    uint2 localCoord = pixelCoord % tileSize;
    
    // 开始稀疏四叉树遍历
    uint currentSize = tileSize;
    uint2 currentCoord = localCoord;
    
    [loop]
    while (currentSize > 1)
    {
        QuadTreeNode currentNode = QuardTree[resourceIndex][nodeIndex];
        nodeType = asuint((currentNode.firstChildOrIndex & 0xC0000000u) >> 30);
        nodeIndex = asuint(currentNode.firstChildOrIndex & 0x3FFFFFFFu);
        if (nodeType != 0)
        {
            // 找到叶子节点，解压深度
            CodeBookEntry entry = CodeBook[resourceIndex][nodeIndex];
            int indexInTile = ((localCoord % currentSize) / (currentSize / 2)).
            y * 2 + ((localCoord % currentSize) / (currentSize / 2)).x;
            return EvaluateCompressedDepth(entry, ndcSpace.xy, indexInTile, nodeType);
        }
        
        // 计算在当前层级的象限
        uint halfSize = currentSize / 2;
        uint quadrantX = currentCoord.x / halfSize;
        uint quadrantY = currentCoord.y / halfSize;
        uint quadrant = quadrantY * 2 + quadrantX;
        
        // 移动到子节点
        nodeIndex = nodeIndex + quadrant;
        
        // 更新坐标为相对于当前象限的坐标
        currentCoord.x %= halfSize;
        currentCoord.y %= halfSize;
        currentSize = halfSize;
    }
    
    return nodeIndex;
}



float SampleShadowMap(uint resourceIndex, uint lightIndex, float3 worldPos)
{
    // 获取光源的视图投影矩阵
    float4x4 lightViewProj = t_shadowContants[lightIndex].matWorldToUvzwShadow;
    
    // 将世界坐标转换到光源空间
    float4 shadowCoord = mul(float4(worldPos, 1.0), lightViewProj);
    shadowCoord.xyz /= shadowCoord.w;
    
    // 转换到纹理坐标 [0,1]
    float2 shadowUV = shadowCoord.xy * 0.5 + 0.5;
    shadowUV.y = 1.0 - shadowUV.y; // 翻转Y轴
    
    // 边界检查
    if (any(shadowUV < 0) || any(shadowUV > 1) || shadowCoord.z < 0 || shadowCoord.z > 1)
        return 1.0; // 超出阴影贴图范围，无阴影
    
    // 解压缩阴影深度
    float shadowDepth = DecompressShadowDepth(lightIndex ,resourceIndex, shadowUV, shadowCoord.xyz);

    // 阴影比较
    float bias = 0.005f;
    float nearPlane = 0.1f;
    float farPlane = t_shadowContants[lightIndex].shadowFalloffDistance;
    
    float z_view = (nearPlane * farPlane) / (farPlane - shadowCoord.z * (farPlane - nearPlane));
    // 线性化到[0,1]范围
    float linearDepth = (z_view - nearPlane) / (farPlane - nearPlane);
    
    return (linearDepth - bias) <= shadowDepth ? 1.0 : 0.0;
}


bool PointInSpotLight(float3 worldPosition, float depth, LightConstants light)
{

    const float3 lightToPoint = (worldPosition - light.position);

    const float cosOuterAngle = cos(light.outerAngle);
    const float cosAlpha = dot(lightToPoint, light.direction);
    return (cosAlpha >= cosOuterAngle) && (dot(lightToPoint, lightToPoint) <= 1 / light.angularSizeOrInvRange);
}


[numthreads(32, 32, 1)]
void CSMain(uint2 dispatchThreadId : SV_DispatchThreadID, uint2 groupId : SV_GroupID)
{
    int2 pixelXY = dispatchThreadId;
    float depth = t_DepthBuffer.Load(uint3(pixelXY, 0));
    uint lightReadSlotIndex = (groupId.y * g_LightTilesX + groupId.x) * MAX_LIGHTS_PER_TILE;


    
    // 边界检查
    if (pixelXY.x >= (uint) viewportSizeXY.x || pixelXY.y >= (uint) viewportSizeXY.y)
        return;
    
    
    
    float4 gbufferChannels[4];
    gbufferChannels[0] = t_GBufferDiffuse[pixelXY];
    gbufferChannels[1] = t_GBufferSpecular[pixelXY];
    gbufferChannels[2] = t_GBufferNormal[pixelXY];
    gbufferChannels[3] = t_GBufferEmissive[pixelXY];
    MaterialSample surfaceMaterial = DecodeGBuffer(gbufferChannels);
    
    float3 surfaceWorldPos = ReconstructWorldPosition(view, float2(pixelXY) + 0.5, depth);
    float3 viewIncident = GetIncidentVector(view.cameraDirectionOrPosition, surfaceWorldPos);
    
    static const bool useCulledLights = true;

    float3 color = float3(0, 0, 0);
    float ambientOcclusion = 1;
    uint lightCount = g_LightCount;
    float3 diffuseTerm = 0;
    float3 specularTerm = 0;

    for (uint i = 0; i < lightCount; i++)
    {
        const uint lightIndex = useCulledLights ? t_CulledLightsData[lightReadSlotIndex + i] : i;

        if (lightIndex == 0xFFFFFFFF)
            break;
        
        int offset = frameIndexBuffer[0] * (pow(4, SHADOW_POOL_LEVEL) - 1) / 3;
        int resourceIndex = GetResourceIndex(lightIndex) + offset;
        
        LightConstants light = t_LightsData[lightIndex];
        if (light.lightType == 2 && !PointInSpotLight(surfaceWorldPos, depth, light))   //聚光灯需要判断是否在范围内
            continue;

        // **添加阴影计算**
        float shadowFactor = 1.0;
    
    // 只对方向光和聚光灯计算阴影（假设点光源暂不支持阴影）
        if (light.lightType == 1 || light.lightType == 2 && u_LightPlaceSlotRW[lightIndex] != 0xFFFFFFFF) // 方向光或聚光灯
        {
            shadowFactor = SampleShadowMap(resourceIndex, lightIndex, surfaceWorldPos);
        }

        float3 diffuseRadiance, specularRadiance;
        ShadeSurface(light, surfaceMaterial, surfaceWorldPos, viewIncident, diffuseRadiance, specularRadiance);

        // **应用阴影衰减**
        diffuseTerm += diffuseRadiance * light.color * shadowFactor;
        specularTerm += specularRadiance * light.color * shadowFactor;
    
        
    }
    
    {
        float3 ambientColor = lerp(ambientColorBottom.rgb, ambientColorTop.rgb, surfaceMaterial.shadingNormal.y * 0.5 + 0.5);

        diffuseTerm += ambientColor * surfaceMaterial.diffuseAlbedo * ambientOcclusion * surfaceMaterial.occlusion;
        specularTerm += ambientColor * surfaceMaterial.specularF0 * ambientOcclusion * surfaceMaterial.occlusion;
    }

 

    float3 outputColor = diffuseTerm
        + specularTerm
        + surfaceMaterial.emissiveColor;
    u_LDRBuffer[pixelXY] = float4(outputColor, 0);
}
