基于SDF的渲染，我从相机位置往每个像素发射射线，进行rayMarching,根据每一步的sdf值来判断是否碰撞到物体及其信息，

```
// 光线投射
float2 raycast(float3 ro, float3 rd, int mnum)
{
    float2 res = float2(-1.0, -1.0);

    //最大行进距离
    float tmax = 20.0;
    
    //行进
    //初始行进距离
    float t = 1.0;
    for (int i = 0; i < mnum && t < tmax; i++)
    {
        float2 h = map(ro + rd * t);
        if (abs(h.x) < (0.0001 * t))
        {
            res = float2(t, h.y);
            break;
        }
        t += h.x;
    }
    
    return res;
}
```

然后计算其法线

```
float3 calcNormal(float3 p, float t)   //对于SDF来说，法线可以通过对距离场进行数值梯度计算得到
{
    float eps = 0.0001;
    eps += eps / 10 * t;
    const float2 h = float2(eps, 0);
    return normalize(float3(map(p + h.xyy).x - map(p - h.xyy).x,
                            map(p + h.yxy).x - map(p - h.yxy).x,
                            map(p + h.yyx).x - map(p - h.yyx).x));
}
```



### Distance Field Soft Shadow

通过如下这样公式来决定软阴影的阴影系数（shadow factor）值，该值在着色计算的时候乘上对应的光照计算值
$$
r_{penumbra}=k⋅(SDF(p)/d_{p-o})
$$
优点：快，高质量

缺点：需要预计算，需要很大的存储空间，对动态物体或形变物体需要一直重计算

```
float softshadow( in vec3 ro, in vec3 rd, float mint, float maxt, float k )
{
    float res = 1.0;
    for( float t=mint; t < maxt; )
    {
        float h = map(ro + rd*t);
        if( h<0.001 )
            return 0.0;
        res = min( res, k*h/t );
        t += h;
    }
    return res;
}
```

    float3 ro,   // 1. 起点（ray origin），其实就是着色点位置
    float3 rd,   // 2. 步进方向（ray direction），着色点到光源的方向
    float mint,  // 3. 光线开始步进距离（min t）
    float maxt,  // 4. 光线追踪最大距离（max t）
    float k      // 5. 阴影柔和度因子（softness factor）

为了消除一些复杂几何下产出的banding effects，我们对其进行一定的优化

```
// 计算软阴影
float calcSoftshadow(in float3 ro, in float3 rd, float mint, float maxt, float k) //基于sdf的软阴影
{
    float res = 1.0;
    float ph = 1e20;
    for (float t = mint; t < maxt;)
    {
        float h = map(ro + rd * t); //获取当前点到最近表面的距离
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
```

