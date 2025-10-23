#include "SDF.hlsli"

struct VertexOut
{
    float4 posH : SV_POSITION;
    float2 tex : TEXCOORD;
};

cbuffer ConstantBuffer : register(b0)
{
    float2 g_Resolution;

}

float4 PS(VertexOut pIn) : SV_Target
{
    //基本测试代码
    //float2 fragCoord = pIn.tex * g_Resolution.xy;
    //float2 uv = fragCoord / g_Resolution.xy;
    //float3 color = 0.5 + 0.5 * cos(g_Time.x + uv.xyx + float3(0, 2, 4));
    //float4 fragColor = float4(color, 1.0);
    //return fragColor;

    //片元屏幕坐标
    return float4(1, 1, 1, 0.5);
    float2 fragCoord = pIn.tex * g_Resolution.xy;
    
    // 相机运动 animaton
    float an = 0.5 * (100 - 10.0);
    // 相机位置 ray origin 
    float3 ro = float3(1.0 * cos(an), 0.4, 1.0 * sin(an));
    // 目标位置 lookat-target
    float3 ta = float3(0.0, 0.0, 0.0);
    // 代表相机朝向的forward vector,相机坐标系的Z轴
    float3 ww = normalize(ta - ro);
    // 与up vector叉积，得到相机坐标系的X轴
    float3 uu = normalize(cross(ww, float3(0.0, 1.0, 0.0)));
    // 相机坐标系的Y轴
    float3 vv = normalize(cross(uu, ww));

    float3 tot = float3(0.0, 0.0, 0.0);
    
    //让屏幕中心为屏幕坐标系原点，并调整xy轴单位长度一致，得到当前像素的位置
    float2 p = (-g_Resolution.xy + 2.0 * fragCoord) / g_Resolution.y;

	// 相机射线方向
    float3 rd = normalize(p.x * uu + p.y * vv + 1.5 * ww);

    // 光线行进
    const float tmax = 5.0;
    // 步长
    float t = 0.0;
    for (int i = 0; i < 256; i++)
    {
        float3 pos = ro + t * rd;
        float h = map(pos);
        if (h < 0.0001 || t > tmax)
            break;
        t += h;
    }
        
    // shading/lighting	
    float3 col = float3(0.0, 0.0, 0.0);
    if (t < tmax)
    {
        float3 pos = ro + t * rd;
        float3 nor = calcNormal(pos);
        float dif = clamp(dot(nor, float3(0.57703, 0.57703, 0.57703)), 0.0, 1.0);
        float amb = 0.5 + 0.5 * dot(nor, float3(0.0, 1.0, 0.0));
        col = float3(0.2, 0.3, 0.4) * amb + float3(0.8, 0.7, 0.5) * dif;
    }

    // gamma        
    col = sqrt(col);
    tot += col;

    float4 fragColor = float4(tot, 1.0);
    return fragColor;
   
}
