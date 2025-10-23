
#define TILE_SIZE 32
#define QUADS_PER_TILE_SIDE 16
#define MAX_QUAD_TREE_LEVELS 5
#define MAX_CODEBOOK_SIZE 1024
#define MAX_QUADTREE_NODES 8192
#define SHADOW_POOL_LEVEL 7
// 压缩类型
#define COMPRESSION_PLANE 0
#define COMPRESSION_QUAD_DEPTHS 1




struct TemplateCodeBookEntry
{
    int compressionType; // 压缩类型
    float4 params; // 平面模式：(nx, ny, nz, d) | 深度模式：(depth0, depth1, depth2, depth3)
};

struct TemplateTreeNode
{
    int isLeaf;
    int level;
    int2 childNodes[4];
    TemplateCodeBookEntry codeBookEntry;
    int compressionIndex;
    
};

struct QuadTreeNode
{
    uint level; // 节点层级
    int isLeaf; // 是否为叶子节点  
    uint firstChildOrCodeBook; // 叶子节点：CodeBook索引；非叶子节点：第一个子节点索引
    int compressionType; // 压缩类型
    float4 params; // 平面模式：(nx, ny, nz, d) | 深度模式：(depth0, depth1, depth2, depth3)
};

cbuffer CompressionParams : register(b0)
{
    uint2 shadowMapSize;
    uint2 tileCount;
    uint maxQuadTreeLevel; 
    uint lightIndex;
    float compressionErrorThreshold; 
    int frameIndex;
    float nearPlane;
    float farPlane;
};

Texture2D<float> InputShadowMap : register(t0);


RWStructuredBuffer<QuadTreeNode> TemplateQuadTree[] : register(u0, space3);
RWStructuredBuffer<uint> TemplateNodeNumPerTile[]   : register(u0, space4);

RWStructuredBuffer<uint> CodeBookHash : register(u0);
RWStructuredBuffer<uint> CodeBookCounter : register(u1);
RWStructuredBuffer<uint> QuadTreeCounter : register(u2);
RWStructuredBuffer<uint> u_LightPlaceSlotRW : register(u3);
RWStructuredBuffer<TemplateCodeBookEntry> CodeBookHashEntry : register(u4);

groupshared TemplateTreeNode gs_levelNodes_0[16][16];
groupshared TemplateTreeNode gs_levelNodes_1[8][8];
groupshared TemplateTreeNode gs_levelNodes_2[4][4];
groupshared TemplateTreeNode gs_levelNodes_3[2][2];
groupshared TemplateTreeNode gs_levelNodes_4[1][1];
groupshared uint quadTreeNodeOffset;
groupshared uint nodeIndexQuardTree;

//返回该光源对应槽位索引
int GetResourceIndex()
{
    return u_LightPlaceSlotRW[lightIndex] - SHADOW_POOL_LEVEL;
}


