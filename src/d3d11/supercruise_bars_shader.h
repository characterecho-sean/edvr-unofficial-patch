#pragma once
// The supercruise bars' private geometry shader (supercruise_bars.h says why). Compiled at BUILD time by
// tools/temporal_shader_build into kSupercruiseBarsGsBytecode (never in the game), created by shaderSwapCreateGs.
//
// WHAT IT DOES. The game draws the bars as a hardware line list: 70 segments of one pixel of the render target, each a
// vertex-shader pair (TEXCOORD1 = the colour with alpha, SV_Position = clip space with z 0). A pixel of the HDR layer is
// a fraction of one of those render pixels (2.5 of them across at 125% of a 2016 wide render), so a hardware line there
// would be a fraction of a render pixel wide: thinner and still aliased. This shader turns each segment into a strip of
// triangles whose alpha is a TENT across the segment: 1 on the line, falling linearly to 0 at `halfWidth` pixels to
// either side. The integral across the strip -- a unit-peak tent is halfWidth wide in all -- is what the caller sets to
// the weight of the render pixel the game's line had (halfWidth = layer pixels per render pixel), so the bar keeps its
// brightness and gains smooth edges. The pixel shader is the game's own: its alpha is the interpolated TEXCOORD1.w, so
// the tent is carried by the vertex colour alone (centre vertices keep the game's alpha, edge vertices take 0).
//
// THE MATHS. A segment's two end points are cut at clip-space w = clipW first (a point behind the camera has no screen
// position; the cut point's colour is interpolated in clip space, as clipping would). The cut segment is measured in
// pixels of the viewport the strip is drawn into (viewport.xy, from the layer's own viewport, never the game's), its
// perpendicular taken there, and the strip is emitted in SCREEN SPACE: every vertex has w = 1 and its NDC position, so
// the rasterizer interpolates the tent linearly in the picture whatever the depth of the segment's two ends. (Perspective
// interpolation across a strip whose ends have different w pulls the profile toward the nearer end: at a depth ratio of
// two the tent skews by a quarter of its height, at a hundred -- a segment cut at the camera plane -- it is a ramp.) What a
// hardware line DOES interpolate perspective-correctly is the colour along the segment, so the strip is cut across into
// kPieces pieces and each cross-section carries the perspective-correct colour of its place on the segment (the hardware
// line's formula at the section's screen fraction); between sections the rasterizer's linear interpolation is within a
// fraction of a percent of it. Both ends behind the plane, or no length, emit nothing.
//
// THE CONSTANT BUFFER is private to this shader (GS slot 0, which the game never binds): viewport width and height in
// pixels, the tent's half-width in the same pixels, and the clip-space w the segment is cut at.
namespace edvr {
constexpr char kSupercruiseBarsGs[] = R"HLSL(
cbuffer Strip : register(b0) {
    float2 viewport;   // the width and height, in pixels, of the viewport the strip is drawn into
    float  halfWidth;  // the tent's half-width, in those pixels: alpha 1 on the line, 0 this far to either side
    float  clipW;      // clip-space w below which a segment is cut: the camera plane
};
struct Vertex {
    float4 colour : TEXCOORD1;
    float4 pos : SV_Position;
};
static const int kPieces = 8;
[maxvertexcount(36)]
void main(line Vertex v[2], inout TriangleStream<Vertex> stream) {
    float4 p0 = v[0].pos, p1 = v[1].pos;
    float4 c0 = v[0].colour, c1 = v[1].colour;
    const bool in0 = p0.w >= clipW;
    const bool in1 = p1.w >= clipW;
    if (!in0 && !in1) return;
    if (!in0) {
        const float t = (clipW - p0.w) / (p1.w - p0.w);
        p0 = lerp(p0, p1, t);
        c0 = lerp(c0, c1, t);
    } else if (!in1) {
        const float t = (clipW - p1.w) / (p0.w - p1.w);
        p1 = lerp(p1, p0, t);
        c1 = lerp(c1, c0, t);
    }
    const float2 pixelsPerNdc = 0.5 * viewport;
    const float2 n0 = p0.xy / p0.w;
    const float2 n1 = p1.xy / p1.w;
    const float2 d = (n1 - n0) * pixelsPerNdc;
    const float length_ = length(d);
    if (!(length_ > 0.001)) return;
    const float2 side = float2(-d.y, d.x) / length_ * halfWidth / pixelsPerNdc;
    float2 centre[kPieces + 1];
    float4 colour[kPieces + 1];
    [unroll] for (int j = 0; j <= kPieces; ++j) {
        const float u = (float)j / (float)kPieces;
        const float wa = (1.0 - u) / p0.w;
        const float wb = u / p1.w;
        centre[j] = lerp(n0, n1, u);
        colour[j] = (wa * c0 + wb * c1) / (wa + wb);
    }
    Vertex o;
    o.pos.zw = float2(0.0, 1.0);
    [unroll] for (int a = 0; a <= kPieces; ++a) {
        o.colour = float4(colour[a].rgb, 0); o.pos.xy = centre[a] - side; stream.Append(o);
        o.colour = colour[a];                o.pos.xy = centre[a];        stream.Append(o);
    }
    stream.RestartStrip();
    [unroll] for (int b = 0; b <= kPieces; ++b) {
        o.colour = colour[b];                o.pos.xy = centre[b];        stream.Append(o);
        o.colour = float4(colour[b].rgb, 0); o.pos.xy = centre[b] + side; stream.Append(o);
    }
}
)HLSL";
}  // namespace edvr
