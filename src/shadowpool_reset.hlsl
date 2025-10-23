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
#define MAX_LIGHTS_PER_TILE 4
#define HANDLE_NEAR_CLIPPING 1
#define MAX_SHADOW_LEVELS 7
// These are root 32-bit values

struct Light
{
    float boundingBoxRadius;
    float3 boundingBoxCenter;
};

// Root constant buffer


cbuffer InlineConstants : register(b0)
{
    uint g_LightCount;
};

cbuffer SceneConstantBuffer : register(b1)
{
    float4x4 viewMatrix;
    float4x4 projMatrix;
    float4x4 viewProjMatrix;
    float4x4 projInverseMatrix;
    float4x4 viewInverseMatrix;
    float nearPlaneDistance;
    int2 viewportSizeXY;
};


StructuredBuffer<Light> t_LightData : register(t0);
RWStructuredBuffer<uint> u_ShadowPoolSlotRW : register(u0);
RWStructuredBuffer<uint> u_LightPlaceSlotRW : register(u1);
RWStructuredBuffer<uint> u_LightLevelChangedSlotRW : register(u2);
RWStructuredBuffer<uint> u_Debug : register(u3);    //debug用，可去掉后面

groupshared uint lightLevelBufferValuableIndex;
groupshared int eachLevelFreeNum[7];

float2 CalculateScreenSpaceBoundingBox(float3 sphereCenter, float sphereRadius, float4x4 ProjMatrix, float4x4 viewProjMatrix, int2 screenSize)
{
    // 1. 将球心变换到裁剪空间
    float4 centerClip = mul(float4(sphereCenter, 1.0), viewProjMatrix);
    
    // 2. 透视除法得到球心的 NDC 坐标（无论 w 正负）
    float3 centerNDC = centerClip.xyz / centerClip.w;
    
    // 3. 计算投影半径
    // 使用投影矩阵的缩放因子除以深度来计算投影半径
    float projectionScaleX = ProjMatrix[0][0] / abs(centerClip.w);
    float projectionScaleY = ProjMatrix[1][1] / abs(centerClip.w);
    
    float radiusNDC_X = sphereRadius * projectionScaleX;
    float radiusNDC_Y = sphereRadius * projectionScaleY;
    
    // 4. 构建球体的 AABB（球心 ± 半径）
    float2 sphereAABBMin = centerNDC.xy - float2(radiusNDC_X, radiusNDC_Y);
    float2 sphereAABBMax = centerNDC.xy + float2(radiusNDC_X, radiusNDC_Y);
    
    // 5. 与 NDC 空间 [-1, 1] 求交
    float2 ndcMin = float2(-1.0, -1.0);
    float2 ndcMax = float2(1.0, 1.0);
    
    float2 intersectionMin = max(sphereAABBMin, ndcMin);
    float2 intersectionMax = min(sphereAABBMax, ndcMax);
    
    // 6. 检查是否相交
    if (intersectionMin.x >= intersectionMax.x || intersectionMin.y >= intersectionMax.y)
    {
        return float2(0, 0); // 不相交，投影像素为 0
    }
    
    // 7. 计算相交区域的像素尺寸
    float2 intersectionSizeNDC = intersectionMax - intersectionMin;
    float2 intersectionSizePixels = intersectionSizeNDC * float2(screenSize.x, screenSize.y) * 0.5;
    
    return intersectionSizePixels;
}

// 处理近裁剪面相交的情况
float2 CalculateScreenSpaceBoundingBoxWithNearClip(float3 sphereCenter, float sphereRadius,
                                                   float4x4 viewMatrix, float4x4 projMatrix,
                                                   int2 screenSize, float nearPlane)
{
    // 1. 变换到视图空间检查与近裁剪面的关系
    float4 centerView = mul(float4(sphereCenter, 1.0), viewMatrix);
    float sphereCenterZ = centerView.z; // 视图空间 Z
    
    // 2. 检查球体与近裁剪面的关系
    float distanceToNearPlane =  sphereCenterZ - nearPlane;
    
    if (distanceToNearPlane <= - sphereRadius)
    {
        return float2(0, 0); // 球体完全被近裁剪面裁掉
    }
    
    // 3. 确定有效的球心和半径
    float3 effectiveCenter;
    float effectiveRadius;
    
    if (distanceToNearPlane >=0)
    {
        // 球体完全在近裁剪面后面，使用原始参数
        effectiveCenter = sphereCenter;
        effectiveRadius = sphereRadius;
    }
    else
    {
        // 球体与近裁剪面相交，使用截面圆
        float intersectionRadius = sqrt(sphereRadius * sphereRadius - distanceToNearPlane * distanceToNearPlane);
        
        // 截面圆心位置（在近裁剪面上）
        float3 intersectionCenterView = float3(centerView.x, centerView.y, -nearPlane);
        
        // 转换回世界空间
        float4x4 invView = viewInverseMatrix;
        effectiveCenter = mul(float4(intersectionCenterView, 1.0), invView).xyz;
        effectiveRadius = intersectionRadius;
    }
    
    // 4. 使用有效参数计算投影
    float4x4 viewProjMatrix = mul(viewMatrix, projMatrix);
    return CalculateScreenSpaceBoundingBox(effectiveCenter, effectiveRadius, projMatrix, viewProjMatrix, screenSize);
}

