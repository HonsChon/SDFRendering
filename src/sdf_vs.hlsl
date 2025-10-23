struct VertexIn
{
    float3 pos : POSITION;
    float2 tex : TEXCOORD;
};

struct VertexOut
{
    float4 posH : SV_POSITION;
    float2 tex : TEXCOORD;
};


VertexOut VS(VertexIn vIn)
{
    VertexOut vOut;
    vOut.posH = float4(vIn.pos, 1.0f);
    vOut.tex = vIn.tex; 
    return vOut;
}
