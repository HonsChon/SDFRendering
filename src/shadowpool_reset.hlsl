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
RWStructuredBuffer<uint> u_Debug : register(u3);    //debug�ã���ȥ������

groupshared uint lightLevelBufferValuableIndex;
groupshared int eachLevelFreeNum[7];

float2 CalculateScreenSpaceBoundingBox(float3 sphereCenter, float sphereRadius, float4x4 ProjMatrix, float4x4 viewProjMatrix, int2 screenSize)
{
    // 1. �����ı任���ü��ռ�
    float4 centerClip = mul(float4(sphereCenter, 1.0), viewProjMatrix);
    
    // 2. ͸�ӳ����õ����ĵ� NDC ���꣨���� w ������
    float3 centerNDC = centerClip.xyz / centerClip.w;
    
    // 3. ����ͶӰ�뾶
    // ʹ��ͶӰ������������ӳ������������ͶӰ�뾶
    float projectionScaleX = ProjMatrix[0][0] / abs(centerClip.w);
    float projectionScaleY = ProjMatrix[1][1] / abs(centerClip.w);
    
    float radiusNDC_X = sphereRadius * projectionScaleX;
    float radiusNDC_Y = sphereRadius * projectionScaleY;
    
    // 4. ��������� AABB������ �� �뾶��
    float2 sphereAABBMin = centerNDC.xy - float2(radiusNDC_X, radiusNDC_Y);
    float2 sphereAABBMax = centerNDC.xy + float2(radiusNDC_X, radiusNDC_Y);
    
    // 5. �� NDC �ռ� [-1, 1] ��
    float2 ndcMin = float2(-1.0, -1.0);
    float2 ndcMax = float2(1.0, 1.0);
    
    float2 intersectionMin = max(sphereAABBMin, ndcMin);
    float2 intersectionMax = min(sphereAABBMax, ndcMax);
    
    // 6. ����Ƿ��ཻ
    if (intersectionMin.x >= intersectionMax.x || intersectionMin.y >= intersectionMax.y)
    {
        return float2(0, 0); // ���ཻ��ͶӰ����Ϊ 0
    }
    
    // 7. �����ཻ��������سߴ�
    float2 intersectionSizeNDC = intersectionMax - intersectionMin;
    float2 intersectionSizePixels = intersectionSizeNDC * float2(screenSize.x, screenSize.y) * 0.5;
    
    return intersectionSizePixels;
}

// ������ü����ཻ�����
float2 CalculateScreenSpaceBoundingBoxWithNearClip(float3 sphereCenter, float sphereRadius,
                                                   float4x4 viewMatrix, float4x4 projMatrix,
                                                   int2 screenSize, float nearPlane)
{
    // 1. �任����ͼ�ռ�������ü���Ĺ�ϵ
    float4 centerView = mul(float4(sphereCenter, 1.0), viewMatrix);
    float sphereCenterZ = centerView.z; // ��ͼ�ռ� Z
    
    // 2. �����������ü���Ĺ�ϵ
    float distanceToNearPlane =  sphereCenterZ - nearPlane;
    
    if (distanceToNearPlane <= - sphereRadius)
    {
        return float2(0, 0); // ������ȫ�����ü���õ�
    }
    
    // 3. ȷ����Ч�����ĺͰ뾶
    float3 effectiveCenter;
    float effectiveRadius;
    
    if (distanceToNearPlane >=0)
    {
        // ������ȫ�ڽ��ü�����棬ʹ��ԭʼ����
        effectiveCenter = sphereCenter;
        effectiveRadius = sphereRadius;
    }
    else
    {
        // ��������ü����ཻ��ʹ�ý���Բ
        float intersectionRadius = sqrt(sphereRadius * sphereRadius - distanceToNearPlane * distanceToNearPlane);
        
        // ����Բ��λ�ã��ڽ��ü����ϣ�
        float3 intersectionCenterView = float3(centerView.x, centerView.y, -nearPlane);
        
        // ת��������ռ�
        float4x4 invView = viewInverseMatrix;
        effectiveCenter = mul(float4(intersectionCenterView, 1.0), invView).xyz;
        effectiveRadius = intersectionRadius;
    }
    
    // 4. ʹ����Ч��������ͶӰ
    float4x4 viewProjMatrix = mul(viewMatrix, projMatrix);
    return CalculateScreenSpaceBoundingBox(effectiveCenter, effectiveRadius, projMatrix, viewProjMatrix, screenSize);
}

// �������ش�Сȷ����ShadowPool�е�level
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
    // �ӵ� level ���� level ���Է���
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
     // �ж�֮ǰ������һ��
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

// ��������ɫ��
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
    
    // ������Ļ�ռ�ͶӰ����
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
    
    // �����ɼ������
    if (projectionPixels.x <= 0.0 || projectionPixels.y <= 0.0) 
    {
        if (u_LightPlaceSlotRW[lightIndex] != 0xFFFFFFFF)  //���֮ǰռ���˲�λ�����ͷ�
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
    
    // ����ͶӰ���ش�С������Ӱ��ͼ
    int shadowPoolSizeCategory = DetermineShadowPoolSizeFromPixels(projectionPixels);
    u_Debug[2*lightIndex] = shadowPoolSizeCategory;
    GroupMemoryBarrierWithGroupSync();
    
    int curLevel = getLevel(lightIndex);
    u_Debug[2 * lightIndex + 1] = curLevel;
    if (curLevel != -1)
    {
        //�ж���Щ��Դλ�ò��䣬ͳ�Ƹò�Ŀǰ�������������
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
        if (eachLevelFreeNum[0] != 0 && shadowPoolSizeCategory == 0)     //����ò���ʣ���λ������Щ��Ҫ����ò��λ�ĵƹ���Գ�������
        {
            uint placeSlot = AllocateSpecificLevel(shadowPoolSizeCategory);
            GroupMemoryBarrierWithGroupSync();
            if (placeSlot != 0xFFFFFFFF)              //����ɹ�
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
    
    
    
    if (u_LightPlaceSlotRW[lightIndex] == 0xFFFFFFFF)                                   //֮ǰû�в�λ
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

