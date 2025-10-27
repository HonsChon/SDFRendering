#include "SDF.hlsli"

Texture2D g_Tex : register(t0);
SamplerState g_SamLinear : register(s0);

struct VertexOut
{
    float4 posH : SV_POSITION;
    float2 tex : TEXCOORD;
};

cbuffer ConstantBuffer : register(b0)
{
    float4 g_Time;
    float4 g_Resolution;
    int4 g_Frame;
    float4x4 g_Camera;
    int4 g_Switch;
    float4 g_Factor;
}

// ����Ͷ��
float2 raycast(float3 ro, float3 rd, int mnum)
{
    float2 res = float2(-1.0, -1.0);

    //����н�����
    float tmax = 20.0;
    
    //�н�
    //��ʼ�н�����
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

float3 render(in float3 ro, in float3 rd)
{
    // Ĭ����ɫ����������ɫ
    float3 col = float3(0, 0, 0);
    
    // ����Ͷ��
    float2 res = raycast(ro, rd, (int) g_Factor.x);
    // �н�����
    float t = res.x;
    // ���ʲ���
    float m = res.y;
    if (m >= 0)
    {
        float3 pos = ro + t * rd;
        float3 nor = (m < 1.5) ? float3(0.0, 1.0, 0.0) : calcNormal(pos, t);
        float3 ref = reflect(rd, nor);
        
        //base color
        if (g_Switch.x == 1)
        {
            // ����        
            col = 0.2 + 0.2 * sin(m * 2.0 + float3(0.0, 1.0, 2.0));
        
            //�ذ����
            if (m == 0)
            {
                col = float3(.3, .0, .0);
                //��������
                col = col * g_Tex.Sample(g_SamLinear, float2(pos.x, pos.z)).rgb;
            }
        }
        
        //������ɫ
        float3 lin = float3(0, 0, 0);

        // �������ڱ�����
        float occ = float3(1, 1, 1);
        if (g_Switch.w == 1)
        {
            occ = calcAO(pos, nor);
        }
        
        // �����
        if (g_Switch.y == 1)
        {
            // ligΪ�Ե�ǰposΪԭ��ʱ��Դ��λ��
            // ��ʱ���ƶ�
            // xzƽ����������Բ���˶�,y�������ƶ�
            float3 lig = normalize(float3(2 * sin(fmod(g_Time.x, 6.28)), 1.5 + cos(fmod(g_Time.x, 6.28)), 2 * cos(fmod(g_Time.x, 6.28))));
            
            // ��������
            float3 hal = normalize(lig - rd);
            // Lambert������
            float dif = clamp(dot(nor, lig), 0.0, 1.0);
            // ���ϻ������ڱ�
            dif *= occ;
            // ����Ӱ
            dif *= calcSoftshadow(pos, lig, 0.02, 2.5, g_Factor.y);
            // blinn-phong�߹�
            float spe = pow(clamp(dot(nor, hal), 0.0, 1.0), 16.0);
            // ��������
            lin += col * 2.20 * dif * float3(1.3, 1., 0.7);
            lin += 0.2 * spe * float3(1.3, 1., 0.7);
        }

        
        // ��չ�
        if (g_Switch.z == 1)
        {
            // �����䣬����Խ������գ�y�᷽�򣩷���Խǿ
            float dif = sqrt(clamp(0.5 + 0.5 * nor.y, 0.0, 1.0));
            // ���ϻ������ڱ�
            dif *= occ;
            // �߹⣬���߷��䷽��Խ�������Խǿ
            float spe = smoothstep(-0.2, 0.2, ref.y);
            // ����Ӱ
            spe *= calcSoftshadow(pos, ref, 0.02, 2.5, g_Factor.y);
            // ��������
            spe *= 5.0 * pow(clamp(1.0 + dot(nor, rd), 0.0, 1.0), 5.0);
            lin += col * 0.60 * dif * float3(0.4, 0.6, 1.15);
            lin += spe;
        }

        // ���ݹ����н������ֵ,�õ����ͼ
        col = lin;
        col = lerp(col, float3(0.9, 0.9, 0.9), 1.0 - exp(-0.0001 * t * t * t));
    }

    return float3(clamp(col, 0.0, 1.0));
}



float4 PS(VertexOut pIn) : SV_Target
{
    float2 fragCoord = pIn.tex * g_Resolution.xy;
 
    // ����Ļ����Ϊ��Ļ����ϵԭ�㣬������xy�ᵥλ����һ�£��õ���ǰ�������������ϵ�µ�λ��
    float2 p = (2.0 * fragCoord - g_Resolution.xy) / g_Resolution.y;

    // ����
    const float fl = 3;
        
    float3 ro = g_Camera._14_24_34;
    
    // ���߷���
    float3 rd = mul((float3x3) g_Camera, normalize(float3(p, fl)));
    
    // ��Ⱦ
    float3 col = render(ro, rd);
        
    // gamma���룬��������ϸ������
    col = pow(col, float3(0.4545, 0.4545, 0.4545));

    
    float4 fragColor = float4(col, 1.0);
    return fragColor;
   
}