// 屏幕空间坐标转NDC空间坐标
float3 ScreenToNDC(float2 screenPos, float depth)
{
    float2 screenUV = screenPos / shadowMapSize;
    float2 ndcXY = screenUV * 2.0 - 1.0;   
    ndcXY.y = -ndcXY.y;
    float ndcZ = depth; 
    
    float z_view = (nearPlane * farPlane) / (farPlane - ndcZ * (farPlane - nearPlane));
// 线性化到[0,1]范围
    float linearDepth = (z_view - nearPlane) / (farPlane - nearPlane);
    
    return float3(ndcXY, linearDepth);
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

bool FitDepthPlaneLeastSquares_SST(float3 screenPos[4], out float4 planeParams, out float maxError)
{
    float2 ata = float2(0.0, 0.0); // [Σ(x²), Σ(y²)]
    float2 atb = float2(0.0, 0.0); // [Σ(xy), Σ(xz), Σ(yz)]
    
    for (int i = 0; i < 4; i++)
    {
        float x = screenPos[i].x;
        float y = screenPos[i].y;
        float z = screenPos[i].z;
        
        ata.x += x * x; // Σ(x²)
        ata.y += y * y; // Σ(y²)
        
        atb.x += x * z; // Σ(x*z)
        atb.y += y * z; // Σ(y*z)
    }
    
    float ddx = atb.x / ata.x;
    float ddy = atb.y / ata.y;
    
    float minz = 1e10;
    float maxz = -1e10;
    
    for (int i = 0; i < 4; i++)
    {
        float x = screenPos[i].x;
        float y = screenPos[i].y;
        float z = screenPos[i].z;
        
        float plane_z = ddx * x + ddy * y;
        
        minz = min(minz, z - plane_z);
        maxz = max(maxz, z - plane_z);
    }
    
    float error = maxz - minz;
    
    
    float3 normal = float3(ddx, ddy, -1.0);
    
    planeParams = float4(normal, maxz);
    maxError = error;
    
    return error < compressionErrorThreshold * 1.5;
}

bool FitDepthPlaneLeastSquares(float3 screenPos[4], out float4 planeParams, out float maxError)
{
    // 累加统计量
    float Sx = 0.0, Sy = 0.0, Sz = 0.0;
    float Sxx = 0.0, Syy = 0.0, Sxy = 0.0;
    float Sxz = 0.0, Syz = 0.0;

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float x = screenPos[i].x;
        float y = screenPos[i].y;
        float z = screenPos[i].z;

        Sx += x;
        Sy += y;
        Sz += z;
        Sxx += x * x;
        Syy += y * y;
        Sxy += x * y;
        Sxz += x * z;
        Syz += y * z;
    }

    // 正规方程 A * [a b c]^T = B
    float A00 = Sxx, A01 = Sxy, A02 = Sx;
    float A10 = Sxy, A11 = Syy, A12 = Sy;
    float A20 = Sx, A21 = Sy, A22 = 4.0; // n = 4
    float B0 = Sxz, B1 = Syz, B2 = Sz;

    // 3x3 行列式
    float detA =
          A00 * (A11 * A22 - A12 * A21)
        - A01 * (A10 * A22 - A12 * A20)
        + A02 * (A10 * A21 - A11 * A20);

    float a = 0.0, b = 0.0, c = 0.0;
    const float eps = 1e-6;

    bool ok = abs(detA) > eps;
    if (ok)
    {
        // Cramer 法则
        float detA_a =
              B0 * (A11 * A22 - A12 * A21)
            - A01 * (B1 * A22 - A12 * B2)
            + A02 * (B1 * A21 - A11 * B2);

        float detA_b =
              A00 * (B1 * A22 - A12 * B2)
            - B0 * (A10 * A22 - A12 * A20)
            + A02 * (A10 * B2 - B1 * A20);

        float detA_c =
              A00 * (A11 * B2 - B1 * A21)
            - A01 * (A10 * B2 - B1 * A20)
            + B0 * (A10 * A21 - A11 * A20);

        a = detA_a / detA;
        b = detA_b / detA;
        c = detA_c / detA;
    }
    else
    {
        // 退化兜底：零均值后按轴向回归 + 截距
        float mx = Sx * 0.25;
        float my = Sy * 0.25;
        float mz = Sz * 0.25;

        float Sxxc = 0.0, Syyc = 0.0, Sxzc = 0.0, Syzc = 0.0;
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            float x = screenPos[i].x - mx;
            float y = screenPos[i].y - my;
            float z = screenPos[i].z - mz;
            Sxxc += x * x;
            Syyc += y * y;
            Sxzc += x * z;
            Syzc += y * z;
        }

        a = (Sxxc > eps) ? (Sxzc / Sxxc) : 0.0;
        b = (Syyc > eps) ? (Syzc / Syyc) : 0.0;
        c = mz - a * mx - b * my;
    }

    // 计算残差并设置“上界”offset
    float min_r = 1e10;
    float max_r = -1e10;

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float x = screenPos[i].x;
        float y = screenPos[i].y;
        float z = screenPos[i].z;
        float fit = a * x + b * y + c;
        float r = z - fit; // 残差
        min_r = min(min_r, r);
        max_r = max(max_r, r);
    }

    float offset = c + max_r; // 保守上界（减少自阴影）
    maxError = max_r - min_r; // 误差范围（可选作质量评估）
    planeParams = float4(a, b, -1, offset);

    return maxError < compressionErrorThreshold;
}

// 创建平面压缩条目
TemplateCodeBookEntry
    CreatePlaneEntry(
    float4 planeParams)
{
    TemplateCodeBookEntry entry;
    entry.compressionType = COMPRESSION_PLANE;
    entry.params = planeParams;

    return entry;
}

// 创建深度存储条目
TemplateCodeBookEntry CreateQuadDepthsEntry(float3 screenPos[4])
{
    TemplateCodeBookEntry entry;
    entry.compressionType = COMPRESSION_QUAD_DEPTHS;
    
    entry.params = float4(screenPos[0].z, screenPos[1].z, screenPos[2].z, screenPos[3].z);
    return entry;
}

// 选择最优压缩方法
TemplateCodeBookEntry ChooseBestCompression(float3 screenPos[4])
{
    float4 planeParams;
    float planeError;
    
    for (int i = 0; i < 4; i++)
    {
        screenPos[i] = ScreenToNDC(screenPos[i].xy, screenPos[i].z);
    }
    
    // 尝试平面拟合
    if (FitDepthPlaneLeastSquares(screenPos, planeParams, planeError))
    {
    // 可以用平面拟合，验证误差
        TemplateCodeBookEntry planeEntry = CreatePlaneEntry(planeParams);
        return planeEntry;
    }
    
    // 无法用平面拟合，直接存储四个深度值
    return CreateQuadDepthsEntry(screenPos);
}

uint AllocateQuadTreeNode(uint resourceIndex)
{
    uint nodeIndex;
    InterlockedAdd(quadTreeNodeOffset, 1, nodeIndex);
    return nodeIndex;
}

uint HashCodeBookEntry(TemplateCodeBookEntry entry)
{
     // 将float参数转换为uint进行哈希
    uint4 intParams = asuint(entry.params);
    
    // 从compressionType开始构建哈希
    uint hash = entry.compressionType;
    
    // 依次混合所有参数
    hash ^= intParams.x + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= intParams.y + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= intParams.z + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= intParams.w + 0x9e3779b9 + (hash << 6) + (hash >> 2);

    return hash % (4096 * 4096);
}

