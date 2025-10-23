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
#define MAX_LIGHTS_PER_TILE 1024
// These are root 32-bit values

struct Light
{
    float boundingBoxRadius;
    float3 boundingBoxCenter;
};

// Root constant buffer


cbuffer InlineConstants : register(b0)
{
    uint g_LightTilesX, g_LightTilesY;
    uint g_LightCount;
};

cbuffer SceneConstantBuffer : register(b1)
{
    float4x4 viewMatrix;
    float4x4 ProjInverse;
    int2 viewportSizeXY;
};

Texture2D<float> t_DepthBuffer : register(t0);
StructuredBuffer<Light> t_LightData : register(t1);
RWStructuredBuffer<uint> u_CulledLightsDataRW : register(u0);

groupshared uint SharedMinDepth;
groupshared uint SharedMaxDepth;
groupshared uint SharedLightCount;
groupshared uint SharedLightIndices[MAX_LIGHTS_PER_TILE];


float3 ScreenToView(float3 NDCPos)
{
    float4 clipPos = float4(NDCPos, 1.0);
    clipPos.y *= -1.0;
    
    float4 viewPos = mul(clipPos,ProjInverse);
    return viewPos.xyz / viewPos.w;
}

float3 WorldToView(float3 worldPos)
{
    float4 screenPos = mul(float4(worldPos, 1.0),viewMatrix);
    return screenPos.xyz / screenPos.w;
}


bool BoundingBoxIntersection(float3 lightBoundingBoxCenter, float lightBoundingBoxRadius, float3 tileBoundingBoxMin, float3 tileBoundingBoxMax)
{
    float3 closestPoint = clamp(lightBoundingBoxCenter, tileBoundingBoxMin, tileBoundingBoxMax);
    float3 distance = lightBoundingBoxCenter - closestPoint;
    return dot(distance, distance) <= (lightBoundingBoxRadius * lightBoundingBoxRadius);
}



[numthreads(32, 32, 1)]
void CSMain(uint2 dispatchThreadId : SV_DispatchThreadID, uint2 groupThreadId : SV_GroupThreadID, uint2 groupId : SV_GroupID)
{
    const bool isThread0 = (groupThreadId.x == 0) && (groupThreadId.y == 0);
    const uint2 pixelXY = dispatchThreadId;
    const uint writeSlotStart = (groupId.y * g_LightTilesX + groupId.x) * MAX_LIGHTS_PER_TILE;

    if (isThread0)
    {
        SharedMinDepth = 0xFFFFFFFF;
        SharedMaxDepth = 0;
        SharedLightCount = 0;
    }
    GroupMemoryBarrierWithGroupSync();
       
    // ������Ȳ�����tile����ȷ�Χ
    float depth = t_DepthBuffer.Load(int3(pixelXY, 0));
    uint depthUint = asuint(depth);
    float cur_minDepth = asfloat(SharedMinDepth);
    float cur_maxDepth = asfloat(SharedMaxDepth);
    InterlockedMin(SharedMinDepth, depthUint);
    InterlockedMax(SharedMaxDepth, depthUint);
    GroupMemoryBarrierWithGroupSync();
    
    //��֤ÿ��tileֻ��һ���߳������й�Դ�ļ���
    if (isThread0)
    {
        float minDepth = asfloat(SharedMinDepth);
        float maxDepth = asfloat(SharedMaxDepth);
            
            // ����tile�ı�׼���豸���귶Χ
        float2 tileScale = 2.0 / float2(viewportSizeXY.x, viewportSizeXY.y);
        float2 minTileNDC = float2(groupId.xy * 32) * tileScale - 1.0;
        float2 maxTileNDC = float2((groupId.xy + 1) * 32) * tileScale - 1.0;
            
            // tile��8�����㣨����ռ䣩
        float3 frustumCorners[8];
            
            // ��ƽ��4������
        frustumCorners[0] = ScreenToView(float3(minTileNDC.x, minTileNDC.y, minDepth));
        frustumCorners[1] = ScreenToView(float3(maxTileNDC.x, minTileNDC.y, minDepth));
        frustumCorners[2] = ScreenToView(float3(maxTileNDC.x, maxTileNDC.y, minDepth));
        frustumCorners[3] = ScreenToView(float3(minTileNDC.x, maxTileNDC.y, minDepth));
        
        // Զƽ��4������
        frustumCorners[4] = ScreenToView(float3(minTileNDC.x, minTileNDC.y, maxDepth));
        frustumCorners[5] = ScreenToView(float3(maxTileNDC.x, minTileNDC.y, maxDepth));
        frustumCorners[6] = ScreenToView(float3(maxTileNDC.x, maxTileNDC.y, maxDepth));
        frustumCorners[7] = ScreenToView(float3(minTileNDC.x, maxTileNDC.y, maxDepth));
            
            // ����AABB
        float3 aabbMin = frustumCorners[0];
        float3 aabbMax = frustumCorners[0];
        
        for (int i = 1; i < 8; ++i)
        {
            aabbMin = min(aabbMin, frustumCorners[i]);
            aabbMax = max(aabbMax, frustumCorners[i]);
        }
        for (uint lightIndex = 0; lightIndex < g_LightCount; lightIndex++)
        {
            Light light = t_LightData[lightIndex];
            float3 center = WorldToView(light.boundingBoxCenter);
            if (BoundingBoxIntersection(center, light.boundingBoxRadius+10, aabbMin, aabbMax))   
            {
                uint index = SharedLightCount;
                if (index < MAX_LIGHTS_PER_TILE)
                {
                    SharedLightIndices[index] = lightIndex;
                    SharedLightCount++;
                }
            }
        }
            
        
        
    }
    GroupMemoryBarrierWithGroupSync();
    
    // ����4: д������ֻ�е�һ���߳�ִ�У�
    if (isThread0)
    {
        
        uint tileIndex = (groupId.y * g_LightTilesX + groupId.x) * MAX_LIGHTS_PER_TILE;
        uint lightCount = min(SharedLightCount, MAX_LIGHTS_PER_TILE);
        
     
        for (uint i = 0; i < lightCount; ++i)
        {
            u_CulledLightsDataRW[tileIndex + i] = SharedLightIndices[i];
        }
        if (lightCount < MAX_LIGHTS_PER_TILE)
        {
            u_CulledLightsDataRW[lightCount] = 0xFFFFFFFF;
        }
        
      

    }
    
    
   
}