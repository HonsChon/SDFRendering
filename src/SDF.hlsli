float sdSphere(float3 p, float r)  //定义球体
{
    return length(p) - r;
}

float sdBox(float3 p, float3 b) 
{
    float3 d = abs(p) - b;
    return min(max(d.x, max(d.y, d.z)), 0.0) + length(max(d, 0.0));
}

float map(in float3 pos)  //返回这时候相对于sphere的sdf距离
{
    //return sdPlane(pos, normalize(float3(0,1,0)), 0);
    //return sdCone(pos, float3(-0.15, -0.2, -0.1), float3(0.2, 0.2, 0.1), 0.4, 0.1);
    return sdSphere(pos, 0.4);
}

float3 calcNormal(in float3 pos)
{
    float2 e = float2(1.0, -1.0) * 0.5773;
    const float eps = 0.0005;
    return normalize(e.xyy * map(pos + e.xyy * eps) +
					  e.yyx * map(pos + e.yyx * eps) +
					  e.yxy * map(pos + e.yxy * eps) +
					  e.xxx * map(pos + e.xxx * eps));
}