uint _UpdateCodeBookLength(TemplateCodeBookEntry codeBookEntry, uint resourceIndex)
{
    uint index = HashCodeBookEntry(codeBookEntry);
    int offset = frameIndex * (pow(4, SHADOW_POOL_LEVEL) - 1) / 3;
    int codeBookWriteIndex = resourceIndex + offset;
    uint flag = 0;
    InterlockedCompareExchange(CodeBookHash[index], 0xFFFFFFFF - 1, 0xFFFFFFFF, flag);
    if (flag == 0xFFFFFFFF-1)
    {
        uint allocateIndex = 0;
        InterlockedAdd(CodeBookCounter[resourceIndex], 1, allocateIndex);
        InterlockedExchange(CodeBookHash[index], allocateIndex, flag);
        
        return allocateIndex;
    }
    else if (flag == 0xFFFFFFFF)
    {
        InterlockedAdd(CodeBookHash[index], 0, flag);
        while (flag == 0xFFFFFFFF)
        {
            InterlockedAdd(CodeBookHash[index], 0, flag);
        }
        return CodeBookHash[index];
    }
    else
    {
        
        return CodeBookHash[index];
    }
}

uint __UpdateCodeBookLength(TemplateCodeBookEntry codeBookEntry, uint resourceIndex)
{
    uint index = HashCodeBookEntry(codeBookEntry);
    uint flag = 0;
    
    InterlockedCompareExchange(CodeBookHash[index], 0xFFFFFFFF - 1, 0xFFFFFFFF, flag);
    if (flag == 0xFFFFFFFF - 1)
    {
        uint allocateIndex = 0;
        InterlockedAdd(CodeBookCounter[resourceIndex], 1, allocateIndex);
        CodeBookHashEntry[allocateIndex] = codeBookEntry;
        InterlockedExchange(CodeBookHash[index], allocateIndex, flag);
        
        return allocateIndex;
    }
    else if (flag == 0xFFFFFFFF)
    {
        InterlockedAdd(CodeBookHash[index], 0, flag);
        while (flag == 0xFFFFFFFF)
        {
            InterlockedAdd(CodeBookHash[index], 0, flag);
        }
        if (any(codeBookEntry.params != CodeBookHashEntry[CodeBookHash[index]].params) || codeBookEntry.compressionType != CodeBookHashEntry[CodeBookHash[index]].compressionType)
        {
            uint curIndex = CodeBookHash[index];
            uint codeBookCurLength = 0;
            InterlockedAdd(CodeBookCounter[resourceIndex], 0, codeBookCurLength);
            while (curIndex < codeBookCurLength)
            {
                int writeFlag = 0;
                InterlockedAdd(CodeBookHashEntry[curIndex].compressionType, 0, writeFlag);
                while (writeFlag == 0xFFFFFFFF)
                {              
                    InterlockedAdd(CodeBookHashEntry[curIndex].compressionType, 0, writeFlag);
                }
                if (all(codeBookEntry.params == CodeBookHashEntry[curIndex].params) && codeBookEntry.compressionType == CodeBookHashEntry[curIndex].compressionType)
                {
                    return curIndex;
                }
                curIndex++;
                InterlockedAdd(CodeBookCounter[resourceIndex], 0, codeBookCurLength);
            }
            InterlockedAdd(CodeBookCounter[resourceIndex], 1, curIndex);
            CodeBookHashEntry[curIndex] = codeBookEntry;
            return curIndex;    
        }
        else
            return CodeBookHash[index];
    }
    else
    {
        if (any(codeBookEntry.params != CodeBookHashEntry[CodeBookHash[index]].params) || codeBookEntry.compressionType != CodeBookHashEntry[CodeBookHash[index]].compressionType)
        {
            uint curIndex = CodeBookHash[index];
            uint codeBookCurLength = 0;
            InterlockedAdd(CodeBookCounter[resourceIndex], 0, codeBookCurLength);
            while (curIndex < codeBookCurLength)
            {
                int writeFlag = 0;
                InterlockedAdd(CodeBookHashEntry[curIndex].compressionType, 0, writeFlag);
                while (writeFlag == 0xFFFFFFFF)
                {
                    InterlockedAdd(CodeBookHashEntry[curIndex].compressionType, 0, writeFlag);
                }
                if (all(codeBookEntry.params == CodeBookHashEntry[curIndex].params) && codeBookEntry.compressionType == CodeBookHashEntry[curIndex].compressionType)
                {
                    return curIndex;
                }
                curIndex++;
                InterlockedAdd(CodeBookCounter[resourceIndex], 0, codeBookCurLength);
            }
            InterlockedAdd(CodeBookCounter[resourceIndex], 1, curIndex);
            CodeBookHashEntry[curIndex] = codeBookEntry;
            return curIndex;
        }
        else
            return CodeBookHash[index];
            
    }

    
}

