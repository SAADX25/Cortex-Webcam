Texture2D<float4> Source : register(t0);
Texture2D<float> Matte : register(t1);
SamplerState LinearClamp : register(s0);
cbuffer Filter : register(b0) {
    float Similarity;
    float Softness;
    float Spill;
    float Mode;
    float4 BackgroundUV;
};
struct Vertex {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};
Vertex Fullscreen(uint id : SV_VertexID) {
    Vertex v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv.x * 2 - 1, 1 - v.uv.y * 2, 0, 1);
    return v;
}
float2 chroma(float3 c) {
    return float2(dot(c, float3(-0.168736, -0.331264, 0.5)),
                  dot(c, float3(0.5, -0.418688, -0.081312)));
}
float4 Composite(Vertex v)
    : SV_TARGET {
    float3 rgb = Source.Sample(LinearClamp, v.uv).rgb;
    float alpha = 1;
    if (Mode > 0.5 && Mode < 1.5) {
        float distance = length(chroma(rgb) - chroma(float3(0, 1, 0)));
        alpha = smoothstep(Similarity, Similarity + Softness, distance);
        rgb.g = lerp(rgb.g, min(rgb.g, max(rgb.r, rgb.b)), Spill * (1 - alpha));
    } else if (Mode > 1.5)
        alpha = saturate(Matte.Sample(LinearClamp, v.uv));
    return float4(rgb * alpha, alpha);  // Premultiplied alpha for ONE / INV_SRC_ALPHA.
}
float4 Background(Vertex v)
    : SV_TARGET {
    float2 uv = v.uv * BackgroundUV.xy + BackgroundUV.zw;
    if (any(uv < 0) || any(uv > 1))
        return 0;
    float4 c = Source.Sample(LinearClamp, uv);
    return float4(c.rgb * c.a, c.a);
}
float4 Export(Vertex v)
    : SV_TARGET {
    float2 uv = Similarity > 0.5 ? float2(v.uv.x, 1 - v.uv.y) : v.uv;
    float4 c = Source.Sample(LinearClamp, uv);
    return Mode > 0.5 ? float4(c.a > 0.00001 ? c.rgb / c.a : float3(0, 0, 0), c.a)
                      : float4(c.rgb, 1);
}
float4 Rotate(Vertex v)
    : SV_TARGET {
    float2 uv = v.uv;
    if (Similarity > 2.5)
        uv = float2(1 - v.uv.y, v.uv.x);
    else if (Similarity > 1.5)
        uv = 1 - v.uv;
    else if (Similarity > 0.5)
        uv = float2(v.uv.y, 1 - v.uv.x);
    return Source.Sample(LinearClamp, uv);
}
// Fixed 512x288 normalized RGB NCHW tensor, generated on the GPU.
RWStructuredBuffer<float> Tensor : register(u0);
[numthreads(16, 16, 1)] void Preprocess(uint3 id : SV_DispatchThreadID) {
    if (id.x >= 512 || id.y >= 288)
        return;
    float3 rgb = Source.SampleLevel(LinearClamp, (float2(id.xy) + 0.5) / float2(512, 288), 0).rgb;
    uint index = id.y * 512 + id.x;
    Tensor[index] = rgb.r;
    Tensor[512 * 288 + index] = rgb.g;
    Tensor[2 * 512 * 288 + index] = rgb.b;
}

float4 TrackingDownsample(Vertex input) : SV_TARGET { return Source.Sample(LinearClamp, input.uv); }
