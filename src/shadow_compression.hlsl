#define TILE_SIZE 32
#define QUADS_PER_TILE_SIDE 16
#define MAX_QUAD_TREE_LEVELS 5
#define MAX_CODEBOOK_SIZE 1024
#define MAX_QUADTREE_NODES 8192
#define SHADOW_POOL_LEVEL 7
// 压缩类型
#define COMPRESSION_PLANE 0
#define COMPRESSION_QUAD_DEPTHS 1

struct CodeBookEntry
{
    uint2 params;
};

struct TemplateTreeNode
{
    uint level; // 节点层级
    int isLeaf; // 是否为叶子节点  
    uint firstChildOrCodeBook; // 叶子节点：CodeBook索引；非叶子节点：第一个子节点索引
    int compressionType; // 压缩类型
    float4 params; // 平面模式：(nx, ny, nz, d) | 深度模式：(depth0, depth1, depth2, depth3)
    
};

struct QuadTreeNode
{
    uint firstChildOrIndex;
};

RWStructuredBuffer<CodeBookEntry> CodeBook[] : register(u0, space1);
RWStructuredBuffer<QuadTreeNode> QuardTree[] : register(u0, space2);
RWStructuredBuffer<TemplateTreeNode> TemplateQuadTree[] : register(u0, space3);
RWStructuredBuffer<uint> TemplateNodeNumPerLevel[] : register(u0, space4);

RWStructuredBuffer<uint> TemplateNodeNumPerLevelRB2[] : register(u0, space12);
RWStructuredBuffer<uint> u_LightPlaceSlotRW : register(u2);
RWStructuredBuffer<uint> u_LevelChangedLight : register(u3);
RWStructuredBuffer<uint> frameIndexConstant : register(u4);

groupshared uint TileNum;
groupshared uint tileCountPerSide;

//将平面参数打包到uint2中
void SetDepthPlaneCompressionParams(float4 parames, inout CodeBookEntry entry)
{
    float zoffset = parames.w;
    uint ddx = f32tof16(parames.x);
    uint ddy = f32tof16(parames.y);
    
    entry.params.y = asuint(zoffset);
    entry.params.x = (entry.params.x & 0x0000FFFF) | (ddx & 0xFFFF) << 16;
    entry.params.x = (entry.params.x & 0xFFFF0000) | (ddy & 0xFFFF);
}

//将深度参数打包到uint2中
void SetDepthValuesParames(float4 parames, inout CodeBookEntry entry)
{
    float2 minMax1 = float2(min(parames.x, parames.y), max(parames.x, parames.y));
    float2 minMax2 = float2(min(parames.z, parames.w), max(parames.z, parames.w));
    float leafMin = min(minMax1.x, minMax2.x);
    float leafMax = max(minMax1.y, minMax2.y);
    
    
    // 打包min/max
    entry.params.y = (f32tof16(leafMin) & 0xFFFF) | ((f32tof16(leafMax) & 0xFFFF) << 16);
    
    // 批量量化 (避免分支)
    float depthRange = leafMax - leafMin;
    float4 normalizedOffsets = saturate((parames - leafMin) / max(depthRange, 1e-4f));
    uint4 quantized = uint4(normalizedOffsets * 255.0f + 0.5f);
    
    // 单指令打包
    entry.params.x = (quantized.x <<24) | (quantized.y << 16) | (quantized.z << 8) | quantized.w;
}

void SetQuadTreeNodeType(uint type, inout QuadTreeNode node)
{
    type = type & 0x3;
    node.firstChildOrIndex = node.firstChildOrIndex  | (type << 30);
}

void SetQuadTreeNodeValue(uint value, inout QuadTreeNode node)
{
    value = value & 0x3FFFFFFF;
    node.firstChildOrIndex = node.firstChildOrIndex | value;
}

//返回该光源对应槽位索引
int GetResourceIndex(uint lightIndex)
{
    return u_LightPlaceSlotRW[lightIndex] - SHADOW_POOL_LEVEL;
}

