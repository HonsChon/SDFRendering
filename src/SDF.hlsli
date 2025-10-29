cbuffer ConstantBuffer : register(b0)
{
    float4 g_Time;
    float4 g_Resolution;
    int4 g_Switch; //一些设置，分别对应开启关闭环境光遮蔽、方向光、天空光等
    float2 g_Factor; //存储着rayMarching最大步进次数和阴影的软硬程度
}

float dot2(in float2 v)
{
    return dot(v, v);
}
float dot2(in float3 v)
{
    return dot(v, v);
}
float ndot(in float2 a, in float2 b)
{
    return a.x * b.x - a.y * b.y;
}

float sdPlane(float3 p, float3 n) //返回点 p 到过原点、法向量为 n 的平面的有符号距离。
{
    return dot(p, n);
}

float sdSphere(float3 p, float r) //点 p 到半径为 r、中心在原点的球面的带符号距离。
{
    return length(p) - r;
}

float sdTorus(float3 p, float2 t)
{
    return length(float2(length(p.xz) - t.x, p.y)) - t.y;
}

 
float sdCylinder(float3 p, float2 h, int mode)  //点 p 到一个有限高度的圆柱体的距离（可选择朝向 X/Y/Z 轴）。
{
    float res;
    float2 d;
    switch (mode)
    {
        // 面向z轴
        case 0:
            d = abs(float2(length(p.xy), p.z)) - h;
            res = min(max(d.x, d.y), 0.0) + length(max(d, 0.0));
            break;
        // 面向x轴
        case 1:
            d = abs(float2(length(p.yz), p.x)) - h;
            res = min(max(d.x, d.y), 0.0) + length(max(d, 0.0));
            break;
        // 面向y轴
        case 2:
            d = abs(float2(length(p.xz), p.y)) - h;
            res = min(max(d.x, d.y), 0.0) + length(max(d, 0.0));
            break;
        default:
            break;
    }
    return res;

}

float sdBox(float3 p, float3 b)
{
    float3 d = abs(p) - b;
    return min(max(d.x, max(d.y, d.z)), 0.0) + length(max(d, 0.0));
}

float sdBoxFrame(float3 p, float3 b, float e)
{
    p = abs(p) - b;
    e /= 2;
    float3 q = abs(p + e) - e;
    
    return min(min(
      length(max(float3(p.x, q.y, q.z), 0.0)) + min(max(p.x, max(q.y, q.z)), 0.0),
      length(max(float3(q.x, p.y, q.z), 0.0)) + min(max(q.x, max(p.y, q.z)), 0.0)),
      length(max(float3(q.x, q.y, p.z), 0.0)) + min(max(q.x, max(q.y, p.z)), 0.0));
}


float sdOctahedron(float3 p, float s)
{
    p = abs(p);
    float m = p.x + p.y + p.z - s;
    float3 q;
    if (3.0 * p.x < m)
        q = p.xyz;
    else if (3.0 * p.y < m)
        q = p.yzx;
    else if (3.0 * p.z < m)
        q = p.zxy;
    else
        return m * 0.57735027;
    
    float k = clamp(0.5 * (q.z - q.y + s), 0.0, s);
    return length(float3(q.x, q.y - s + k, q.z - k));
}

//多个物体并集
float2 opU(float2 d1, float2 d2)
{
    return (d1.x < d2.x) ? d1 : d2;
}

//三个集合操作
float opUnion(float d1, float d2)
{
    return min(d1, d2);
}

float opSubtraction(float d1, float d2)   //保留 “在d1内但不在内” 的区域
{
    return max(d1, -d2);
}

float opIntersection(float d1, float d2)  //找共同在的部分
{
    return max(d1, d2);
}

//三个平滑集合操作
float opSmoothUnion(float d1, float d2, float k)
{
    float h = clamp(0.5 + 0.5 * (d2 - d1) / k, 0.0, 1.0);
    return lerp(d2, d1, h) - k * h * (1.0 - h);
}

float opSmoothSubtraction(float d1, float d2, float k)
{
    float h = clamp(0.5 - 0.5 * (d2 + d1) / k, 0.0, 1.0);
    return lerp(d2, -d1, h) + k * h * (1.0 - h);
}

float opSmoothIntersection(float d1, float d2, float k)
{
    float h = clamp(0.5 - 0.5 * (d2 - d1) / k, 0.0, 1.0);
    return lerp(d2, d1, h) + k * h * (1.0 - h);
}

// 位移
float opDisplace(float d1)
{
    float an = fmod(g_Time.x, 6.28);
    float d2 = 0.2 * sin(3 * an);
    d2 = d2;
    return d1 + d2;
}

//圆角
float opRound(float sdf, float thickness)
{
    return sdf - thickness;
}

