Texture2D<float4> Source : register(t0);
Texture2D<float> Matte : register(t1);
SamplerState LinearClamp : register(s0);
cbuffer Filter : register(b0) {
    float Similarity;
    float Softness;
    float Spill;
    float Mode;
    float4 BackgroundUV;
    float4 Enhance;
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
float3 sharpenCAS(float2 uv, float strength) {
    float3 e = Source.Sample(LinearClamp, uv).rgb;
    if (strength <= 0.001)
        return e;
    uint w = 1, h = 1;
    Source.GetDimensions(w, h);
    float2 px = 1.0 / float2(max(w, 1u), max(h, 1u));
    float3 b = Source.Sample(LinearClamp, uv + float2(0, -px.y)).rgb;
    float3 d = Source.Sample(LinearClamp, uv + float2(-px.x, 0)).rgb;
    float3 f = Source.Sample(LinearClamp, uv + float2(px.x, 0)).rgb;
    float3 g = Source.Sample(LinearClamp, uv + float2(0, px.y)).rgb;
    float3 mn = min(e, min(min(b, d), min(f, g)));
    float3 mx = max(e, max(max(b, d), max(f, g)));
    float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
    float peak = -1.0 / lerp(10.0, 5.5, saturate(strength));
    float3 wgt = amp * peak;
    return saturate((e + (b + d + f + g) * wgt) / (1.0 + 4.0 * wgt));
}
float4 Composite(Vertex v)
    : SV_TARGET {
    float3 rgb = sharpenCAS(v.uv, Enhance.x);
    float alpha = 1;
    if (Mode > 0.5 && Mode < 1.5) {
        float distance = length(chroma(rgb) - chroma(float3(0, 1, 0)));
        alpha = smoothstep(Similarity, Similarity + Softness, distance);
        rgb.g = lerp(rgb.g, min(rgb.g, max(rgb.r, rgb.b)), Spill * (1 - alpha));
    } else if (Mode > 1.5) {
        float m = saturate(Matte.Sample(LinearClamp, v.uv));
        alpha = smoothstep(0.04, 0.96, m);
    }
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
    float4 c;
    if (BackgroundUV.x > 1.05 || BackgroundUV.y > 1.05) {
        float2 stepUV = BackgroundUV.zw * 0.25;
        c = 0.25 * (Source.Sample(LinearClamp, uv + float2(-stepUV.x, -stepUV.y)) +
                    Source.Sample(LinearClamp, uv + float2(stepUV.x, -stepUV.y)) +
                    Source.Sample(LinearClamp, uv + float2(-stepUV.x, stepUV.y)) +
                    Source.Sample(LinearClamp, uv + float2(stepUV.x, stepUV.y)));
    } else {
        c = Source.Sample(LinearClamp, uv);
    }
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
// Fixed 1024x576 normalized RGB NCHW tensor for Deep Guided Filter RVM (ratio 0.5 => 512x288 backbone).
RWStructuredBuffer<float> Tensor : register(u0);
[numthreads(16, 16, 1)] void Preprocess(uint3 id : SV_DispatchThreadID) {
    if (id.x >= 1024 || id.y >= 576)
        return;
    float3 rgb = Source.SampleLevel(LinearClamp, (float2(id.xy) + 0.5) / float2(1024.0, 576.0), 0).rgb;
    uint index = id.y * 1024 + id.x;
    Tensor[index] = rgb.r;
    Tensor[1024 * 576 + index] = rgb.g;
    Tensor[2 * 1024 * 576 + index] = rgb.b;
}

float4 TrackingDownsample(Vertex input) : SV_TARGET { return Source.Sample(LinearClamp, input.uv); }