uint UpdateCodeBookLength(TemplateCodeBookEntry codeBookEntry, uint resourceIndex)
{
    uint index = HashCodeBookEntry(codeBookEntry);
    uint flag = 0;
    
    InterlockedCompareExchange(CodeBookHash[index], 0xFFFFFFFF - 1, 0xFFFFFFFF, flag);
    if (flag == 0xFFFFFFFF - 1)
    {
        uint allocateIndex = 0;
        InterlockedAdd(CodeBookCounter[resourceIndex], 1, allocateIndex);
        CodeBookHashEntry[allocateIndex] = codeBookEntry;
        InterlockedExchange(CodeBookHash[index], allocateIndex, flag);
        
        return allocateIndex;
    }
    else if (flag == 0xFFFFFFFF)
    {
        InterlockedAdd(CodeBookHash[index], 0, flag);
        while (flag == 0xFFFFFFFF)
        {
            InterlockedAdd(CodeBookHash[index], 0, flag);
        }
        if (any(codeBookEntry.params != CodeBookHashEntry[CodeBookHash[index]].params) || codeBookEntry.compressionType != CodeBookHashEntry[CodeBookHash[index]].compressionType)
        {
            uint allocateIndex = 0;
            InterlockedAdd(CodeBookCounter[resourceIndex], 1, allocateIndex);
            CodeBookHashEntry[allocateIndex] = codeBookEntry;
            InterlockedExchange(CodeBookHash[index], allocateIndex, flag);
        
            return allocateIndex;
        }
        else
            return CodeBookHash[index];
    }
    else
    {
        if (any(codeBookEntry.params != CodeBookHashEntry[CodeBookHash[index]].params) || codeBookEntry.compressionType != CodeBookHashEntry[CodeBookHash[index]].compressionType)
        {
            uint allocateIndex = 0;
            InterlockedAdd(CodeBookCounter[resourceIndex], 1, allocateIndex);
            CodeBookHashEntry[allocateIndex] = codeBookEntry;
            InterlockedExchange(CodeBookHash[index], allocateIndex, flag);
        
            return allocateIndex;
        }
        else
            return CodeBookHash[index];
            
    }

    
}