float2 map(float3 pos)  //计算sdf值
{
    // 地板
    // float2 res = float2(pos.y, 0.0);
    float2 res = float2(sdPlane(pos, float3(0, 1, 0)), 0.0);
    float tmp[5];

    //牟合方盖    
    tmp[0] = sdCylinder(pos - float3(0, 0.3, 1.5), float2(0.3, 0.3), 0);
    tmp[1] = sdCylinder(pos - float3(0, 0.3, 1.5), float2(0.3, 0.3), 1);
    res = opU(res, float2(opIntersection(tmp[0], tmp[1]), 8));
    
    //框中金字塔
    tmp[0] = sdBoxFrame(pos - float3(1, 0.3, 0.5), float3(0.3, 0.3, 0.3), 0.06);
    tmp[1] = sdOctahedron(pos - float3(1, 0.3, 0.5), 0.3);
    res = opU(res, float2(opUnion(tmp[0], tmp[1]), 14));
    
    //卡通
    tmp[0] = sdSphere(pos - float3(-1, 0.3, 0.), 0.35);
    if (tmp[0] < res.x)
    {
        tmp[4] = opDisplace(tmp[0]);
        //这里用1反而效果更好，就这么顺延下去
        tmp[0] = opUnion(tmp[4], tmp[0]);
    }
    tmp[1] = sdSphere(pos - float3(-1.5, 0.9, 0.), 0.35);
    if (tmp[1] < res.x)
    {
        tmp[4] = opDisplace(tmp[1]);
        tmp[1] = opUnion(tmp[4], tmp[1]);
    }
    tmp[2] = sdSphere(pos - float3(-0.5, 0.9, 0.), 0.35);
    if (tmp[2] < res.x)
    {
        tmp[4] = opDisplace(tmp[2]);
        tmp[2] = opUnion(tmp[4], tmp[2]);
    }
    tmp[3] = sdSphere(pos - float3(-1, 1.5, 0.), 0.35);
    if (tmp[3] < res.x)
    {
        tmp[4] = opDisplace(tmp[3]);
        tmp[3] = opUnion(tmp[4], tmp[3]);
    }
    res = opU(res, float2(opSmoothUnion(tmp[0], tmp[1], 0.25), 6.0));
    res = opU(res, float2(opSmoothUnion(tmp[0], tmp[2], 0.25), 6.0));
    res = opU(res, float2(opSmoothUnion(tmp[3], tmp[1], 0.25), 6.0));
    res = opU(res, float2(opSmoothUnion(tmp[3], tmp[2], 0.25), 6.0));
    
    //五环
    res = opU(res, float2(sdTorus((pos - float3(1.0, 0.3, -0.5)).xyz, float2(0.27, 0.03)), 7));
    res = opU(res, float2(sdTorus((pos - float3(0.3, 0.3, -0.5)).xyz, float2(0.27, 0.03)), 7));
    res = opU(res, float2(sdTorus((pos - float3(0.0, 0.6, -0.5)).xyz, float2(0.27, 0.03)), 7));
    res = opU(res, float2(sdTorus((pos - float3(.65, 0.6, -0.5)).xyz, float2(0.27, 0.03)), 7));
    res = opU(res, float2(sdTorus((pos - float3(1.3, 0.6, -0.5)).xyz, float2(0.27, 0.03)), 7));
   
    //镂空方块
    tmp[0] = opRound(sdBox(pos - float3(-1, 0.3, 1), float3(0.3, 0.3, 0.3)), 0.1);
    tmp[1] = sdBox(pos - float3(-1, 0.6, 1), float3(0.3, 0.15, 0.15));
    res = opU(res, float2(opSubtraction(tmp[0], tmp[1]), 37.0));
    
    return res;
}

// 计算软阴影
float calcSoftshadow(in float3 ro, in float3 rd, float mint, float maxt, float k) //基于sdf的软阴影
{
    float res = 1.0;
    float ph = 1e20;
    for (float t = mint; t < maxt;)
    {
        float h = map(ro + rd * t);
        if (h < 0.001)
            return 0.0;
        float y = h * h / (2.0 * ph);
        float d = sqrt(h * h - y * y);
        res = min(res, k * d / max(0.0, t - y));
        ph = h;
        t += h;
    }
    return res;
}

// 计算法线
float3 calcNormal(float3 p, float t)
{
    float eps = 0.0001;
    eps += eps / 10 * t;
    const float2 h = float2(eps, 0);
    return normalize(float3(map(p + h.xyy).x - map(p - h.xyy).x,
                            map(p + h.yxy).x - map(p - h.yxy).x,
                            map(p + h.yyx).x - map(p - h.yyx).x));
}

// 计算环境光遮蔽
// 使用iq的经验公式
float calcAO(in float3 pos, in float3 nor)
{
    float occ = 0.0;
    float decay = 1.0;
    for (int i = 0; i < 5; i++)
    {
        float h = 0.01 + 0.12 * float(i) / 4.0;
        float d = map(pos + h * nor).x;
        occ += (h - d) * decay;
        decay *= 0.95;
        if (occ > 0.35)
            break;
    }
    return clamp(1.0 - 3.0 * occ, 0.0, 1.0) * (0.5 + 0.5 * nor.y);
}
