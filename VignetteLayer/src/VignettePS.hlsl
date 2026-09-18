cbuffer VignetteBuffer : register(b0)
{
    float edge_outer;
    float edge_inner;
    float edge_top;
    float edge_bottom;
    float softness;
    float corner_radius;
    float isLeftEye;
    float padding;
};

struct VS_OUTPUT
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(VS_OUTPUT input) : SV_Target
{
    float left_edge = (isLeftEye > 0.5) ? edge_outer : edge_inner;
    float right_edge = (isLeftEye > 0.5) ? edge_inner : edge_outer;
    float2 minBound = float2(left_edge, edge_top);
    float2 maxBound = float2(1.0 - right_edge, 1.0 - edge_bottom);
    float2 boxCenter = (minBound + maxBound) * 0.5;
    float2 boxHalfSize = (maxBound - minBound) * 0.5;
    float max_radius = min(boxHalfSize.x, boxHalfSize.y);
    float radius = min(corner_radius, max_radius);
    float2 p = input.uv - boxCenter;
    float2 d = abs(p) - boxHalfSize + radius;
    float dist = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - radius;
    float alpha = smoothstep(0.0, softness + 0.0001, dist);
    return float4(0.0, 0.0, 0.0, alpha);
}