// 根据像素大小确定在ShadowPool中的level
int DetermineShadowPoolSizeFromPixels(float2 projectionPixels)
{
    float maxDimension = projectionPixels.x *  projectionPixels.y;

    if (maxDimension > 1024.0 * 1024.0 && eachLevelFreeNum[0] > 0)
        return 0;
    else if(maxDimension > 512.0 * 512.0 && eachLevelFreeNum[1] > 0)
        return 1;
    else if (maxDimension > 256.0 * 256.0 && eachLevelFreeNum[2] > 0)
        return 2;
    else if (maxDimension > 128.0 * 128.0 && eachLevelFreeNum[3] > 0)
        return 3;
    else if (maxDimension > 64.0 * 64.0 && eachLevelFreeNum[4] > 0)
        return 4;
    else if (maxDimension > 32.0 * 32.0 && eachLevelFreeNum[5] > 0)
        return 5;
    else
        return 6;

}

uint AllocateShadowPoolSlot(uint startLevel)
{
    // 从低 level 到高 level 尝试分配
    for (uint level = startLevel; level < MAX_SHADOW_LEVELS; level++)
    {
        int beforeHead = u_ShadowPoolSlotRW[level];
        while (beforeHead != 0XFFFFFFFF)
        {   
            int currentHead;
            InterlockedCompareExchange(u_ShadowPoolSlotRW[level], beforeHead, u_ShadowPoolSlotRW[beforeHead], currentHead);
            if (currentHead == beforeHead&&currentHead != 0XFFFFFFFF)
            {
                u_ShadowPoolSlotRW[currentHead]=0XFFFFFFFF;
                return currentHead;
            }
            else
            {
                beforeHead = u_ShadowPoolSlotRW[level];
            }  
        }
    }  
    return 0xFFFFFFFF;   
}



void FreeShadowPoolSlot(uint level,uint slot)
{
    int slotNext;
    InterlockedExchange(u_ShadowPoolSlotRW[level], slot, slotNext);
    u_ShadowPoolSlotRW[slot]=slotNext;
}

int getLevel(int lightIndex)
{
    if(u_LightPlaceSlotRW[lightIndex]==0xFFFFFFFF)
        return -1;
    int SlotToLevel[MAX_SHADOW_LEVELS] = { 7, 8, 12, 28, 92, 348, 1372 };
     // 判断之前是在哪一层
    int curLevel = MAX_SHADOW_LEVELS - 1;
    for (int i = 0; i < MAX_SHADOW_LEVELS; i++)
    {
        if (u_LightPlaceSlotRW[lightIndex] < SlotToLevel[i])
        {
            curLevel = i - 1;
            break;
        }
    }
    return curLevel;
}

int AllocateTopSlot()
{
    int beforeHead = u_ShadowPoolSlotRW[0];
    if(beforeHead != 0XFFFFFFFF)
    {
        int currentHead;
        InterlockedCompareExchange(u_ShadowPoolSlotRW[0], beforeHead, u_ShadowPoolSlotRW[beforeHead], currentHead);
        if (currentHead == beforeHead && currentHead != 0XFFFFFFFF)
        {
            u_ShadowPoolSlotRW[currentHead] = 0XFFFFFFFF;
            return currentHead;
        }
        else
        {
            beforeHead = u_ShadowPoolSlotRW[0];
        }
    }
    return 0xFFFFFFFF;
}