int getLevel(int lightIndex)       // 计算层级
{
    if (u_LightPlaceSlotRW[lightIndex] == 0xFFFFFFFF)
        return -1;
    int SlotToLevel[SHADOW_POOL_LEVEL] = { 7, 8, 12, 28, 92, 348, 1372 };

    int curLevel = 6;
    for (int i = 0; i < SHADOW_POOL_LEVEL; i++)
    {
        if (u_LightPlaceSlotRW[lightIndex] < SlotToLevel[i])
        {
            curLevel = i - 1;
            break;
        }
    }
    return curLevel;
}


[numthreads(1024, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 groupId : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
    if (u_LevelChangedLight[groupId.x] == 0xFFFFFFFF)
    {
        return;
    }
    int curLevel;
    int lightIndex = u_LevelChangedLight[groupId.x];
    curLevel = getLevel(lightIndex);
    if (localId.x == 0)
    { 
        tileCountPerSide = 2048 / ((pow(2, curLevel)) * 32);
        TileNum = tileCountPerSide * tileCountPerSide;
    } 
    GroupMemoryBarrierWithGroupSync();
    int threadTileNum = 1;
    if (curLevel == 0)
        threadTileNum = 4;
    if (curLevel > 0 && localId.x >= TileNum || curLevel == 0 && localId.x >= TileNum/4)
    {
        return;
    }
    
    int frameIndex = frameIndexConstant[0];
    int offset = frameIndex * (pow(4, SHADOW_POOL_LEVEL) - 1) / 3;
    int resourceIndex = GetResourceIndex(lightIndex)+offset;

    int curIndex = TileNum;
    for (int i = 0; i < threadTileNum * localId.x; i++)
    {
        curIndex += TemplateNodeNumPerLevel[resourceIndex][i];
    }
    int indexEndLastTile = curIndex;
    
    int curTile = threadTileNum * localId.x;
   
    for (int i = 0; i < threadTileNum; i++)
    {
        int nodeNum = TemplateNodeNumPerLevel[resourceIndex][curTile];
        QuardTree[resourceIndex][curTile].firstChildOrIndex = QuardTree[resourceIndex][curTile].firstChildOrIndex & 0x00000000;
        SetQuadTreeNodeType(0, QuardTree[resourceIndex][curTile]);
        SetQuadTreeNodeValue(curIndex, QuardTree[resourceIndex][curTile]);
        
        for (int j = 0; j < nodeNum; j++)
        {
            if (TemplateQuadTree[resourceIndex][342 * curTile + j].isLeaf == 1)
            {
                CodeBookEntry codeBookEntry;
                if (TemplateQuadTree[resourceIndex][342 * curTile + j].compressionType == 0)
                {
                    SetDepthPlaneCompressionParams(TemplateQuadTree[resourceIndex][342 * curTile + j].params, codeBookEntry);
                    SetQuadTreeNodeType(1, QuardTree[resourceIndex][curIndex]);
                }
                else if (TemplateQuadTree[resourceIndex][342 * curTile + j].compressionType == 1)
                {
                    SetDepthValuesParames(TemplateQuadTree[resourceIndex][342 * curTile + j].params, codeBookEntry);
                    SetQuadTreeNodeType(2, QuardTree[resourceIndex][curIndex]);
                }   
                SetQuadTreeNodeValue(TemplateQuadTree[resourceIndex][342 * curTile + j].firstChildOrCodeBook, QuardTree[resourceIndex][curIndex]);
                CodeBook[resourceIndex][QuardTree[resourceIndex][curIndex].firstChildOrIndex] = codeBookEntry;
            }
            else
            {
                SetQuadTreeNodeValue(TemplateQuadTree[resourceIndex][342 * curTile + j].firstChildOrCodeBook - 342 * curTile + indexEndLastTile, QuardTree[resourceIndex][curIndex]);
                SetQuadTreeNodeType(0, QuardTree[resourceIndex][curIndex]);
            }
            curIndex++;
        }
        indexEndLastTile = curIndex;
        curTile++;
    }

}