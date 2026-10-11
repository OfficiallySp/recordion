// Synthetic, game-like content for repeatable benchmarks: moving colour fields, scrolling bars and
// per-frame noise (so it is roughly as hard to encode as real game footage).

cbuffer Params : register(b0) {
    uint frame;
    uint width;
    uint height;
    uint pad;
};

struct VsOut {
    float4 pos : SV_Position;
};

// Full-screen triangle from the vertex id; no vertex buffers.
VsOut vsMain(uint id : SV_VertexID) {
    VsOut o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float hash(uint2 p) {
    uint h = p.x * 374761393u + p.y * 668265263u + frame * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (h & 0xFFFFu) / 65535.0;
}

float4 psMain(VsOut i) : SV_Target {
    const float2 uv = i.pos.xy / float2(width, height);
    const float t = frame / 60.0;
    float3 c = 0.5 + 0.5 * cos(t + uv.xyx * 6.2831853 + float3(0, 2, 4));
    const float bars = step(0.5, frac(uv.x * 8 + t * 0.5));
    c = lerp(c, c * 0.6, bars * 0.5);
    c += (hash(uint2(i.pos.xy)) - 0.5) * 0.08;
    return float4(saturate(c), 1);
}