// 从子节点深度值合并成父节点的代表深度
TemplateCodeBookEntry MergeChildDepths(int level, uint2 childBase)
{
    // 取每个子quad的平均深度作为代表
    TemplateTreeNode childNodes[4];
    switch (level)
    {
        case 0:
            childNodes[0] = gs_levelNodes_0[childBase.y][childBase.x];
            childNodes[1] = gs_levelNodes_0[childBase.y][childBase.x + 1];
            childNodes[2] = gs_levelNodes_0[childBase.y + 1][childBase.x];
            childNodes[3] = gs_levelNodes_0[childBase.y + 1][childBase.x + 1];
            break;
        case 1:
            childNodes[0] = gs_levelNodes_1[childBase.y][childBase.x];
            childNodes[1] = gs_levelNodes_1[childBase.y][childBase.x + 1];
            childNodes[2] = gs_levelNodes_1[childBase.y + 1][childBase.x];
            childNodes[3] = gs_levelNodes_1[childBase.y + 1][childBase.x + 1];
            break;
        case 2:
            childNodes[0] = gs_levelNodes_2[childBase.y][childBase.x];
            childNodes[1] = gs_levelNodes_2[childBase.y][childBase.x + 1];
            childNodes[2] = gs_levelNodes_2[childBase.y + 1][childBase.x];
            childNodes[3] = gs_levelNodes_2[childBase.y + 1][childBase.x + 1];
            break;
        case 3:
            childNodes[0] = gs_levelNodes_3[childBase.y][childBase.x];
            childNodes[1] = gs_levelNodes_3[childBase.y][childBase.x + 1];
            childNodes[2] = gs_levelNodes_3[childBase.y + 1][childBase.x];
            childNodes[3] = gs_levelNodes_3[childBase.y + 1][childBase.x + 1];
            break;
        case 4:
            childNodes[0] = gs_levelNodes_4[childBase.y][childBase.x];
            childNodes[1] = gs_levelNodes_4[childBase.y][childBase.x + 1];
            childNodes[2] = gs_levelNodes_4[childBase.y + 1][childBase.x];
            childNodes[3] = gs_levelNodes_4[childBase.y + 1][childBase.x + 1];
            break;
    }
    int planeNodeNum = 0;
    for (int i = 0; i < 4; i++)
    {
        if (childNodes[i].codeBookEntry.compressionType == COMPRESSION_PLANE)   //采用平面方法
        {
            planeNodeNum++;
        }
        else if (childNodes[i].codeBookEntry.compressionType == -1)        //该节点本身不是压缩节点，其父节点不能压缩
        {
            TemplateCodeBookEntry c;
            c.compressionType = -1;
            return c;
        }

    }
    
    if (planeNodeNum == 4)
    {
        float4 mergePlane = childNodes[0].codeBookEntry.params;
    // 使用第一个平面作为参考方向
        float3 referenceNormal = childNodes[0].codeBookEntry.params.xyz;
    
        for (int index = 1; index < 4; index++)                                              //dirction alignment
        {
        
            mergePlane += childNodes[index].codeBookEntry.params;
        }
    
    // 归一化法向量并调整距离
        mergePlane = mergePlane / 4;
   
        float error = 0;
        for (int j = 0; j < 4; j++)
        {
        // 计算每个平面与合并平面的参数差异
            float4 diff = childNodes[j].codeBookEntry.params - mergePlane;
            error = error + dot(diff, diff); // 使用欧几里德距离的平方
        }
   
        if (error > compressionErrorThreshold / 10)
        {
            TemplateCodeBookEntry c;
            c.compressionType = -1;
            return c;
        }
    
        switch (level)
        {
            case 0:
                gs_levelNodes_0[childBase.y][childBase.x].isLeaf = -1;
                gs_levelNodes_0[childBase.y][childBase.x + 1].isLeaf = -1;
                gs_levelNodes_0[childBase.y + 1][childBase.x].isLeaf = -1;
                gs_levelNodes_0[childBase.y + 1][childBase.x + 1].isLeaf = -1;
                break;
            case 1:
                gs_levelNodes_1[childBase.y][childBase.x].isLeaf = -1;
                gs_levelNodes_1[childBase.y][childBase.x + 1].isLeaf = -1;
                gs_levelNodes_1[childBase.y + 1][childBase.x].isLeaf = -1;
                gs_levelNodes_1[childBase.y + 1][childBase.x + 1].isLeaf = -1;
                break;
            case 2:
                gs_levelNodes_2[childBase.y][childBase.x].isLeaf = -1;
                gs_levelNodes_2[childBase.y][childBase.x + 1].isLeaf = -1;
                gs_levelNodes_2[childBase.y + 1][childBase.x].isLeaf = -1;
                gs_levelNodes_2[childBase.y + 1][childBase.x + 1].isLeaf = -1;
                break;
            case 3:
                gs_levelNodes_3[childBase.y][childBase.x].isLeaf = -1;
                gs_levelNodes_3[childBase.y][childBase.x + 1].isLeaf = -1;
                gs_levelNodes_3[childBase.y + 1][childBase.x].isLeaf = -1;
                gs_levelNodes_3[childBase.y + 1][childBase.x + 1].isLeaf = -1;
                break;
            case 4:
                gs_levelNodes_4[childBase.y][childBase.x].isLeaf = -1;
                gs_levelNodes_4[childBase.y][childBase.x + 1].isLeaf = -1;
                gs_levelNodes_4[childBase.y + 1][childBase.x].isLeaf = -1;
                gs_levelNodes_4[childBase.y + 1][childBase.x + 1].isLeaf = -1;
                break;
        }

    // 创建合并后的代码本条目
        TemplateCodeBookEntry mergedEntry;
        mergedEntry.compressionType = COMPRESSION_PLANE; // 平面压缩类型
        mergedEntry.params = mergePlane;
    
        return mergedEntry;
    }
    else if(planeNodeNum == 0)
    {
       
        float4 average = float4(0, 0, 0, 0);
        for (int i = 0; i < 4; i++)
        {
            average += childNodes[i].codeBookEntry.params;
        }
        average = average * 0.25f;
        float error = 0;
        for (int i = 0; i < 4; i++)
        {
            float4 diff = childNodes[i].codeBookEntry.params - average;
            error += sqrt(dot(diff, diff));
        }
        //error = error * 0.25;
        
        if (error > compressionErrorThreshold / 10)
        {
            TemplateCodeBookEntry c;
            c.compressionType = -1;
            return c;
        }
    
        switch (level)
        {
            case 0:
                gs_levelNodes_0[childBase.y][childBase.x].isLeaf = -1;
                gs_levelNodes_0[childBase.y][childBase.x + 1].isLeaf = -1;
                gs_levelNodes_0[childBase.y + 1][childBase.x].isLeaf = -1;
                gs_levelNodes_0[childBase.y + 1][childBase.x + 1].isLeaf = -1;
                break;
            case 1:
                gs_levelNodes_1[childBase.y][childBase.x].isLeaf = -1;
                gs_levelNodes_1[childBase.y][childBase.x + 1].isLeaf = -1;
                gs_levelNodes_1[childBase.y + 1][childBase.x].isLeaf = -1;
                gs_levelNodes_1[childBase.y + 1][childBase.x + 1].isLeaf = -1;
                break;
            case 2:
                gs_levelNodes_2[childBase.y][childBase.x].isLeaf = -1;
                gs_levelNodes_2[childBase.y][childBase.x + 1].isLeaf = -1;
                gs_levelNodes_2[childBase.y + 1][childBase.x].isLeaf = -1;
                gs_levelNodes_2[childBase.y + 1][childBase.x + 1].isLeaf = -1;
                break;
            case 3:
                gs_levelNodes_3[childBase.y][childBase.x].isLeaf = -1;
                gs_levelNodes_3[childBase.y][childBase.x + 1].isLeaf = -1;
                gs_levelNodes_3[childBase.y + 1][childBase.x].isLeaf = -1;
                gs_levelNodes_3[childBase.y + 1][childBase.x + 1].isLeaf = -1;
                break;
            case 4:
                gs_levelNodes_4[childBase.y][childBase.x].isLeaf = -1;
                gs_levelNodes_4[childBase.y][childBase.x + 1].isLeaf = -1;
                gs_levelNodes_4[childBase.y + 1][childBase.x].isLeaf = -1;
                gs_levelNodes_4[childBase.y + 1][childBase.x + 1].isLeaf = -1;
                break;
        }

    // 创建合并后的代码本条目
        TemplateCodeBookEntry mergedEntry;
        mergedEntry.compressionType = COMPRESSION_QUAD_DEPTHS; // 平面压缩类型
        mergedEntry.params = average;
    
        return mergedEntry;
    }
    else
    {
        TemplateCodeBookEntry c;
        c.params = float4(1, 1, 1, 1);
        c.compressionType = -1;
        return c;
    }

}


