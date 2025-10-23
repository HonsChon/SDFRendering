// ShadowMapReconstruction.hlsl
#define SHADOW_POOL_LEVEL 7

struct QuadTreeNode
{
    uint firstChildOrIndex;
};

cbuffer ReconstructionConstants : register(b0)
{
    uint lightIndex;
}

struct CodeBookEntry
{
    uint2 parames;
};

RWStructuredBuffer<CodeBookEntry> CodeBook[] : register(u0, space1);
RWStructuredBuffer<QuadTreeNode> QuardTree[] : register(u0, space2);
RWTexture2D<float> ReconstructedShadowMap[] : register(u0, space3);
RWStructuredBuffer<uint> u_LightPlaceSlotRW : register(u1);
RWStructuredBuffer<uint> u_LightPlaceFrameIndexRW : register(u2);

groupshared uint shadowMapSize;


// 将屏幕空间坐标和深度转换为NDC空间
// 屏幕空间坐标转NDC空间坐标
float3 ScreenToNDC(float2 screenPos, float depth)
{
    float2 screenUV = screenPos / shadowMapSize;
    float2 ndcXY = screenUV * 2.0 - 1.0;
    ndcXY.y = -ndcXY.y;
    float ndcZ = depth;
    return float3(ndcXY, ndcZ);
}

float3 ScreenToTile(float2 screenPos, float depth)
{
    float tileWidth = 32.0f;
    
    // Step 2: 确定所属的瓦片
    uint2 tileIndex = uint2(floor(screenPos / tileWidth));
    
    // Step 3: 计算瓦片内的像素坐标
    float2 tilePixelCoord = screenPos - (tileIndex * tileWidth);
    
    // Step 4: 转换为瓦片的归一化局部坐标 [-0.5, +0.5]
    float2 tileLocalCoord = tilePixelCoord / (tileWidth - 1.0f) - 0.5f;
    
    // Step 5: 组装最终结果
    return float3(tileLocalCoord.x, tileLocalCoord.y, depth);
}

// 深度评估函数
float EvaluateCompressedDepth(CodeBookEntry entry, float2 ndcPos, int indexInTile, uint compressionType)
{
    if (compressionType == 1) // 平面模式
    {
        float zoffset = asfloat(entry.parames.y);
        float ddx = f16tof32((entry.parames.x >> 16) & 0xFFFF);
        float ddy = f16tof32(entry.parames.x & 0xFFFF);   

        return ddx * ndcPos.x + ddy * ndcPos.y + zoffset;
    }
    else if (compressionType == 2) // 深度模式
    {
        uint packedMinMax = entry.parames.y;
        float leafMin = f16tof32(packedMinMax & 0xFFFF); // 低16位是min
        float leafMax = f16tof32((packedMinMax >> 16) & 0xFFFF); // 高16位是max
        
        uint packedOffsets = entry.parames.x;
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
    
    return 0;
}

// 重构单个像素的深度值
float ReconstructPixelDepth(uint2 pixelCoord, uint resourceIndex)
{
    // 计算tile坐标
    uint tilesPerRow = shadowMapSize / 32;
    uint2 tileCoord = pixelCoord / 32;
    uint tileIndex = tileCoord.y * tilesPerRow + tileCoord.x;
    
    // 获取tile根节点
    QuadTreeNode tileNode = QuardTree[resourceIndex][tileIndex];
    uint nodeType = asuint((tileNode.firstChildOrIndex & 0xC0000000u) >> 30);
    uint nodeIndex = asuint(tileNode.firstChildOrIndex & 0x3FFFFFFFu);
    if (nodeType != 0)
    {
        CodeBookEntry entry = CodeBook[resourceIndex][nodeIndex];
        float2 ndcPos = ScreenToNDC(pixelCoord, 0).xy;
        return EvaluateCompressedDepth(entry, ndcPos, 0, nodeType);
    }
    
    // 四叉树遍历
    uint2 localCoord = pixelCoord % 32;
    
    uint currentSize = 32;
    uint2 currentCoord = localCoord;
    
    int debugFlag = 0;
    
    [loop]
    while (currentSize > 1)
    {
        uint nodeOffset =  nodeIndex;
        QuadTreeNode currentNode = QuardTree[resourceIndex][nodeOffset];
        nodeType = asuint((currentNode.firstChildOrIndex & 0xC0000000) >> 30);
        nodeIndex = asuint(currentNode.firstChildOrIndex & 0x3FFFFFFF);
        
        
        
        if (nodeType != 0)
        {
            uint entryOffset = nodeIndex;
            CodeBookEntry entry = CodeBook[resourceIndex][entryOffset];
            float2 ndcPos = ScreenToNDC(pixelCoord, 0).xy;
            int indexInTile = ((localCoord % currentSize) / (currentSize / 2)).
            y * 2 + ((localCoord % currentSize) / (currentSize / 2)).x;
            
            return EvaluateCompressedDepth(entry, ndcPos, indexInTile, nodeType);
        }
        
        uint halfSize = currentSize / 2;
        uint quadrantX = currentCoord.x / halfSize;
        uint quadrantY = currentCoord.y / halfSize;
        uint quadrant = quadrantY * 2 + quadrantX;
        
        nodeIndex = nodeIndex + quadrant;
        currentCoord.x %= halfSize;
        currentCoord.y %= halfSize;
        currentSize = halfSize;
    }
    
    return 0;
  
}

[numthreads(32, 32, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    int SlotToLevel[SHADOW_POOL_LEVEL] = { 7, 8, 12, 28, 92, 348, 1372 };
    int offset = u_LightPlaceFrameIndexRW[lightIndex] * (pow(4, SHADOW_POOL_LEVEL) - 1) / 3;;
    int curLevel = SHADOW_POOL_LEVEL-1;
    for (int i = 0; i < SHADOW_POOL_LEVEL; i++)
    {
        if (u_LightPlaceSlotRW[lightIndex] < SlotToLevel[i])
        {
            curLevel = i - 1;
            break;
        }
    }
    
    shadowMapSize = uint(2048 / pow(2, curLevel));
    if (id.x >= shadowMapSize || id.y >= shadowMapSize)
        return;
    int resourceIndex = u_LightPlaceSlotRW[lightIndex] - SHADOW_POOL_LEVEL + offset;
    float depth = ReconstructPixelDepth(id.xy, resourceIndex);
    ReconstructedShadowMap[resourceIndex-offset][id.xy] = depth;
}