uint AllocateSpecificLevel(uint level)
{
    int beforeHead = u_ShadowPoolSlotRW[level];
    while (beforeHead != 0XFFFFFFFF)
    {
        int currentHead;
        InterlockedCompareExchange(u_ShadowPoolSlotRW[level], beforeHead, u_ShadowPoolSlotRW[beforeHead], currentHead);
        if (currentHead == beforeHead && currentHead != 0XFFFFFFFF)
        {
            u_ShadowPoolSlotRW[currentHead] = 0XFFFFFFFF;
            return currentHead;
        }
        else
        {
            beforeHead = u_ShadowPoolSlotRW[level];
        }
    }
    return 0xFFFFFFFF;
}

// 主计算着色器
[numthreads(128, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint lightIndex = id.x;
    int release_flag = 0;   
    if (lightIndex >= g_LightCount)
        return;
    if (lightIndex == 0)
    {
        lightLevelBufferValuableIndex = 0;
        eachLevelFreeNum[0] = 1;
        eachLevelFreeNum[1] = 4;
        eachLevelFreeNum[2] = 16;
        eachLevelFreeNum[3] = 64;
        eachLevelFreeNum[4] = 256;
        eachLevelFreeNum[5] = 1024;
        eachLevelFreeNum[6] = 4096;
    }
        
    GroupMemoryBarrierWithGroupSync();
    Light currentLight = t_LightData[lightIndex];
    
    // 计算屏幕空间投影像素
    float2 projectionPixels;
    
#ifdef HANDLE_NEAR_CLIPPING
        projectionPixels = CalculateScreenSpaceBoundingBoxWithNearClip(
            currentLight.boundingBoxCenter,
            currentLight.boundingBoxRadius,
            viewMatrix,
            projMatrix,
            viewportSizeXY,
            nearPlaneDistance
        );
#else
    projectionPixels = CalculateScreenSpaceBoundingBox(
            currentLight.boundingBoxCenter,
            currentLight.boundingBoxRadius,
            viewProjMatrix,
            viewportSizeXY
        );
#endif
    
    // 处理不可见的情况
    if (projectionPixels.x <= 0.0 || projectionPixels.y <= 0.0) 
    {
        if (u_LightPlaceSlotRW[lightIndex] != 0xFFFFFFFF)  //如果之前占据了槽位，则释放
        {
            int curLevel = getLevel(lightIndex);
            FreeShadowPoolSlot(curLevel, u_LightPlaceSlotRW[lightIndex]);
        }
        //////////////////////////////debug///////////////////////////////
        //int insertIndex = 0;
        //InterlockedAdd(lightLevelBufferValuableIndex, 1, insertIndex);
        //u_LightLevelChangedSlotRW[insertIndex] = lightIndex;
        
        u_LightPlaceSlotRW[lightIndex] = 0xFFFFFFFF; 
        return;
    }
    
    // 根据投影像素大小分配阴影贴图
    int shadowPoolSizeCategory = DetermineShadowPoolSizeFromPixels(projectionPixels);
    u_Debug[2*lightIndex] = shadowPoolSizeCategory;
    GroupMemoryBarrierWithGroupSync();
    
    int curLevel = getLevel(lightIndex);
    u_Debug[2 * lightIndex + 1] = curLevel;
    if (curLevel != -1)
    {
        //判断哪些光源位置不变，统计该层目前可以申请的数量
        if (curLevel == 0 && curLevel == shadowPoolSizeCategory)
        {
            InterlockedAdd(eachLevelFreeNum[curLevel], -1);
            return;
        }
        else if (curLevel == 0)
        {
            FreeShadowPoolSlot(curLevel, u_LightPlaceSlotRW[lightIndex]);
            release_flag = 1;
        }
        GroupMemoryBarrierWithGroupSync();  
        if (eachLevelFreeNum[0] != 0 && shadowPoolSizeCategory == 0)     //如果该层有剩余槽位，则那些想要申请该层槽位的灯光可以尝试申请
        {
            uint placeSlot = AllocateSpecificLevel(shadowPoolSizeCategory);
            GroupMemoryBarrierWithGroupSync();
            if (placeSlot != 0xFFFFFFFF)              //申请成功
            {
                InterlockedAdd(eachLevelFreeNum[shadowPoolSizeCategory], -1);
                if (release_flag == 0)
                {
                    FreeShadowPoolSlot(curLevel, u_LightPlaceSlotRW[lightIndex]);
                    release_flag = 1;
                }
                u_LightPlaceSlotRW[lightIndex] = placeSlot;
                int insertIndex = 0;
                InterlockedAdd(lightLevelBufferValuableIndex, 1, insertIndex);
                u_LightLevelChangedSlotRW[insertIndex] = lightIndex;
                return;
            }
        }
        GroupMemoryBarrierWithGroupSync();
        shadowPoolSizeCategory = DetermineShadowPoolSizeFromPixels(projectionPixels);
        u_Debug[2 * lightIndex] = shadowPoolSizeCategory;
        
        if (curLevel == 1 && curLevel == shadowPoolSizeCategory)
        {
            InterlockedAdd(eachLevelFreeNum[curLevel], -1);
            return;
        }
        else if (curLevel == 1)
        {
            FreeShadowPoolSlot(curLevel, u_LightPlaceSlotRW[lightIndex]);
            release_flag = 1;
        }
        GroupMemoryBarrierWithGroupSync();  
        if (eachLevelFreeNum[1] != 0 && shadowPoolSizeCategory == 1)
        {
            uint placeSlot = AllocateSpecificLevel(shadowPoolSizeCategory);
            GroupMemoryBarrierWithGroupSync();
            if (placeSlot != 0xFFFFFFFF)
            {
                InterlockedAdd(eachLevelFreeNum[shadowPoolSizeCategory], -1);
                if (release_flag == 0)
                {
                    FreeShadowPoolSlot(curLevel, u_LightPlaceSlotRW[lightIndex]);
                    release_flag = 1;
                } 
                u_LightPlaceSlotRW[lightIndex] = placeSlot;
                int insertIndex = 0;
                InterlockedAdd(lightLevelBufferValuableIndex, 1, insertIndex);
                u_LightLevelChangedSlotRW[insertIndex] = lightIndex;
                return;
            }
        }
        GroupMemoryBarrierWithGroupSync();
        shadowPoolSizeCategory = DetermineShadowPoolSizeFromPixels(projectionPixels);
        u_Debug[2 * lightIndex] = shadowPoolSizeCategory;
        
        if (curLevel == 2 && curLevel == shadowPoolSizeCategory)
        {
            InterlockedAdd(eachLevelFreeNum[curLevel], -1);
            return;
        }
        else if (curLevel == 2)
        {
            FreeShadowPoolSlot(curLevel, u_LightPlaceSlotRW[lightIndex]);
            release_flag = 1;
        }
        GroupMemoryBarrierWithGroupSync();
        if (eachLevelFreeNum[2] != 0 && shadowPoolSizeCategory == 2)
        {
            uint placeSlot = AllocateSpecificLevel(shadowPoolSizeCategory);
            GroupMemoryBarrierWithGroupSync();
            if (placeSlot != 0xFFFFFFFF)
            {
                InterlockedAdd(eachLevelFreeNum[shadowPoolSizeCategory], -1);
                if (release_flag == 0)
                {
                    FreeShadowPoolSlot(curLevel, u_LightPlaceSlotRW[lightIndex]);
                    release_flag = 1;
                }
                u_LightPlaceSlotRW[lightIndex] = placeSlot;
                int insertIndex = 0;
                InterlockedAdd(lightLevelBufferValuableIndex, 1, insertIndex);
                u_LightLevelChangedSlotRW[insertIndex] = lightIndex;
                return;
            }
        }
        GroupMemoryBarrierWithGroupSync();
        shadowPoolSizeCategory = DetermineShadowPoolSizeFromPixels(projectionPixels);
        u_Debug[2 * lightIndex] = shadowPoolSizeCategory;
       
        if (curLevel == 3 && curLevel == shadowPoolSizeCategory)
        {
            InterlockedAdd(eachLevelFreeNum[curLevel], -1);
            return;
        }
        else if (curLevel == 3)
        {
            FreeShadowPoolSlot(curLevel, u_LightPlaceSlotRW[lightIndex]);
            release_flag = 1;
        }
        GroupMemoryBarrierWithGroupSync();
        if (eachLevelFreeNum[3] != 0 && shadowPoolSizeCategory == 3)
        {
            uint placeSlot = AllocateSpecificLevel(shadowPoolSizeCategory);
            GroupMemoryBarrierWithGroupSync();
            if (placeSlot != 0xFFFFFFFF)
            {
                InterlockedAdd(eachLevelFreeNum[shadowPoolSizeCategory], -1);
                if (release_flag == 0)
                {
                    FreeShadowPoolSlot(curLevel, u_LightPlaceSlotRW[lightIndex]);
                    release_flag = 1;
                }
                u_LightPlaceSlotRW[lightIndex] = placeSlot;
                int insertIndex = 0;
                InterlockedAdd(lightLevelBufferValuableIndex, 1, insertIndex);
                u_LightLevelChangedSlotRW[insertIndex] = lightIndex;
                return;
            }
        }
        GroupMemoryBarrierWithGroupSync();
        shadowPoolSizeCategory = DetermineShadowPoolSizeFromPixels(projectionPixels);
        
        if (curLevel == 4 && curLevel == shadowPoolSizeCategory)
        {
            InterlockedAdd(eachLevelFreeNum[curLevel], -1);
            return;
        }
        else if (curLevel == 4)
        {
            FreeShadowPoolSlot(curLevel, u_LightPlaceSlotRW[lightIndex]);
            release_flag = 1;
        }
        GroupMemoryBarrierWithGroupSync();
        if (eachLevelFreeNum[4] != 0 && shadowPoolSizeCategory == 4)
        {
            uint placeSlot = AllocateSpecificLevel(shadowPoolSizeCategory);
            GroupMemoryBarrierWithGroupSync();
            if (placeSlot != 0xFFFFFFFF)
            {
                InterlockedAdd(eachLevelFreeNum[shadowPoolSizeCategory], -1);
                if (release_flag == 0)
                {
                    FreeShadowPoolSlot(curLevel, u_LightPlaceSlotRW[lightIndex]);
                    release_flag = 1;
                }
                u_LightPlaceSlotRW[lightIndex] = placeSlot;
                int insertIndex = 0;
                InterlockedAdd(lightLevelBufferValuableIndex, 1, insertIndex);
                u_LightLevelChangedSlotRW[insertIndex] = lightIndex;
                return;
            }
        }
        GroupMemoryBarrierWithGroupSync();
        shadowPoolSizeCategory = DetermineShadowPoolSizeFromPixels(projectionPixels);
        
        if (curLevel == 5 && curLevel == shadowPoolSizeCategory)
        {
            InterlockedAdd(eachLevelFreeNum[curLevel], -1);
            return;
        }
        else if (curLevel == 5)
        {
            FreeShadowPoolSlot(curLevel, u_LightPlaceSlotRW[lightIndex]);
            release_flag = 1;
        }
        GroupMemoryBarrierWithGroupSync();
        if (eachLevelFreeNum[5] != 0 && shadowPoolSizeCategory == 5)
        {
            uint placeSlot = AllocateSpecificLevel(shadowPoolSizeCategory);
            GroupMemoryBarrierWithGroupSync();
            if (placeSlot != 0xFFFFFFFF)
            {
                InterlockedAdd(eachLevelFreeNum[shadowPoolSizeCategory], -1);
                if (release_flag == 0)
                {
                    FreeShadowPoolSlot(curLevel, u_LightPlaceSlotRW[lightIndex]);
                    release_flag = 1;
                }
                u_LightPlaceSlotRW[lightIndex] = placeSlot;
                int insertIndex = 0;
                InterlockedAdd(lightLevelBufferValuableIndex, 1, insertIndex);
                u_LightLevelChangedSlotRW[insertIndex] = lightIndex;
                return;
            }
        }
        GroupMemoryBarrierWithGroupSync();
        shadowPoolSizeCategory = DetermineShadowPoolSizeFromPixels(projectionPixels);
        
        if (curLevel == 6 && curLevel == shadowPoolSizeCategory)
        {
            InterlockedAdd(eachLevelFreeNum[curLevel], -1);
            return;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    shadowPoolSizeCategory = DetermineShadowPoolSizeFromPixels(projectionPixels);
    
    
    
    if (u_LightPlaceSlotRW[lightIndex] == 0xFFFFFFFF)                                   //之前没有槽位
    {
        uint placeSlot = AllocateShadowPoolSlot(shadowPoolSizeCategory);
        u_LightPlaceSlotRW[lightIndex] = placeSlot;
        int insertIndex = 0;
        InterlockedAdd(lightLevelBufferValuableIndex, 1, insertIndex);
        u_LightLevelChangedSlotRW[insertIndex] = lightIndex;
        return;
    }
    GroupMemoryBarrierWithGroupSync();
  
    if (release_flag == 0)
        FreeShadowPoolSlot(curLevel, u_LightPlaceSlotRW[lightIndex]);
    GroupMemoryBarrierWithGroupSync();
    uint placeSlot = AllocateShadowPoolSlot(shadowPoolSizeCategory);
    u_LightPlaceSlotRW[lightIndex] = placeSlot;
    int insertIndex = 0;
    InterlockedAdd(lightLevelBufferValuableIndex, 1, insertIndex);
    u_LightLevelChangedSlotRW[insertIndex] = lightIndex;
    
        
}