[numthreads(20, 20, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 groupId : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
    uint tileIndex = groupId.y * tileCount.x + groupId.x;
    uint2 tileCoord = groupId.xy;
    uint2 quadCoord = localId.xy;
    int resourceIndex = GetResourceIndex();
    if (id.x==0 && id.y==0)
    {
        CodeBookCounter[resourceIndex] = 0;
        QuadTreeCounter[resourceIndex] = 0;
    }
    if (quadCoord.x == 0 && quadCoord.y == 0)
    {
        quadTreeNodeOffset = tileIndex * 342;
    }
    GroupMemoryBarrierWithGroupSync();
    if (localId.x < 16 && localId.y < 16)
    {
        
        // 计算当前quad在阴影贴图中的位置
        uint quadSize = 2;
        uint2 quadBaseCoord = tileCoord * TILE_SIZE + quadCoord * quadSize;
    
        // 采样quad的4个角的深度值
        float2 texelSize = 1.0f / shadowMapSize;
        float3 screenPos[4];
        screenPos[0] = float3(quadBaseCoord, InputShadowMap[quadBaseCoord]);
        screenPos[1] = float3(quadBaseCoord + uint2(0, 1), InputShadowMap[quadBaseCoord + uint2(0, 1)]);
        screenPos[2] = float3(quadBaseCoord + uint2(1, 0), InputShadowMap[quadBaseCoord + uint2(1, 0)]);
        screenPos[3] = float3(quadBaseCoord + uint2(1, 1), InputShadowMap[quadBaseCoord + uint2(1, 1)]);
    
        // 第0层：为每个quad选择最优压缩方法
        TemplateCodeBookEntry entry = ChooseBestCompression(screenPos);

        TemplateTreeNode leafNode;
        leafNode.isLeaf = 1;
        leafNode.codeBookEntry = entry;
        leafNode.level = 0;
        leafNode.compressionIndex = 100;
        gs_levelNodes_0[quadCoord.y][quadCoord.x] = leafNode;
        
        
        
    
        GroupMemoryBarrierWithGroupSync();
        // 构建上层四叉树
        for (uint level = 1; level < 5; level++)
        {
            uint levelQuadCount = QUADS_PER_TILE_SIDE >> level;
    
            if (quadCoord.x < levelQuadCount && quadCoord.y < levelQuadCount)
            {
                uint2 childBase = quadCoord * 2;

                // 压缩决策
                TemplateCodeBookEntry mergedEntry = MergeChildDepths(level - 1, childBase);

                // 构建父节点
                TemplateTreeNode parentNode;
        
                if (mergedEntry.compressionType != -1)
                {
                    // 压缩成功 - 叶子节点
                    parentNode.isLeaf = 1;
                    parentNode.codeBookEntry = mergedEntry;
                    parentNode.level = level;
                    parentNode.compressionIndex = 0;
                }
                else
                {
                    // 压缩失败 - 内部节点（暂时存储子节点坐标）
                    parentNode.isLeaf = 0;
                    parentNode.level = level;
                    parentNode.childNodes[0] = uint2(childBase.y, childBase.x);
                    parentNode.childNodes[1] = uint2(childBase.y, childBase.x + 1);
                    parentNode.childNodes[2] = uint2(childBase.y + 1, childBase.x);
                    parentNode.childNodes[3] = uint2(childBase.y + 1, childBase.x + 1);
                    parentNode.codeBookEntry = mergedEntry;
                    parentNode.compressionIndex = 0;
                
                }
                switch (level)
                {
                    case 0:
                        gs_levelNodes_0[quadCoord.y][quadCoord.x] = parentNode;
                        break;
                    case 1:
                        gs_levelNodes_1[quadCoord.y][quadCoord.x] = parentNode;
                        break;
                    case 2:
                        gs_levelNodes_2[quadCoord.y][quadCoord.x] = parentNode;
                        break;
                    case 3:
                        gs_levelNodes_3[quadCoord.y][quadCoord.x] = parentNode;
                        break;
                    case 4:
                        gs_levelNodes_4[quadCoord.y][quadCoord.x] = parentNode;
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
        

    }
    
    GroupMemoryBarrierWithGroupSync();
    
    int writeIndex = localId.y*20 + localId.x;

    if(writeIndex>340)
        return;
 
    int offset = frameIndex * (pow(4, SHADOW_POOL_LEVEL) - 1) / 3;
    
    int resourceIndexQuardTree = GetResourceIndex() + offset;
    int curIndex = 0;
    for (int i = 0; i < 1; i++)
    {
        for (int j = 0; j < 1; j++)
        {
            if (curIndex == writeIndex && gs_levelNodes_4[i][j].isLeaf!=-1)
            {
                InterlockedAdd(TemplateNodeNumPerTile[resourceIndexQuardTree][tileIndex], 1);
                InterlockedAdd(QuadTreeCounter[resourceIndex], 1);
                QuadTreeNode newNode;
                newNode.isLeaf = gs_levelNodes_4[i][j].isLeaf;
                newNode.level = gs_levelNodes_4[i][j].level;
                newNode.compressionType = gs_levelNodes_4[i][j].codeBookEntry.compressionType;
                newNode.params = gs_levelNodes_4[i][j].codeBookEntry.params;
                gs_levelNodes_4[i][j].compressionIndex = curIndex;
                if (gs_levelNodes_4[i][j].isLeaf == 1)
                {
                    newNode.firstChildOrCodeBook = UpdateCodeBookLength(gs_levelNodes_4[i][j].codeBookEntry, resourceIndex);
                }
                else
                {
                    newNode.params.xy = gs_levelNodes_4[i][j].childNodes[0];
                }
                TemplateQuadTree[resourceIndexQuardTree][curIndex + quadTreeNodeOffset] = newNode;
            }
            if (gs_levelNodes_4[i][j].isLeaf!=-1)
                curIndex++;
        }
    }
    
    for (int i = 0; i < 1; i++)
    {
        for (int j = 0; j < 1; j++)
        {
            for (int column = 0; column < 2; column++)
            {
                for (int row = 0; row < 2; row++)
                {
                    uint pos_x = 2 * i + column;
                    uint pos_y = 2 * j + row;
                    if (curIndex == writeIndex && gs_levelNodes_3[pos_x][pos_y].isLeaf != -1)
                    {
                        InterlockedAdd(TemplateNodeNumPerTile[resourceIndexQuardTree][tileIndex], 1);
                        InterlockedAdd(QuadTreeCounter[resourceIndex], 1);
                        QuadTreeNode newNode;
                        newNode.isLeaf = gs_levelNodes_3[pos_x][pos_y].isLeaf;
                        newNode.level = gs_levelNodes_3[pos_x][pos_y].level;
                        newNode.compressionType = gs_levelNodes_3[pos_x][pos_y].codeBookEntry.compressionType;
                        newNode.params = gs_levelNodes_3[pos_x][pos_y].codeBookEntry.params;
                        gs_levelNodes_3[pos_x][pos_y].compressionIndex = curIndex;
                        if (gs_levelNodes_3[pos_x][pos_y].isLeaf == 1)
                        {
                            newNode.firstChildOrCodeBook = UpdateCodeBookLength(gs_levelNodes_3[pos_x][pos_y].codeBookEntry, resourceIndex);
                        }
                        else
                        {
                            newNode.params.xy = gs_levelNodes_3[pos_x][pos_y].childNodes[0];
                        }
                        TemplateQuadTree[resourceIndexQuardTree][curIndex + quadTreeNodeOffset] = newNode;
                    }
                    if (gs_levelNodes_3[pos_x][pos_y].isLeaf != -1)
                        curIndex++;
                }
            }
                
        }
    }
    
    for (int i = 0; i < 2; i++)
    {
        for (int j = 0; j < 2; j++)
        {
            for (int column = 0; column < 2; column++)
            {
                for (int row = 0; row < 2; row++)
                {
                    uint pos_x = 2 * i + column;
                    uint pos_y = 2 * j + row;
                    if (curIndex == writeIndex && gs_levelNodes_2[pos_x][pos_y].isLeaf != -1)
                    {
                        InterlockedAdd(TemplateNodeNumPerTile[resourceIndexQuardTree][tileIndex], 1);
                        InterlockedAdd(QuadTreeCounter[resourceIndex], 1);
                        QuadTreeNode newNode;
                        newNode.isLeaf = gs_levelNodes_2[pos_x][pos_y].isLeaf;
                        newNode.level = gs_levelNodes_2[pos_x][pos_y].level;
                        newNode.compressionType = gs_levelNodes_2[pos_x][pos_y].codeBookEntry.compressionType;
                        newNode.params = gs_levelNodes_2[pos_x][pos_y].codeBookEntry.params;
                        gs_levelNodes_2[pos_x][pos_y].compressionIndex = curIndex;
                        if (gs_levelNodes_2[pos_x][pos_y].isLeaf == 1)
                        {
                            newNode.firstChildOrCodeBook = UpdateCodeBookLength(gs_levelNodes_2[pos_x][pos_y].codeBookEntry, resourceIndex);
                        }
                        else
                        {
                            newNode.params.xy = gs_levelNodes_2[pos_x][pos_y].childNodes[0];
                        }
                        TemplateQuadTree[resourceIndexQuardTree][curIndex + quadTreeNodeOffset] = newNode;
                    }
                    if (gs_levelNodes_2[pos_x][pos_y].isLeaf != -1)
                        curIndex++;
                }
            }
        }
    }
    
    for (int i = 0; i < 4; i++)
    {
        for (int j = 0; j < 4; j++)
        {
            for (int column = 0; column < 2; column++)
            {
                for (int row = 0; row < 2; row++)
                {
                    uint pos_x = 2 * i + column;
                    uint pos_y = 2 * j + row;
                    if (curIndex == writeIndex && gs_levelNodes_1[pos_x][pos_y].isLeaf != -1)
                    {
                        InterlockedAdd(TemplateNodeNumPerTile[resourceIndexQuardTree][tileIndex], 1);
                        InterlockedAdd(QuadTreeCounter[resourceIndex], 1);
                        QuadTreeNode newNode;
                        newNode.isLeaf = gs_levelNodes_1[pos_x][pos_y].isLeaf;
                        newNode.level = gs_levelNodes_1[pos_x][pos_y].level;
                        newNode.compressionType = gs_levelNodes_1[pos_x][pos_y].codeBookEntry.compressionType;
                        newNode.params = gs_levelNodes_1[pos_x][pos_y].codeBookEntry.params;
                        gs_levelNodes_1[pos_x][pos_y].compressionIndex = curIndex;
                        if (gs_levelNodes_1[pos_x][pos_y].isLeaf == 1)
                        {
                            newNode.firstChildOrCodeBook = UpdateCodeBookLength(gs_levelNodes_1[pos_x][pos_y].codeBookEntry, resourceIndex);
                        }
                        else
                        {
                            newNode.params.xy = gs_levelNodes_1[pos_x][pos_y].childNodes[0];
                        }
                        TemplateQuadTree[resourceIndexQuardTree][curIndex + quadTreeNodeOffset] = newNode;
                    }
                    if (gs_levelNodes_1[pos_x][pos_y].isLeaf != -1)
                        curIndex++;
                }
            }
        }
    }
    
    for (int i = 0; i < 8; i++)
    {
        for (int j = 0; j < 8; j++)
        {
            for (int column = 0; column < 2; column++)
            {
                for (int row = 0; row < 2; row++)
                {
                    int pos_x = 2 * i + column;
                    int pos_y = 2 * j + row;
         
                    if (curIndex == writeIndex && gs_levelNodes_0[pos_x][pos_y].isLeaf != -1)
                    {
                        InterlockedAdd(TemplateNodeNumPerTile[resourceIndexQuardTree][tileIndex], 1);
                        InterlockedAdd(QuadTreeCounter[resourceIndex], 1);
                        QuadTreeNode newNode;
                        newNode.isLeaf = gs_levelNodes_0[pos_x][pos_y].isLeaf;
                        newNode.level = gs_levelNodes_0[pos_x][pos_y].level;
                        newNode.compressionType = gs_levelNodes_0[pos_x][pos_y].codeBookEntry.compressionType;
                        newNode.params = gs_levelNodes_0[pos_x][pos_y].codeBookEntry.params;
                        gs_levelNodes_0[pos_x][pos_y].compressionIndex = curIndex;
                        
                        if (gs_levelNodes_0[pos_x][pos_y].isLeaf == 1)
                        {
                            newNode.firstChildOrCodeBook = UpdateCodeBookLength(gs_levelNodes_0[pos_x][pos_y].codeBookEntry, resourceIndex);
                        }
                        else
                        {
                            newNode.params.xy = gs_levelNodes_0[pos_x][pos_y].childNodes[0];
                        }
           
                        TemplateQuadTree[resourceIndexQuardTree][curIndex + quadTreeNodeOffset] = newNode;
                    }
                    
                   
                    
                    if (gs_levelNodes_0[pos_x][pos_y].isLeaf != -1)
                        curIndex++;
 
                }
            }
        }
    }
    
    GroupMemoryBarrierWithGroupSync();
    if (TemplateQuadTree[resourceIndexQuardTree][writeIndex + quadTreeNodeOffset].isLeaf == 0)
    {
        uint x = TemplateQuadTree[resourceIndexQuardTree][writeIndex + quadTreeNodeOffset].params.x;
        uint y = TemplateQuadTree[resourceIndexQuardTree][writeIndex + quadTreeNodeOffset].params.y;
        
        
        switch (TemplateQuadTree[resourceIndexQuardTree][writeIndex + quadTreeNodeOffset].level - 1)
        {
            case 0:
                TemplateQuadTree[resourceIndexQuardTree][writeIndex + quadTreeNodeOffset].firstChildOrCodeBook = gs_levelNodes_0[x][y].compressionIndex + quadTreeNodeOffset;
                break;
            case 1:
                TemplateQuadTree[resourceIndexQuardTree][writeIndex + quadTreeNodeOffset].firstChildOrCodeBook = gs_levelNodes_1[x][y].compressionIndex + quadTreeNodeOffset;
                break;
            case 2:
                TemplateQuadTree[resourceIndexQuardTree][writeIndex + quadTreeNodeOffset].firstChildOrCodeBook = gs_levelNodes_2[x][y].compressionIndex + quadTreeNodeOffset;
                break;
            case 3:
                TemplateQuadTree[resourceIndexQuardTree][writeIndex + quadTreeNodeOffset].firstChildOrCodeBook = gs_levelNodes_3[x][y].compressionIndex + quadTreeNodeOffset;
                break;
            case 4:
                TemplateQuadTree[resourceIndexQuardTree][writeIndex + quadTreeNodeOffset].firstChildOrCodeBook = gs_levelNodes_4[x][y].compressionIndex + quadTreeNodeOffset;
                break;
        }
    }
      
    
}