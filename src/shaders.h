// HLSL sources, runtime-compiled with D3DCompile.
// Compute passes are direct ports of reference/script.js's GLSL fragment
// passes (including the custom dual-rate decay, saturation restore, and the
// proportional brightness cap in the splat).
#pragma once

// Shared by every compute pass via 32-bit root constants (b0).
// Keep in sync with struct SimCB in fluid.cpp.
static const char* kComputeSrc = R"hlsl(
cbuffer CB : register(b0) {
    float2 texelSize;      // 1 / sim resolution
    float  dt;
    float  dissipation;
    float  dissipationFast;
    float  decayThreshold;
    float  satRestore;     // already converted to per-step amount on CPU
    float  curlStrength;
    float  value;          // clear-pressure multiplier
    float  aspect;         // width / height
    float2 point_;         // splat center, uv
    float3 color;          // splat color (or velocity delta)
    float  radius;         // splat radius (uv^2 scale)
    float  cap;            // proportional brightness cap
    int2   dims;           // target texture dimensions
    float  baroclinic;     // dye-front torque strength (0 = off)
    float  gravity;        // dye-weighted gravity, sim texels/s^2 (+y = down, 0 = off)
    float  gravityPow;     // rho^p, so thin veils hang and dense cores fall
    float  gravityBlur;    // sim texels: blur radius of the density gravity reads
    float  pad0_;
};

SamplerState linearClamp : register(s0);

Texture2D<float4> SrcA : register(t0);
Texture2D<float4> SrcB : register(t1);
Texture2D<float4> SrcC : register(t2);   // dye (baroclinic pass only)
RWTexture2D<float2> DstV : register(u0);   // velocity-typed targets
RWTexture2D<float4> Dst4 : register(u1);   // dye / generic float4 targets
RWTexture2D<float>  Dst1 : register(u2);   // single-channel targets

int2 ClampCoord(int2 c) { return clamp(c, int2(0, 0), dims - 1); }

// --- init clears -----------------------------------------------------------
[numthreads(8, 8, 1)]
void CSClearV(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dims)) return;
    DstV[id.xy] = float2(0, 0);
}
[numthreads(8, 8, 1)]
void CSClear4(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dims)) return;
    Dst4[id.xy] = float4(0, 0, 0, 1);
}
[numthreads(8, 8, 1)]
void CSClear1(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dims)) return;
    Dst1[id.xy] = 0;
}

// --- curl ------------------------------------------------------------------
// SrcA = velocity (rg). Dst1 = curl.
[numthreads(8, 8, 1)]
void CSCurl(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dims)) return;
    int2 p = int2(id.xy);
    float L = SrcA.Load(int3(ClampCoord(p + int2(-1, 0)), 0)).y;
    float R = SrcA.Load(int3(ClampCoord(p + int2( 1, 0)), 0)).y;
    float T = SrcA.Load(int3(ClampCoord(p + int2( 0,-1)), 0)).x;
    float B = SrcA.Load(int3(ClampCoord(p + int2( 0, 1)), 0)).x;
    // Screen y is down; reference GL had y up. R-L-T+B with T/B swapped keeps
    // the same physical handedness as the reference.
    float vorticity = R - L - (B - T);
    Dst1[id.xy] = 0.5 * vorticity;
}

// --- vorticity confinement ---------------------------------------------------
// SrcA = velocity, SrcB = curl. DstV = new velocity.
[numthreads(8, 8, 1)]
void CSVorticity(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dims)) return;
    int2 p = int2(id.xy);
    float L = SrcB.Load(int3(ClampCoord(p + int2(-1, 0)), 0)).x;
    float R = SrcB.Load(int3(ClampCoord(p + int2( 1, 0)), 0)).x;
    float T = SrcB.Load(int3(ClampCoord(p + int2( 0,-1)), 0)).x;
    float B = SrcB.Load(int3(ClampCoord(p + int2( 0, 1)), 0)).x;
    float C = SrcB.Load(int3(p, 0)).x;

    // Verbatim port of the reference (its vT is +v = our B at y+1, and it
    // flips force.y). Getting this sign wrong makes confinement DAMP swirls
    // instead of amplifying them — the sim turns into honey.
    float2 force = 0.5 * float2(abs(B) - abs(T), abs(L) - abs(R));
    force /= length(force) + 0.0001;
    force *= curlStrength * C;

    // Baroclinic torque ("forms resist forms"): bend the flow along dye
    // fronts so wakes curl around existing masses instead of cutting
    // straight through. Perp(density gradient), scaled by local density so
    // empty regions are untouched. Dye is high-res; sample by uv.
    if (baroclinic > 0.0f) {
        float2 uv = (float2(p) + 0.5) * texelSize;
        float2 ex = float2(texelSize.x, 0), ey = float2(0, texelSize.y);
        float3 lw = float3(0.299, 0.587, 0.114);
        float dl = dot(SrcC.SampleLevel(linearClamp, uv - ex, 0).rgb, lw);
        float dr = dot(SrcC.SampleLevel(linearClamp, uv + ex, 0).rgb, lw);
        float dt_ = dot(SrcC.SampleLevel(linearClamp, uv - ey, 0).rgb, lw);
        float db = dot(SrcC.SampleLevel(linearClamp, uv + ey, 0).rgb, lw);
        float rho = dot(SrcC.SampleLevel(linearClamp, uv, 0).rgb, lw);
        float2 g = 0.5 * float2(dr - dl, db - dt_);
        force += baroclinic * rho * float2(g.y, -g.x);
    }

    // Dye-weighted gravity: ink is denser than water, so a dye-laden parcel
    // sinks (positive = down, negative = a buoyant/smoke rise). rho^p with
    // p > 1 means the dense head keeps falling while the thin veils, whose
    // density is a fraction of it, barely move — which is the shape of the
    // ref-1 plume. Style-agnostic; exactly zero work when gravity == 0.
    if (gravity != 0.0) {
        float2 guv = (float2(p) + 0.5) * texelSize;
        float3 glw = float3(0.299, 0.587, 0.114);
        // BLURRED density, not the per-texel one. A buoyancy force driven by a
        // noisy density field seeds Rayleigh-Taylor fingers at the GRID scale,
        // and vorticity confinement then amplifies them: the plume came out as
        // a fuzzy cauliflower instead of the references' smooth sheets. A box
        // blur over +-gravityBlur sim texels pushes the fastest-growing
        // wavelength up to that scale, so gravity sets the sink rate of whole
        // lobes and stops carving fringe.
        float2 gb = texelSize * max(gravityBlur, 0.0);
        float  grho = dot(SrcC.SampleLevel(linearClamp, guv, 0).rgb, glw) * 2.0;
        grho += dot(SrcC.SampleLevel(linearClamp, guv + float2( gb.x,  gb.y), 0).rgb, glw);
        grho += dot(SrcC.SampleLevel(linearClamp, guv + float2(-gb.x,  gb.y), 0).rgb, glw);
        grho += dot(SrcC.SampleLevel(linearClamp, guv + float2( gb.x, -gb.y), 0).rgb, glw);
        grho += dot(SrcC.SampleLevel(linearClamp, guv + float2(-gb.x, -gb.y), 0).rgb, glw);
        grho /= 6.0;
        force.y += gravity * pow(max(grho, 0.0), gravityPow);
    }

    float2 vel = SrcA.Load(int3(p, 0)).xy;
    DstV[id.xy] = vel + force * dt;
}

// --- divergence --------------------------------------------------------------
// SrcA = velocity. Dst1 = divergence.
[numthreads(8, 8, 1)]
void CSDivergence(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dims)) return;
    int2 p = int2(id.xy);
    float2 C = SrcA.Load(int3(p, 0)).xy;
    float L = (p.x == 0)          ? -C.x : SrcA.Load(int3(p + int2(-1, 0), 0)).x;
    float R = (p.x == dims.x - 1) ? -C.x : SrcA.Load(int3(p + int2( 1, 0), 0)).x;
    float T = (p.y == 0)          ? -C.y : SrcA.Load(int3(p + int2( 0,-1), 0)).y;
    float B = (p.y == dims.y - 1) ? -C.y : SrcA.Load(int3(p + int2( 0, 1), 0)).y;
    // y-down: T is the -y neighbor. div = d(vx)/dx + d(vy)/dy.
    float div = 0.5 * ((R - L) + (B - T));
    Dst1[id.xy] = div;
}

// --- pressure ----------------------------------------------------------------
// Clear: SrcA = pressure. Dst1 = pressure * value.
[numthreads(8, 8, 1)]
void CSClearPressure(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dims)) return;
    Dst1[id.xy] = SrcA.Load(int3(int2(id.xy), 0)).x * value;
}

// Jacobi iteration: SrcA = pressure, SrcB = divergence. Dst1 = new pressure.
[numthreads(8, 8, 1)]
void CSPressure(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dims)) return;
    int2 p = int2(id.xy);
    float L = SrcA.Load(int3(ClampCoord(p + int2(-1, 0)), 0)).x;
    float R = SrcA.Load(int3(ClampCoord(p + int2( 1, 0)), 0)).x;
    float T = SrcA.Load(int3(ClampCoord(p + int2( 0,-1)), 0)).x;
    float B = SrcA.Load(int3(ClampCoord(p + int2( 0, 1)), 0)).x;
    float divergence = SrcB.Load(int3(p, 0)).x;
    Dst1[id.xy] = (L + R + T + B - divergence) * 0.25;
}

// --- gradient subtract ---------------------------------------------------------
// SrcA = pressure, SrcB = velocity. DstV = new velocity.
[numthreads(8, 8, 1)]
void CSGradientSubtract(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dims)) return;
    int2 p = int2(id.xy);
    float L = SrcA.Load(int3(ClampCoord(p + int2(-1, 0)), 0)).x;
    float R = SrcA.Load(int3(ClampCoord(p + int2( 1, 0)), 0)).x;
    float T = SrcA.Load(int3(ClampCoord(p + int2( 0,-1)), 0)).x;
    float B = SrcA.Load(int3(ClampCoord(p + int2( 0, 1)), 0)).x;
    float2 velocity = SrcB.Load(int3(p, 0)).xy;
    velocity -= float2(R - L, B - T);   // y-down
    DstV[id.xy] = velocity;
}

// --- advection ------------------------------------------------------------------
// Velocity: SrcA = velocity (also the advected source). DstV = new velocity.
[numthreads(8, 8, 1)]
void CSAdvectVelocity(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dims)) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(dims);
    float2 vel = SrcA.SampleLevel(linearClamp, uv, 0).xy;
    float2 coord = uv - dt * vel * texelSize;
    float2 v = SrcA.SampleLevel(linearClamp, coord, 0).xy;
    DstV[id.xy] = v * dissipation;
}

// Dye: SrcA = velocity (sim res), SrcB = dye. Dst4 = new dye.
// Custom features from the reference: dual-rate decay (faint dye decays at
// dissipationFast) and per-step saturation restore.
[numthreads(8, 8, 1)]
void CSAdvectDye(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dims)) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(dims);
    float2 vel = SrcA.SampleLevel(linearClamp, uv, 0).xy;
    float2 coord = uv - dt * vel * texelSize;   // texelSize = sim texel, like reference
    float4 c = SrcB.SampleLevel(linearClamp, coord, 0);

    float lum = max(c.r, max(c.g, c.b));
    float k = lerp(dissipationFast, dissipation, smoothstep(0.0, max(decayThreshold, 0.0001), lum));
    c *= k;

    float mx = max(c.r, max(c.g, c.b));
    float mn = min(c.r, min(c.g, c.b));
    if (satRestore > 0.0 && mx > 0.001) {
        float3 satC = (c.rgb - mn) * (mx / max(mx - mn, 0.0001));
        c.rgb = lerp(c.rgb, satC, satRestore * smoothstep(0.0, 0.05, mx));
    }
    c.a = 1.0;
    Dst4[id.xy] = c;
}

// --- splat ------------------------------------------------------------------------
// Velocity splat: SrcA = velocity. DstV = velocity + gaussian * color.xy (uncapped).
[numthreads(8, 8, 1)]
void CSSplatVelocity(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dims)) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(dims);
    float2 p = uv - point_;
    p.x *= aspect;
    float2 splat = exp(-dot(p, p) / radius) * color.xy;
    float2 base = SrcA.Load(int3(int2(id.xy), 0)).xy;
    DstV[id.xy] = base + splat;
}

// Dye diffusion: 3x3 tent blur blended by 'value' — the D∇²c term real
// dye/smoke has. Softens splat stamps and lets fronts fade into clear fluid
// instead of stopping at a hard edge. SrcA = dye, Dst4 = new dye.
[numthreads(8, 8, 1)]
void CSDiffuseDye(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dims)) return;
    int2 p = int2(id.xy);
    float4 C = SrcA.Load(int3(p, 0));
    float4 sum = C * 4.0;
    sum += SrcA.Load(int3(ClampCoord(p + int2(-1, 0)), 0)) * 2.0;
    sum += SrcA.Load(int3(ClampCoord(p + int2( 1, 0)), 0)) * 2.0;
    sum += SrcA.Load(int3(ClampCoord(p + int2( 0,-1)), 0)) * 2.0;
    sum += SrcA.Load(int3(ClampCoord(p + int2( 0, 1)), 0)) * 2.0;
    sum += SrcA.Load(int3(ClampCoord(p + int2(-1,-1)), 0));
    sum += SrcA.Load(int3(ClampCoord(p + int2( 1,-1)), 0));
    sum += SrcA.Load(int3(ClampCoord(p + int2(-1, 1)), 0));
    sum += SrcA.Load(int3(ClampCoord(p + int2( 1, 1)), 0));
    sum /= 16.0;
    Dst4[id.xy] = lerp(C, sum, value);
}

// Coverage governor: bilinear-downsample the dye field into a 48x27 grid the
// CPU reads back once per second (SrcB = dye at t1 not used; SrcA = dye).
[numthreads(8, 8, 1)]
void CSDownsample(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dims)) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(dims);
    Dst4[id.xy] = SrcA.SampleLevel(linearClamp, uv, 0);
}

// Dye splat with the reference's proportional cap: brightness limited, hue kept.
// DROP_COMPACT: the same splat with FINITE SUPPORT. A Gaussian has infinite
// tails, and for an ink drop that is a real artefact, not a nicety: the outer
// halo of the dye stamp lies outside the velocity impulse that drives the
// drop, never acquires momentum, and is left parked at the injection point for
// the rest of the drop's life — soft mist on paper, an opaque white orb in
// inverted + HDR. Multiplying by a smooth cutoff at ~1.5-1.8 sigma removes
// exactly that halo and nothing that is visibly part of the cap.
// The cutoff is in units of dot(p,p)/radius, i.e. (d/sigma)^2 / 2: 1.8 is
// 1.90 sigma (weight 0.165) and 3.2 is 2.53 sigma (weight 0.041).
// Compiled as a SECOND PSO from this same source; the PSO the fluid look uses
// is compiled without the macro and is byte-identical to before.
[numthreads(8, 8, 1)]
void CSSplatDye(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dims)) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(dims);
    float2 p = uv - point_;
    p.x *= aspect;
    float3 splat = exp(-dot(p, p) / radius) * color;
#ifdef DROP_COMPACT
    splat *= 1.0 - smoothstep(1.8, 3.2, dot(p, p) / max(radius, 1e-9));
#endif
    float3 base = SrcA.Load(int3(int2(id.xy), 0)).rgb;
    float3 c = base + splat;
    float m = max(c.r, max(c.g, c.b));
    c *= cap / max(m, cap);
    Dst4[id.xy] = float4(c, 1.0);
}

)hlsl"
// (MSVC caps one string literal at 16380 bytes, so the compute source is
// split here and concatenated by the preprocessor. Nothing above changes.)
R"hlsl(
// ===========================================================================
// OIL DRAG ([liquid_acid] oil_drag / oil_dye_block). Three passes, dispatched
// only while one of those keys is non-zero -- the fluid look and every acid
// ini that leaves them at 0 never runs a line of this.
//
// The user: "make it impossible for the fluid sim underneath, the mono ink, to
// get under the oil, or rather an intense friction that makes it hard for it
// to get under."
//
// CSOilMask evaluates the SAME Wyvill metaball field the display shader
// thresholds, at sim resolution (~256x144, so ~37k texels x <=128 blobs = a
// rounding error next to the Jacobi solve), and soft-thresholds it. Because it
// is the same field, a negative "hole" blob is negative here too: a hole is
// NOT oil, so ink inside it is neither dragged nor faded, which is exactly the
// behaviour the holes need (the ink you see through one is the ink layer).
//
// Packing (the pass builds its own SimCB, so these are free):
//   value  = threshold        radius = support scale
//   point_ = (blob count, soft-edge half width in field units)
//   color  = (drag k, edge push, viscous diffusion k)   [drag pass]
//   cap    = dye block k                                [dye pass]
// ===========================================================================
struct OilBlobGPU { float4 a; float4 b; float4 c; };
StructuredBuffer<OilBlobGPU> OilBlobs : register(t3);

[numthreads(8, 8, 1)]
void CSOilMask(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dims)) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(dims);
    float2 pp = float2(uv.x * aspect, uv.y);
    float  field = 0.0;
    int nb = (int)point_.x;
    [loop]
    for (int bi = 0; bi < nb; bi++) {
        OilBlobGPU B = OilBlobs[bi];
        float2 q = pp - float2(B.a.x * aspect, B.a.y);
        float  sup = B.a.z * radius;
        float  s2 = sup * sup;
        float  d2 = dot(q, q);
        if (d2 >= s2) continue;
        float u = 1.0 - d2 / s2;
        field += u * u * u * B.a.w;
    }
    // Soft edge a few sim texels wide, expressed in FIELD units: the Wyvill
    // field crosses the threshold over roughly its own gradient, and point_.y
    // is tuned on the CPU from the mean blob size so the band is the same
    // handful of texels whatever the resolution.
    float m = smoothstep(value - point_.y, value + point_.y, field);
    Dst1[id.xy] = m;
}

// Velocity under the mask. Two terms:
//   1. friction -- an exponential decay toward rest, fps-normalised on the
//      CPU, so ink already under the oil comes to a stop and nothing new can
//      stream in and keep its momentum;
//   2. the mask's own GRADIENT as a gentle outward push at the rim. grad(m)
//      points INTO the oil, so -grad deflects the flow AROUND an island and
//      the ink piles up along its edge instead of leaking underneath.
// Plus, when oil_viscosity is on, a Laplacian blend toward the neighbourhood
// mean under the mask: a thick fluid's own velocity diffusion.
[numthreads(8, 8, 1)]
void CSOilDrag(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dims)) return;
    int2 c = int2(id.xy);
    float2 v = SrcA.Load(int3(c, 0)).xy;
    float  m = SrcB.Load(int3(c, 0)).x;
    float  mL = SrcB.Load(int3(ClampCoord(c - int2(1, 0)), 0)).x;
    float  mR = SrcB.Load(int3(ClampCoord(c + int2(1, 0)), 0)).x;
    float  mD = SrcB.Load(int3(ClampCoord(c - int2(0, 1)), 0)).x;
    float  mU = SrcB.Load(int3(ClampCoord(c + int2(0, 1)), 0)).x;
    if (color.z > 0.0) {
        float2 vL = SrcA.Load(int3(ClampCoord(c - int2(1, 0)), 0)).xy;
        float2 vR = SrcA.Load(int3(ClampCoord(c + int2(1, 0)), 0)).xy;
        float2 vD = SrcA.Load(int3(ClampCoord(c - int2(0, 1)), 0)).xy;
        float2 vU = SrcA.Load(int3(ClampCoord(c + int2(0, 1)), 0)).xy;
        v = lerp(v, 0.25 * (vL + vR + vD + vU), saturate(color.z * m));
    }
    v *= 1.0 - saturate(color.x * m);
    // Sim texels per unit mask: the gradient is in texel space already, which
    // is the same space the velocity lives in.
    float2 g = float2(mR - mL, mU - mD) * 0.5;
    v -= g * color.y;
    DstV[id.xy] = v;
}

// Dye that lands under the oil fades out, so the oil reads as sitting ON the
// water instead of as a colour filter over trapped ink. Ink outside the mask
// is multiplied by exactly 1.0.
[numthreads(8, 8, 1)]
void CSOilDyeBlock(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dims)) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(dims);
    float  m = SrcB.SampleLevel(linearClamp, uv, 0).x;
    float4 d = SrcA.Load(int3(int2(id.xy), 0));
    d.rgb *= 1.0 - saturate(cap * m);
    Dst4[id.xy] = d;
}
)hlsl";

// Display: fullscreen triangle sampling the dye texture, with the reference's
// pseudo-normal shading. Output is linear scRGB (1.0 = 80 nits).
static const char* kDisplaySrc = R"hlsl(
cbuffer CB : register(b0) {
    float2 texelSize;   // 1 / screen resolution (reference uses screen texels)
    float  shading;
    float  sdrScale;    // scRGB multiplier for SDR-reference white (1.0 when HDR off)
    float  gamut;       // 0 = sRGB, 1 = Display-P3 (WE parity), 2 = BT.2020 (QD-OLED),
                        // 3 = the matrix in fm2.yzw / fmOff (HDR off: gamut -> panel primaries)
    float  peakGain;    // HDR highlight expansion: peakNits / sdrWhiteNits (1 = off)
    float  knee;        // dye brightness where highlight expansion starts
    float  capBright;   // dye brightness cap (expansion reaches peakGain here)
    // CSS-filter chain parameters, evaluated primitive by primitive in gamma
    // space with a clamp to [0,1] after each one — exactly what Chromium/Skia
    // does with the reference's canvas filter (each filter function is its
    // own colour-matrix stage, and Skia clamps after every stage).
    //   fm0 = (hdrSaturate, hdrBrightness, hdrContrast, hdrEnabled)
    //   fm1 = (hueBurstDeg, postSaturate, postBrightness, postContrast)
    //   fm2 = (postHueDeg, m00, m01, m10)   fmOff = (m11, m20, m21, clip): the first two
    //         entries of each row of the gamut-3 matrix (each row sums to 1);
    //         clip (Oil3D L1b): 0 = the panel gets the scRGB value as is, 1 =
    //         the wide-gamut negatives are clipped first (the [post]
    //         aberration's max(d, 0)), 2 = HDR off on an SDR desktop DWM does
    //         not manage (clipped to 0..1): read by the OIL_LAMP cap only
    float4 fm0;
    float4 fm1;
    float4 fm2;
    float4 fmOff;
    // Response curve (Lightroom-style hump): x=enabled, y=center, z=width,
    // w=height. Remaps brightness through a Gaussian bump so mids near
    // 'center' glow to 'height' while brighter cores drop back down —
    // producing bright outlines of each splat.
    float4 curve;
    // Shadow floor ("bottom knee"): x = gray floor level, y = knee width.
    // Near-black output lifts smoothly toward the floor so dark regions keep
    // visible marbling instead of crushing to pure black. x = 0 disables.
    // z = [post] sdr_shoulder (liquid acid, HDR off only; 0 = today), w spare.
    float4 shadow;
};

SamplerState linearClamp : register(s0);
Texture2D<float4> Dye : register(t0);

// ---------------------------------------------------------------------------
// Screen mirroring / kaleidoscope ([mirror]). A uv transform applied FIRST in
// PSMain, so EVERY look inherits it: the dye sample, the acid metaball field
// (pp derives from uv) and InkWater all see the folded coordinate, and blobs,
// rims, swarms and speckle mirror with the dye instead of floating over it.
//
// This lives in the SHARED part of the source — no macro, no extra PSO — so
// mode 0 must be an exact no-op. It is: MirrorFold() returns uv untouched
// before doing any arithmetic, which is why style=fluid stays bit-identical.
//
// Structured for the SWEEP transition sketched in PROGRESS.md: (1) MirrorFold
// returns BOTH the source uv and the distance to the nearest fold line, and
// (2) the line definition (centre, angle, mode) is this one cbuffer block. A
// later "side weight" can call it for the distance alone.
// ---------------------------------------------------------------------------
cbuffer MirrorCB : register(b3) {
    float4 mrP0;   // x mode     y segments  z aspect         w time
    float4 mrP1;   // xy centre              z rotate_period  w drift
    float4 mrP2;   // x soft     y source    z grainChroma    w grainDensity
    // ---- [post]: the final composite trim, shared by every look ----------
    // Same root-constants slot because it is bound on exactly the same draws
    // and needs exactly the same two numbers (time, aspect) that mrP0 already
    // carries. Both amounts are 0 by default and the code is branched out.
    float4 poP0;   // x film_grain y grain_size z grain_speed w grain_color
    float4 poP1;   // x vignCx(uv) y vignCy(uv) z grainFps    w vignette
};

// |d| with the corner rounded off over a band of half-width s: equals abs(d)
// for |d| >> s, and has zero derivative at 0. That is the whole of `soft` —
// a mirror is already continuous in VALUE at the seam, what gives it away is
// the reversed gradient, the hard V. Rounding the fold itself removes it for
// one sqrt, where "sample both sides and lerp" would cost a second run of the
// entire look pipeline. The band is very slightly compressed in exchange.
float MirSoftAbs(float d, float s) {
    return (s > 1e-5) ? (sqrt(d * d + s * s) - s) : abs(d);
}
// Quasi-random wander in ~[-1,1]: three incommensurate sines, i.e. an fbm of
// time with no texture and no hash. The slowest term sets the pace (~2 min).
float MirWander(float t) {
    return 0.55 * sin(t) + 0.30 * sin(t * 1.913 + 1.7) + 0.15 * sin(t * 3.271 + 4.1);
}
// One folded axis. The fold line maps to the MIDDLE of the sim (0.5) and the
// screen edge to the outer edge of the chosen half, so the shown half is
// stretched to fill its mirrored tile: the full sim resolution ends up on
// screen and nothing is wasted. sgn picks which half of the sim is shown.
// sd = distance from the pixel to the fold line, uv units.
float MirFoldAxis(float u, float c, float sgn, float soft, out float sd) {
    float d = u - c;
    sd = abs(d);
    float M = max(max(c, 1.0 - c), 1e-4);
    return 0.5 + sgn * saturate(MirSoftAbs(d, soft) / M) * 0.5;
}

float2 MirrorFold(float2 uv, out float seam) {
    seam = 1e9;
    int mode = (int)(mrP0.x + 0.5);
    if (mode <= 0) return uv;          // the no-op path: nothing below runs

    float2 c = mrP1.xy;
    if (mrP1.w > 0.0005) {
        // drift: unglue the seam from the screen centre. Clamped well inside
        // the frame so one tile never collapses to nothing.
        float t = mrP0.w * 0.05;
        c += 0.18 * mrP1.w * float2(MirWander(t), MirWander(t * 0.83 + 11.0));
        c = clamp(c, 0.2, 0.8);
    }
    const float soft = max(mrP2.x, 0.0);

    if (mode >= 4) {
        // Kaleidoscope: N wedges about c, every other one REFLECTED, so each
        // wedge boundary is a mirror seam and never a jump. Angles are taken
        // in aspect-corrected space or the wedges come out sheared.
        const float aspect = max(mrP0.z, 1e-3);
        const float rot = (mrP1.z > 0.01) ? (6.2831853 * mrP0.w / mrP1.z) : 0.0;
        float2 p = (uv - c) * float2(aspect, 1.0);
        // Scale the radius so the wedge reads the sim's central DISC rather
        // than running off the texture: the wedge points in one direction for
        // the whole screen, so without this the far corners all land outside
        // [0,1]^2 and the clamp sampler smears one row of texels into radial
        // streaks. 0.5 is the largest disc that fits the frame in this space.
        float  r = length(p) * (0.5 / max(length(float2(0.5 * aspect, 0.5)), 1e-3));
        float  seg = 6.2831853 / max(mrP0.y, 2.0);
        float  a = atan2(p.y, p.x) - rot;
        float  m2 = a - seg * 2.0 * floor(a / (seg * 2.0));   // wrapped to [0, 2seg)
        float  as = soft / max(r, 1e-3);                      // uv band -> radians
        float  af = MirSoftAbs(m2 - seg, as);                 // soft at the wedge axis
        af = seg - MirSoftAbs(seg - af, as);                  // and at the wedge edge
        seam = r * min(af, seg - af);
        float  ang = af + rot;
        return c + float2(cos(ang), sin(ang)) * r * float2(1.0 / aspect, 1.0);
    }

    // Axis-aligned folds. Rotation is deliberately NOT applied here: a rotated
    // quad fold no longer tiles the rectangle exactly, and the reference look
    // is the fixed cross. The kaleidoscope is the mode that turns.
    int   src  = (int)(mrP2.y + 0.5);
    float sgnX = (src & 1) ? 1.0 : -1.0;
    float sgnY = (src & 2) ? 1.0 : -1.0;
    float2 s = uv;
    float sdx = 1e9, sdy = 1e9;
    if (mode == 1 || mode == 3) s.x = MirFoldAxis(uv.x, c.x, sgnX, soft, sdx);
    if (mode == 2 || mode == 3) s.y = MirFoldAxis(uv.y, c.y, sgnY, soft, sdy);
    seam = min(sdx, sdy);
    return s;
}

// Cheap hash for the [post] film grain (no texture, no table). Same shape as
// the acid look's own AcidHash21, duplicated here because that one lives
// inside the LIQUID_ACID block and the grain belongs to every look.
float PostHash21(float2 p) {
    p = frac(p * float2(234.34, 435.345));
    p += dot(p, p + 34.23);
    return frac(p.x * p.y);
}
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes; Oil3D L2 split
// here, at the blank line before LIQUID_ACID, so the cbuffer piece below has
// headroom. The pieces concatenate to the identical text.)
R"hlsl(
#ifdef LIQUID_ACID
// --------------------------------------------------------------------------
// "Liquid Acid" look. Compiled as a SECOND PSO from this same source with
// LIQUID_ACID defined; the fluid PSO is compiled without it and therefore
// contains none of this code (bit-identical to the pre-feature shader).
// Everything here is parameterised from LiquidAcidConfig (see fluid.h).
// --------------------------------------------------------------------------
cbuffer AcidCB : register(b1) {
    // laMix, the 20x12 hue2 mix field, is four cells per float4. Small on
    // purpose: the patches the reference shows are a quarter to a half of the
    // frame, so this carries them with room to spare and costs one cbuffer
    // fetch and a bilinear blend per pixel -- no texture, no descriptor.
    // ---- BEGIN GENERATED by tools\slot-check.ps1 -Fix from src\acid_slots.h + AcidParamsGPU;
    // do not hand-edit. The acid code reads each packed scalar as LA_<NAME>, a
    // '#define LA_<NAME> laP<n>.<c>' that kAcidSlotMacros (fluid.cpp) passes to
    // D3DCompile. Units, ini keys and hardcoded values: src\acid_slots.h.
    float4 laOil[4];  // oil palette, rgb
    float4 laInk[4];  // ink ramp stops (dark -> bright), rgb
    float4 laP0;      // x BLOB_COUNT  y THRESHOLD  z SUPPORT_SCALE  w AA_SCALE
    float4 laP1;      // x RIM_WIDTH  y RIM_INSET  z RIM_DARK  w REFRACTION
    float4 laP2;      // x MENISCUS  y MENISCUS_W  z TRANSLUCENCY  w OIL_TEXTURE
    float4 laP3;      // x INK_LEVELS  y INK_SOFT  z INK_MIX  w INK_HUE_VARY
    float4 laP4;      // x INK_GAIN  y INK_BIAS  z SEAM_STR  w SEAM_SCALE
    float4 laP5;      // x SEAM_LO  y SEAM_HI  z GRAIN  w GRAIN_SCALE
    float4 laP6;      // x SPECKLE  y SPECKLE_SCALE  z TIME  w ASPECT
    float4 laP7;      // x OIL_HDR  y RIM_HDR  z MENISCUS_OFF  w INK_SHADING
    float4 laP8;      // x SWARM_HOLES  y SWARM_DROPS  z SWARM_DENSITY  w SWARM_RIM_DARK
    float4 laP9;      // x SWARM_SCALE_HOLES  y SWARM_SCALE_DROPS  z SWARM_R_MIN  w SWARM_R_MAX
    float4 laP10;     // x SWARM_CLUMP  y SWARM_DARK  z INK_WATER  w TOE_TINT
    float4 laP11;     // x INK_LOCK  y INK_LOCK_SPAN  z INK_TARGET_HUE  w DYE_DROP_RGB
    float4 laP12;     // x RIM_VARY  y RIM_INK_FOLLOW  z RIM_ORDER  w GRAIN_SHADOW_W
    float4 laP13;     // x OIL_THIN_EDGE  y OIL_EDGE_FRAC  z OIL_SPECULAR  w OIL_IRID
    float4 laP14;     // x SWARM_LENS  y MEN_FROM_INK  z OIL_GLOW  w REFR_WIDTH
    float4 laP15;     // x OIL_TRANSP  y OIL_ABSORB  z OIL_FILM_BUMP  w OIL_REFR_BODY
    float4 laP16;     // x OIL_INK_BLUR  y DYE_DEPTH_W  z MEN_FILM_MIX  w DYE_SMOKE
    float4 laP17;     // x RISE_BOTTOM_LIGHT  y POST_CHROMA  z POST_LIFT  w FILM_LEVEL
    float4 laP18;     // x DROPS_ON  y DROP_GRID_W  z DROP_GRID_H  w OIL_EDGE_MODE
    float4 laP19;     // x DROP_SUPPORT  y DROP_WEIGHT  z DROP_OIL_W  w DROP_RING_BASE
    float4 laP20;     // x DROP_RING_WIDTH  y DROP_RING_LIFT  z OIL_EDGE_CURVE  w DIFFR_SCALE
    float4 laP21;     // x HALO  y HALO_W  z SOFTNESS  w BAND_MIN
    float4 laP22;     // x PENUMBRA  y PENUMBRA_W  z PENUMBRA_HUE  w PENUMBRA_DARK
    float4 laP23;     // x CELL_INK  y CELL_OIL  z CELL_SCALE  w CELL_DRIFT
    float4 laMen;     // meniscus halo colour, rgb
    float4 laP24;     // x CAM_AXIS_X  y CAM_AXIS_Y  z FOCUS_DEPTH  w DOF_MAX_PX
    float4 laP25;     // x FIELD_CURVE  y TILT_AMT  z TILT_COS  w TILT_SIN
    float4 laP26;     // x FOCUS_BAND  y COC_SPAN_INV  z FOV_K  w DIFFRACTION
    float4 laP27;     // x LENS  y LENS_CENTRE  z LENS_BAND  w DROP_SPEC
    float4 laP28;     // x LAMP_X  y LAMP_Y  z DYE_DEPTH  w DYE_TILT
    float4 laP29;     // x MASS_RIM  y MASS_RIM_W  z DYE_AMT  w DYE_HUE
    float4 laP30;     // x HUE2_AMT  y HUE2_DEG  z HUE3_AMT  w HUE3_DEG
    float4 laP31;     // x CRUST_HUE_MIX  y REFLECT_R  z REFLECT_AMT  w DYE_SAT
    float4 laP32;     // x SHADOW_AMT  y SHADOW_LEN  z SHADOW_SOFT  w LIGHT_Z
    float4 laP33;     // x OIL_FLUOR  y OIL_FLUOR_REACH  z DYE_LAMP_FOLLOW  w DARK_SAT
    float4 laP34;     // x DYE_LUM_VARY  y DYE_HUE_VARY  z DYE_THICK_HUE  w DYE_ID_RISE
    float4 laP35;     // x DYE_CORE  y HUE3_SHARE  z EQUAL_LOAD  w HUE2_SEAM
    float4 laP36;     // x GREY_K  y GREY_SIZE  z GREY_COOL  w GREY_CX
    float4 laP37;     // x TONE_R  y TONE_G  z TONE_B  w GREY_CY
    float4 laP38;     // x EQUAL_LOAD_P  y HEART_K  z HEART_SIZE  w HEART_LIFT
    float4 laP39;     // x HTONE_R  y HTONE_G  z HTONE_B  w TONE_BAL
    float4 laP40;     // x OIL_H_LAMBDA  y OIL_H_TOE  z OIL_H_DEBUG  w OIL3D_ABSORB
    float4 laP41;     // x TANK_FADE  y SH_OFF_X  z SH_OFF_Y  w PEN_R
    float4 laP42;     // x SH_UNDER  y SH_FILM  z FP_STRETCH  w ABS_SHARE
    float4 laP43;     // x CS_REACH  y WS_SHARE  z WS_TAU  w COV_AX
    float4 laP44;     // x COV_AY  y COV_BX  z COV_BY  w L2_WLEVEL
    float4 laMix[60]; // hue2 mix field: 20x12 cells, four per float4 (brief AE)
    float4 laLamp[64]; // Oil3D L1 lamp 1 window LUT over the hue2 share k2: hue shift, chroma, lum ratio, bare lum
    float4 laGlow;    // Oil3D L1b lamp 1 window colour, linear rgb (white 1 1 1), w = not white: read by the LAMP_GLOW post variants only
    float4 laP45;     // x L2_OFF_X  y L2_OFF_Y  z L2_PEN_R  w L2_CS_REACH
    float4 laLamp2[64]; // Oil3D L2 lamp 2 window LUT over k2 (BuildLampLut(2)): hue shift, chroma, lum ratio x lamp2_level, bare lum: OIL_LAMP2 only
    float4 laMass[4]; // Oil3D L3 masses under the lamps (BuildMassConsts): [0]/[1] dyed-mass change per lamp, [2]/[3] dark-dye oil rgb windowed/white: OIL_MASSDYE only
    float4 laLampW[64]; // Oil3D L3b the WHITE state with the film dyes (BuildLampLut(0)): hue shift, chroma, lum ratio: OIL_DYEWHITE only (the white track)
    float4 laMurk[2]; // Oil3D W1 murk: [0] uv -> tile scale xy + drift xy, [1] tiles per unit depth, z drift, self-shadow k, murk_light: OIL_MURK only
    float4 laMote[3]; // Oil3D C3 motes: [0] grid w h + record/cell bases in the droplet buffers, [1] frame px w h + blur px + 1/span, [2].x brightness: OIL_MOTES only
    float4 laWater[2]; // Oil3D W2 water colour: [0] the water gain, linear rgb (white = clear) + w the normaliser cap, [1] water x silt rgb: OIL_WTINT only
    float4 laGrain[3]; // Oil3D W3 fine silt + haze: [0] grain, lattice px, haze tau, veil lift, [1] slice 0/1 offsets, [2] slice 2 offset, slices, churn clock: OIL_GRAIN only
    float4 laDrop[3]; // droplet life: [0] droplet_shadow, cot(light_elev), tan(light_size/2), droplet_wobble, [1] away-from-lamp dir xy, glint turn, old droplet shadow keep, [2] droplet_core, size ramp lo hi: DROP_LIFE only
    // ---- END GENERATED
};
// xy = centre uv, z = radius, w = field weight (+1 oil, negative = hole)
// rgb of .b = flat fill colour, .w = rise_stretch anisotropy (0 = round)
// .c = mouse_oil_mode 2 (comb): x = stretch amount along the DRAG direction
// (0 = off, and then the whole comb branch is skipped and the round/rise
// path below is the original code), yz = that direction as a unit vector in
// p-space. Zero for every blob unless the comb is actually running.
// .c.w = the blob's dye IDENTITY in (0,1), a CPU hash fixed for the blob's
// life (brief AG-b). Read for hole blobs only, and only with dye variation on.
struct AcidBlobGPU { float4 a; float4 b; float4 c; };
StructuredBuffer<AcidBlobGPU> AcidBlobs : register(t1);

// ---------------------------------------------------------------------------
// DROPLET PARTICLE SIM. One float4 per particle: xy = centre uv, z = visible
// radius SIGNED (negative = water trapped in the oil, i.e. a hole; positive =
// an oil droplet on open ink), w = the CPU-side contribution gate (0 while a
// droplet is on the wrong side of the interface or below a pixel across --
// invisible either way, and without the gate its steep little gradient would
// still collapse sdf and print a hollow rim ring; see StepAcidDroplets). The CPU bins them into a uniform
// grid and uploads them sorted by cell; DropCells[c] = (first, count), so a
// pixel only walks the 3x3 cells around it. The support radius is clamped on
// the CPU to one cell, which is what makes 3x3 exact rather than approximate.
// Both buffers are all-zero (count 0 everywhere) when the sim is off.
// ---------------------------------------------------------------------------
StructuredBuffer<float4> AcidDrops : register(t2);
StructuredBuffer<uint2>  DropCells : register(t4);
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
float AcidHash21(float2 p) {
    p = frac(p * float2(234.34, 435.345));
    p += dot(p, p + 34.23);
    return frac(p.x * p.y);
}
float AcidVNoise(float2 p) {
    float2 i = floor(p), f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(AcidHash21(i),               AcidHash21(i + float2(1, 0)), u.x),
                lerp(AcidHash21(i + float2(0, 1)), AcidHash21(i + float2(1, 1)), u.x), u.y);
}
float AcidFbm(float2 p) {
    float v = 0.0, amp = 0.5;
    for (int i = 0; i < 3; i++) { v += amp * AcidVNoise(p); p = p * 2.13 + 19.19; amp *= 0.5; }
    return v;
}
// n flat bands with a soft shoulder at each step: the ink's "flat colour
// bands" without the hard 8-bit staircase.
float AcidBand(float x, float n, float soft) {
    float f = floor(x * n), fr = frac(x * n);
    return (f + smoothstep(0.5 - soft, 0.5 + soft, fr)) / n;
}
// Hue rotation that PRESERVES saturation and value, so a colour is exactly as
// vivid at its new hue as it was at its old one. CssHueRotate (the W3C matrix)
// holds luma instead, so a bright orange rotated toward yellow lands on olive
// and a saturated red-orange lands on pastel lavender - which is what the
// first sweep build did. Every reference frame is vivid at every hue, so the
// sweep and the complement lock both rotate here, not in the matrix.
float3 AcidRgb2Hsv(float3 c) {
    float mx = max(c.r, max(c.g, c.b));
    float mn = min(c.r, min(c.g, c.b));
    float d  = mx - mn;
    float h  = 0.0;
    if (d > 1e-7) {
        if (mx == c.r)      h = (c.g - c.b) / d + (c.g < c.b ? 6.0 : 0.0);
        else if (mx == c.g) h = (c.b - c.r) / d + 2.0;
        else                h = (c.r - c.g) / d + 4.0;
        h /= 6.0;
    }
    return float3(h, (mx > 1e-7) ? d / mx : 0.0, mx);
}
float3 AcidHsv2Rgb(float3 c) {
    float3 q = abs(frac(c.x + float3(1.0, 0.6666667, 0.3333333)) * 6.0 - 3.0);
    return c.z * lerp(1.0, saturate(q - 1.0), c.y);
}
float3 AcidHueShift(float3 c, float deg) {
    float3 hsv = AcidRgb2Hsv(c);
    hsv.x = frac(hsv.x + deg * 0.0027777778);
    return AcidHsv2Rgb(hsv);
}
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
// ---- dark_sat (brief BP): darker must mean MORE saturated ----------------
// The user: "it reduces colours and then reduces saturation as well, when
// those should be inversely proportional." A chroma gain that grows as the
// ELEMENT's level falls (film_level for the film, the dye level for a mass
// or a droplet -- never per-pixel luminance, which would re-grade every
// shadow), applied about the pixel's own LINEAR luminance: the colours here
// are sRGB-encoded, and a gain about the encoded luma (the first version)
// raised the displayed luminance by 10-40% through the decode's curvature
// (measured on the .jxr), so it decodes, scales about linear Y, re-encodes.
// Y is kept, so mean_lum, the HDR peak and the panel's ABL do not move. The
// only clamps are the gamut edges: no channel below 0, and no channel pushed
// past 1 that was not already there (the composite is saturate()d later, so a
// clipped channel would lose luminance), so HDR on and off agree. Callers
// skip it at dark_sat 0, so today's frame is untouched.
float3 DsToLin(float3 c) {
    c = max(c, 0.0);
    return lerp(c / 12.92, pow((c + 0.055) / 1.055, 2.4), step(0.04045, c));
}
float3 DsToSrgb(float3 l) {
    l = max(l, 0.0);
    return lerp(l * 12.92, 1.055 * pow(l, 1.0 / 2.4) - 0.055, step(0.0031308, l));
}
float3 DarkSat(float3 c, float lvl) {
    float3 l = DsToLin(c);
    float Y = dot(l, float3(0.2126, 0.7152, 0.0722));
    float m = min(l.r, min(l.g, l.b));
    float M = max(l.r, max(l.g, l.b));
    float k = 1.0 + LA_DARK_SAT * (1.0 - saturate(lvl));
    k = min(k, Y / max(Y - m, 1e-6));
    k = min(k, max((1.0 - Y) / max(M - Y, 1e-6), 1.0));
    float3 o = DsToSrgb(Y + (l - Y) * k);
    // post_chroma (acid-rise-12: 1.2) later scales chroma about the ENCODED
    // luma and clamps at 0, which turns extra chroma back into luminance
    // (measured +3..8% mean_lum at dark_sat 1 without this). So hold the
    // luminance the pixel will have AFTER that trim: two fixed-point steps of
    // a linear-light gain, each within a fraction of a percent of the last.
    [branch] if (abs(LA_POST_CHROMA - 1.0) > 0.001) {
        const float3 W = float3(0.2126, 0.7152, 0.0722);
        float  pc = LA_POST_CHROMA;
        float  y0 = dot(c, W);
        float  t0 = dot(DsToLin(max(y0 + (c - y0) * pc, 0.0)), W);
        [unroll] for (int it = 0; it < 2; it++) {
            float y1 = dot(o, W);
            float t1 = dot(DsToLin(max(y1 + (o - y1) * pc, 0.0)), W);
            o = DsToSrgb(DsToLin(o) * (t0 / max(t1, 1e-8)));
        }
    }
    return o;
}
#ifdef OIL_STAGETAP
// ---- Oil3D L3b: --shot-stage-tap, the OIL_STAGETAP shot-only variants -------
// OIL_STAGETAP = n returns the WINDOWED track's value right after stage n of
// the chain (1 the two-lamp sum ... 14 the pass's output; 16..19 split stage
// 3: before the thin film, the transparent film, the relief, the rim terms),
// 100 + n the WHITE track's (the OIL_LAMP variants; the windowed one
// elsewhere), 15 / 115 the
// two tracks at the output cap's input, 99 the cap's (yN, yW, s). Linear light:
// an encoded composite is decoded (signed). Built only by CaptureStageTap,
// so no default variant ever sees the define.
float4 OilTapLin(float3 c) { return float4(sign(c) * DsToLin(abs(c)), 1.0); }
float4 OilTapRaw(float3 c) { return float4(c, 1.0); }
#ifdef OIL_LAMP
#define OIL_TAPW(w, t) (t)
#else
#define OIL_TAPW(w, t) (w)
#endif
#endif
#ifdef OIL_LAMP
// ---- Oil3D L1: LAMP 1 AS A SPECTRAL WINDOW (the OIL_LAMP PSO variant) -----
// film colour = lamp E x T_film x visibility. The CPU (BuildLampLut) evaluates
// the window x dye model -- the Beer-Lambert product of the film's dyes on a
// 48-sample hue circle relative to the anchor, under lamp 1's window -- into
// laLamp[64] over the hue2 share k2, as the CHANGE against today's state under
// a white lamp: x = hue shift (deg), y = chroma headroom (>= 0: that fraction
// of the way to full saturation; < 0: saturation x (1 + y)), z = luminance
// ratio, <= 1 (a window only removes light: the OLED frame-mean cap).
// Applied TO today's colour, so the film keeps its per-pixel shading (palette
// shades, lamp ramp) and a white window is today; an RGB filter could only
// dim a saturated film, while a window off its band has to flip its hue (the
// reveal: a film that reads one colour under white reads another).
// L1b: z's cap holds the FILM's own Y; the binding cap is per pixel in the
// OUTPUT space at the end of PSMain (the white track, see "THE CAP" there),
// because the steps between the film and the panel lift a purer film above
// the mixed film it replaces. lamp1_level is the compensation knob: it
// recovers what the window cost, up to today's light per pixel, never more.
float4 OilLampLut(float k2) {
    float x  = saturate(k2) * 63.0;
    float i0 = min(floor(x), 62.0);
    int   i  = (int)i0;
    return lerp(laLamp[i], laLamp[i + 1], x - i0);
}
float3 OilLampFilm(float3 c, float4 L) {
    const float3 WY = float3(0.2126, 0.7152, 0.0722);
    float  y0  = dot(DsToLin(c), WY);
    float3 hsv = AcidRgb2Hsv(c);
    hsv.x = frac(hsv.x + L.x * (1.0 / 360.0));
    hsv.y = saturate((L.y >= 0.0) ? hsv.y + L.y * (1.0 - hsv.y) : hsv.y * (1.0 + L.y));
    float3 l1  = DsToLin(AcidHsv2Rgb(hsv));
    // today's luminance x the ratio, in linear light; no channel past 1 (the
    // composite is saturate()d later, so a clipped channel would only lose)
    float  s   = L.z * y0 / max(dot(l1, WY), 1e-6);
    s = min(s, 1.0 / max(max(l1.r, max(l1.g, l1.b)), 1e-6));
    return DsToSrgb(l1 * s);
}
#ifdef OIL_LAMP2
// ---- Oil3D L2: LAMP 2's LUT (the OIL_LAMP2 variants, always with OIL_LAMP) -
// laLamp2 = the same construction as laLamp from the lamp2_* keys (CPU,
// BuildLampLut(2, ...)): per hue2 share k2 the change lamp 2's window makes to
// today's film; z = lamp2_level x its luminance ratio (<= 1). OilLampFilm turns
// it into the film as lamp 2 ALONE lights it, at z x today's Y.
float4 OilLamp2Lut(float k2) {
    float x  = saturate(k2) * 63.0;
    float i0 = min(floor(x), 62.0);
    int   i  = (int)i0;
    return lerp(laLamp2[i], laLamp2[i + 1], x - i0);
}
// Lamp 2's DELIVERED share: its film's linear Y against the white track's, both
// with the same light terms and neither occluded yet (re-taken after each
// shared term: the films are sRGB-encoded and a common encoded factor does not
// keep a linear ratio exactly, ~1.5 % of the share at the footprint's 1.2).
// y2 / yW: the two unoccluded Ys themselves (the --shot-debug lamps view).
float OilLamp2Share(float3 c2, float3 cW, out float y2, out float yW) {
    const float3 WY = float3(0.2126, 0.7152, 0.0722);
    y2 = dot(DsToLin(c2), WY);
    yW = dot(DsToLin(cW), WY);
    return y2 / max(yW, 1e-8);
}
#endif
#ifdef OIL_DYEWHITE
// ---- Oil3D L3b: the WHITE track carries the SAME film (OIL_DYEWHITE) -------
// laLampW = BuildLampLut(0): the window x dye model under a WHITE window at
// level 1 with the film's dyes (dye_a2, fluid B's dye_b1 / dye_b2), as the
// change against today's film: what the dyes alone make of it. The white
// track (the output cap's reference, lamp 2's share, the filled reference)
// is today's film THROUGH the same dyes under white light, so a window is
// capped against the same film under white, never against the undyed one
// (port chat L3 sheets ruling (2): the stale cap reference).
float4 OilLampWLut(float k2) {
    float x  = saturate(k2) * 63.0;
    float i0 = min(floor(x), 62.0);
    int   i  = (int)i0;
    return lerp(laLampW[i], laLampW[i + 1], x - i0);
}
#endif
#ifdef OIL_MASSDYE
// ---- Oil3D L3 (3): the DYED MASS under the lamps (the OIL_MASSDYE variants) -
// laMass[0] / [1] = the mass dye's own ONE-SAMPLE product per lamp (CPU, one
// constant per lamp per frame, not the f LUT): the change lamp 1's / lamp 2's
// window makes to the dye_hue dye (hue shift, chroma, lum ratio; [1].z x
// lamp2_level), applied to today's dye colour like the film's LUT, summed in
// linear light over 1 + lamp 2's share. The white state = today's dye.
float3 OilMassWin(float3 c) {
    float3 l = DsToLin(OilLampFilm(c, laMass[0]));
#ifdef OIL_LAMP2
    l = (l + DsToLin(OilLampFilm(c, laMass[1]))) / (1.0 + laMass[1].z);
#endif
    return DsToSrgb(l);
}
#endif
#endif
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
// ---- brief BU: LAMP GREY (the far corner), one Y-flat desaturation -------
// The corner (lamp_grey, BU) calls it on the composite right after the film
// is laid down, with a cool white balance. (The centred grey heart, BX,
// used it too until the 2026-09-27 fix round; it now has its own OKLab
// HeartGrey on the finished colour. liftMul stays: the corner passes 1.0.)
// gk = desaturation weight 0..1, gkCool = share of the Y-normalised cool
// white balance, liftMul = linear-light multiplier on the result (1 = none,
// a literal that folds away at the corner's call).
// Desaturation about the pixel's LINEAR luminance (the composite is
// sRGB-encoded, and a mix about encoded luma would darken saturated film by
// 20-40%): Y is kept, so ABL and mean_lum do not move and black stays black.
// Then the post_chroma hold: that trim scales chroma about the ENCODED luma
// and clamps at 0, which gives a saturated pixel extra luminance a greyed
// one no longer gets (measured -1..-2% mean_lum on a bright film), so hold
// the luminance the ORIGINAL pixel would have after it (times the lift; a
// target taken from the lifted GREY pixel would be its own luminance, a
// no-op), exactly as DarkSat does: two fixed-point gain steps.
float3 LampGreyY(float3 col, float gk, float gkCool, float liftMul) {
    const float3 GW = float3(0.2126, 0.7152, 0.0722);
    float3 gl = DsToLin(col);
    float  gY = dot(gl, GW);
    gl = gY + (gl - gY) * (1.0 - gk);
    float3 gc = gl * float3(0.80, 0.94, 1.30);
    gc *= gY / max(dot(gc, GW), 1e-7);
    gl = lerp(gl, gc, gkCool);
    gl *= liftMul;
    float3 go = DsToSrgb(gl);
    [branch] if (abs(LA_POST_CHROMA - 1.0) > 0.001) {
        float  pc = LA_POST_CHROMA;
        float  y0 = dot(col, GW);
        float  t0 = dot(DsToLin(max(y0 + (col - y0) * pc, 0.0)), GW) * liftMul;
        [unroll] for (int it = 0; it < 2; it++) {
            float y1 = dot(go, GW);
            float t1 = dot(DsToLin(max(y1 + (go - y1) * pc, 0.0)), GW);
            go = DsToSrgb(DsToLin(go) * (t0 / max(t1, 1e-8)));
        }
    }
    return go;
}

// ---- brief BU-b: the luminance the PANEL gets from a composite colour ----
// x = linear (709-coded) composite after post_chroma / post_lift / the clip.
// The gamut stretch below (gamut 2 = BT.2020, 1 = P3) turns a saturated
// film into out-of-gamut values whose NEGATIVE channels are clamped to 0 on
// the way out, and that clamp ADDS luminance (measured: the magenta film's
// G -> -0.075 clamp is +33% of its displayed Y). A shift that holds any
// fixed-weight Y still loses that bonus (-19% mean_lum at desat 0.5), so
// the highlight tone holds THIS instead: exact, and 1-homogeneous
// (BubLum(k x) = k BubLum(x), k >= 0), so a scale is one step.
float BubLum(float3 x) {
    float3 y = x;
    if (gamut > 2.5) {
        y = float3(dot(float3(fm2.y, fm2.z, 1.0 - fm2.y - fm2.z), x),
                   dot(float3(fm2.w, fmOff.x, 1.0 - fm2.w - fmOff.x), x),
                   dot(float3(fmOff.y, fmOff.z, 1.0 - fmOff.y - fmOff.z), x));
    } else if (gamut > 1.5) {
        y = float3(dot(float3( 1.66049, -0.58764, -0.07285), x),
                   dot(float3(-0.12455,  1.13290, -0.00835), x),
                   dot(float3(-0.01815, -0.10058,  1.11873), x));
    } else if (gamut > 0.5) {
        y = float3(dot(float3( 1.22494, -0.22494,  0.0),     x),
                   dot(float3(-0.04206,  1.04206,  0.0),     x),
                   dot(float3(-0.01964, -0.07868,  1.09832), x));
    }
    return dot(max(y, 0.0), float3(0.2126, 0.7152, 0.0722));
}

// ---- brief BX (fix round 2026-09-27): the GREY HEART's own desaturation ---
// Runs on the FINISHED linear colour, after the post_chroma trim and the
// gamut stretch (the old site, before the final trim, was re-saturated by
// both: post_chroma 1.2 scaled the residual chroma while the saturated
// original clamps at 0, and the stretch's clamp did the same), and in OKLab
// instead of about linear Y: a Y-flat mix keeps a HUE-DEPENDENT share of the
// perceived chroma (0.20 yellow .. 0.35 blue at 0.75), OKLab a/b x (1 - g)
// keeps exactly 1 - g on every hue. The cube root is signed so a stretched
// colour's negative channels (wide gamut, shown by the QD-OLED and counted by
// mean_lum) are carried, and g = 0 is the identity. Then a luminance hold on
// the pixel's own Rec.709 Y of this scRGB value (what the panel emits and
// what mean_lum sums, negatives included), times the lift.
float3 HeartGrey(float3 lin, float g, float liftMul) {
    const float3 W = float3(0.2126, 0.7152, 0.0722);
    float  yT  = dot(lin, W) * liftMul;
    float3 lms = float3(dot(float3(0.4122214708, 0.5363325363, 0.0514459929), lin),
                        dot(float3(0.2119034982, 0.6806995451, 0.1073969566), lin),
                        dot(float3(0.0883024619, 0.2817188376, 0.6299787005), lin));
    lms = sign(lms) * pow(abs(lms), 1.0 / 3.0);
    float3 lab = float3(dot(float3(0.2104542553,  0.7936177850, -0.0040720468), lms),
                        dot(float3(1.9779984951, -2.4285922050,  0.4505937099), lms),
                        dot(float3(0.0259040371,  0.7827717009, -0.8086757660), lms));
    lab.yz *= 1.0 - g;
    lms = float3(lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z,
                 lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z,
                 lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z);
    lms = lms * lms * lms;
    float3 o = float3(dot(float3( 4.0767416621, -3.3077115913,  0.2309699292), lms),
                      dot(float3(-1.2684380046,  2.6097574011, -0.3413193965), lms),
                      dot(float3(-0.0041960863, -0.7034186147,  1.7076147010), lms));
    float  y = dot(o, W);
    return (abs(y) > 1e-8) ? o * (yT / y) : o;
}
#ifdef OIL_LAMP
// ---- Oil3D L1b: the white track's copies of two PSMain steps ---------------
// The OIL_LAMP variants carry TODAY's composite beside the windowed one to the
// end of the pass (the output cap). Most steps are one line and are repeated
// inline; these two are not, so they are copies, kept in step by hand:
// OilHTone = PSMain's HIGHLIGHT TONE block (brief BU-b), verbatim on its
// argument; OilLampGamut = the output gamut stretch (BubLum's matrix,
// without its clamp), the same branches PSMain takes on `lin`.
float3 OilHTone(float3 col, float alpha) {
    const float3 GW = float3(0.2126, 0.7152, 0.0722);
    float  hPc = max(LA_POST_CHROMA, 1e-3), hPl = max(LA_POST_LIFT, 1e-3);
    float  hY0 = dot(col, GW);
    float3 d0 = DsToLin(saturate(max(hY0 + (col - hY0) * hPc, 0.0) * hPl));
    float  hT = BubLum(d0);
    float  hSp = 0.5 - 0.25 * LA_TONE_BAL;
    float  hW = alpha * smoothstep(hSp - 0.25, hSp + 0.05, DsToSrgb(hT.xxx).x);
    [branch] if (hW > 0.0) {
        float3 hU = float3(LA_HTONE_R, LA_HTONE_G, LA_HTONE_B);
        float  hMag = dot(hU, GW);
        float3 hc = d0 * (hU / hMag);
        float  hY = dot(hc, GW);
        hc = hY + (hc - hY) * (1.0 - saturate(hMag - 1.0));
        float hM0 = max(max(d0.r, max(d0.g, d0.b)), hT);
        float hM1 = max(hc.r, max(hc.g, hc.b));
        if (hM1 > hM0) hc *= hM0 / hM1;
        float3 hx = lerp(d0, hc, hW);
        float  hL = BubLum(hx);
        [branch] if (hL >= hT) {
            hx *= hT / max(hL, 1e-8);
        } else {
            float lo = 0.0, hi = 1.0;
            [unroll] for (int it = 0; it < 7; it++) {
                float mid = 0.5 * (lo + hi);
                if (BubLum(lerp(hx, hM0.xxx, mid)) < hT) lo = mid; else hi = mid;
            }
            hx = lerp(hx, hM0.xxx, 0.5 * (lo + hi));
        }
        float3 he = DsToSrgb(hx);
        float  hy = dot(he, GW) / hPl;
        col = max(hy + (he / hPl - hy) / hPc, 0.0);
    }
    return col;
}
float3 OilLampGamut(float3 x) {
    if (gamut > 2.5) {
        return float3(dot(float3(fm2.y, fm2.z, 1.0 - fm2.y - fm2.z), x),
                      dot(float3(fm2.w, fmOff.x, 1.0 - fm2.w - fmOff.x), x),
                      dot(float3(fmOff.y, fmOff.z, 1.0 - fmOff.y - fmOff.z), x));
    } else if (gamut > 1.5) {
        return float3(dot(float3( 1.66049, -0.58764, -0.07285), x),
                      dot(float3(-0.12455,  1.13290, -0.00835), x),
                      dot(float3(-0.01815, -0.10058,  1.11873), x));
    } else if (gamut > 0.5) {
        return float3(dot(float3( 1.22494, -0.22494,  0.0),     x),
                      dot(float3(-0.04206,  1.04206,  0.0),     x),
                      dot(float3(-0.01964, -0.07868,  1.09832), x));
    }
    return x;
}
#ifdef OIL_LAMP2
// ---- Oil3D L3b: the TRIM HOLD's metric ---------------------------------------
// The luminance the PANEL gets from composite c after the final trim, step for
// step as the end of PSMain makes it: post_chroma about the encoded luma (clamp
// 0), post_lift, the clip, the vignette vg, the gamut stretch, and then the
// OUTPUT's own clip as the cap measures it (fmOff.w > 1.5: HDR off, 0..1; > 0.5:
// the negatives, the [post] aberration's max(d, 0); else the scRGB as is).
float OilTrimLum(float3 c, float vg) {
    const float3 GW = float3(0.2126, 0.7152, 0.0722);
    float  y0 = dot(c, GW);
    float3 o  = OilLampGamut(DsToLin(saturate(max(y0 + (c - y0) * max(LA_POST_CHROMA, 1e-3), 0.0)
                                              * max(LA_POST_LIFT, 1e-3)) * vg));
    if (fmOff.w > 1.5)      o = saturate(o);
    else if (fmOff.w > 0.5) o = max(o, 0.0);
    return dot(o, GW);
}
#endif
#endif

// ---- the 20x12 hue2 MIX FIELD, one bilinear fetch (brief AE / AJ) --------
// Factored out of the display pass so the boundary-reflection reach (AJ) can
// probe the field a few times without repeating the index arithmetic. No
// texture and no descriptor: laMix is 240 floats already resident in the
// cbuffer, so a "tap" here is four indexed loads and three lerps.
// mt is smoothstepped, so the 20x12 cells never show as a quad grid.
float AcidMixAt(float2 uv) {
    const float MW = 20.0, MH = 12.0;
    float2 mf = float2(uv.x * MW - 0.5, uv.y * MH - 0.5);
    float2 mi = floor(mf), mt = mf - mi;
    mt = mt * mt * (3.0 - 2.0 * mt);
    int x0 = (int)clamp(mi.x, 0.0, MW - 1.0), x1 = (int)clamp(mi.x + 1.0, 0.0, MW - 1.0);
    int y0 = (int)clamp(mi.y, 0.0, MH - 1.0), y1 = (int)clamp(mi.y + 1.0, 0.0, MH - 1.0);
    int i00 = y0 * 20 + x0, i10 = y0 * 20 + x1;
    int i01 = y1 * 20 + x0, i11 = y1 * 20 + x1;
    float m00 = laMix[i00 >> 2][i00 & 3], m10 = laMix[i10 >> 2][i10 & 3];
    float m01 = laMix[i01 >> 2][i01 & 3], m11 = laMix[i11 >> 2][i11 & 3];
    return lerp(lerp(m00, m10, mt.x), lerp(m01, m11, mt.x), mt.y);
}
#ifdef SEAM_STRETCH
// film_hue2_seam_stretch: AcidMixAt's analytic gradient, d(mix)/d(uv)
float2 AcidMixGrad(float2 uv) {
    const float MW = 20.0, MH = 12.0;
    float2 mf = float2(uv.x * MW - 0.5, uv.y * MH - 0.5);
    float2 mi = floor(mf), t = mf - mi;
    float2 s = t * t * (3.0 - 2.0 * t), d = 6.0 * t * (1.0 - t);
    int x0 = (int)clamp(mi.x, 0.0, MW - 1.0), x1 = (int)clamp(mi.x + 1.0, 0.0, MW - 1.0);
    int y0 = (int)clamp(mi.y, 0.0, MH - 1.0), y1 = (int)clamp(mi.y + 1.0, 0.0, MH - 1.0);
    int i00 = y0 * 20 + x0, i10 = y0 * 20 + x1;
    int i01 = y1 * 20 + x0, i11 = y1 * 20 + x1;
    float m00 = laMix[i00 >> 2][i00 & 3], m10 = laMix[i10 >> 2][i10 & 3];
    float m01 = laMix[i01 >> 2][i01 & 3], m11 = laMix[i11 >> 2][i11 & 3];
    return float2(MW * d.x * lerp(m10 - m00, m11 - m01, s.y),
                  MH * d.y * (lerp(m01, m11, s.x) - lerp(m00, m10, s.x)));
}
#endif

// ---- DIFFRACTION (item W) ------------------------------------------------
// The user, on an in-focus ring: "a thin hair on a film will never cause pure
// darkness because the light bends around it; it's not out of focus per se."
// A feature narrower than the point spread has light filled in from all round
// it, so it cannot reach full darkness however well it is focused -- that is
// diffraction, not defocus, and it is why a hair on film is grey. `size` and
// `kp` are both in p-space: one point spread across gives 63% of full
// darkness, two gives 98%, and anything several spreads wide is untouched.
// Only ever REMOVES darkness from small things, so the black under a big mass
// is exactly the black it was.
float AcidDiffract(float size, float kp, float amt) {
    [branch] if (amt <= 0.0 || kp <= 0.0) return 1.0;
    float x = size / kp;
    return lerp(1.0, 1.0 - exp(-x * x), saturate(amt));
}

// Procedural bubble swarm. A jittered cellular layer of round droplets with a
// wide (squared-hash) size range: hundreds of bubbles of every size for ~20
// hashes per pixel, where the same thing in metaballs would cost hundreds of
// loop iterations. Returns the signed distance to the nearest droplet in
// p-space units (negative inside).
// `nrm` comes back as the unit vector from the winning droplet's centre
// toward the pixel and `rad` as that droplet's radius (both in p-space), so
// the caller can shade a droplet as a lens (soft edge, offset highlight)
// instead of a flat disc. Free: the loop already has both.
float AcidSwarm(float2 p, float scale, float density, float clumpAmt,
                float rmin, float rmax, out float2 nrm, out float rad) {
    float2 pc = p * scale;
    float2 base = floor(pc);
    float d = 1e9;
    float2 bestV = float2(1.0, 0.0);
    float  bestR = 1e-3;
    [unroll] for (int oy = -1; oy <= 1; oy++) {
        [unroll] for (int ox = -1; ox <= 1; ox++) {
            float2 c = base + float2(ox, oy);
            // Clustering is decided PER CELL, from a low-frequency value noise
            // of the cell index. It must not depend on the shading pixel: an
            // earlier version modulated `density` by a per-pixel fbm, so the
            // presence test could flip part-way across one droplet and slice a
            // straight-edged wedge out of it (the dark chips in the oil).
            float  dens = density * lerp(1.0,
                          smoothstep(0.34, 0.62, AcidVNoise(c * 0.17)) * 1.8, clumpAmt);
            float  h  = AcidHash21(c * 1.37 + 0.11);
            // cube on the radius hash: many tiny droplets, a few large ones
            float  hr = AcidHash21(c * 2.71 + 5.3);
            // empty cells get a negative radius, so their distance never wins
            float  r  = (h < dens) ? lerp(rmin, rmax, hr * hr * hr) : -2.0;
            float2 j  = float2(AcidHash21(c + 3.71), AcidHash21(c + 9.13));
            float2 v  = pc - (c + 0.18 + 0.64 * j);
            float  dd = length(v) - r;
            if (dd < d) { d = dd; bestV = v; bestR = max(r, 1e-3); }
        }
    }
    nrm = bestV / max(length(bestV), 1e-5);
    rad = bestR / max(scale, 1e-3);
    return d / max(scale, 1e-3);
}
#endif

)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
#if defined(INK) || defined(LIQUID_ACID)
// ==========================================================================
// SHARED "ink in water" block — compiled into the INK PSO (style=ink) and
// into the LIQUID_ACID PSO (used when [liquid_acid] ink_mode=water, so the
// oil floats on ink-in-water instead of on posterised bands). One
// implementation, two looks; the fluid PSO has neither macro and therefore
// none of this code.
//
// The dye field stops being a colour and becomes an optical DEPTH. Refs:
// reference/shots/photos/ink-in-water-ref-*.jpg.
// ==========================================================================
cbuffer InkCB : register(b2) {
    float4 ikP0;        // x density k, y chroma, z edgeStrength, w edgeScale
    float4 ikP1;        // x edgeLo,   y edgeHi,  z inverted,     w vignette
    float4 ikP2;        // x parallax, y parallaxScale, z parallaxDrift, w time
    float4 ikP3;        // x coreKnee, y hdrCore, z motionLo, w motionHi
    float4 ikP4;        // x motionOpacity, y veilFloor, z tintMidDip, w tonemap
    float4 ikPaper;     // paper / background colour
    float4 ikTintThin;  // inverted: light through a thin veil
    float4 ikTintThick; // inverted: light out of an opaque core
    float4 ikP5;        // x whiteScRGB, y blackScRGB, z toneKnee, w toneChroma
    float4 ikP6;        // x tintHueBlend, y peakScRGB, z -, w -
};

// HSV for the duotone pair blend. Named apart from the LIQUID_ACID pair so
// both can live in the acid PSO.
float3 InkRgb2Hsv(float3 c) {
    float mx = max(c.r, max(c.g, c.b));
    float mn = min(c.r, min(c.g, c.b));
    float d = mx - mn;
    float h = 0.0;
    if (d > 1e-6) {
        if (mx == c.r)      h = frac(((c.g - c.b) / d) / 6.0);
        else if (mx == c.g) h = (((c.b - c.r) / d) + 2.0) / 6.0;
        else                h = (((c.r - c.g) / d) + 4.0) / 6.0;
    }
    return float3(h, (mx > 1e-6) ? d / mx : 0.0, mx);
}
float3 InkHsv2Rgb(float3 c) {
    float3 p = abs(frac(c.x + float3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return c.z * lerp(float3(1.0, 1.0, 1.0), saturate(p - 1.0), c.y);
}

// Ink "thickness" carried by one dye texel. Max channel, not luma: a coloured
// ink must absorb by its strongest component or a pure blue drop reads thinner
// than a grey one of the same concentration.
// Low-res (64x36) copy of the velocity field, the same texture the Liquid Acid
// blob advection reads back. Used only to tell MOVING ink from ink that is
// just sitting there.
Texture2D<float4> VelLow : register(t3);

float InkDensity(float3 c) { return max(c.r, max(c.g, c.b)); }

// Motion gate. A drop's dye stamp always leaves some dye at the injection
// point that never got any momentum; in inverted + HDR that stationary patch
// is the brightest thing on screen, which is not what a drop entering water
// looks like. Gate the HDR lift on |velocity| so only ink that is actually
// being carried gets to go above SDR white. motionHi <= motionLo disables it.
float InkMotion(float2 uv) {
    if (ikP3.w <= ikP3.z) return 1.0;
    float2 v = VelLow.SampleLevel(linearClamp, uv, 0).xy;
    return smoothstep(ikP3.z, ikP3.w, length(v));
}

// Edge darkening. A sheet of ink seen EDGE-ON is a longer optical path, which
// is why every fold in the references has a dark outline; in 2D the stand-in
// is the density gradient. 4 taps at edge_scale screen texels.
float InkEdge(float2 uv, float2 texel, float scale) {
    float2 ex = float2(texel.x * scale, 0.0), ey = float2(0.0, texel.y * scale);
    float L = InkDensity(Dye.SampleLevel(linearClamp, uv - ex, 0).rgb);
    float R = InkDensity(Dye.SampleLevel(linearClamp, uv + ex, 0).rgb);
    float T = InkDensity(Dye.SampleLevel(linearClamp, uv - ey, 0).rgb);
    float B = InkDensity(Dye.SampleLevel(linearClamp, uv + ey, 0).rgb);
    return smoothstep(ikP1.x, max(ikP1.y, ikP1.x + 1e-4),
                      0.5 * length(float2(R - L, B - T)));
}

// C   = RAW dye (pre-emboss, pre-CSS chain).
// pos = SV_Position.xy, for the paper vignette.
// Returns the display colour in the SAME sRGB-encoded space the fluid look's
// C is in before SRGBToLinear, plus the level that drives the HDR gain.
float3 InkWater(float3 C, float2 uv, float2 texel, float2 pos, out float hdrM) {
    float  d = InkDensity(C);
    // Unit-max chroma of this parcel. Dye-free pixels are exactly (0,0,0), so
    // the divide must be guarded or the NaN propagates through every lerp
    // below and blacks the pixel out (the same trap the acid atan2 fell into).
    float3 chroma = (d > 1e-4) ? C / d : float3(1.0, 1.0, 1.0);
    float  thick = d * (1.0 + ikP0.z * InkEdge(uv, texel, ikP0.w));
    // Same motion gate, on the optical path itself: ink that is not being
    // carried anywhere reads thinner. This is what demotes the stationary
    // entry patch from an opaque white disc to a faint mist, without touching
    // the plume, whose interior is still swirling.
    if (ikP4.x > 0.001) thick *= lerp(1.0 - saturate(ikP4.x), 1.0, InkMotion(uv));
    if (ikP2.x > 0.001) {
        // Cheap second layer: the SAME dye sampled at a slightly different
        // scale and drift, blurred, added to the path length. Reads as ink
        // hanging at another depth. A real second field would double the
        // dominant dye-res passes; this is 4 taps.
        float2 dr = float2(ikP2.z * ikP2.w, ikP2.z * ikP2.w * 0.6);
        float2 duv = (uv - 0.5) * ikP2.y + 0.5 + dr;
        float2 e = texel * 2.0;
        float d2 = 0.25 * (InkDensity(Dye.SampleLevel(linearClamp, duv + float2( e.x,  e.y), 0).rgb)
                         + InkDensity(Dye.SampleLevel(linearClamp, duv + float2(-e.x,  e.y), 0).rgb)
                         + InkDensity(Dye.SampleLevel(linearClamp, duv + float2( e.x, -e.y), 0).rgb)
                         + InkDensity(Dye.SampleLevel(linearClamp, duv + float2(-e.x, -e.y), 0).rgb));
        thick += ikP2.x * d2;
    }
    // Coloured absorption: the channel the ink is made of absorbs least, so
    // the light that gets through is the ink's own colour. chroma = 0 gives
    // a = 1 on every channel = a neutral (black) ink.
    float3 a = 1.0 + ikP0.y * (1.0 - chroma);
    float3 T = exp(-ikP0.x * thick * a);
    if (ikP1.z < 0.5) {
        // ---- PAPER: backlit white ground, ink subtracts from it ----------
        float2 sp = pos * texel - 0.5;
        float  v = 1.0 - ikP1.w * smoothstep(0.35, 1.0, length(sp) * 1.6);
        hdrM = 0.0;                       // a white field never gets HDR gain
        return ikPaper.rgb * T * v;
    }
    // ---- INVERTED: pale ink on black (the OLED-friendly one) -------------
    float  op = 1.0 - exp(-ikP0.x * thick);
    // veil floor: faint dye (old, diffused wash) goes to the background
    // instead of a semi-transparent haze
    if (ikP4.y > 0.001) op = saturate((op - ikP4.y) / max(1.0 - ikP4.y, 1e-3));
    float  tk = smoothstep(min(ikP3.x, 0.99), 1.0, op);
    float3 tint = lerp(ikTintThin.rgb, ikTintThick.rgb, tk);
    // tint_hue_blend: THE reason a vermillion ink reads as pale peach. The RGB
    // lerp above walks a COMPLEMENTARY pair straight through grey — teal
    // (0.04,0.63,0.65) to vermillion (0.90,0.27,0.15) crosses (0.47,0.45,0.40),
    // a dead beige — and `tk` spends most of a plume in that middle, so the
    // beige is most of what the look actually shows. Restore the blend's
    // SATURATION and VALUE to the anchors' own (lerped), keeping the RGB-lerp
    // HUE: the duotone stays a duotone (no third colour is invented, which is
    // what walking the hue wheel does — teal->vermillion the short way is a
    // trip through green and yellow) but nothing in it is ever washed out.
    if (ikP6.x > 0.001) {
        float3 hsv = InkRgb2Hsv(tint);
        float3 a = InkRgb2Hsv(ikTintThin.rgb), b = InkRgb2Hsv(ikTintThick.rgb);
        float w = saturate(ikP6.x);
        hsv.y = lerp(hsv.y, lerp(a.y, b.y, tk), w);
        hsv.z = lerp(hsv.z, lerp(a.z, b.z, tk), w);
        tint = InkHsv2Rgb(hsv);
    }
    // Complementary tints blend to grey-brown at the midpoint (the flat "mud"
    // on smooth mid-density plumes). Real ink goes DARK where it is neither
    // thin nor saturated, so dip the blend toward black around the midpoint.
    // Done on the opacity, so the dip shows the BACKGROUND colour (paper_color),
    // which need not be black.
    op *= 1.0 - ikP4.z * pow(4.0 * tk * (1.0 - tk), 2.0);
    hdrM = d * ikP3.y * InkMotion(uv);    // hot cores expand, veils stay SDR
    return lerp(ikPaper.rgb, tint * lerp(float3(1.0, 1.0, 1.0), chroma,
                                         saturate(ikP0.y)), op);
}
#endif
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

// The reference wrote dye values into an sRGB canvas: the display decoded them
// through the sRGB curve. Our swap chain is LINEAR scRGB, so we must apply
// that decode ourselves or the tone curve (and color mixing) is completely
// different. sdrScale then maps 1.0 to the user's SDR white level in HDR mode.
float3 SRGBToLinear(float3 c) {
    float3 lo = c / 12.92;
    float3 hi = pow((c + 0.055) / 1.055, 2.4);
    return lerp(lo, hi, step(0.04045, c));
}

// CSS/SVG colour-matrix primitives (W3C Filter Effects), Rec.709 luma.
float3 CssSaturate(float3 c, float s) {
    return float3(
        dot(float3(0.213 + 0.787 * s, 0.715 - 0.715 * s, 0.072 - 0.072 * s), c),
        dot(float3(0.213 - 0.213 * s, 0.715 + 0.285 * s, 0.072 - 0.072 * s), c),
        dot(float3(0.213 - 0.213 * s, 0.715 - 0.715 * s, 0.072 + 0.928 * s), c));
}
float3 CssHueRotate(float3 c, float deg) {
    float a = radians(deg);
    float co = cos(a), si = sin(a);
    return float3(
        dot(float3(0.213 + 0.787 * co - 0.213 * si, 0.715 - 0.715 * co - 0.715 * si, 0.072 - 0.072 * co + 0.787 * si), c),
        dot(float3(0.213 - 0.213 * co + 0.143 * si, 0.715 + 0.285 * co + 0.140 * si, 0.072 - 0.072 * co - 0.283 * si), c),
        dot(float3(0.213 - 0.213 * co - 0.787 * si, 0.715 - 0.715 * co + 0.715 * si, 0.072 + 0.928 * co + 0.072 * si), c));
}
float3 CssContrast(float3 c, float k) { return c * k + 0.5 * (1.0 - k); }

#if defined(LIQUID_ACID) && defined(OIL_HEIGHT)
// Oil3D Stage A: one kernel's share of the CURVATURE TENSOR K = u * Hess(w)
// (each kernel's Hessian faded by its own u, so it meets its support circle
// with zero slope: C1), and K's exact gradient (dK/dx, dK/dy, each as
// xx, xy, yy). For w = u^3 with u = 1 - q^T D q / s^2, g = D q:
//   K = A g g^T + Bc D,   A = 24 u^2 / s^4,   Bc = -6 u^3 / s^2,
//   grad A = -96 u g / s^6, grad Bc = 36 u^2 g / s^4, d(g_i)/dx_k = D_ik.
void OilHKernel(float u, float is2, float2 g, float3 D, float wt,
                inout float3 K, inout float3 Kx, inout float3 Ky) {
    float  A  = 24.0 * u * u * is2 * is2;
    float  Bc = -6.0 * u * u * u * is2;
    float2 gA = (-96.0 * u * is2 * is2 * is2) * g;
    float2 gB = (36.0 * u * u * is2 * is2) * g;
    float3 gg = float3(g.x * g.x, g.x * g.y, g.y * g.y);
    K  += wt * (A * gg + Bc * D);
    Kx += wt * (gA.x * gg + A * float3(2.0 * g.x * D.x, D.x * g.y + g.x * D.y, 2.0 * g.y * D.y) + gB.x * D);
    Ky += wt * (gA.y * gg + A * float3(2.0 * g.x * D.y, D.y * g.y + g.x * D.z, 2.0 * g.y * D.z) + gB.y * D);
}
// The same for a RADIAL profile (the hollow ring's band): K = a P + b (I - P),
// P = un un^T, with a = su f'', b = su f'/r and their r-derivatives da, db.
void OilHRadial(float a, float b, float da, float db, float2 un, float ir, float wt,
                inout float3 K, inout float3 Kx, inout float3 Ky) {
    float  ab = a - b, dab = da - db;
    float3 P  = float3(un.x * un.x, un.x * un.y, un.y * un.y);
    K += wt * (float3(b, 0.0, b) + ab * P);
    // d/dx_k (b I + (a-b) P)_ij = (db d_ij + (da-db) P_ij) un_k
    //                             + (a-b) (Q_ki un_j + un_i Q_kj) / r,  Q = I - P
    float3 rad = float3(db, 0.0, db) + dab * P;
    float  qxx = 1.0 - P.x, qxy = -P.y, qyy = 1.0 - P.z;
    Kx += wt * (rad * un.x + (ab * ir) * float3(2.0 * qxx * un.x, qxx * un.y + un.x * qxy, 2.0 * qxy * un.y));
    Ky += wt * (rad * un.y + (ab * ir) * float3(2.0 * qxy * un.x, qxy * un.y + un.x * qyy, 2.0 * qyy * un.y));
}
// h and the slope of lambda*h from a field value, its gradient and Hessian and
// the curvature tensor K with its gradient (see the Stage A block in PSMain).
void OilHeightEval(float thresh, float fld, float2 grd, float3 hss, float3 kK, float3 kX, float3 kY,
                   out float h, out float d, out float2 slope) {
    const float lam = max(LA_OIL_H_LAMBDA, 1e-5);
    const float tw  = max(LA_OIL_H_TOE, 1e-6);
    float  dlt = thresh - fld;
    float  g0  = thresh / (12.0 * lam);
    float  g02 = g0 * g0;
    float  ft  = g02 / max(thresh * thresh, 1e-8);
    float  le  = g02 / max(thresh, 1e-4);
    float  km  = 0.5 * (kK.x + kK.z);
    float  kr  = sqrt(0.5 * (kK.x * kK.x + kK.z * kK.z) + kK.y * kK.y + le * le);
    float  kap = 0.5 * (km + kr);
    float  dp  = max(dlt, 0.0);
    float  Q   = dot(grd, grd) + 2.0 * kap * dp + ft * dlt * dlt;
    float  iq  = rsqrt(Q);
    d = dlt * iq;
    // grad d = iq grad(delta) - delta/2 Q^-3/2 grad Q,
    // grad Q = 2 Hess(field) grad + 2 kap grad(delta) [delta > 0]
    //        + 2 delta+ grad(kap) + 2 ft delta grad(delta)
    float2 Hg  = float2(hss.x * grd.x + hss.y * grd.y, hss.y * grd.x + hss.z * grd.y);
    // kap's own gradient comes through the third derivatives (kX/kY):
    // with it the normal is the exact slope of h, not an approximation.
    float2 gkm = 0.5 * float2(kX.x + kX.z, kY.x + kY.z);
    float2 gkr = (kK.x * float2(kX.x, kY.x) + kK.z * float2(kX.z, kY.z)
                  + 2.0 * kK.y * float2(kX.y, kY.y)) / (2.0 * kr);
    float2 gQ  = 2.0 * Hg - ((dlt > 0.0) ? 2.0 * kap : 0.0) * grd
               + dp * (gkm + gkr) - (2.0 * ft * dlt) * grd;
    float2 gd  = -iq * grd - (0.5 * dlt * iq * iq * iq) * gQ;
    float  z   = d / tw;
    float  zc  = min(z, 30.0);
    float  sp  = tw * (log(1.0 + exp(zc)) + (z - zc));   // softplus, exact past 30
    float  sg  = 1.0 / (1.0 + exp(-zc));
    float  ex  = exp(-sp / lam);
    h = 1.0 - ex;
    slope = (ex * sg) * gd;
}
#ifndef OIL_SURFACE
// The surface pre-pass's target (Oil3D split), bound through the display root
// signature's t3 table (its 2nd descriptor; the 3rd, t6, is a null SRV): no
// new root DWORD. P1: ONE R16G16F target with the only two channels a reader
// samples (it was two RGBA16F targets, 12 of their 16 bytes never read):
//   OilSurf0 = (wax coverage, h_oil (blobs only))
// A later reader of h, n or the droplet weights adds its channel back here.
Texture2D<float2> OilSurf0 : register(t5);
// Oil3D C2: the PADDED MASS COVERAGE (quarter res, its own pass, PSCoverage):
// the masses only, over the frame plus a pad of D cot(elev) + the penumbra,
// so a mass outside the frame still shadows the column inside it. Indexed by
// SCREEN uv through COV_A/COV_B (OilMassCovAt), like every surface buffer.
Texture2D<float>  OilMassCov : register(t7);
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
// The occluder both shadows read: the MASSES only. The wax coverage (surf0.x,
// AA'd at the isoline) gated by h_oil, the blob field's own height, which no
// droplet is in: at the contact line h_oil is already ~0.04 (the 1 px
// softplus toe over lambda), inside a punched droplet or ring it is ~0. So the
// occluder starts AT the isoline (h_oil's own ramp would start it ~5 px
// in), and the droplets and rings, punched into the film rather than
// standing on it, cast nothing: no copy of one can print.
// A lit hole inside a mass has wax 0, so it is a receiver, not an occluder.
// The pre-pass buffers are indexed by SCREEN pixel: every lookup starts from
// i.pos, never from uv, which the refraction has already bent near each edge
// by the time the lamp terms run (sampling at it dilated the masses by
// ~10-30 px and ate the contact shadow's first pixels).
float2 OilSurfUV(float2 px) {
    uint w, h;
    OilSurf0.GetDimensions(w, h);
    return px / float2(w, h);
}
float OilOccluder(float2 uv) {
    float2 s = OilSurf0.SampleLevel(linearClamp, uv, 0);
    return s.x * smoothstep(0.005, 0.04, s.y);
}
// Oil3D C1: the lamp's shadow of the oil on the column/floor tank_depth below.
// The receiver at uv sees the lamp through the oil at uv + offset (D cot(elev)
// toward the light); the lamp's angular size spreads that into a disc of
// radius D tan(size/2) (PCSS with one receiver depth: no blocker search).
// 24 bilinear taps of the masses' coverage on a Vogel spiral, the
// spiral rotated per PIXEL by interleaved gradient noise: no fixed grid, no
// blocky penumbra, and static in time (no shimmer). Returns lit fraction.
// Read through the film at <= 5 % (SH_UNDER's cap): a dimming zone only.
float OilLampShadow(float2 uv, float2 px) {
    float2 c   = uv + float2(LA_SH_OFF_X, LA_SH_OFF_Y);
    float  r   = max(LA_PEN_R, 1e-5);
    float  rot = 6.2831853 * frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715))));
    float  occ = 0.0;
    [unroll] for (int k = 0; k < 24; k++) {
        float  rr = sqrt((k + 0.5) / 24.0) * r;
        float  a  = k * 2.39996323 + rot;
        float2 o  = float2(cos(a), sin(a)) * rr;
        occ += OilOccluder(c + float2(o.x / LA_ASPECT, o.y));
    }
    return 1.0 - occ / 24.0;
}
// Oil3D C1 fix: shadow_on_film as a CONTACT shadow. The film is at z = 0 and
// the oil floats ON it, so the oil's shadow on the film travels only the
// wax's own height, never the tank depth (the old single lookup displaced it
// by D cot(elev): a blurred dark COPY of every mass ~290 px up-left). The
// receiver looks TOWARD the light (ld, unit, p-space) over [0, CS_REACH]:
// 12 midpoint taps weighted by the bell 6u(1-u), so a half-plane mass at
// distance e gives 1 - smoothstep(0, reach, e) -- darkest at the contact line,
// flat there (no crease), half at reach/2 = lambda cot(elev), gone by reach:
// one soft one-sided ramp on the mass's FAR side, nothing on its lit side.
// Tap spacing (reach/12, ~1.8 px) is under the occluder's own ramp, so no
// steps and no noise.
float OilContactShadow(float2 uv, float2 ld) {
    float occ = 0.0;
    [unroll] for (int k = 0; k < 12; k++) {
        float  u  = (k + 0.5) / 12.0;
        float2 o  = ld * (u * LA_CS_REACH);
        float2 tc = uv + float2(o.x / LA_ASPECT, o.y);
        occ += u * (1.0 - u) * OilOccluder(tc);
    }
    return saturate(occ * (6.0 / 12.0));
}
// ---- Oil3D C2 -----------------------------------------------------------
// The padded mass coverage at a SCREEN uv (one that came from OilSurfUV(i.pos),
// never the refraction-bent uv): COV_A/COV_B put the frame inside the pad.
float OilMassCovAt(float2 suv) {
    return OilMassCov.SampleLevel(linearClamp, suv * float2(LA_COV_AX, LA_COV_AY)
                                               + float2(LA_COV_BX, LA_COV_BY), 0);
}
// OilLampShadow's twin on the padded coverage, used while the column is lit
// (water_scatter > 0): the same 24-tap PCSS disc, so a mass outside the frame
// darkens the under-layer too, and one R16 tap instead of two RGBA16 ones.
float OilLampShadowCov(float2 uv, float2 px) {
    float2 c   = uv + float2(LA_SH_OFF_X, LA_SH_OFF_Y);
    float  r   = max(LA_PEN_R, 1e-5);
    float  rot = 6.2831853 * frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715))));
    float  occ = 0.0;
    [unroll] for (int k = 0; k < 24; k++) {
        float  rr = sqrt((k + 0.5) / 24.0) * r;
        float  a  = k * 2.39996323 + rot;
        float2 o  = float2(cos(a), sin(a)) * rr;
        occ += OilMassCovAt(c + float2(o.x / LA_ASPECT, o.y));
    }
    return 1.0 - occ / 24.0;
}
#ifdef OIL_MURK
// ---- Oil3D W1: MURK -- the column's scatterers get a DENSITY ---------------
// (the OIL_MURK variants only) OilMurk = the baked density d, mean exactly 1
// over the tile (CPU: BakeMurk: four noise layers, shaped and multiplied,
// then divided by their mean), tiled by the WRAP sampler. laMurk[0] = screen
// uv -> tile (scale x with the aspect, scale y, drift x, drift y), laMurk[1] =
// (tiles per unit of depth t, drift z, the self-shadow k = murk_shadow x
// WS_TAU, murk_light). A uniform d = 1 is today's column to the arithmetic.
Texture3D<float> OilMurk : register(t8);
SamplerState linearWrap : register(s1);
float OilMurkAt(float2 suv, float t) {
    float3 q = float3(suv * laMurk[0].xy + laMurk[0].zw, t * laMurk[1].x + laMurk[1].y);
#ifdef OIL_MURKK
    // post-night T3, director_water: the colour director's murk level k = laMote[2].y over the one bake (linear
    // in water_murk, its 0 clamp out of reach below water_murk x cloud share 1), d' = 1 + k (d - 1): mean 1, no re-bake
    return 1.0 + laMote[2].y * (OilMurk.SampleLevel(linearWrap, q, 0) - 1.0);
#else
    return OilMurk.SampleLevel(linearWrap, q, 0);
#endif
}
#endif
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
#ifdef OIL_GRAIN
// ---- Oil3D W3: FINE SILT -- the particles the murk is made of -------------
// (the OIL_GRAIN variants only; always with OIL_MURK) User 2026-11-02: "the
// algorithm should simulate fine particles. It's simulating oil blobs." The
// bake above stays the slow large-scale density; the scatterers themselves
// are dense FINE particles, evaluated per PIXEL here: per depth slice a
// lattice of cells laGrain[0].y px apart (x 1 / 1.45 / 2 for the slices at
// t = 0.15 / 0.45 / 0.8 of the column: deeper = bigger and softer), one
// particle per cell at a hashed spot, a soft Gaussian of sigma 0.42 cell
// (dense and overlapping: a grain, not specks; the user rejected resolved
// bright specks and bokeh discs) with a hashed brightness b = h^2 (many
// faint, a few brighter). It lives: the lattice drifts with the murk at the
// slice's own rate (laGrain[1].xy / .zw / [2].xy, the parallax), each
// particle wobbles round its spot (two sines, 0.6..1.6 rad/s, +-0.16 cell:
// Brownian churn) and tumbles (its brightness breathing +-35 % at 0.3..1.1
// rad/s): drifting and churning, never static, never per-frame noise. Cells
// hash mod 1024 and the offsets wrap at 1024 cells: seamless.
uint OilGrainHash(uint2 c, uint s) {
    uint h = (c.x * 0x8DA6B343u) ^ (c.y * 0xD8163841u) ^ (s * 0xCB1AB31Fu);
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    return h;
}
// one slice at pixel px: n / E[n] - 1 (mean 0; E[n] = E[b] 2 pi sigma^2 =
// (1/3) 2 pi 0.42^2 = 0.3695 for one particle per cell)
float OilGrainSlice(float2 px, float cs, float2 off, uint s, float tm) {
    float2 q  = px / cs + off;
    float2 ci = floor(q);
    float2 f  = q - ci;
    float  n  = 0.0;
    [unroll] for (int j = -1; j <= 1; j++) {
        [unroll] for (int k = -1; k <= 1; k++) {
            uint2  cc = (uint2)((int2)ci + int2(k, j)) & 1023u;
            uint   h1 = OilGrainHash(cc, s), h2 = OilGrainHash(cc, s + 7u);
            float4 r  = float4(h1 & 0xFFFFu, h1 >> 16, h2 & 0xFFFFu, h2 >> 16) * (1.0 / 65535.0);
            float2 w  = 0.6 + float2(r.w, frac(r.w * 7.31 + r.x));
            float2 ps = float2(k, j) + 0.2 + 0.6 * r.xy
                      + 0.16 * sin(w * tm + 6.2831853 * float2(r.y, r.z));
            float  b  = r.z * r.z * (1.0 + 0.35 * sin((0.3 + 0.8 * r.w) * tm + 6.2831853 * r.x));
            float2 d  = f - ps;
            n += b * exp(-dot(d, d) * 2.8345);   // 1 / (2 x 0.42^2)
        }
    }
    return n * (1.0 / 0.3695) - 1.0;
}
// The silt's grain under the film pixel: gC = the slices weighted by the
// water above them (e^-(tau + haze) t) and LIT only where no mass shadows
// their depth (one coverage tap at the surface point + t SH_OFF, the
// column's geometry) = the single-scatter grain; gH = the same unshadowed =
// the haze's multiply-scattered grain. Both mean 0.
void OilGrainField(float2 suv, float2 px, out float gC, out float gH) {
    const float3 kT = float3(0.15, 0.45, 0.80), kG = float3(1.0, 1.45, 2.0);
    const float  cs = laGrain[0].y, tm = laGrain[2].w;
    const float2 so = float2(LA_SH_OFF_X, LA_SH_OFF_Y);
    float sc = 0.0, sh = 0.0, sw = 0.0;
    [unroll] for (int s = 0; s < 3; s++) {
        [branch] if (s < (int)laGrain[2].z) {
            float2 off = (s == 0) ? laGrain[1].xy : ((s == 1) ? laGrain[1].zw : laGrain[2].xy);
            float  g   = OilGrainSlice(px, cs * kG[s], off, 3u + 101u * (uint)s, tm);
            float  w   = exp(-(LA_WS_TAU + laGrain[0].z) * kT[s]);
            sc += w * (1.0 - OilMassCovAt(suv + kT[s] * so)) * g;
            sh += w * g;
            sw += w;
        }
    }
    gC = sc / max(sw, 1e-6);
    gH = sh / max(sw, 1e-6);
}
#endif
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
#ifdef OIL_MOTES
// ---- Oil3D C3: MOTES -- the column's RESOLVED particles --------------------
// (the OIL_MOTES variants only) CPU: StepMotes (screen uv, depth t in [0,1],
// riding the murk's drift, born where it is dense) binned into a 160x90 grid
// in the droplet buffers' TAIL: AcidDrops[MB + 2k] = (uv, t, radius px in
// focus), AcidDrops[MB + 2k + 1].x = the gain (spawn fade x glint flash);
// DropCells[CB + cell] = (first, count). laMote[0] = (grid w, grid h, MB, CB),
// laMote[1] = (frame w px, frame h px, the oil's dof_max_px, 1 / the depth of
// full blur), laMote[2].x = mote_bright. The focus is the SURFACE, so a mote's
// disc has radius R = max(r, 0.75 px, dof x(2-x)), x = t / span: the oil's
// own steep shoulder (near = a sharp speck, deep = a soft disc), its light
// thinned by r / R over the disc (not r^2 / R^2: the first sheet's deep motes,
// spread 25x over a 5 px disc, vanished, where a macro lens shows a lit speck
// out of focus as a soft but visible disc), never under 0.75 px (no sub-pixel
// flicker as it moves). Lit unless a MASS shadows its depth (one coverage tap at its
// surface point + t SH_OFF, OilColumnLit's geometry), seen up through e^(-tau t).
// Returns the summed brightening m >= 0 at this pixel (0 almost everywhere).
// The walk is the 2x2 cells around the pixel (the cells covering +- half a
// cell around it), exact while a disc stays under half a cell: R is clamped
// there (7.5 px at 1440p, over the live dof_max_px 5). 3x3 cost +0.75 ms at
// 2000 motes on the live config; most of it was the 9 cell reads per pixel.
float OilMotes(float2 suv) {
    const int  gw = (int)laMote[0].x, gh = (int)laMote[0].y;
    const uint mb = (uint)laMote[0].z, cb = (uint)laMote[0].w;
    const float2 res = laMote[1].xy;
    const float2 so  = float2(LA_SH_OFF_X, LA_SH_OFF_Y);
    const float rMax = 0.5 * res.y / gh - 0.5;
    const int x0 = clamp((int)floor(suv.x * gw - 0.5), 0, gw - 2);
    const int y0 = clamp((int)floor(suv.y * gh - 0.5), 0, gh - 2);
    float m = 0.0;
    [loop]
    for (int yy = y0; yy <= y0 + 1; yy++) {
        [loop]
        for (int xx = x0; xx <= x0 + 1; xx++) {
            uint2 cell = DropCells[cb + yy * gw + xx];
            [loop]
            for (uint k = 0; k < cell.y; k++) {
                uint   ri = mb + 2 * (cell.x + k);
                float4 A  = AcidDrops[ri];
                float  x  = saturate(A.z * laMote[1].w);
                float  r  = max(A.w, 0.75);
                float  R  = min(max(r, laMote[1].z * x * (2.0 - x)), rMax);
                float  dist = length((A.xy - suv) * res);
                [branch] if (dist < R + 0.5) {
                    float e   = saturate(R + 0.5 - dist) * A.w / R;
                    float lit = 1.0 - OilMassCovAt(A.xy + A.z * so);
                    m += e * lit * exp(-LA_WS_TAU * A.z) * AcidDrops[ri + 1].x;
                }
            }
        }
    }
    return m;
}
#endif
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
// THE LIT COLUMN under the film pixel at suv. Ortho, top-down: the view ray
// goes straight down; at depth z = t D the lamp reaches it through the surface
// at suv + t SH_OFF (D cot(elev) toward the lamp, scaled by t), through the
// lamp's disc there (radius t PEN_R: the penumbra grows with depth), and a
// MASS at that surface point blocks it. The light walks z / sin(elev) down
// and the view z back up, so a scatterer at z is weighted by T = e^(-tau t)
// (tau = WS_TAU, the optical depth at the floor). Returns the T-weighted mean
// lit fraction: 16 depths at equal steps of T's CDF, t = -ln(1 - u E) / tau,
// E = 1 - e^-tau (uniform in t as tau -> 0), so every tap weighs the same;
// u stratified (tap k in its own 1/16) and jittered per PIXEL by interleaved
// gradient noise, rotated per tap by the golden ratio so the 16 offsets are
// not one comb shifted as a block (that printed 16 contour steps along each
// shaft); fixed in time, no temporal noise. Each tap is one point of its
// depth's disc (golden-angle spiral, radius stratified). Low frequency by
// construction: at the live depth the disc is 0 px at the surface and ~35 px
// at the floor.
#ifdef OIL_MURK
// murk_light (user 2026-10-31): the mean murk density over the column's 16
// depth taps, set by OilColumnLit; the film's own light is scaled by
// 1 + laMurk[1].w (g_wsMurkMean - 1) (mean 1 over the tile).
static float g_wsMurkMean = 1.0;
#endif
#if defined(OIL_MURK) && defined(OIL_WTINT)
// Oil3D W2: the same taps' CLEAR-water lit fraction (every d = 1), set by
// OilColumnLit: the murk's lit fraction minus this one is the silt's OWN
// light, which takes the silt colour.
static float g_wsLitU = 1.0;
#endif
float OilColumnLit(float2 suv, float2 px) {
    float  j   = frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715))));
    float  tau = LA_WS_TAU;
    float  E   = 1.0 - exp(-tau);
    float2 so  = float2(LA_SH_OFF_X, LA_SH_OFF_Y);
    float  occ = 0.0;
#ifdef OIL_MURK
    float  lit = 0.0, cum = 0.0;
#endif
    [unroll] for (int k = 0; k < 16; k++) {
        float  u  = (k + frac(j + k * 0.6180340)) * (1.0 / 16.0);
        float  t  = (tau > 1e-3) ? -log(1.0 - u * E) / tau : u;
        float  rr = sqrt(frac((k + 0.5) * 0.6180340 + j)) * t * LA_PEN_R;
        float  a  = k * 2.39996323 + 6.2831853 * j;
        float2 o  = t * so + float2(cos(a) / LA_ASPECT, sin(a)) * rr;
#ifdef OIL_MURK
        // the scatterer at depth t straight below the pixel scatters d times
        // the uniform medium's light; the light down to it and the view back
        // up cross the murk ABOVE it (the taps run shallow to deep: their
        // running mean), so a dense clump shades what lies below it:
        // exp(-k t (mean density above - 1)), 1 for a uniform medium
        float d  = OilMurkAt(suv, t);
        float da = (k > 0) ? cum / k : d;
        lit += d * exp(-laMurk[1].z * t * (da - 1.0)) * (1.0 - OilMassCovAt(suv + o));
        cum += d;
#ifdef OIL_WTINT
        occ += OilMassCovAt(suv + o);
#endif
#else
        occ += OilMassCovAt(suv + o);
#endif
    }
#ifdef OIL_MURK
    g_wsMurkMean = cum * (1.0 / 16.0);
#ifdef OIL_WTINT
    g_wsLitU = 1.0 - occ * (1.0 / 16.0);
#endif
    return lit * (1.0 / 16.0);
#else
    return 1.0 - occ * (1.0 / 16.0);
#endif
}
#ifdef OIL_WTINT
// ---- Oil3D W2: WATER COLOUR -- the shadows take the water's colour ---------
// (the OIL_WTINT variants only) User 2026-10-30 ("mostly in the shadows"): in
// coloured water the light that still reaches a SHADOW column came in from the
// lit water around it, the long way through the water, so it carries the
// water's colour W (laWater[0].rgb, linear, white = clear); the open lit water
// keeps the film's colour. The shadow's depth m = 1 - litU (the clear-water
// lit fraction: 0 in open water, ~0.7 in a deep column) leans the pixel from
// F to F x W. The silt's own light (the murk's lit fraction over clear
// water's, negative in the clear pockets) takes the silt's S x W
// (laWater[1].rgb). Both colours are NORMALISED per pixel to the film's own
// luminance, min(Y(F) / Y(F x W), laWater[0].w): only the hue moves, today's
// light stays (C2b's luminance, wsShade); a colour the film's light cannot
// carry (an opposite hue) hits the cap and goes darker, never brighter.
// Returns the LINEAR per-channel gain on the encoded film colour fEnc (apply
// as pow(gain, 1 / 2.2), like wsShade); with W = S = white it is wsShade
// exactly. litU = litM without murk.
float3 OilWaterGain(float3 fEnc, float litM, float litU) {
    const float3 Lw = float3(0.2126, 0.7152, 0.0722);
    float3 F  = pow(max(fEnc, 0.0), 2.2);
    float  Y  = dot(F, Lw);
    float3 W  = laWater[0].rgb, SW = laWater[1].rgb;
    float3 Wn  = W  * min(Y / max(dot(F * W,  Lw), 1e-6), laWater[0].w);
    float3 SWn = SW * min(Y / max(dot(F * SW, Lw), 1e-6), laWater[0].w);
    float  m   = saturate(1.0 - litU);
    return max((1.0 - LA_WS_SHARE * (1.0 - litU)) * (1.0 - m + m * Wn)
               + LA_WS_SHARE * (litM - litU) * SWn, 0.0);
}
// The motes' light (C3) is silt's: it takes S x W, normalised the same way.
float3 OilSiltTint(float3 fEnc) {
    const float3 Lw = float3(0.2126, 0.7152, 0.0722);
    float3 F  = pow(max(fEnc, 0.0), 2.2);
    float3 SW = laWater[1].rgb;
    return SW * min(dot(F, Lw) / max(dot(F * SW, Lw), 1e-6), laWater[0].w);
}
#endif
#ifdef OIL_LAMP2
// ---- Oil3D L2: LAMP 2's occlusion (the OIL_LAMP2 variants) ----------------
// Lamp 2 stands over the tank at its own elevation / azimuth / size; its
// shadows follow lamp 1's LAWS with lamp 2's numbers (L2_* from the CPU), so
// the two lamps' shadows differ only in direction and softness:
//   OilContactShadow2: shadow_on_film's CONTACT shadow, OilContactShadow's
//     12-tap bell toward lamp 2 over L2_CS_REACH (the C1-fix reach with
//     lamp2_elev / lamp2_size): one soft ramp on each mass's far side from
//     lamp 2;
//   OilLamp2Shadow: the UNDER-LAYER, OilLampShadow's PCSS disc offset D
//     cot(e2) toward lamp 2 (L2_OFF), radius D tan(s2/2) (L2_PEN_R), read
//     through the film at the same <= 5 % share (SH_UNDER). L2_UNDER_N taps,
//     not 24: in the frame the term is <= SH_UNDER x lamp 2's share (~1 %), so
//     12 Vogel taps sit far under a code of noise and keep the lamp-2 cost
//     inside the +0.3 ms budget (raise to 24 if the timings allow).
// Both read the masses-only occluder of the SCREEN-space pre-pass (callers
// pass suv = OilSurfUV(i.pos)). Lamp 2 has NO water column (C2b stays lamp
// 1's), so it never reads the padded coverage: its pad and scissor follow
// lamp 1's light and would not cover lamp 2's side. So the under-layer's taps
// that land OUTSIDE the frame count as open water (no mass known there): the
// clamp sampler would repeat the edge row and print a mass leaving the frame
// as a streak D cot(e2) long (~500 px at 45 deg).
#define L2_UNDER_N 12
float OilContactShadow2(float2 uv, float2 ld) {
    float occ = 0.0;
    [unroll] for (int k = 0; k < 12; k++) {
        float  u  = (k + 0.5) / 12.0;
        float2 o  = ld * (u * LA_L2_CS_REACH);
        float2 tc = uv + float2(o.x / LA_ASPECT, o.y);
        occ += u * (1.0 - u) * OilOccluder(tc);
    }
    return saturate(occ * (6.0 / 12.0));
}
float OilLamp2Shadow(float2 uv, float2 px) {
    float2 c   = uv + float2(LA_L2_OFF_X, LA_L2_OFF_Y);
    float  r   = max(LA_L2_PEN_R, 1e-5);
    float  rot = 6.2831853 * frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715))));
    float  occ = 0.0;
    [unroll] for (int k = 0; k < L2_UNDER_N; k++) {
        float  rr = sqrt((k + 0.5) / (float)L2_UNDER_N) * r;
        float  a  = k * 2.39996323 + rot;
        float2 o  = float2(cos(a), sin(a)) * rr;
        float2 tc = c + float2(o.x / LA_ASPECT, o.y);
        float  inF = step(0.0, tc.x) * step(tc.x, 1.0) * step(0.0, tc.y) * step(tc.y, 1.0);
        occ += inF * OilOccluder(tc);
    }
    return 1.0 - occ / (float)L2_UNDER_N;
}
#endif
#endif
#endif
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
#if defined(LIQUID_ACID) && defined(OIL_HEIGHT) && defined(OIL_SURFACE)
// Oil3D C2: the PADDED MASS COVERAGE pass (kDisplaySrc compiled with the
// surface pre-pass's defines, entry PSCoverage; its own quarter-res R16F
// target). The MASSES only: the blob field alone below the isoline, AA'd over
// one texel. Droplets and rings are not in the blob field, so they cast
// nothing (their shadows would blur away in the column anyway), and every
// blob is on the CPU list, on screen or not, so the pad is evaluated for real.
// This texel's SCREEN uv runs past [0, 1]: COV_A/COV_B put the frame inside
// the pad; the write is at this target's own i.pos (no bent uv exists here).
float4 PSCoverage(VSOut i) : SV_Target {
    float2 suv = (i.uv - float2(LA_COV_BX, LA_COV_BY)) / float2(LA_COV_AX, LA_COV_AY);
    float  seam;
    float2 uv = MirrorFold(suv, seam);
    const float aspect = LA_ASPECT;
    float2 pp = float2(uv.x * aspect, uv.y);
    float  fieldB = 0.0;
    int nb = (int)LA_BLOB_COUNT;
    [loop]
    for (int bi = 0; bi < nb; bi++) {
        AcidBlobGPU B = AcidBlobs[bi];
        float2 q = pp - float2(B.a.x * aspect, B.a.y);
        float  e = 1.0 + B.b.w;
        float2 qs;
        [branch] if (B.c.x > 0.0) {
            float2 ax = B.c.yz, pv = float2(-B.c.z, B.c.y);
            qs = float2(dot(q, ax) / (1.0 + B.c.x), dot(q, pv));
        } else {
            qs = float2(q.x, q.y / e);
        }
        float d2  = dot(qs, qs);
        float sup = B.a.z * LA_SUPPORT_SCALE;
        float s2  = sup * sup;
        if (d2 >= s2) continue;
        float u = 1.0 - d2 / s2;
        fieldB += u * u * u * B.a.w;
    }
    float aa = fwidth(fieldB) + 1e-4;
    return (1.0 - smoothstep(LA_THRESHOLD - aa, LA_THRESHOLD + aa, fieldB)).xxxx;
}
#endif

#if defined(LIQUID_ACID) && defined(DROP_LIFE)
// droplet_shadow: the SUMMED solid-hole kernels at a point q (p-space), the
// punch's own dNeg (gate x diffraction x (1 - d^2/S^2)^3, counted per grid
// registration exactly as the field walk counts them), so the isoline test
// below is the visible hole's own; dep / rad = the depth and the kernel
// radius of the strongest one there.
float DropHoleSum(float2 q, float aspect, out float dep, out float rad) {
    dep = 0.5; rad = 0.0;
    float sum = 0.0, best = 0.0;
    const int gw = (int)LA_DROP_GRID_W, gh = (int)LA_DROP_GRID_H;
    const int cx = clamp((int)floor(q.x / aspect * gw), 0, gw - 1);
    const int cy = clamp((int)floor(q.y * gh), 0, gh - 1);
    [loop]
    for (int oy = -1; oy <= 1; oy++) {
        int yy = cy + oy;
        if (yy < 0 || yy >= gh) continue;
        [loop]
        for (int ox = -1; ox <= 1; ox++) {
            int xx = cx + ox;
            if (xx < 0 || xx >= gw) continue;
            uint2 cell = DropCells[yy * gw + xx];
            [loop]
            for (uint di = 0; di < cell.y; di++) {
                float4 D = AcidDrops[cell.x + di];
                if (D.z >= 0.0) continue;                    // holes only
                float dqz = floor(D.w * 0.25);
                float dwr = D.w - 4.0 * dqz;
                if (dwr > 1.5 || dwr <= 0.002) continue;     // rings keep their own film
                float R  = -D.z;
                float ds = R * LA_DROP_SUPPORT;
                float2 dq = q - float2(D.x * aspect, D.y);
                float dd2 = dot(dq, dq);
                if (dd2 >= ds * ds) continue;
                float du = 1.0 - dd2 / (ds * ds);
                float w  = du * du * du * dwr * AcidDiffract(R, LA_DIFFR_SCALE, LA_DIFFRACTION);
                sum += w;
                if (w > best) { best = w; rad = R; dep = (dqz > 0.5) ? (dqz - 1.0) * (1.0 / 254.0) : 0.5; }
            }
        }
    }
    return sum;
}
#endif

#if defined(LIQUID_ACID) && defined(OIL_HEIGHT) && defined(OIL_SURFACE)
// The surface PRE-PASS is this same PSMain compiled with OIL_SURFACE: it stops
// right after the height block and writes ONE R16G16F target (P1: wax, h_oil;
// .zw are dropped by the format). Every colour term is dead code there, so
// the compiler keeps only the field.
#endif
float4 PSMain(VSOut i) : SV_Target {
    // Mirroring first: everything downstream reads the folded coordinate.
    // NOT mirrored, by design: i.pos.xy (the paper vignette and the film
    // grain stay screen-space effects, as they would be on real film), and
    // the one-pixel fwidth() spike exactly on a seam.
    float mirrorSeam;
    float2 uv = MirrorFold(i.uv, mirrorSeam);
    // The [post] camera pass reads this pixel's CIRCLE OF CONFUSION out of the
    // render target's ALPHA (px at 1440p). Only the acid look can produce one
    // -- it is the only look with an isoline and a per-droplet depth -- so
    // every other path leaves the 1.0 the target has always carried, and the
    // post pass is told (dof_max_px = 0) not to read it.
    float outCoc = 1.0;
#ifdef LIQUID_ACID
    // ---- oil metaball field (evaluated first: it refracts the ink sample) ----
    const float aspect = LA_ASPECT;
    float2 pp = float2(uv.x * aspect, uv.y);
    float  field = 0.0;
    float2 grad  = float2(0.0, 0.0);
#ifdef OIL_HEIGHT
    // Oil3D Stage A (oil_height): the field's HESSIAN (xx, xy, yy), summed
    // analytically beside field/grad from the same C2 kernels. Only the
    // OIL_HEIGHT PSO variant has this; the default acid PSO never sees it.
    float3 hessF = float3(0.0, 0.0, 0.0);
    // kapK = sum of u * Hess(kernel): the body's CURVATURE TENSOR, each
    // kernel's share faded by its own u so it reaches its support circle with
    // zero SLOPE (C1; the bare Hessian is only C0 there and printed a crease
    // in h at every support circle). kapX/kapY = its exact gradient.
    float3 kapK  = float3(0.0, 0.0, 0.0);
    float3 kapX  = float3(0.0, 0.0, 0.0), kapY = float3(0.0, 0.0, 0.0);
#endif
    float  colW = 0.0;
    float3 colSum = float3(0.0, 0.0, 0.0);
    // How deep inside a hollow droplet's interior this pixel is; 0 unless
    // droplet_ring_frac is on.
    float  ringIn = 0.0;
    // brief AG-b: the dye IDENTITY of the mass under this pixel, soft-maxed
    // (w^4, the oil colour's trick) over the HOLE blobs' hashes in c.w, so two
    // merging holes blend smoothly. Accumulated only while dye_lum_vary or
    // dye_hue_vary is on; otherwise the loop below is the old loop.
    float  idS = 0.0, idW = 0.0, idH = 0.0;   // idH = the holes' own (negative) field
    const bool dyeIdOn = LA_DYE_AMT > 0.0005 &&
                         (LA_DYE_LUM_VARY > 0.0005 || LA_DYE_HUE_VARY > 0.0005);
    int nb = (int)LA_BLOB_COUNT;
    const float thresh = LA_THRESHOLD;
    [loop]
    for (int bi = 0; bi < nb; bi++) {
        AcidBlobGPU B = AcidBlobs[bi];
        float2 q  = pp - float2(B.a.x * aspect, B.a.y);
        // rise_stretch: an ANISOTROPIC kernel is the cheapest teardrop there
        // is -- squash the distance along the blob's travel (here y, the rise
        // axis) and the same round kernel draws an ellipse. e = 1 is exact
        // arithmetic, so a blob with no stretch is bit-identical to before.
        // Written as dot() on the SCALED vector, not as an expanded
        // q.x*q.x + qy*qy: with no stretch e is exactly 1, q.y / 1 is exact,
        // and dot() then emits the instruction the untouched loop emitted, so
        // an existing acid ini renders bit-for-bit as it did. (Expanding it
        // by hand moved the rounding and changed every acid frame.)
        float  e  = 1.0 + B.b.w;
        // mouse_oil_mode = 2 (comb) stretches along the DRAG direction rather
        // than the rise axis, so the same anisotropy is built in the blob's
        // own frame. B.c.x is 0 for every blob unless the comb is running and
        // the condition is uniform across the wave (all lanes read the same
        // blob), so [branch] genuinely skips it and the else arm below is the
        // untouched original line.
        float2 qs;
        [branch] if (B.c.x > 0.0) {
            float2 ax = B.c.yz, pv = float2(-B.c.z, B.c.y);
            qs = float2(dot(q, ax) / (1.0 + B.c.x), dot(q, pv));
        } else {
            qs = float2(q.x, q.y / e);
        }
        float  d2 = dot(qs, qs);
        float  sup = B.a.z * LA_SUPPORT_SCALE;              // support radius
        float  s2 = sup * sup;
        if (d2 >= s2) continue;
        // Wyvill/Blinn compact kernel (1 - t^2)^3, t = d / support. Bounded,
        // C2, no singularity and no clamping — unlike the inverse-square
        // metaball, whose broad field ~= 1 plateau made hole boundaries
        // ragged and forced the gradient-clamp guard.
        float  u  = 1.0 - d2 / s2;
        float  u2 = u * u;
        float  w  = u2 * u;
        field += w * B.a.w;
        // d(d2)/dp for the scaled distance is 2*(q.x, q.y/e^2), so the
        // gradient stays consistent with the ellipse and the sdf, the rim and
        // the lens radius all follow the stretched shape instead of the
        // circle it was drawn from. (e = 1 leaves this the original line.)
        float2 gq;
        [branch] if (B.c.x > 0.0) {
            float2 ax = B.c.yz, pv = float2(-B.c.z, B.c.y);
            float  ec = 1.0 + B.c.x;
            gq = (dot(q, ax) / (ec * ec)) * ax + dot(q, pv) * pv;
        } else {
            gq = float2(q.x, q.y / (e * e));
        }
        grad  += (-6.0 * u2 / s2) * gq * B.a.w;
#ifdef OIL_HEIGHT
        // Hess(u^3) = 24u/s^4 gq gq^T - 6u^2/s^2 D, with D = Hess(d2)/2 of
        // the (possibly stretched) distance: exact, so h's normal is analytic.
        {
            float3 hD;
            [branch] if (B.c.x > 0.0) {
                float2 ax = B.c.yz, pv = float2(-B.c.z, B.c.y);
                float  ec = 1.0 + B.c.x, ie = 1.0 / (ec * ec);
                hD = ie * float3(ax.x * ax.x, ax.x * ax.y, ax.y * ax.y)
                   + float3(pv.x * pv.x, pv.x * pv.y, pv.y * pv.y);
            } else {
                hD = float3(1.0, 0.0, 1.0 / (e * e));
            }
            float is2 = 1.0 / s2;
            hessF += ((24.0 * u * is2 * is2) * float3(gq.x * gq.x, gq.x * gq.y, gq.y * gq.y)
                      - (6.0 * u2 * is2) * hD) * B.a.w;
            OilHKernel(u, is2, gq, hD, B.a.w, kapK, kapX, kapY);
        }
#endif
        // Flat fill from a SOFT-max over the blob weights. A hard argmax drew
        // a crisp circle wherever the dominant blob handed over inside a
        // merged mass; a plain influence-weighted mean is what turned the oil
        // PoC milky. w^4 is the middle road: tails contribute nothing, the
        // handover is a soft gradient a few pixels wide.
        if (B.a.w > 0.0) {
            float w2 = w * w, w4 = w2 * w2;
            colSum += B.b.rgb * w4;
            colW   += w4;
        } else if (dyeIdOn) {
            float w2 = w * w, w4 = w2 * w2;
            idS += B.c.w * w4;
            idW += w4;
            idH += w * B.a.w;
        }
    }
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
    // ---- droplets: the SAME surface, not an overlay ----------------------
    // Every droplet's Wyvill kernel is added into `field`/`grad` BEFORE the
    // threshold, so a trapped water droplet is a literal hole in the oil and
    // an oil droplet on the ink is a literal bump of it: both get the soft
    // thickness edge, the thin-oil colour fringe, the emergent halo, the lens
    // highlight and the metaball NECK as two of them approach -- which is
    // what makes a coalescence read as surface tension instead of a cut.
    //
    // The negative (hole) weight is scaled by the LOCAL oil field, not by a
    // constant: deep inside a merged mass the field is several units high and
    // a fixed -1.2 would not reach the isoline, so small holes would simply
    // vanish wherever the oil is thick. Scaling by max(field, 1) punches the
    // same relative depth everywhere. Its own spatial derivative is ignored
    // in `grad` on purpose: the blob field varies over ~0.1 p-units and a
    // droplet over ~0.01, so the droplet term dominates the edge anyway.
    float2 gradB = grad;      // blobs only -- the surface the LENS belongs to
    // ...and the blobs-only FIELD with it. A crust droplet (brief AB) sits in
    // a dye mass but PUNCHES the field above the threshold at its own pixels,
    // so its core reads as oil and sdf alone cannot tell it from the open
    // film. The blob field before any droplet is added can: below the
    // threshold here means "this is inside a mass", droplet or not, which is
    // what crust_hue_mix has to key off.
    float fieldB = field;
#ifdef OIL_HEIGHT
    // Oil3D Stage A: h's OWN copy of the field. The visible path counts a big
    // hollow ring once per grid cell it is registered in that the pixel's 3x3
    // walk reaches (buildGrid's `spread`), so its punch steps at every 40-px
    // cell border -- hidden inside the black today, a hard seam in h. The h
    // path counts each droplet once (its canonical cell, below) and leaves out
    // the ring-interior lift (a saturate/max kink; < 0.3 px of edge shift).
    float  fieldH = field;
    float2 gradH  = grad;
    // ...and the BLOBS-ONLY surface (Stage B absorbs through the oil alone,
    // never through a droplet), plus the h path's droplet weights.
    float3 hessB = hessF, kapKB = kapK, kapXB = kapX, kapYB = kapY;
    float  dropNegH = 0.0, dropPosH = 0.0;
#endif
    // ---- per-pixel DEPTH, for the circle of confusion (items N + R) ------
    // A weighted mean of the depths of whatever covers this pixel, primed
    // with the MASSES' own plane (0.5) at a low weight: open film reads as the
    // base depth, a pixel well inside a droplet reads as that droplet's, and
    // the ground between them is a smooth blend rather than a stencil -- which
    // matters, because a hard depth edge would print a hard blur edge in the
    // post pass and look like a cut-out instead of a lens.
    // Where the optical axis meets the dish, in p-space. Everything about the
    // perspective view -- foreshortening, field curvature, the tilt gradient
    // -- is measured from here.
    const float2 axP = float2(LA_CAM_AXIS_X * aspect, LA_CAM_AXIS_Y);
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
    // ---- THE DYE'S OWN DEPTH (item AA) -----------------------------------
    // This prior used to be the constant 0.5 -- which is exactly camera_focus,
    // so every pixel with no droplet in it, i.e. every big dye mass, sat on
    // the plane of focus BY DEFINITION and could only blur through the shape
    // of the focus surface. The user saw it straight away: "the bottom right
    // blob isn't getting more out of focus even as it approaches the edge."
    //
    // The dye is a layer at its own height instead. Its slope runs along the
    // direction from the lens centre to the RIG's lamp, so the slab is not
    // parallel to the focus surface: the two cross on a LINE rather than
    // agreeing over a region, and that line travels as the lamp drifts. With
    // dye_depth 0.5 and dye_depth_tilt 0 this is the old constant, to the bit.
    float2 lampP = float2(LA_LAMP_X * aspect, LA_LAMP_Y) - axP;
    float2 lampD = lampP / max(length(lampP), 1e-5);
    float  dyeW  = LA_DYE_DEPTH_W;
    float  dyeZ  = LA_DYE_DEPTH + LA_DYE_TILT * dot(pp - axP, lampD);
    float depSum = dyeZ * dyeW, depW = dyeW;
#ifdef DROP_LIFE
    // droplet life: the solid HOLE this pixel is deepest inside -- its offset
    // in units of its KERNEL radius, that radius, its contrast (gate x
    // diffraction), its shaped squared distance and its squared support --
    // which with the blobs-only field (fieldB) give its VISIBLE radius after
    // the walk (dlRv, dlRho below)
    float2 dlQ = float2(2.0, 0.0);
    float  dlR = 0.0, dlK = 0.0, dlD2 = 0.0, dlS2 = 1.0, dlN = 0.0;
#endif
    if (LA_DROPS_ON > 0.5) {
        const int gw = (int)LA_DROP_GRID_W, gh = (int)LA_DROP_GRID_H;
        const int cx = clamp((int)floor(uv.x * gw), 0, gw - 1);
        const int cy = clamp((int)floor(uv.y * gh), 0, gh - 1);
        float  dNeg = 0.0, dPos = 0.0;
        float2 gNeg = float2(0.0, 0.0), gPos = float2(0.0, 0.0);
#ifdef OIL_HEIGHT
        float3 hNeg = float3(0.0, 0.0, 0.0), hPos = float3(0.0, 0.0, 0.0);
        float3 kNeg = float3(0.0, 0.0, 0.0), kPos = float3(0.0, 0.0, 0.0);   // u * Hess
        float3 kNegX = kNeg, kNegY = kNeg, kPosX = kNeg, kPosY = kNeg;       // its gradient
        float  dNegH = 0.0, dPosH = 0.0;
        float2 gNegH = float2(0.0, 0.0), gPosH = float2(0.0, 0.0);
#endif
        [loop]
        for (int oy = -1; oy <= 1; oy++) {
            int yy = cy + oy;
            if (yy < 0 || yy >= gh) continue;
            [loop]
            for (int ox = -1; ox <= 1; ox++) {
                int xx = cx + ox;
                if (xx < 0 || xx >= gw) continue;
                uint2 cell = DropCells[yy * gw + xx];
                [loop]
                for (uint di = 0; di < cell.y; di++) {
                    float4 D = AcidDrops[cell.x + di];
                    // .w packs the gate with the ring FLAG (see
                    // UploadAcidConstants): w = gate + 2*ring, gate in [0,1].
                    // A flag, not a size: the ring's width is a global key, so
                    // nothing per-droplet is lost and the particle stays one
                    // float4.
                    // ...and, above both, an 8-bit DEPTH with 0 as the "no
                    // depth" sentinel: w = 4*dq + 2*ring + gate. Three extra
                    // ALU in the hottest loop of the frame, against a second
                    // buffer fetch per droplet per pixel if depth had its own
                    // record -- and with droplet_depth 0 the arithmetic here
                    // is exactly the two lines it replaces.
                    float  dwf  = D.w;
                    float  dqz  = floor(dwf * 0.25);
                    float  dwr  = dwf - 4.0 * dqz;
                    float  rng  = (dwr > 1.5) ? 1.0 : 0.0;
                    float  gate = dwr - 2.0 * rng;
                    float  ddep = (dqz > 0.5) ? (dqz - 1.0) * (1.0 / 254.0) : 0.5;
                    float2 dq = pp - float2(D.x * aspect, D.y);
                    float  R  = abs(D.z);
                    float  ds = R * LA_DROP_SUPPORT;
                    float  ds2 = ds * ds;
                    float  dd2 = dot(dq, dq);
#ifdef DROP_LIFE
                    // droplet_wobble: a bulging solid reaches past its round
                    // support (1.1 x covers the breath and the out-of-round)
                    if (dd2 >= ((rng < 0.5 && laDrop[0].w > 0.0005) ? ds2 * 1.21 : ds2)) continue;
#else
                    if (dd2 >= ds2) continue;
#endif
#ifdef OIL_HEIGHT
                    // hk = 1 in exactly ONE of the cells this droplet is
                    // registered in among the pixel's 3x3: the registration
                    // rectangle is rebuilt from the CPU's own rule (home cell
                    // +- ceil(support / cell) - 1, capped at 8), intersected
                    // with the walk, and the cell nearest home wins. For every
                    // ordinary droplet (support <= one cell) that is its home
                    // cell, i.e. hk == 1 on its only visit.
                    float hk;
                    {
                        int hx = clamp((int)floor(D.x * gw), 0, gw - 1);
                        int hy = clamp((int)floor(D.y * gh), 0, gh - 1);
                        int rx = clamp((int)ceil((ds / aspect) * gw - 1.0001), 0, 8);
                        int ry = clamp((int)ceil(ds * gh - 1.0001), 0, 8);
                        int lx = max(max(hx - rx, cx - 1), 0), ux = min(min(hx + rx, cx + 1), gw - 1);
                        int ly = max(max(hy - ry, cy - 1), 0), uy = min(min(hy + ry, cy + 1), gh - 1);
                        hk = (xx == clamp(hx, lx, ux) && yy == clamp(hy, ly, uy)) ? 1.0 : 0.0;
                    }
#endif
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
                    [branch] if (rng > 0.5) {
                        // HOLLOW LENS: the punch is a BAND centred on the
                        // droplet's own visible radius, so the interior never
                        // crosses the isoline and keeps its oil -- the "empty
                        // double" the user photographed, drawn deliberately.
                        // Its gradient points radially away from the band's
                        // centre line, so the dark rim, the meniscus and the
                        // thickness ramp all wrap the annulus exactly as they
                        // wrap a disc: no special-case shading anywhere.
                        // ---- PERSPECTIVE: seen at an angle, not from above -
                        // The user's sketch: a LENS at the tip of a view cone
                        // over a flat dish. Only the point where the optical
                        // axis meets the dish is seen face on; every other
                        // element is seen at an angle that grows with its
                        // distance from that point, so a ring there is a
                        // circle in PERSPECTIVE -- foreshortened along the
                        // radius toward the axis, its far wall thicker than
                        // its near one. That is the real answer to "these are
                        // pixel perfect circles": not a wobble bolted on, but
                        // a camera that cannot draw a symmetric one off-axis.
                        // camera_fov 0 leaves invc exactly 1 and every line
                        // below is the arithmetic that was here before.
                        float2 av   = float2(1.0, 0.0);
                        float  tanT = 0.0, invc = 1.0;
                        float2 dqv  = dq;
                        [branch] if (LA_FOV_K > 0.0) {
                            float2 toAx = axP - float2(D.x * aspect, D.y);
                            float  la   = length(toAx);
                            [branch] if (la > 1e-5) {
                                av   = toAx / la;
                                tanT = LA_FOV_K * la;
                                // the apparent extent along that radius is
                                // multiplied by cos(theta), so testing the
                                // pixel against a circle means stretching its
                                // offset along it by 1/cos(theta).
                                invc = sqrt(1.0 + tanT * tanT);
                                dqv  = dq + av * (dot(dq, av) * (invc - 1.0));
                            }
                        }
                        float  dd = length(dqv);
                        float2 un = dqv / max(dd, 1e-6);
                        // ---- never a perfect circle -----------------------
                        // The user, on the first rings: "these are pixel
                        // perfect circles." The band's radius is a Fourier
                        // series in the polar angle (see UploadAcidConstants),
                        // and cos/sin of 2*phi and 3*phi come straight out of
                        // the radial unit vector -- no atan2, no trig at all.
                        float4 S  = AcidDrops[(uint)LA_DROP_RING_BASE + cell.x + di];
                        float  c2 = un.x * un.x - un.y * un.y;
                        float  s2 = 2.0 * un.x * un.y;
                        float  c3 = un.x * (4.0 * un.x * un.x - 3.0);
                        float  s3 = un.y * (3.0 - 4.0 * un.y * un.y);
                        float  wv = S.x * c2 + S.y * s2 + S.z * c3 + S.w * s3;
                        float  wd = 2.0 * (S.y * c2 - S.x * s2)
                                  + 3.0 * (S.w * c3 - S.z * s3);   // d/d(phi)
                        float  Rp = R * (1.0 + wv);
                        float  bw = max(saturate(LA_DROP_RING_WIDTH) * ds, 1e-6);
                        // a stretched film THINS: the wall is narrower where
                        // the ring bulges out and thicker where it pinches in,
                        // so the wall is not a uniform stroke either.
                        bw *= clamp(1.0 - 1.6 * wv, 0.55, 1.7);
                        // ...and the OBLIQUE view thickens the wall on the far
                        // side and thins it on the near one: the line of sight
                        // crosses more of the bubble's skin where it enters at
                        // a glancing angle. dot(un, av) is +1 on the side
                        // facing the optical axis, -1 on the side away from
                        // it, so this is one mad and it is exactly the
                        // asymmetry a stamped "O" can never have.
                        float  asym = clamp(0.90 * tanT, 0.0, 0.45);
                        bw *= 1.0 - asym * dot(un, av);
                        // ...and a ring diffracts by its WALL WIDTH, not by
                        // its radius: a big hoop drawn with a two-pixel wall
                        // is exactly the hair in the user's photograph, and it
                        // has no business being pure black either.
                        float  gW = gate * AcidDiffract(bw, LA_DIFFR_SCALE, LA_DIFFRACTION);
                        float  sgn = dd - Rp;
                        float  su = 1.0 - (sgn * sgn) / (bw * bw);
                        if (su > 0.0) {
                            float  su2 = su * su;
                            float  dw  = su2 * su * gW;
                            // d/d(dd) of the profile, carried onto grad(sgn).
                            // grad(phi) = (-un.y, un.x)/dd, so an out-of-round
                            // band's normal leans off the radius exactly as
                            // much as its radius is changing with the angle --
                            // which is what keeps the rim, the meniscus and
                            // the halo wrapped square on the wall.
                            float2 gs = un - (R * wd / max(dd, 1e-6))
                                             * float2(-un.y, un.x);
                            // gs lives in the foreshortened frame; the map
                            // back to p-space is the same symmetric stretch,
                            // so the rim, the meniscus and the halo stay
                            // square on the wall of an oblique ring instead of
                            // sliding round it. Identity at camera_fov 0.
                            gs += av * (dot(gs, av) * (invc - 1.0));
                            float2 dg  = ((-6.0 * su2 / (bw * bw)) * sgn * gW) * gs;
                            dNeg += dw; gNeg += dg;
#ifdef OIL_HEIGHT
                            // the band's Hessian: grad(sgn) = gs, Hess(sgn) =
                            // the circle's curvature (I - un un^T)/dd (the
                            // wobble's own second derivative is left out).
                            {
                                float  ib2 = 1.0 / (bw * bw);
                                float3 gg  = float3(gs.x * gs.x, gs.x * gs.y, gs.y * gs.y);
                                float3 tc  = float3(1.0 - un.x * un.x, -un.x * un.y,
                                                    1.0 - un.y * un.y) / max(dd, 1e-6);
                                dNegH += hk * dw; gNegH += hk * dg;
                                hNeg += (hk * gW) * ((24.0 * su * sgn * sgn * ib2 * ib2) * gg
                                                     - (6.0 * su2 * ib2) * (gg + sgn * tc));
                                // the band as a radial profile f(r) = su^3:
                                // K = su (f'' P + f'/r (I - P)), f''' for its gradient
                                float  ir  = 1.0 / max(dd, 1e-6);
                                float  f1  = -6.0 * su2 * sgn * ib2;
                                float  f2  = 24.0 * su * sgn * sgn * ib2 * ib2 - 6.0 * su2 * ib2;
                                float  f3  = (-48.0 * sgn * sgn * ib2 + 72.0 * su) * sgn * ib2 * ib2;
                                float  sud = -2.0 * sgn * ib2;
                                OilHRadial(su * f2, su * f1 * ir, sud * f2 + su * f3,
                                           (sud * f1 + su * f2) * ir - su * f1 * ir * ir,
                                           un, ir, hk * gW, kNeg, kNegX, kNegY);
                            }
#endif
                            depSum += dw * ddep; depW += dw;
                        }
                        // droplet_ring_lift: the interior is a little lens, so
                        // it reads slightly brighter than the film around it.
                        // max(), not a sum, so a raft of touching rings does
                        // not stack into a glowing patch.
                        float li = saturate((Rp - 0.5 * bw - dd) / max(0.5 * Rp, 1e-5));
                        // The oblique view also swings the highlight: a lens
                        // tipped away from you shows its bright face on the
                        // side turned toward the axis. So the interior lift is
                        // not centred either -- the whole ring is asymmetric,
                        // wall and light together, which is what stops it
                        // reading as a glyph.
                        li *= 1.0 + clamp(1.10 * tanT, 0.0, 0.55) * dot(un, av);
                        ringIn = max(ringIn, saturate(li) * gW);
                        depSum += li * gW * ddep * 0.5; depW += li * gW * 0.5;
                        continue;
                    }
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
                    // DIFFRACTION by the droplet's own size: a 3-px dot
                    // cannot punch the film fully black, so it comes out a
                    // soft grey spot instead of a hard hole. Carried on the
                    // gate, which means the field AND its gradient stay
                    // consistent and the edge softens with the fill -- no
                    // separate shading path, and nothing to fight the halo.
                    float  gS = gate * AcidDiffract(R, LA_DIFFR_SCALE, LA_DIFFRACTION);
#ifdef DROP_LIFE
                    // ---- droplet_wobble: a solid droplet is never still -----
                    // Its outline is the ring's Fourier series too (the CPU
                    // writes the record for solids while the key is on, with
                    // the breath already on R): the kernel reads the offset
                    // shrunk by 1 / (1 + w(phi)), so the isoline sits at
                    // R (1 + w(phi)), and the gradient keeps the angular term
                    // so the rim, the lens and the glint stay square on the
                    // wobbling edge. The height path below reads the shrunk
                    // offset (its angular term is left out: < 5 deg of tilt).
                    float2 gK = dq;
                    [branch] if (laDrop[0].w > 0.0005) {
                        float4 S  = AcidDrops[(uint)LA_DROP_RING_BASE + cell.x + di];
                        float  dl = sqrt(dd2);
                        float2 un = dq / max(dl, 1e-6);
                        float  c2 = un.x * un.x - un.y * un.y;
                        float  s2 = 2.0 * un.x * un.y;
                        float  c3 = un.x * (4.0 * un.x * un.x - 3.0);
                        float  s3 = un.y * (3.0 - 4.0 * un.y * un.y);
                        float  wv = S.x * c2 + S.y * s2 + S.z * c3 + S.w * s3;
                        float  wd = 2.0 * (S.y * c2 - S.x * s2) + 3.0 * (S.w * c3 - S.z * s3);
                        float  iw = 1.0 / max(1.0 + wv, 0.5);
                        // grad(dd2 iw^2) / 2 = iw^2 (dq - dl iw wd t), t = (-un.y, un.x)
                        gK  = (iw * iw) * (dq - (dl * iw * wd) * float2(-un.y, un.x));
                        dq *= iw;
                        dd2 = dot(dq, dq);
                        if (dd2 >= ds2) continue;
                    }
#endif
                    float  du = 1.0 - dd2 / ds2;
                    float  du2 = du * du;
                    float  dw = du2 * du * gS;
                    float2 dg = (-6.0 * du2 / ds2) * dq * gS;
#ifdef DROP_LIFE
                    dg = (-6.0 * du2 / ds2) * gK * gS;
                    [branch] if (D.z < 0.0) {
                        float2 qn = dq / max(R, 1e-6);   // the shaped offset, in kernel radii
                        if (dot(qn, qn) < dot(dlQ, dlQ)) { dlQ = qn; dlR = R; dlK = gS; dlD2 = dd2; dlS2 = ds2; }
                    }
#endif
                    if (D.z < 0.0) { dNeg += dw; gNeg += dg; }
                    else           { dPos += dw; gPos += dg; }
#ifdef OIL_HEIGHT
                    {
                        float3 dh = (hk * gS) * ((24.0 * du / (ds2 * ds2))
                                          * float3(dq.x * dq.x, dq.x * dq.y, dq.y * dq.y)
                                          - (6.0 * du2 / ds2) * float3(1.0, 0.0, 1.0));
                        if (D.z < 0.0) {
                            hNeg += dh; dNegH += hk * dw; gNegH += hk * dg;
                            OilHKernel(du, 1.0 / ds2, dq, float3(1.0, 0.0, 1.0), hk * gS, kNeg, kNegX, kNegY);
                        } else {
                            hPos += dh; dPosH += hk * dw; gPosH += hk * dg;
                            OilHKernel(du, 1.0 / ds2, dq, float3(1.0, 0.0, 1.0), hk * gS, kPos, kPosX, kPosY);
                        }
                    }
#endif
                    depSum += dw * ddep; depW += dw;
                }
            }
        }
#ifdef DROP_LIFE
        dlN = dNeg;        // droplet life: the summed hole kernels (dlRho below)
#endif
        if (dNeg > 0.0) {
            float pun = max(field, 1.0) * LA_DROP_WEIGHT;
            field -= dNeg * pun;
            grad  -= gNeg * pun;
        }
        if (dPos > 0.0) {
            // Symmetric with the hole punch above, and for the same reason: a
            // FIXED positive weight only clears the isoline where the ink is
            // already close to it, so an oil droplet that drifted into a deep
            // hole raised the field a little, crossed nothing, and left just
            // its rim -- a pink-ringed dot inside a black tail, which is one
            // of the three the user photographed. Carry whatever deficit is
            // actually here, plus the authored weight on top.
            float pup = LA_DROP_OIL_W + max(thresh - field, 0.0);
            field += dPos * pup;
            grad  += gPos * pup;
            // same soft-max fill blend the blobs use, so an oil droplet takes
            // the palette's own bright shade and merges colour with a blob it
            // touches instead of stamping a flat disc over it.
            float dq2 = saturate(dPos), dq4 = dq2 * dq2 * dq2 * dq2;
            colSum += laOil[3].rgb * dq4;
            colW   += dq4;
        }
        // ---- droplet_ring_lift: the interior of a hollow droplet ---------
        // A bubble's middle is a lens, not a hole: it is the same film seen
        // through a curved surface, so it sits a hair thicker and a hair
        // brighter than the sheet around it. Both go in BEFORE the threshold
        // and through the same soft-max fill the blobs use, so the interior
        // keeps the local palette colour and the band around it is untouched.
        [branch] if (ringIn > 0.0 && LA_DROP_RING_LIFT > 0.0005) {
            float k  = saturate(LA_DROP_RING_LIFT);
            field += ringIn * k * 0.25 * thresh;
            float wl = ringIn * ringIn; wl = wl * wl * k;
            colSum += laOil[3].rgb * wl;
            colW   += wl;
        }
#ifdef OIL_HEIGHT
        // the h path's punches: the visible arithmetic, on the deduplicated sums
        if (dNegH > 0.0) {
            float punH = max(fieldH, 1.0) * LA_DROP_WEIGHT;
            fieldH -= dNegH * punH; gradH -= gNegH * punH;
            hessF  -= hNeg * punH;
            kapK -= kNeg * punH;  kapX -= kNegX * punH;  kapY -= kNegY * punH;
        }
        dropNegH = dNegH; dropPosH = dPosH;
        if (dPosH > 0.0) {
            float pupH = LA_DROP_OIL_W + max(thresh - fieldH, 0.0);
            fieldH += dPosH * pupH; gradH += gPosH * pupH;
            hessF  += hPos * pupH;
            kapK += kPos * pupH;  kapX += kPosX * pupH;  kapY += kPosY * pupH;
        }
#endif
    }
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
#ifdef DROP_LIFE
    // ---- droplet life: where this pixel sits in its droplet ----------------
    // R is the kernel's radius, not the disc the eye sees: the punch scales
    // with the film's own field (max(field, 1) x droplet_weight), so a lone
    // droplet's isoline sits where its kernel (1 - d^2/S^2)^3 has fallen to
    // kE = (fieldB - thresh) / (punch x contrast): R_v = S sqrt(1 - kE^(1/3)).
    // The position is read off the SUMMED punch instead of the distance, the
    // kernel value du^3 = dlN / contrast, so a merging pair or an overlapping
    // satellite (whose disc is bigger than either kernel says) still puts
    // dlRho = 1 exactly on the visible isoline: dlRho = sqrt((1 - du) /
    // (1 - kE^(1/3))), 0 where the punch is full depth, > 1 on the film
    // around it, 2 = in no droplet (or one that never opens the film).
    // dlRv (the lone estimate) only gates the specks' size ramp.
    float dlRv = 0.0, dlRho = 2.0;
    [branch] if (dlK > 0.0005 && fieldB > thresh) {
        float kE = (fieldB - thresh) / max(max(fieldB, 1.0) * LA_DROP_WEIGHT * dlK, 1e-5);
        [branch] if (kE < 1.0) {
            float cE = pow(kE, 1.0 / 3.0);
            float du = pow(saturate(dlN / dlK), 1.0 / 3.0);
            dlRv  = sqrt(dlS2 * max(1.0 - cE, 1e-4));
            dlRho = sqrt(max(1.0 - du, 0.0) / max(1.0 - cE, 1e-4));
        }
    }
#endif
    // ---- CIRCLE OF CONFUSION -> the render target's ALPHA (items N + R) ---
    // The [post] pass used to defocus the whole frame by one radius, which is
    // what a flatbed scanner does, not a lens. A lens has ONE surface in focus
    // and everything else blurs in proportion to how far off it is -- the
    // Requiem frame the user picked out is one cell sharp with the rings
    // behind it dissolved into ghosts. So the depth of field is decided HERE,
    // where the depths are known, and handed to the post pass per pixel:
    //   * the surface in focus is not a plane parallel to the dish. It is
    //     CURVED (camera_field_curve: a real lens cannot hold the centre and
    //     the corners at once) and TILTED (focus_tilt: the freelensing sweet
    //     spot, a slanted strip of sharpness), both measured from where the
    //     optical axis meets the dish;
    //   * focus_band_px is a width in the PICTURE, so it is turned into a
    //     depth tolerance by the local gradient of that surface -- the sharp
    //     strip is that many pixels across however hard the lens is tilted,
    //     which is the number a photographer would actually reach for;
    //   * outside the band the radius ramps up over a fixed depth interval
    //     and is clamped at dof_max_px, so a far element goes soft and stays
    //     soft instead of dissolving.
    [branch] if (LA_DOF_MAX_PX > 0.0) {
        float2 qo  = pp - axP;
        float  rr  = length(qo);
        float  fz  = LA_FOCUS_DEPTH + LA_FIELD_CURVE * rr * rr
                             + LA_TILT_AMT * dot(qo, float2(LA_TILT_COS, LA_TILT_SIN));
        float  gm  = abs(LA_TILT_AMT) + 2.0 * abs(LA_FIELD_CURVE) * rr;
        float  tol = 0.5 * LA_FOCUS_BAND * gm;
        float  dz  = abs(depSum / max(depW, 1e-6) - fz);
        // ...and it falls away FAST. The user's focus reference is a macro
        // shot where the one plane in focus resolves fine texture and
        // everything off it is already a wash: a shallow depth of field with a
        // STEEP shoulder, not a gentle ramp. So the radius leaves zero the
        // moment the element clears the sharp band and is most of the way to
        // the clamp within a tenth of the depth range -- x*(2-x) rather than a
        // straight line, over a deliberately short span.
        float  x   = saturate((dz - tol) * LA_COC_SPAN_INV);
        outCoc = LA_DOF_MAX_PX * x * (2.0 - x);
    }
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
#ifdef OIL_HEIGHT
    // --shot-dump film frame (debug 5): the lit film everywhere, no oil -- the
    // field lifted far above the isoline, so every pixel is film interior.
    if (LA_OIL_H_DEBUG > 4.5 && LA_OIL_H_DEBUG < 5.5) {
        field = thresh + 8.0; grad = float2(0.0, 0.0);
        fieldB = field; gradB = grad;
    }
#endif
    float3 oilBase = (colW > 1e-9) ? colSum / colW : laOil[0].rgb;
    float  gl   = length(grad) + 1e-6;
    float  sdf  = clamp((field - thresh) / gl, -0.25, 0.25);  // >0 inside the oil
    // ---- [post] softness: nothing in a macro shot is razor-sharp ---------
    // The user, on the live panel: "these edges are way too accurate and
    // focused." A lens defocuses the SURFACE, so this widens the isoline's own
    // transition rather than blurring the frame: the coverage AA band, the
    // film-edge band and the rim/meniscus widths all grow by the same blur
    // radius. The grain, which is added after everything, stays sharp -- which
    // is exactly right: the grain is in the camera, the blur is in the lens.
    // LA_SOFTNESS is the radius in uv-y (authored in px at 1440p), and the field
    // units it takes here are that distance times the local |grad|.
    float  soft = LA_SOFTNESS;
    // ---- MINIMUM BAND WIDTHS (band_min) ----------------------------------
    // The user, photographing a big mass beside a medium droplet: "what
    // happened that the bubbles got a solid outline -- whatever shading is in
    // this photo needs to be everywhere." Every band below (film edge, dark
    // rim, meniscus, halo, penumbra) is a fraction of the LENS radius, which
    // is right for a big mass and wrong for a small droplet: the same shading
    // squeezed into a sub-pixel line is a hard outline, not a soft glow. Each
    // one now has a floor authored in px at 1440p and carried as a fraction
    // of the frame, so a 3-px droplet is shaded like a 300-px mass and the
    // preview and the panel agree. band_min = 0 restores the old widths.
    const float PX1440 = 1.0 / 1440.0;
    float  bmin = saturate(LA_BAND_MIN);
    float  aaF  = fwidth(field) * LA_AA_SCALE + 1e-4;
    if (soft > 1e-6) aaF += soft * gl;
    float  cov  = smoothstep(thresh - aaF, thresh + aaF, field);
    // ---- local lens radius and film thickness (oil_thin_edge) -----------
    // The Wyvill kernel gives the blob radius for free: at the isoline
    // |grad| ~= 1.72/S while the visible radius is 0.454*S, so R ~= 0.78/|grad|.
    // That is what makes the soft edge a fixed FRACTION of each disc instead
    // of a fixed number of pixels. Deep inside a merged mass |grad| -> 0, so
    // the band is clamped; there the sdf clamp (0.25) saturates thk to 1.
    // Clamped: on a broad merged mass |grad| collapses and an unclamped R
    // blew the halo up into frame-sized pale lobes. The largest authored
    // blob radius is ~0.4 p-units, so that is the ceiling.
    // ---- "is this pixel really ON the isoline?" (isoOk) ------------------
    // sdf = (field - thresh) / |grad| is a true signed distance only where the
    // field behaves like a plane: there it changes by exactly one pixel per
    // pixel, so |grad(sdf)| == 1. Around a blob or a droplet whose field dips
    // TOWARD the threshold without crossing it, sdf collapses to nearly 0 and
    // springs back within a couple of pixels, and its own gradient is many
    // times too steep. That is exactly where the dark rim band painted a
    // hollow RING with oil still inside it, and where the halo painted a soft
    // plume with no surface under it (the user photographed both). One number
    // off two derivatives the quad already has, and it is ~1 on every real
    // edge, so a genuine rim keeps its authored width.
    float sdfWarp = length(float2(ddx(sdf), ddy(sdf))) / max(texelSize.y, 1e-7);
    // A true signed distance has |grad(sdf)| == 1 EXACTLY, so the band can sit
    // tight above 1; at 2.5..7.0 it only clipped the worst of each ring and
    // left the rest as a dotted circle.
    // A true signed distance has |grad(sdf)| == 1 EXACTLY, so the band starts
    // just above 1. It ENDS far out (6) on purpose: this is a blend weight,
    // and a narrow band put a hard bright seam around the plume it was meant
    // to clean up. Wide = gradual = invisible.
    float isoOk   = 1.0 - smoothstep(1.35, 6.00, sdfWarp);
    // ---- the lens radius that sets the soft band's width -----------------
    // A droplet's kernel is tiny and therefore very steep, so a droplet merely
    // PASSING THROUGH the wide band around a big hole spikes |grad| there and
    // makes the band width jump frame to frame -- the user's "individual dots"
    // flickering, "usually the blur around the dark spots".
    //
    // The blob-only gradient cannot simply be substituted: sdf still divides by
    // the FULL |grad|, and deep inside a merged mass the blob gradient goes to
    // zero, so lensR pinned to its ceiling, edgeW to 0.060, and the oil went
    // black (tried, reverted). Use it only where it is a credible surface
    // gradient -- i.e. near a blob isoline, which is exactly where the band
    // lives and exactly where droplets were modulating it. Deep inside, where
    // gradB is meaningless, fall back to the full gradient, which is what this
    // line always did. With the droplet sim off gradB == grad and both branches
    // are the same number.
    float  glB = length(gradB) + 1e-6;
    float  glLens = lerp(gl, glB, smoothstep(0.35, 0.90, glB));
    float  lensR = clamp(0.78 / max(glLens, 1e-3), 0.02, 0.35);
    // ---- oil_edge_mode ---------------------------------------------------
    // 0 (default, every existing ini): the film thins over a band proportional
    // to the LENS radius, so a big disc fades out over a wide translucent
    // gradient and a droplet over a couple of pixels.
    // 1 CRISP: every surface gets the droplets' treatment -- one narrow,
    // SIZE-INDEPENDENT band a few rim-widths across, so a big hole ends on the
    // same hard isoline a droplet does and carries only its meniscus. The film
    // BODY (oil_transparency, absorption, bump, refraction) is untouched in
    // both: this is the edge and nothing else.
    // The user, on stills of the two: "hard to say in stills, apply all" --
    // hence a key and both shipped, to be judged live from the tray.
    float  edgeW = clamp(max(LA_OIL_EDGE_FRAC, 0.02) * lensR, LA_RIM_WIDTH * 2.0, 0.060);
    if (LA_OIL_EDGE_MODE > 0.5) edgeW = max(LA_RIM_WIDTH * 2.5, 1e-5);
    // ...and no film edge may be tighter than the lens's own blur circle.
    if (soft > 1e-6) edgeW = max(edgeW, soft * 2.0);
    // ...nor than the floor: a droplet's film has to thin over the same few
    // px a mass's does, or its edge is a cut.
    edgeW = max(edgeW, bmin * 3.0 * PX1440);
    float  thk   = 1.0;                       // 1 = full-thickness oil
    // oil_transparency needs the same thickness proxy even when the soft
    // thin edge itself is off: a transparent film MUST be clearest where it
    // is thinnest, or it reads as a sheet of tinted glass cut with scissors.
    if (LA_OIL_THIN_EDGE > 0.0005 || LA_OIL_TRANSP > 0.0005) {
        thk = smoothstep(0.0, edgeW, sdf);
        // ---- oil_edge_curve --------------------------------------------
        // The user, on the panel: "it looks like it goes from green to black,
        // then stops; it should be more S-curved: distance vs mix of colours".
        // smoothstep is an S in the middle but its ENDS are only C1 -- the
        // ramp arrives at full thickness (and at black) with a visible knee.
        // smootherstep (6t^5-15t^4+10t^3) is flat to second order at both
        // ends, so both knees vanish. Same band, same width: a profile
        // change only, and the branch is the original line at 0.
        [branch] if (LA_OIL_EDGE_CURVE > 0.0005) {
            float t = saturate(sdf / max(edgeW, 1e-7));
            thk = lerp(thk, t * t * t * (t * (t * 6.0 - 15.0) + 10.0),
                       saturate(LA_OIL_EDGE_CURVE));
        }
        // ...but only as far as sdf can be believed. Where it cannot (isoOk
        // < 1: the field dips toward the threshold without crossing it, so
        // sdf collapses although the oil is thick) the thin-edge model would
        // otherwise conclude the film had thinned to nothing -- which drew a
        // hollow dark RING around every bubble that failed to punch through,
        // with oil still inside it. Fall back to the plain coverage, which
        // cannot lie: 1 in the oil, 0 on the ink.
        thk = lerp(cov, thk, isoOk);
    }
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
#ifdef OIL_HEIGHT
    // ==== OIL3D STAGE A: the wax's HEIGHT FIELD h and its normal n =========
    // The black bodies (field < thresh: the gaps between the lit blobs, the
    // hole blobs, the punched droplets) get a real thickness h in [0, 1], and
    // the lit film stays flat (h ~ 0). Nothing consumes h yet but the debug
    // view; Stage B's transmission (T = exp(-k h)) will.
    //
    // NO POLYGONS, by construction: h is a closed-form function of the field,
    // its gradient and its Hessian -- all analytic sums of the C2 Wyvill
    // kernels at THIS pixel -- so there is no grid, no texture, no clamp, no
    // argmax and no finite difference anywhere in it. The pieces:
    //   delta = thresh - field         > 0 inside a body, C2
    //   kap   = (mean + rms)/2 of kapK's eigenvalues: the body's curvature,
    //           direction-free, never negative; = k on a round dome, k/2 on a
    //           saddle (a Laplacian alone is 0 there, and the floor below then
    //           threw a thin neck between two holes up to full thickness),
    //           0 on a bowl
    //   Q     = |grad|^2 + 2 kap delta+ + (g0 delta / thresh)^2
    //   d     = delta / sqrt(Q)
    // d is a SMOOTH distance-to-the-contact-line: where the field is a plane
    // it is the true distance (delta/|grad|); over a round body it is the
    // paraboloid (R^2 - r^2)/(2R) -- unit slope at the edge, R/2 at the
    // centre, no cone tip (|grad| only ever enters squared); and where the
    // field is flat (deep in a big mass, field -> 0) the g0 floor turns it
    // into a constant 12 lambda: a plateau reached smoothly, never clamped.
    // The floor grows with delta^2, so at every edge (delta -> 0) d is the
    // true distance however shallow the field's slope there is.
    //   h = 1 - exp(-softplus_w(d) / lambda)
    // is the gravity puddle's own profile (the screened Laplace solution in
    // 1-D): a sessile cap on small droplets, a flat-topped pancake on big
    // masses, its shoulder lambda (oil_height_px) wide. The softplus is the
    // contact line's toe, w = 1 px: C-infinity across the edge instead of a
    // kink, so even h's slope has no seam where the lit film begins.
    // The normal is the analytic gradient of the surface z = lambda * h (a
    // 45-degree contact angle), kap's own gradient included through the
    // field's third derivatives (kapX/kapY): n IS h's slope, no facets.
    // (The one approximation is the existing one: the droplet punch's weight
    // max(field, 1) is held constant, exactly as `grad` already holds it.)
    float  oil3dH = 0.0;
    float3 oil3dN = float3(0.0, 0.0, 1.0);
    float  oil3dD = 0.0;         // the smooth signed distance d (p-units)
    float2 oil3dG = float2(0.0, 0.0);   // grad of lambda*h: the surface slope
    OilHeightEval(thresh, fieldH, gradH, hessF, kapK, kapX, kapY, oil3dH, oil3dD, oil3dG);
    oil3dN = normalize(float3(-oil3dG, 1.0));
#ifdef OIL_SURFACE
    {
        float  hOil, dOil;
        float2 gOil;
        OilHeightEval(thresh, fieldB, gradB, hessB, kapKB, kapXB, kapYB, hOil, dOil, gOil);
        float  aaW = fwidth(fieldH) * LA_AA_SCALE + 1e-4;
        float  wax = 1.0 - smoothstep(thresh - aaW, thresh + aaW, fieldH);
        return float4(wax, hOil, 0.0, 0.0);
    }
#endif
    // ---- --shot-debug height | normal | contour --------------------------
    // Straight to the target, no post pass (the renderer turns it off): the
    // png's sRGB byte IS the value (SDR white = 1, so /sdrScale undoes it).
    [branch] if (LA_OIL_H_DEBUG > 3.5 && LA_OIL_H_DEBUG < 4.5) {
        // --shot-dump: h at ~21 bits through the half-float target
        float hk = oil3dH * 1024.0;
        return float4(floor(hk) * (1.0 / 1024.0), frac(hk), 0.0, 1.0);
    }
#ifdef OIL_COLUMN
    [branch] if (LA_OIL_H_DEBUG > 6.5) {
        // debug 7 (--shot-debug column, needs water_scatter > 0): r = the lit
        // column (OilColumnLit: 1 = open water, 0 = the whole depth in a
        // mass's shadow), g = the padded mass coverage at this pixel, b = the
        // coverage 0.5 D cot(elev) toward the lamp. Both read from the SCREEN
        // pixel (OilSurfUV), like the lamp code. The png byte IS the value.
        float2 suv = OilSurfUV(i.pos.xy);
        float3 v = float3(OilColumnLit(suv, i.pos.xy), OilMassCovAt(suv),
                          OilMassCovAt(suv + 0.5 * float2(LA_SH_OFF_X, LA_SH_OFF_Y)));
        return float4(SRGBToLinear(saturate(v)) * sdrScale, 1.0);
    }
#endif
#ifndef OIL_SURFACE
    [branch] if (LA_OIL_H_DEBUG > 5.5) {
        // debug 6: the PRE-PASS buffer at this pixel, raw (g = h_oil, b = wax,
        // the wiring proof); r = the in-register h (P1: h is no longer stored)
        float2 sv = OilSurf0.Load(int3(i.pos.xy, 0));
        return float4(oil3dH, sv.y, sv.x, 1.0);
    }
#endif
    [branch] if (LA_OIL_H_DEBUG > 0.5 && LA_OIL_H_DEBUG < 3.5) {
        float3 v;
        if (LA_OIL_H_DEBUG < 1.5) {
            v = oil3dH.xxx;
        } else if (LA_OIL_H_DEBUG < 2.5) {
            v = oil3dN * 0.5 + 0.5;
        } else {
            // contour: 20 iso-height lines over a dim grey h. A crease or a
            // terrace shows as a kink or a bunching of the lines; the line
            // width is held at ~1 px by h's own analytic slope.
            float  c   = oil3dH * 20.0;
            float  fc  = abs(frac(c + 0.5) - 0.5);
            float  pw  = length(oil3dG) / max(LA_OIL_H_LAMBDA, 1e-5) * 20.0 * texelSize.y;
            float  ln  = 1.0 - smoothstep(0.5 * pw, 1.5 * pw + 1e-6, fc);
            v = lerp(oil3dH.xxx * 0.45, float3(1.0, 0.85, 0.30), ln * step(1e-4, oil3dH));
        }
        return float4(SRGBToLinear(saturate(v)) * sdrScale, 1.0);
    }
#endif
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
    // Refraction: the ink is seen through thinning oil near the rim, so shift
    // the ink lookup along the field gradient inside an edge band. Its width
    // is rim_width * refraction_width (default the shipped 7), widened toward
    // the soft-edge band so the ink is actually seen bending under the oil
    // instead of only inside a hairline.
    float  refrW = max(LA_RIM_WIDTH * ((LA_REFR_WIDTH > 0.0005) ? LA_REFR_WIDTH : 7.0), 1e-4);
    if (LA_OIL_THIN_EDGE > 0.0005) refrW = lerp(refrW, max(refrW, edgeW), saturate(LA_OIL_THIN_EDGE));
    float  eb = sdf / refrW;
    float  edgeB = exp(-eb * eb);
    float2 rOff = (grad / gl) * (LA_REFRACTION * edgeB);
    // ---- body refraction (oil_refract_body) ------------------------------
    // The edge band above bends the ink only in a hairline around each disc.
    // A real film of oil has thickness EVERYWHERE, so what you see through
    // its middle is displaced too — and unevenly, because the surface is not
    // flat. Offset along the gradient of (field + a slow fbm bump): the field
    // part leans the whole disc's contents one way, the fbm part is what makes
    // the marbling wobble as it passes under. Capped to a unit vector, so the
    // key is the offset in uv units and can never run away.
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
    if (LA_OIL_REFR_BODY > 0.0005) {
        float2 bq = pp * 3.1 + float2(LA_TIME * 0.006, -LA_TIME * 0.0045);
        const float bh = 0.05;
        float  b0 = AcidFbm(bq);
        float2 bg = float2(AcidFbm(bq + float2(bh, 0.0)) - b0,
                           AcidFbm(bq + float2(0.0, bh)) - b0) / bh;
        float2 nd = (grad / gl) * 0.35 + bg * 0.85;
        nd /= max(1.0, length(nd));
        rOff += nd * (LA_OIL_REFR_BODY * cov);
    }
    uv = saturate(uv + rOff * float2(1.0 / aspect, 1.0));
#endif
    float3 C = Dye.SampleLevel(linearClamp, uv, 0).rgb;
#ifdef LIQUID_ACID
    // ---- oil_ink_blur ----------------------------------------------------
    // The ink under a film of oil is slightly out of focus (refs 3/4/7): the
    // marbling reads through, but softened, and more so where the oil is
    // thick. Four diagonal dye taps, weighted by thickness * coverage, taken
    // BEFORE the ink pipeline so the bands, seams and water absorption all
    // inherit the defocus instead of being blurred after the fact.
    if (LA_OIL_INK_BLUR > 0.0005) {
        float bw = saturate(LA_OIL_INK_BLUR) * thk * cov;
        if (bw > 0.002) {
            float2 bo = texelSize * (2.0 + 11.0 * saturate(LA_OIL_INK_BLUR));
            float3 b4 = 0.25 * (Dye.SampleLevel(linearClamp, uv + bo, 0).rgb
                              + Dye.SampleLevel(linearClamp, uv - bo, 0).rgb
                              + Dye.SampleLevel(linearClamp, uv + float2(bo.x, -bo.y), 0).rgb
                              + Dye.SampleLevel(linearClamp, uv - float2(bo.x, -bo.y), 0).rgb);
            C = lerp(C, b4, bw);
        }
    }
#endif
    // Raw dye intensity, before shading/filters/clamps: drives the HDR
    // highlight expansion below so it can see how hot the dye really is
    // (the clamped, filtered colour can't exceed 1.0 any more).
    float m = max(C.r, max(C.g, C.b));
#if defined(INK) || defined(LIQUID_ACID)
    const float3 C0 = C;    // raw dye: what the ink-in-water block absorbs
#endif
#ifdef INK
    // The ink look REPLACES the dye->colour step (and the emboss with it: an
    // absorbing medium has no pseudo-3D bevel, its depth cue is InkEdge).
    C = InkWater(C0, uv, texelSize, i.pos.xy, m);
#else
    if (shading > 0.5) {
        float3 L = Dye.SampleLevel(linearClamp, uv - float2(texelSize.x, 0), 0).rgb;
        float3 R = Dye.SampleLevel(linearClamp, uv + float2(texelSize.x, 0), 0).rgb;
        float3 T = Dye.SampleLevel(linearClamp, uv - float2(0, texelSize.y), 0).rgb;
        float3 B = Dye.SampleLevel(linearClamp, uv + float2(0, texelSize.y), 0).rgb;
        float dx = length(R) - length(L);
        float dy = length(B) - length(T);
        float3 n = normalize(float3(dx, dy, length(texelSize)));
        float diffuse = clamp(dot(n, float3(0, 0, 1)) + 0.7, 0.7, 1.0);
#ifdef LIQUID_ACID
        // The reference ink bands are FLAT. The fluid look's pseudo-3D emboss
        // puts a glossy bevel on every dye front, which reads as melted
        // plastic once the bands are posterised — attenuate it here instead of
        // making the user turn `shading` off (it belongs to the fluid look).
        C *= lerp(1.0, diffuse, saturate(LA_INK_SHADING));
#else
        C *= diffuse;
#endif
    }
#endif
    // The reference composited the dye into an 8-bit canvas before any CSS
    // filter touched it, so dye above 1.0 (cap 1.35, bursts 1.5) was flattened
    // to 1.0 first. Then: canvas filter (HDR compensation + hue-rotate burst),
    // then Wallpaper Engine's right-panel adjust, each primitive clamped.
    C = saturate(C);
    if (fm0.w > 0.5) {
        C = saturate(CssSaturate(C, fm0.x));
        C = saturate(C * fm0.y);
        C = saturate(CssContrast(C, fm0.z));
    }
    if (abs(fm1.x) > 0.05) C = saturate(CssHueRotate(C, fm1.x));
    C = saturate(CssSaturate(C, fm1.y));
    C = saturate(C * fm1.z);
    C = saturate(CssContrast(C, fm1.w));
    if (abs(fm2.x) > 0.05) C = saturate(CssHueRotate(C, fm2.x));

    // Response curve: remap brightness through a Gaussian hump. Hue preserved
    // (all channels scaled together); pixels below ~0.2% forced to black so the
    // background stays truly dark instead of exploding through the divide.
    if (curve.x > 0.5) {
        float x = max(C.r, max(C.g, C.b));
        float d = (x - curve.y) / max(curve.z, 0.001);
        float hump = exp(-0.5 * d * d) * curve.w;
        float scale = (x > 0.002) ? (hump / x) : 0.0;
        C *= scale;
    }

    // Shadow floor: chroma-preserving lift, strongest at black, gone above
    // the knee. Scales the pixel's own colour up toward the floor (hue and
    // saturation intact) instead of adding neutral grey, so dark marbling
    // stays visible without washing the blacklight look. Pixels that are
    // exactly zero stay zero — there is no marbling to reveal in empty fluid.
    if (shadow.x > 0.0001) {
        float lum = max(C.r, max(C.g, C.b));
        float lift = shadow.x * (1.0 - smoothstep(0.0, max(shadow.y, 0.001), lum));
        C *= (lum + lift) / max(lum, 1e-4);
    }
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
#ifdef LIQUID_ACID
    // =====================================================================
    // Liquid Acid: restyle the parity colour C as INK, then composite OIL.
    // =====================================================================
    float3 inkC;
    if (LA_INK_WATER > 0.5) {
        // ---- ink_mode=water: the SHARED ink-in-water block ---------------
        // Reads the RAW dye (C0), so it bypasses the parity CSS chain exactly
        // the way bands mode does at ink_mix ~= 1. The oil, rims, meniscus and
        // swarms below are unchanged and simply composite over this ink.
        float inkM;
        inkC = InkWater(C0, uv, texelSize, i.pos.xy, inkM);
        m = inkM;
    } else {
    // ---- INK: flat colour bands, dark seams, duotone ramp ----------------
    float inkLum = max(C.r, max(C.g, C.b));
    float bandIn = saturate(inkLum * LA_INK_GAIN + LA_INK_BIAS);
    float band   = AcidBand(bandIn, max(LA_INK_LEVELS, 1.0), clamp(LA_INK_SOFT, 0.001, 0.5));
    // Keep the BOTTOM of the range continuous: posterising the fluid's low-dye
    // regions down to level 0 turns every faint wisp into a hard-edged black
    // cut-out. Below the first band edge, fade back to the smooth value.
    band = lerp(bandIn, band, smoothstep(0.0, 1.5 / max(LA_INK_LEVELS, 1.0), bandIn));
    // chroma-preserving quantise of the sim's own colour
    inkC = C * (band / max(inkLum, 1e-4));
    // 4-stop ramp indexed by the banded luminance
    float  rs = saturate(band) * 3.0;
    int    ri = (int)floor(rs);
    float3 rampC = lerp(laInk[min(ri, 3)].rgb, laInk[min(ri + 1, 3)].rgb, rs - ri);
    // Regional hue variation: rotate the ramp by the DYE's own hue so the ink
    // still drifts (purple <-> magenta, red <-> orange) instead of reading as
    // one flat duotone. 0 = pure duotone.
    if (LA_INK_HUE_VARY > 0.01) {
        float2 cc = float2(C.r - 0.5 * (C.g + C.b), 0.8660254 * (C.g - C.b));
        // atan2(0,0) is NaN, and dye-free pixels are exactly (0,0). The NaN
        // propagates through lerp() and blacks the pixel out even where the
        // OIL covers it - that is what painted hard, fluid-shaped black smears
        // across the oil discs. Guard on the chroma magnitude.
        if (dot(cc, cc) > 1e-10)
            rampC = AcidHueShift(rampC, atan2(cc.y, cc.x) * LA_INK_HUE_VARY * 0.31831);
    }
    // Complement lock: clamp the ramp's hue into a window centred on the oil's
    // opposite. Applied AFTER the regional variation, so the ink still drifts
    // — it just drifts inside the complementary window instead of wandering
    // toward the oil hue. Near-greys have no hue to clamp, so skip them.
    if (LA_INK_LOCK > 0.5) {
        float3 rhsv = AcidRgb2Hsv(rampC);
        if (rhsv.y > 0.04) {                             // near-greys have no hue
            float d = LA_INK_TARGET_HUE - rhsv.x * 360.0;
            d = d - 360.0 * floor(d / 360.0 + 0.5);      // wrap to [-180, 180]
            float half = max(LA_INK_LOCK_SPAN, 0.0) * 0.5;
            if (abs(d) > half) rampC = AcidHueShift(rampC, d - sign(d) * half);
        }
    }
    inkC = lerp(inkC, rampC, saturate(LA_INK_MIX));
    // dark seams where |grad dye| is steep (the marbled acrylic-pour edging)
    {
        float2 st = texelSize * LA_SEAM_SCALE;
        float gx = length(Dye.SampleLevel(linearClamp, uv + float2(st.x, 0), 0).rgb)
                 - length(Dye.SampleLevel(linearClamp, uv - float2(st.x, 0), 0).rgb);
        float gy = length(Dye.SampleLevel(linearClamp, uv + float2(0, st.y), 0).rgb)
                 - length(Dye.SampleLevel(linearClamp, uv - float2(0, st.y), 0).rgb);
        float gm = sqrt(gx * gx + gy * gy);
        // Only the steepest fronts get a seam, AND only where the tap actually
        // crosses more than one colour band. Without that band-index test a
        // seam lands between two ADJACENT levels of the same hue, which draws a
        // dark ring around every band and makes each one read as a lit ridge
        // (light-dark-light) — a glossy relief instead of the references' flat
        // bands. gm * inkGain * inkLevels is the number of bands the tap spans.
        float gBands = gm * LA_INK_GAIN * max(LA_INK_LEVELS, 1.0);
        inkC *= 1.0 - LA_SEAM_STR * smoothstep(LA_SEAM_LO, max(LA_SEAM_HI, LA_SEAM_LO + 1e-3), gm)
                             * smoothstep(1.1, 2.1, gBands);
    }
    }   // end ink_mode == bands
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
    // ---- MULTICOLOUR OIL (brief AE) --------------------------------------
    // A second dye hue living IN the film, not over it. The reference the
    // user kept is a magenta film with soft cyan patches that read like a
    // light leak -- but a leak is additive and would lift the black corners,
    // and in that reference the cyan sits UNDER the masses. Only a dye in the
    // film does both, so this shifts the film's own colour and everything
    // downstream (the thin-film edge, the penumbra, the droplets' fill)
    // inherits it for free. The masses stay black over it because they are
    // drawn from the ink ramp, which this never touches.
    //
    // The hue rotation is AcidHueShift, which holds saturation and value, so
    // a patch is exactly as vivid as the palette it came from -- the W3C
    // matrix would have landed the cyan on a pastel.
    float3 oilC = oilBase;
    // brief AJ: how far, in degrees, a droplet's RIM is rotated past the film
    // it stands in, so that a rim can carry a seam that is not under it. 0 =
    // today, and with boundary_reflect_r 0 it stays 0 for every pixel.
    float rimHueD = 0.0;
    // brief BV: the patch shares, kept for the equal-load / lift split below
    // (0 when both hues are off).
    float bvK2 = 0.0, bvK3 = 0.0;
    [branch] if (LA_HUE2_AMT > 0.0005 || LA_HUE3_AMT > 0.0005) {
        float mixV = AcidMixAt(uv);
        // A soft threshold, not the raw field: the reference's patches have
        // an edge to them, and a linear ramp over the whole field is a tint.
        // The band is deliberately narrow and sits ABOVE the field's own
        // mean: in the reference the second colour is a MINORITY -- patches
        // in a film, not half the frame -- and a wide band spends most of the
        // picture in the intermediate hues, which reads as a rainbow gradient
        // rather than as two dyes meeting. 0.50..0.74 puts roughly a third of
        // the frame in the patch and keeps the magenta-to-cyan run short.
        // The band has to be NARROW, and the wider the rotation the narrower
        // it has to be. k2 sweeps the hue continuously from 0 to film_hue2,
        // so with a 180 deg contrast the transition passes through EVERY
        // intermediate hue -- at 0.50..0.74 that put a full rainbow across a
        // quarter of the frame and the picture stopped reading as two dyes
        // meeting. 0.56..0.68 keeps the rainbow as a thin rim, which is what
        // the reference actually shows at a patch edge.
        const float MIXSEAM = 0.62;   // the band's centre: THE seam (brief AJ)
        // film_hue2_seam (brief BW): the band's WIDTH around that centre, as a
        // factor of today's half-width 0.06 -- narrower = one thin seam line
        // instead of red + green stripes. 1 = today: the literal band, never
        // the runtime form (0.62 - 0.06 * 1 is not bit-identical to 0.56).
        float k2b;
        [branch] if (LA_HUE2_SEAM != 1.0)
            k2b = smoothstep(MIXSEAM - 0.06 * LA_HUE2_SEAM, MIXSEAM + 0.06 * LA_HUE2_SEAM, mixV);
        else
            k2b = smoothstep(0.56, 0.68, mixV);
#ifdef SEAM_STRETCH
        // film_hue2_seam_stretch (T8b, user 2026-11-18: "where it's short keep
        // it short, but where it's long stretch it out more ... roughly
        // exponential"): the band's width on screen is band / |grad mix|. A
        // seam under 1 % of the screen height stays; past it the band widens
        // by exp(stretch x (width - 1 %) / 4 %), at most max(6, 2 stretch)x.
        {
            float2 gm = AcidMixGrad(uv);
            float  gl = length(float2(gm.x / LA_ASPECT, gm.y));
            float  hw = 0.06 * LA_HUE2_SEAM;
            float  wn = 2.0 * hw / max(gl, 1e-4);
            float  ss = SEAM_STRETCH * 0.25;
            float  ks = min(exp(min(ss * max(wn - 0.01, 0.0) / 0.04, 4.0)), max(6.0, 2.0 * ss));
            k2b = smoothstep(MIXSEAM - hw * ks, MIXSEAM + hw * ks, mixV);
        }
#endif
        float k2 = k2b * saturate(LA_HUE2_AMT);
        float k3 = 0.0;
        // The droplets INSIDE a mass take their own share of it (the ref's
        // cyan-lit specks in the black). 1 = the same as the film.
        if (fieldB < thresh) k2 *= saturate(LA_CRUST_HUE_MIX);
        // ROTATE the hue by the patch's share of it -- do NOT cross-fade to
        // the rotated colour. lerp(magenta, cyan, 0.5) in RGB is GREY, and
        // that is exactly what the first render produced: pale lavender fog
        // instead of the reference's cyan. A dye shifts a hue; it does not
        // blend a colour with its own opposite. Rotating also keeps
        // saturation and value flat across the whole patch, so the film is as
        // vivid in the second colour as it is in the first.
        oilC = AcidHueShift(oilC, LA_HUE2_DEG * k2);
        // ...and an optional THIRD hue off the other end of the SAME field,
        // so a second colour costs no second field and no second fetch.
        [branch] if (LA_HUE3_AMT > 0.0005) {
            // film_hue3_share (brief BV): both thresholds move down together,
            // so the hue3 end of the field shrinks (1 = today's 0.18 / 0.46).
            float h3d = (1.0 - LA_HUE3_SHARE) * 0.16;
            // film_hue2_seam narrows this band the same way, about its own
            // centre 0.32 - h3d (half-width 0.14 today); 1 = the old line.
            float h3b;
            [branch] if (LA_HUE2_SEAM != 1.0)
                h3b = smoothstep(0.32 - h3d - 0.14 * LA_HUE2_SEAM, 0.32 - h3d + 0.14 * LA_HUE2_SEAM, mixV);
            else
                h3b = smoothstep(0.18 - h3d, 0.46 - h3d, mixV);
            k3 = (1.0 - h3b) * saturate(LA_HUE3_AMT);
            if (fieldB < thresh) k3 *= saturate(LA_CRUST_HUE_MIX);
            oilC = AcidHueShift(oilC, LA_HUE3_DEG * k3);
        }
        bvK2 = k2; bvK3 = k3;
        // ---- BOUNDARY REFLECTION REACH (boundary_reflect_r, brief AJ) ----
        // The user, on the hue2 seam live: "whatever algo is mixing the oil
        // boundary is insanely good", "and the way the bubbles reflect it,
        // accurately", "chef's kiss" -- then: "turn up the radius of effect
        // maybe, so further particles also reflect it on the boundary."
        //
        // Today every rim term (mass_rim, the droplet lens's meniscus and
        // specular, the bright-field halo, oil_glow, the swarm's caustics) is
        // tinted with oilC -- the film colour AT THAT PIXEL. So a droplet
        // carries the seam's colour only while it is standing IN the seam,
        // i.e. inside the 0.56..0.68 band of the mix field, which at
        // film_hue2_scale 0.35 is about one droplet across. That band's width
        // IS today's reach, and there is no radius to turn up: the term that
        // gives a rim the neighbouring film's colour is simply k2 at the
        // pixel, bounded by the width of the field's own transition.
        //
        // So give the RIMS a second, much longer reach: find how far away the
        // nearest seam is and rotate the rim's hue toward the seam's own hue
        // with a smooth falloff over that distance. The film is untouched,
        // nothing is blurred and no texture is sampled -- the probe walks the
        // 20x12 field, which is 240 floats already resident in the cbuffer, so
        // a "tap" is four indexed loads next to the dozens of TEXTURE taps the
        // depth of field alone already spends. Eight of them, no long tap
        // chains and no extra blur, so the rims stay exactly as sharp.
        //
        // Four of the taps are a central difference at a QUARTER of the reach
        // -- the field's large-scale slope, which points at the nearest seam.
        // (Not the analytic derivative of the bilinear: its x term vanishes on
        // every vertical cell line, which would print the 20x12 grid.) The
        // other four MARCH that direction in equal steps and take the first
        // segment whose ends straddle the seam. Marching, rather than testing
        // only the far end of one long ray, is what makes the reach monotonic:
        // a single ray that runs past a patch and out the other side comes
        // back with the SAME sign and reports no seam at all, so raising the
        // radius was losing droplets that a shorter radius had found.
        [branch] if (LA_REFLECT_R > 0.0005) {
            float  R  = max(LA_REFLECT_R, 1e-4);
            float  s0 = mixV - MIXSEAM;
            float  e  = R * 0.25;
            float2 ex = float2(e / aspect, 0.0), ey = float2(0.0, e);
            float2 g  = float2(AcidMixAt(uv + ex) - AcidMixAt(uv - ex),
                               AcidMixAt(uv + ey) - AcidMixAt(uv - ey));
            // toward the seam: down the slope from above it, up from below
            float2 dir = (g / max(length(g), 1e-6)) * -sign(s0);
            float dN = 1.0, sp = s0;
            [unroll] for (int mi = 1; mi <= 4; mi++) {
                float t  = (float)mi * 0.25;
                float2 o = dir * (R * t);
                float  sn = AcidMixAt(uv + float2(o.x / aspect, o.y)) - MIXSEAM;
                if (dN >= 1.0 && sp * sn < 0.0)
                    dN = t - 0.25 * (1.0 - saturate(-sp / (sn - sp)));
                sp = sn;
            }
            // near = full, far = 0, smootherstep so the nearest stay strongest
            // and the reach dies out with no edge of its own.
            float w = saturate(LA_REFLECT_AMT) * (1.0 - smoothstep(0.0, 1.0, dN));
            // The seam's own colour is the band's MIDPOINT -- half the
            // rotation, which off a blue film at film_hue2 180 is the magenta
            // the user photographed. Rotating by the DELTA (k at the seam
            // minus k here) means a rim already standing in the seam changes
            // by nothing, so the near droplets the user already loves keep
            // exactly the colour they have.
            float k2s = 0.5 * saturate(LA_HUE2_AMT);
            if (fieldB < thresh) k2s *= saturate(LA_CRUST_HUE_MIX);
            rimHueD = LA_HUE2_DEG * (k2s - k2);
            [branch] if (LA_HUE3_AMT > 0.0005) {
                float h3d = (1.0 - LA_HUE3_SHARE) * 0.16;
                float h3lo = 0.18 - h3d, h3hi = 0.46 - h3d;
                // film_hue2_seam (brief BW): the reflection reads the SAME
                // (narrowed) bands as the film, or a rim would carry a seam
                // colour the film beside it no longer shows.
                [branch] if (LA_HUE2_SEAM != 1.0) {
                    h3lo = 0.32 - h3d - 0.14 * LA_HUE2_SEAM;
                    h3hi = 0.32 - h3d + 0.14 * LA_HUE2_SEAM;
                }
                float k3s = (1.0 - smoothstep(h3lo, h3hi, MIXSEAM)) * saturate(LA_HUE3_AMT);
                if (fieldB < thresh) k3s *= saturate(LA_CRUST_HUE_MIX);
                rimHueD += LA_HUE3_DEG * (k3s - k3);
                // brief BV (AJ seam reach): the march above only looks for the
                // hue2 seam, so a rim beside a hue3 patch never took its seam.
                // March to the hue3 seam too (same slope, four more taps) and
                // blend the two targets by reach, normalised so they never add
                // past one full seam rotation.
                const float S3 = 0.5 * (h3lo + h3hi);
                float  t0 = mixV - S3;
                float2 dir3 = (g / max(length(g), 1e-6)) * -sign(t0);
                float dN3 = 1.0, tp = t0;
                [unroll] for (int mj = 1; mj <= 4; mj++) {
                    float t  = (float)mj * 0.25;
                    float2 o = dir3 * (R * t);
                    float  tn = AcidMixAt(uv + float2(o.x / aspect, o.y)) - S3;
                    if (dN3 >= 1.0 && tp * tn < 0.0)
                        dN3 = t - 0.25 * (1.0 - saturate(-tp / (tn - tp)));
                    tp = tn;
                }
                float w3 = saturate(LA_REFLECT_AMT) * (1.0 - smoothstep(0.0, 1.0, dN3));
                float k3m = 0.5 * saturate(LA_HUE3_AMT);
                float k2mb;
                [branch] if (LA_HUE2_SEAM != 1.0)
                    k2mb = smoothstep(MIXSEAM - 0.06 * LA_HUE2_SEAM, MIXSEAM + 0.06 * LA_HUE2_SEAM, S3);
                else
                    k2mb = smoothstep(0.56, 0.68, S3);
                float k2m = k2mb * saturate(LA_HUE2_AMT);
                if (fieldB < thresh) { k3m *= saturate(LA_CRUST_HUE_MIX); k2m *= saturate(LA_CRUST_HUE_MIX); }
                float d3 = LA_HUE2_DEG * (k2m - k2) + LA_HUE3_DEG * (k3m - k3);
                rimHueD = (rimHueD * w + d3 * w3) / max(w + w3, 1.0);
            } else {
                rimHueD *= w;
            }
        }
    })hlsl"
// (split: MSVC caps a single string literal at 16380 bytes; Oil3D L3b split
// here, before lamp 1's window over the film, for the OIL_DYEWHITE lines)
R"hlsl(
#ifdef OIL_LAMP
    // ---- Oil3D L1: lamp 1's window over the film (OilLampFilm) ------------
    // Right after the film's dyes are known (bvK2 = the hue2 share) and before
    // every light term: the footprint, the C2 column shade, the lamp shadows,
    // the thin edge and the rims all work on the lamp-lit film, so they take
    // its colour. The masses and droplets are the ink: never touched.
#ifdef OIL_FLUIDB
    // ---- Oil3D L3 (2): FLUID B on the hue2 concentration field (OIL_FLUIDB)
    // dye_b1 / dye_b2 live in fluid B, the hue2 patches. With film_hue2_amt
    // 0 today draws no patch and the block above never ran, yet fluid B is
    // there: f = the SAME field through the SAME band (film_hue2_seam, the
    // crust share), so the canon patch rules hold (photo-sized, off-screen
    // seeded, rising, film_hue2_cover). Both lamps read their LUT by f (the
    // CPU builds it over f when hue2 is off); with hue2 on, f = bvK2 as before.
    float bvF = bvK2;
    [branch] if (LA_HUE2_AMT <= 0.0005) {
        float mixF = AcidMixAt(uv);
        [branch] if (LA_HUE2_SEAM != 1.0)
            bvF = smoothstep(0.62 - 0.06 * LA_HUE2_SEAM, 0.62 + 0.06 * LA_HUE2_SEAM, mixF);
        else
            bvF = smoothstep(0.56, 0.68, mixF);
        if (fieldB < thresh) bvF *= saturate(LA_CRUST_HUE_MIX);
    }
    float4 lampL = OilLampLut(bvF);
#else
    float4 lampL = OilLampLut(bvK2);
#endif
    // Oil3D L1b: TODAY'S film rides along. oilW (then wtCol, CW, linW) takes
    // every film and composite step below too (the #ifdef OIL_LAMP lines), so
    // the end of the pass holds this pixel's output under the WHITE state
    // beside the windowed one, and the cap compares the two there.
    float3 oilW = oilC;
#ifdef OIL_MASSDYE
    float3 oilW0 = oilW;   // L3 (3): today's film here, before any light term (the dark oil's level)
#endif
    oilC = OilLampFilm(oilC, lampL);
#ifdef OIL_DYEWHITE
    // L3b: the white track = the same dyed film under white light (laLampW);
    // lamp 2's own LUT is relative to TODAY's colour, so it keeps oilT0
    float3 oilT0 = oilW;
#ifdef OIL_FLUIDB
    oilW = OilLampFilm(oilW, OilLampWLut(bvF));
#else
    oilW = OilLampFilm(oilW, OilLampWLut(bvK2));
#endif
#endif
#ifdef OIL_LAMP2
    // ---- Oil3D L2: LAMP 2's film, beside lamp 1's ---------------------------
    // The same film as lamp 2 ALONE lights it (its own relative LUT, applied
    // to today's colour like lamp 1's), at lamp2Z x today's Y: lamp2Z = the
    // share lamp 2 actually DELIVERS here (z, unless OilLampFilm's gamut guard
    // took some), the normaliser of the sum below (THE SUM). It takes the
    // light terms lamp 1 takes (the ramp, the footprint) and its OWN
    // occlusion; never lamp 1's shadows or the C2b column, so where lamp 1 is
    // blocked lamp 2's colour is what is left.
#ifdef OIL_FLUIDB
    float4 lamp2L = OilLamp2Lut(bvF);
#else
    float4 lamp2L = OilLamp2Lut(bvK2);
#endif
#ifdef OIL_DYEWHITE
    float3 oil2   = OilLampFilm(oilT0, lamp2L);
#else
    float3 oil2   = OilLampFilm(oilW, lamp2L);
#endif
    float  l2Y2u, l2YWu;                       // unoccluded Ys (debug view)
    float  lamp2Z = OilLamp2Share(oil2, oilW, l2Y2u, l2YWu);
#ifdef OIL_L2FILL
    // ---- Oil3D L3 (1): the FILLED reference's lamp-2 half (OIL_L2FILL) -----
    // oilW2 = the WHITE film as a white lamp 2 alone lights it: today's colour
    // with the light terms lamp 2 shares (the ramp, the footprint) and lamp
    // 2's OWN occlusion, never lamp 1's. At THE SUM the cap's reference becomes
    // the white two-lamp state (oilW + L2_WLEVEL oilW2) / (1 + L2_WLEVEL).
    float3 oilW2 = oilW;
#endif
#endif
#endif
#if defined(OIL_MASSDYE) && !defined(OIL_LAMP)
    // Oil3D L3b: the dark-dye oil under WHITE lamps (lamp 1 white, no film dyes,
    // lamp 2 off) runs this variant WITHOUT the lamp path: no window, no white
    // track, no output cap, so the film and every droplet are today's to the bit
    // (the lamp path's noise-level track differences, rectified by the cap's
    // one-FP16-step margin, moved ~7 % of the lit film one code DOWN). Its level:
    float3 oilW0 = oilC;   // today's film here, before any light term
#endif
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
    // rise_bottom_light: a lava lamp is lit and heated from BELOW, so the wax
    // near the base is hotter and brighter and cools on the way up. One
    // vertical ramp on the oil (not on the ink: the glass is not lit, the wax
    // is), applied here so it also feeds the film's own scattered light.
#ifdef DROP_LIFE
    // droplet_core: the film BEHIND a hole, lit but before any shadow term (the
    // droplet's own contact shadow would darken its own lens centre); taken
    // again below once the footprint and the column have lit it
    float3 dlFilm = oilC;
#ifdef OIL_LAMP
    float3 dlFilmW = oilW;
#endif
#endif
    float lampG = 1.0;
    if (LA_RISE_BOTTOM_LIGHT > 0.0005) {
        lampG = lerp(1.0, 0.80 + 0.50 * smoothstep(0.0, 1.0, uv.y), saturate(LA_RISE_BOTTOM_LIGHT));
        oilC *= lampG;
#ifdef OIL_LAMP
        oilW *= lampG;
#ifdef OIL_LAMP2
        oil2 *= lampG;
        lamp2Z = OilLamp2Share(oil2, oilW, l2Y2u, l2YWu);
#ifdef OIL_L2FILL
        oilW2 *= lampG;
#endif
#endif
#endif
    }
#ifdef OIL_COLUMN
    // Oil3D C2: the lit column at this pixel (1 = open water) and the shade
    // it puts on the film's light (linear, 1 = today), read again by the HDR
    // highlight level below. OIL_COLUMN is its own acid PSO variant
    // (OIL_HEIGHT + OIL_COLUMN, built when water_scatter > 0 with a tank):
    // the OIL_HEIGHT PSO without it compiles to e615b23's bytecode, so
    // water_scatter 0 is today to the bit, not just to the eye.
    float wsLit = 1.0, wsShade = 1.0;
#endif
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
#if defined(OIL_HEIGHT) && !defined(OIL_SURFACE)
    // ---- Oil3D C1: ONE LAMP over the tank (tank_depth > 0) ----------------
    // The FOOTPRINT: the lamp's cone on the film, an ellipse toward the light
    // stretched along its azimuth by 1/sin(elev), on the 0.85..1.30 scale
    // (rise_bottom_light's was 0.80..1.30); it takes over from that
    // ramp (and lamp_grey, on the CPU) as tank_depth goes 0 -> 0.1.
    // The SHADOWS: the oil's shadow on the column/floor, read THROUGH the film
    // (only the film's share of light from below, SH_UNDER <= 5 %, darkens),
    // and shadow_on_film: the CONTACT shadow of each mass on the film it
    // floats on, one soft ramp lambda cot(elev) long on its far side. The
    // film's plateau is a multiplier, never a cut; the black masses are not
    // touched (oilC only reaches them through alpha).
    [branch] if (LA_TANK_FADE > 0.0005) {
        float2 ld  = float2(LA_SH_OFF_X * aspect, LA_SH_OFF_Y);
        ld /= max(length(ld), 1e-6);
        // aimed 0.6 frame heights from the centre toward the lamp, sigma 0.8:
        // the frame mean stays ~1.03, today's ramp mean ~1.05 (no dimming)
        float2 q   = pp - (float2(0.5 * aspect, 0.5) + 0.6 * ld);
        float  qa  = dot(q, ld) / max(LA_FP_STRETCH, 1.0);
        float  qb  = dot(q, float2(-ld.y, ld.x));
        float  fp  = 0.85 + 0.45 * exp(-(qa * qa + qb * qb) / 1.28);
        float  lT  = lerp(lampG, fp, saturate(LA_TANK_FADE));
        oilC *= lT / max(lampG, 1e-4);
#ifdef OIL_LAMP
        oilW *= lT / max(lampG, 1e-4);
#ifdef OIL_LAMP2
        oil2 *= lT / max(lampG, 1e-4);   // L2: the tank's light pool, shared
        lamp2Z = OilLamp2Share(oil2, oilW, l2Y2u, l2YWu);   // the last shared term: occlusion follows
#ifdef OIL_L2FILL
        oilW2 *= lT / max(lampG, 1e-4);
#endif
#endif
#endif
        lampG = lT;
        // every surface-buffer read below starts from the SCREEN pixel
        float2 suv = OilSurfUV(i.pos.xy);
        // ---- Oil3D C2: SHAFTS, the lit column under the film --------------
        // (port chat re-ruling) TODAY IS THE FULLY LIT STATE: an unoccluded
        // column under the lamp is what the film has always shown, so the
        // column adds no light; the masses REMOVE it. wsLit = the column's
        // T-weighted lit fraction (exactly 1 in open water: the coverage there
        // is exactly 0), WS_SHARE = the share of the film's light a fully
        // shadowed column takes away (oil_transparency x L / (1 + L), L the
        // column's in-scatter against the direct light). In LINEAR light the
        // film keeps 1 - WS_SHARE (1 - wsLit): open water is today to the
        // bit, and the water beside each mass goes darker in a soft
        // volumetric shadow column (penumbra growing with depth, 16
        // transmission-stepped taps) that streams away from the lamp, so the
        // shafts are lit water standing between them. A multiplier on the
        // film's plateau, never a cut; the masses themselves are untouched.
#ifdef OIL_COLUMN
        [branch] if (LA_WS_SHARE > 0.0) {
            wsLit   = OilColumnLit(suv, i.pos.xy);
            wsShade = 1.0 - LA_WS_SHARE * (1.0 - wsLit);
            oilC   *= pow(wsShade, 1.0 / 2.2);
#ifdef OIL_LAMP
            oilW   *= pow(wsShade, 1.0 / 2.2);
#endif
#ifdef OIL_MURK
            // ---- murk_light: the silt shades the FILM's own light, in linear
            // light, the same plateau the column share scales (black stays
            // black, the masses untouched): 1 + murk_light (d_col - 1), d_col
            // the column's mean density (mean 1 over the tile, so the frame
            // stays ~1). murk_light 0 = a straight x 1.0. Before W2's gain,
            // whose normaliser is scale-free and divides by wsShade alone.
            {
                float mlG = pow(1.0 + laMurk[1].w * (g_wsMurkMean - 1.0), 1.0 / 2.2);
                oilC *= mlG;
#ifdef OIL_LAMP
                oilW *= mlG;
#endif
            }
#endif
#ifdef OIL_GRAIN
            // ---- Oil3D W3: FINE SILT + DEPTH HAZE on the film's light ---------
            // Linear, against today's film (wsShade; murk_light's gain is
            // scale-free): the column's single-scatter share WS_SHARE x wsLit
            // carries the lit slices' grain (gA x 0.6 x gC); the haze replaces
            // the direct part 1 - WS_SHARE by veil, 1 - T(1) of it (T(1) =
            // e^-laGrain[0].z: the film lies under the whole column), lift
            // bright, carrying the unshadowed grain (gA x 0.4 x gH: multiple
            // scattering averages it) and no shadow columns
            // (turbid water fills its own shadows). Grain mean 0 + lift 1 =
            // the open film keeps its level.
            {
                float gC, gH;
                OilGrainField(suv, i.pos.xy, gC, gH);
                const float gA = laGrain[0].x;
                const float tF = exp(-laGrain[0].z);
                const float vg = max(1.0 + gA * 0.4 * gH, 0.0);
                const float ws = max(wsShade, 1e-4);
                float fN = (1.0 - LA_WS_SHARE) * (tF + laGrain[0].w * (1.0 - tF) * vg)
                         + LA_WS_SHARE * (wsLit + gA * 0.6 * gC);
                fN = max(fN, 1e-4);
                const float gk = pow(fN / ws, 1.0 / 2.2);
                oilC *= gk;
#ifdef OIL_LAMP
                oilW *= gk;
#endif
            }
#endif
#ifdef OIL_WTINT
            // ---- Oil3D W2: the column's share takes the water's colour (the
            // murk's excess the silt's): the gain OVER wsShade, which the lines
            // above applied already (OilWaterGain's normaliser is scale-free)
            {
#ifdef OIL_MURK
                const float litU = g_wsLitU;
#else
                const float litU = wsLit;
#endif
                const float ws = max(wsShade, 1e-4);
                oilC *= pow(OilWaterGain(oilC, wsLit, litU) / ws, 1.0 / 2.2);
#ifdef OIL_LAMP
                oilW *= pow(OilWaterGain(oilW, wsLit, litU) / ws, 1.0 / 2.2);
#endif
            }
#endif
#ifdef OIL_MOTES
            // ---- Oil3D C3: the MOTES add their light through the film (its
            // share WS_SHARE, like the column) in the film's OWN colour, in
            // linear light: no new hue, black stays black; under a mass (the
            // coverage at this pixel) nothing shows. 0 almost everywhere.
            {
                float cov = OilMassCovAt(suv);
                [branch] if (cov < 1.0) {   // inside a mass the product below is 0: skip the walk
                    float mt = OilMotes(suv) * (1.0 - cov);
                    [branch] if (mt > 0.0) {
                        float mg = pow(1.0 + LA_WS_SHARE * laMote[2].x * mt, 1.0 / 2.2);
                        oilC *= mg;
#ifdef OIL_LAMP
                        oilW *= mg;
#endif
#ifdef OIL_WTINT
                        {   // W2: a mote is silt: its light takes S x W (same luminance)
                            float sm = LA_WS_SHARE * laMote[2].x * mt;
                            oilC *= pow((1.0 + sm * OilSiltTint(oilC)) / (1.0 + sm), 1.0 / 2.2);
#ifdef OIL_LAMP
                            oilW *= pow((1.0 + sm * OilSiltTint(oilW)) / (1.0 + sm), 1.0 / 2.2);
#endif
                        }
#endif
                    }
                }
            }
#endif
        }
#ifdef DROP_LIFE
        dlFilm = oilC;     // droplet_core: lit by the footprint + the column, not yet shadowed
#ifdef OIL_LAMP
        dlFilmW = oilW;
#endif
#endif
        // under-layer (displaced by the depth, <= 5 %) x contact (on the film);
        // with the column lit the under-layer reads the PADDED coverage too
#ifdef OIL_LAMP
        { float shU = 1.0 - (1.0 - OilLampShadowCov(suv, i.pos.xy)) * LA_SH_UNDER; oilC *= shU; oilW *= shU; }
#else
        oilC *= 1.0 - (1.0 - OilLampShadowCov(suv, i.pos.xy)) * LA_SH_UNDER;
#endif
#else
        // under-layer (displaced by the depth, <= 5 %) x contact (on the film)
#ifdef OIL_LAMP
        { float shU = 1.0 - (1.0 - OilLampShadow(suv, i.pos.xy)) * LA_SH_UNDER; oilC *= shU; oilW *= shU; }
#else
        oilC *= 1.0 - (1.0 - OilLampShadow(suv, i.pos.xy)) * LA_SH_UNDER;
#endif
#endif
#ifdef OIL_LAMP
        [branch] if (LA_SH_FILM > 0.0005) {
            float shF = 1.0 - OilContactShadow(suv, ld) * saturate(LA_SH_FILM);
            oilC *= shF; oilW *= shF;
        }
#else
        [branch] if (LA_SH_FILM > 0.0005)
            oilC *= 1.0 - OilContactShadow(suv, ld) * saturate(LA_SH_FILM);
#endif
#ifdef OIL_LAMP2
        // ---- Oil3D L2: lamp 2's OWN occlusion (lamp 1's laws, lamp 2's
        // direction and softness): the under-layer at the same <= 5 % share
        // and shadow_on_film's contact shadow toward lamp 2. Lamp 2 only:
        // lamp 1's track and the white track never see these.
#ifdef OIL_L2FILL
        // (L3 filled: the same factors also on oilW2, the reference's lamp 2)
        [branch] if (LA_SH_UNDER > 0.0005) {
            float sh2 = 1.0 - (1.0 - OilLamp2Shadow(suv, i.pos.xy)) * LA_SH_UNDER;
            oil2 *= sh2; oilW2 *= sh2;
        }
        [branch] if (LA_SH_FILM > 0.0005) {
            float2 ld2 = float2(LA_L2_OFF_X * aspect, LA_L2_OFF_Y);
            ld2 /= max(length(ld2), 1e-6);
            float sf2 = 1.0 - OilContactShadow2(suv, ld2) * saturate(LA_SH_FILM);
            oil2 *= sf2; oilW2 *= sf2;
        }
#else
        [branch] if (LA_SH_UNDER > 0.0005)
            oil2 *= 1.0 - (1.0 - OilLamp2Shadow(suv, i.pos.xy)) * LA_SH_UNDER;
        [branch] if (LA_SH_FILM > 0.0005) {
            float2 ld2 = float2(LA_L2_OFF_X * aspect, LA_L2_OFF_Y);
            ld2 /= max(length(ld2), 1e-6);
            oil2 *= 1.0 - OilContactShadow2(suv, ld2) * saturate(LA_SH_FILM);
        }
#endif
#endif
    }
#endif
#ifdef OIL_LAMP2
    // ---- Oil3D L2: THE SUM of the two lamps --------------------------------
    // Light adds in LINEAR light: film = lamp 1's (oilC, its window, its
    // shadows, the column) + lamp 2's (oil2, its window, its own shadows),
    // over 1 + lamp2Z, lamp 2's delivered share where nothing is blocked, so
    // open water keeps today's luminance exactly (frame mean, ABL) and the hue
    // moves by that share alone (level 0.3 -> <= 23 %: the accent). Where lamp 1 is
    // blocked and lamp 2 is not, the share swings to lamp 2: the shadow picks
    // lamp 2's LUT (the coloured shadow). Where lamp 2 is blocked, its share
    // falls: a shadow of lamp 2 in lamp 1's colour.
    // THE CAP, at the film already: the sum's Y may not exceed the white
    // track's (oilW: today's film with lamp 1's shadows), so lamp 2 re-colours
    // lamp 1's shadows at today's depth but never lifts them (a second lamp
    // only takes light from lamp 1, the white state is the most light there
    // is). No channel clamp here: past the footprint (up to 1.30) the film may
    // run over 1 and both tracks clip at the composite's saturate() as today.
    // The binding per-pixel cap stays the output one at the end of PSMain (it
    // now holds the SUM). The ink seen through the film takes the same
    // two-lamp level.
    {
        const float3 WY = float3(0.2126, 0.7152, 0.0722);
        float3 lS = (DsToLin(oilC) + DsToLin(oil2)) / (1.0 + lamp2Z);
        float  yS = dot(lS, WY);
        float  yW = dot(DsToLin(oilW), WY);
#ifdef OIL_L2FILL
        // Oil3D L3 (1), THE FILLED REFERENCE (port chat cap correction): the
        // sum is already exposure-normalised (open water = today), so the cap
        // above only removed lamp 2's physical FILL of lamp 1's shadows. The
        // reference is the same pixel under the WHITE two-lamp state at the
        // same level and occlusions: open water = today exactly (oilW2 =
        // oilW there), a lamp-1 shadow g deep lifts by <= Z g / (1 + Z), never
        // above open water. Windows only remove light from THAT. It becomes the
        // white track (oilW) below, so the output cap holds it through every
        // later step. L2_WLEVEL = min(lamp2_level, 1), a white lamp 2's share.
        float3 lR = (DsToLin(oilW) + LA_L2_WLEVEL * DsToLin(oilW2)) / (1.0 + LA_L2_WLEVEL);
        float  yR = dot(lR, WY);
        float  s  = (yS > yR) ? yR / max(yS, 1e-12) : 1.0;
#else
        float  s  = (yS > yW) ? yW / max(yS, 1e-12) : 1.0;
#endif
        // --shot-debug lamps (LA_OIL_H_DEBUG -1, the reject criteria's map; no
        // older debug branch tests below 0.5): r = lamp 2's share of the
        // film's light (who WINS: > 0.5 = lamp 2), g = lamp 1's occlusion (the
        // white track's loss to lamp 1's shadows + the column), b = lamp 2's
        // own occlusion; the film's values at EVERY pixel (mask the masses
        // with the plain render). The png byte IS the value, as in debug 7.
        [branch] if (LA_OIL_H_DEBUG < -0.5) {
            float y2 = dot(DsToLin(oil2), WY);
            float3 v = float3(y2 / (1.0 + lamp2Z) / max(yS, 1e-8),
                              (l2YWu > 1e-6) ? 1.0 - yW / l2YWu : 0.0,
                              (l2Y2u > 1e-6) ? 1.0 - y2 / l2Y2u : 0.0);
            return float4(SRGBToLinear(saturate(v)) * sdrScale, 1.0);
        }
        oilC = DsToSrgb(lS * s);
        lampL.z = min((lampL.z + lamp2Z) / (1.0 + lamp2Z), 1.0);
#ifdef OIL_L2FILL
        oilW = DsToSrgb(lR);
#endif
    }
#endif
#if defined(OIL_STAGETAP) && (OIL_STAGETAP == 1 || OIL_STAGETAP == 101)
    return OilTapLin((OIL_STAGETAP > 100) ? OIL_TAPW(oilC, oilW) : oilC);
#endif
    // ---- COLOUR SCHEMES: equal load (brief BV) -----------------------------
    // film_equal_load, per pixel, the BASE film (1 - k2 - k3) only -- patches
    // are never touched, so a yellow patch stays bright: dimmed in linear
    // light to the luminance the same S/V would have at magenta (325), never
    // brightened, so a yellow/green/cyan film loads the panel like magenta.
    // 0 = today: the branch is skipped. (The yellow-patch lifts were dropped:
    // patches sit at V 0.92..1 with nothing to lift, MAD 0.49 / 0.01.)
    // film_equal_load_patches (brief BW): the same target on the PATCH
    // CORES too (mint/cyan dimmed to the magenta load), weighted by
    // smoothstep(0.8, 1, k) -- never by raw k, or the seam's yellow-orange
    // (k ~0.3-0.6) would be dimmed to olive. 0 = today: the inner branch is
    // skipped and the weight is the old expression.
    [branch] if (LA_EQUAL_LOAD > 0.0005 || LA_EQUAL_LOAD_P > 0.0005) {
        const float3 WY = float3(0.2126, 0.7152, 0.0722);
        float3 hsv = AcidRgb2Hsv(oilC);
        float  wb  = saturate(LA_EQUAL_LOAD) * saturate(1.0 - bvK2 - bvK3);
        [branch] if (LA_EQUAL_LOAD_P > 0.0005)
            wb = saturate(wb + saturate(LA_EQUAL_LOAD_P)
                             * saturate(smoothstep(0.8, 1.0, bvK2) + smoothstep(0.8, 1.0, bvK3)));
        float  Y   = dot(DsToLin(oilC), WY);
        float  Yt  = dot(DsToLin(AcidHsv2Rgb(float3(0.9027778, hsv.y, hsv.z))), WY);
        float  gl  = lerp(1.0, min(1.0, Yt / max(Y, 1e-6)), wb);
        oilC = DsToSrgb(DsToLin(oilC) * gl);
#ifdef OIL_LAMP
        {   // the white track's own load (its hue and chroma are today's)
            float3 hsvW = AcidRgb2Hsv(oilW);
            float  YW   = dot(DsToLin(oilW), WY);
            float  YtW  = dot(DsToLin(AcidHsv2Rgb(float3(0.9027778, hsvW.y, hsvW.z))), WY);
            oilW = DsToSrgb(DsToLin(oilW) * lerp(1.0, min(1.0, YtW / max(YW, 1e-6)), wb));
        }
#endif
    }
#if defined(OIL_STAGETAP) && (OIL_STAGETAP == 2 || OIL_STAGETAP == 102)
    return OilTapLin((OIL_STAGETAP > 100) ? OIL_TAPW(oilC, oilW) : oilC);
#endif
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
    // ---- DYE THE DARK MASSES (brief AG / AM) -----------------------------
    // TRACE (2026-09-22, branch dye4) -- where a dark-mass pixel gets its
    // final colour in ink_mode=water, end to end:
    //   1. src\shaders.h, acid display literal, further down past this block:
    //      `float3 col = lerp(inkC, oilC, alpha);`  -- inside a mass the oil
    //      field is BELOW the threshold, so alpha -> 0 and the pixel IS inkC.
    //   2. same file, top of the LIQUID_ACID block ("restyle the parity colour
    //      C as INK"): `if (LA_INK_WATER > 0.5) inkC = InkWater(C0,..)`
    //      LA_INK_WATER = (ink_mode == water) (src\acid_slots.h).
    //      acid-rise-12 sets ink_mode=water, so the whole `else` bands branch
    //      under it is dead code for this preset.
    //   3. InkWater() (the shared INK block) returns
    //      `lerp(ikPaper.rgb, tint*.., op)`. In a
    //      mass the sim dye density is ~0, so op ~= 0 and the pixel is
    //      ikPaper.rgb = [ink] paper_color = 0 0 0. THAT is the black.
    //   4. laInk[] (= effInk, the ramp the first two attempts dyed on the CPU)
    //      is read in exactly TWO places in this whole shader: the bands
    //      branch's `rampC` (dead here) and the toe_tint lift (the
    //      `if (LA_TOE_TINT > 0.001)` block), gated on LA_TOE_TINT = toe_tint,
    //      which acid-rise-12 leaves at its 0 default. So dyeing effInk could not change one bit of this preset
    //      -- which is what the byte-identical four-hue sheet was measuring,
    //      and why both earlier fixes read delta 0.
    // So the dye is applied HERE, on inkC, as a deep translucent wax:
    // thickness = depth into the mass (-sdf), light left = exp(-depth/w), so
    // the thin edge passes most of the lamp and the thick core keeps a floor
    // of the same hue instead of crushing to black. Gated by (1 - cov) so the
    // film is untouched, by the lamp ramp so the wax is lit from below like
    // the oil is, and rolled off where the ink film is actually BRIGHT -- this
    // dyes the black, nothing else. crust and mass_rim are added to `col`
    // further down and still read against it.
    // LA_DYE_AMT = 0 (dye_sat 0 or dye_lum 0) skips the branch: byte-identical.
    // brief BK: ...unless the droplets carry a dye of their own (DYE_DROP_RGB
    // >= 0, see below); it is -1 whenever the split is off, so this is the old
    // test for every existing preset.
#ifdef OIL_MASSDYE
    float3 mdDelta = 0.0;   // L3 (3): the windowed masses' change, composite only
#endif
    [branch] if (LA_DYE_AMT > 0.0005 || LA_DYE_DROP_RGB > -0.5) {
        // value 1: the amount and the thickness carry the level, so the hue is
        // as vivid deep in the mass as it is at the rim.
        float3 dyeC = InkHsv2Rgb(float3(LA_DYE_HUE, LA_DYE_SAT, 1.0));
        float dIn  = max(-sdf, 0.0);                 // p-units into the mass
        float Tw   = exp(-dIn / 0.055);              // light left after the wax
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
        // ---- brief AG-b: dye GRADIENTS, so the masses are not all one wax --
        // User: "there should probably be gradients or something, so they
        // aren't all the same." Every mass gets an IDENTITY id in 0..1:
        //   * a hole blob carries a hash in c.w (CPU, fixed for its life,
        //     re-rolled only when it respawns off-screen); the blob loop
        //     soft-maxed them with w^4 into idS / idW, so merging holes blend;
        //   * a gap between oil blobs has no hole near it (idW ~ 0) and falls
        //     back to a slow fbm at mass scale (~0.4 screen) that climbs with
        //     rise_speed, so a mass keeps its shade while it rises. NOT the
        //     hue2 field: that would couple the dye to the film's second hue.
        // dye_lum_vary scales the level by 1 +- value, dye_hue_vary turns the
        // hue by +- value degrees, and dye_thick_hue turns it with thickness
        // (thin edge = dye_hue, thick core = dye_hue + value; Tw saturates in
        // a small mass, so it stays edge-coloured and a big one gets a core).
        // MASSES only: mKv is BK's mass-territory test (the blob-only surface,
        // 3 px into the sheet), 0 on a droplet hole, so droplets keep dyeC.
        // All three 0 skips the block and dyeC is the old line.
        [branch] if (LA_DYE_LUM_VARY > 0.0005 || LA_DYE_HUE_VARY > 0.0005 ||
                     abs(LA_DYE_THICK_HUE) > 0.05) {
            float sBv = (fieldB - thresh) / max(length(gradB), 1e-6);
            float mKv = 1.0 - smoothstep(0.0, 3.0 * PX1440 + saturate(LA_DYE_SMOKE) * 0.030, sBv);
            float dh = LA_DYE_THICK_HUE * (1.0 - Tw);    // Tw = 1 at the edge
            float lk = 1.0;
            [branch] if (dyeIdOn) {
                float  fb  = AcidFbm(float2(pp.x, pp.y + LA_TIME * LA_DYE_ID_RISE) * 2.5
                                     + float2(7.31, 1.93));
                float  idF = saturate(0.5 + (fb * 1.143 - 0.5) * 2.2);
                // A hole's identity counts only where that hole CARVED the
                // mass: the oil-only field (fieldB minus the holes' own) is
                // above the isoline there. A hole that has drifted into a gap
                // carves nothing, and would otherwise print a disc of its own
                // colour inside that gap's mass (first sheet, hue_vary 90).
                float  kH  = smoothstep(0.0, 0.02, idW) *
                             smoothstep(thresh - 0.05, thresh + 0.20, fieldB - idH);
                float  id  = lerp(idF, idS / max(idW, 1e-20), kH);
                float  sg  = 2.0 * id - 1.0;
                dh += LA_DYE_HUE_VARY * sg;
                lk  = 1.0 + LA_DYE_LUM_VARY * sg;          // 0.5 .. 1.5
            }
            dyeC = lerp(dyeC, InkHsv2Rgb(float3(LA_DYE_HUE + dh * (1.0 / 360.0), LA_DYE_SAT, lk)), mKv);
        }
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
        // ---- brief BL: dye_lamp_follow (ambient dye) ---------------------
        // The lamp lights the OIL; the fluid under it should only see a dim
        // ambient. At 1 (today) the dye rides lampG (rise_bottom_light) and
        // the thin-edge profile lerp(dye_core, 1, Tw); toward 0 both fade out
        // and the dye sits at its thick-core level everywhere: a colour the
        // lamp never lifts. At 1 the branch is skipped and lampGd IS lampG.
        float lampGd = lampG;
        [branch] if (LA_DYE_LAMP_FOLLOW < 0.9995) {
            float fl = saturate(LA_DYE_LAMP_FOLLOW);
            lampGd = lerp(1.0, lampG, fl);
            Tw *= fl;
        }
        float inkL = max(inkC.r, max(inkC.g, inkC.b));
        float dkG  = 1.0 - smoothstep(0.0, 0.55, inkL);
        float wD   = (1.0 - cov) * dkG;
        float3 dyeAdd = dyeC * (LA_DYE_AMT * lerp(LA_DYE_CORE, 1.0, Tw) * lampGd * wD);
        [branch] if (LA_DARK_SAT > 0.0005)
            dyeAdd = DarkSat(dyeC * LA_DYE_AMT, LA_DYE_AMT) * (lerp(LA_DYE_CORE, 1.0, Tw) * lampGd * wD);
        // ---- brief BK: masses vs droplets, and smoke ---------------------
        // "Too bubbly": the dye on EVERY droplet is what reads as bubbles.
        // Mass vs droplet is decided by WHICH sim object made the dark, not
        // by size or depth (a mass's rim is as thin as a droplet): fieldB is
        // the blob field before any droplet is punched in, so fieldB below
        // the threshold is a mass (a gap between blobs) and a dark pixel with
        // fieldB above it is a droplet hole in the sheet -- the same test the
        // crust already keys off. sB is that blob-only surface as a distance;
        // mK = 1 in mass territory, easing to 0 over 3 px (plus the smoke's
        // reach, so a mass's own smoke is never cut by the droplet colour)
        // into the sheet. The two territories take two colours:
        //   masses   = dye_hue/sat/lum * dye_masses  (folded into DYE_AMT on
        //              the CPU while the split is on)
        //   droplets = DYE_DROP_RGB: dye_droplet_hue/sat/lum (each -1 =
        //              inherit the mass dye) * dye_droplets, built on the CPU
        //              and packed as three 8-bit channels r + 256 g + 65536 b
        //              in one float (exact: < 2^24); -1 = split off.
        // dye_smoke: the old gate is (1 - cov), a hard stop at the isoline,
        // and the thin-edge-brightest profile, i.e. a lit disc. Smoke
        // instead thickens GRADUALLY inward, leaks wOut out under the thin
        // film (the film's transparency shows it, tinted), carries no edge
        // peak and is broken into wisps by a slow fbm that both modulates the
        // density and warps the edge. Where sdf is not a real distance (isoOk
        // < 1) the gate falls back to plain coverage. Smoke is a MASS property
        // (weighted by mK): a droplet is narrower than the smoke's inward ramp,
        // so smoking it only dimmed it -- a second dye_droplets (first sheet).
        // Split off and smoke 0 skip all of this: dyeAdd above is the old line.
        [branch] if (LA_DYE_SMOKE > 0.0005 || LA_DYE_DROP_RGB > -0.5) {
            float s    = saturate(LA_DYE_SMOKE);
            float wOut = s * 0.030;
            float sB   = (fieldB - thresh) / max(length(gradB), 1e-6);
            float mK   = 1.0 - smoothstep(0.0, 3.0 * PX1440 + wOut, sB);
            float prof = lerp(LA_DYE_CORE, 1.0, Tw);
            float gate = 1.0 - cov;
            float leakK = 0.0;
            [branch] if (s > 0.0005) {
                float n  = saturate(AcidFbm(pp * 6.0 + float2(LA_TIME * 0.007,
                                                              -LA_TIME * 0.011)) * 1.143);
                float dS = -sdf + s * 0.030 * (n - 0.5);
                gate = lerp(gate, smoothstep(-wOut, 0.010 + 0.080 * s, dS), isoOk * mK);
                prof = lerp(prof, lerp(prof, 0.75, s) * lerp(1.0, 0.30 + 1.40 * n, s), mK);
                // brief BP: the LEAK (the smoke under the film, cov > 0) takes
                // the FILM's hue. A dye of another hue seen through the film
                // mixes additively toward grey -- the "middle values bad" of
                // the smoke sheet. Inside the mass (cov = 0) nothing changes.
                leakK = cov * mK;
            }
            float3 massC = dyeC * LA_DYE_AMT;
            [branch] if (leakK > 0.0005) {
                float3 fhs = AcidRgb2Hsv(oilC);
                massC = lerp(massC, InkHsv2Rgb(float3(fhs.x, LA_DYE_SAT, 1.0)) * LA_DYE_AMT, leakK);
            }
            float3 dropC = massC;
            float  dropL = LA_DYE_AMT;
            [branch] if (LA_DYE_DROP_RGB > -0.5) {
                // hue8 + 256 sat8 + 65536 lum8 (brief BP; was 8-bit RGB)
                float v  = LA_DYE_DROP_RGB;
                float cl = floor(v * (1.0 / 65536.0));
                float cs = floor((v - cl * 65536.0) * (1.0 / 256.0));
                float ch = v - cl * 65536.0 - cs * 256.0;
                dropL = cl * (1.0 / 255.0);
                dropC = InkHsv2Rgb(float3(ch * (1.0 / 256.0), cs * (1.0 / 255.0), 1.0)) * dropL;
            }
            [branch] if (LA_DARK_SAT > 0.0005) {
                massC = DarkSat(massC, LA_DYE_AMT);
                dropC = DarkSat(dropC, dropL);
            }
            dyeAdd = lerp(dropC, massC, mK) * (prof * lampGd * gate * dkG);
        }
        inkC += dyeAdd;
#if defined(OIL_MASSDYE) && defined(OIL_LAMP)
        // ---- Oil3D L3 (3): the DYED MASS under the lamps -------------------
        // Masses only (the BK mass territory, 0 on a droplet): the windowed
        // colour minus today's goes to the WINDOWED composite alone (col, at
        // the composite below); the white track keeps today's dye, so the
        // output cap holds every mass pixel at today's dyed-mass Y.
        // L3b: gated by the HARD mass-only mask (the blob field below its
        // isoline) before any blend: the composite's own alpha anti-aliases the
        // edge, and no droplet hole (blob field above it) ever takes a share.
        [branch] if (LA_DYE_AMT > 0.0005) {
            float sBm = (fieldB - thresh) / max(length(gradB), 1e-6);
            float mKm = (sBm < 0.0) ? 1.0 : 0.0;
            mdDelta = (OilMassWin(dyeAdd) - dyeAdd) * mKm;
        }
#endif
    }
#ifdef OIL_MASSDYE
    // ---- Oil3D L3 (3): THE DARK-DYE OIL (oil_dye, oil_dye_amount) ---------
    // The black oil as a very dark dye: transmittance 0 except ONE ~20-deg
    // band (oil_dye, rel. to the anchor) peaking at 20 % x amount. What shows
    // = lamp window x the film's dyes (fluid A: the masses sit on it, f = 0)
    // x that band, on the CPU's 48-sample circle: laMass[2] = the two-lamp
    // WINDOWED rgb, laMass[3] = the WHITE state's, per unit of the film's
    // linear Y, x today's film Y here (its lamp ramp / footprint included).
    // A window off the band = black oil; a film passing nothing there = black
    // oil; white covers it (dim). The white state's light goes into the ink
    // (both tracks), the windowed change into the composite alone (the output
    // cap holds it). MASSES only: the BK territory test is exactly 0 on a
    // droplet, which is skipped, so droplets stay bit-exact black.
    // L3b: the HARD mass-only mask (the blob field below its isoline) before
    // any blend, not the 3-px soft territory edge (that reached droplet holes
    // within 3 px of a mass); the composite's alpha anti-aliases the edge.
    // Without the lamp path (white lamps) the windowed change is 0: not added.
    [branch] if (laMass[3].w > 0.5) {
        float sBd = (fieldB - thresh) / max(length(gradB), 1e-6);
        float mKd = (sBd < 0.0) ? 1.0 : 0.0;
        [branch] if (mKd > 0.0) {
            float  yF   = dot(DsToLin(oilW0 * lampG), float3(0.2126, 0.7152, 0.0722)) * mKd;
            float3 inkL = DsToLin(inkC);
            float3 inkD = DsToSrgb(inkL + laMass[3].rgb * yF);
#ifdef OIL_LAMP
            mdDelta += DsToSrgb(inkL + laMass[2].rgb * yF) - inkD;
#endif
            inkC = inkD;
        }
    }
#endif
#ifdef OIL_LAMP
    {
        float txM = lerp(1.0, 0.93 + 0.14 * AcidFbm(pp * 7.0 + float2(LA_TIME * 0.010,
                                                                      -LA_TIME * 0.007)),
                         saturate(LA_OIL_TEXTURE));
        oilC *= txM; oilW *= txM;
    }
#else
    oilC *= lerp(1.0, 0.93 + 0.14 * AcidFbm(pp * 7.0 + float2(LA_TIME * 0.010,
                                                              -LA_TIME * 0.007)),
                 saturate(LA_OIL_TEXTURE));
#endif
    // Thin film: the ink underneath modulates the oil, but only BROADLY. The
    // raw per-pixel band printed the dye's texel structure onto the oil as
    // short horizontal dashes, so drive it from a wide, heavily smoothed
    // luminance tap instead of the pixel's own band.
    if (LA_TRANSLUCENCY > 0.002) {
        float2 bt = texelSize * 9.0;
        float lw = length(Dye.SampleLevel(linearClamp, uv + bt, 0).rgb)
                 + length(Dye.SampleLevel(linearClamp, uv - bt, 0).rgb)
                 + length(Dye.SampleLevel(linearClamp, uv + float2(bt.x, -bt.y), 0).rgb)
                 + length(Dye.SampleLevel(linearClamp, uv - float2(bt.x, -bt.y), 0).rgb);
        oilC *= lerp(1.0, 0.88 + 0.30 * saturate(lw * 0.45 * LA_INK_GAIN), saturate(LA_TRANSLUCENCY));
#ifdef OIL_LAMP
        oilW *= lerp(1.0, 0.88 + 0.30 * saturate(lw * 0.45 * LA_INK_GAIN), saturate(LA_TRANSLUCENCY));
#endif
    }
#if defined(OIL_STAGETAP) && (OIL_STAGETAP == 16 || OIL_STAGETAP == 116)
    return OilTapLin((OIL_STAGETAP > 100) ? OIL_TAPW(oilC, oilW) : oilC);
#endif
    // ---- thin film: the oil is a LENS, not a cut-out (oil_thin_edge) -----
    // refs 4/5/6: there is NO stroked boundary. The film thins to nothing at
    // the edge, so the light reaching the eye there has crossed the ink AND a
    // sliver of oil: the colour slides toward the PRODUCT of the two (orange
    // over red ink goes red; white over teal goes teal) and loses level, over
    // a band a few percent of the lens's own radius. That soft thickness
    // gradient is the whole reason a real oil disc is not a sticker.
    float3 oilThinC = oilC;
#ifdef OIL_LAMP
    float3 oilThinW = oilW;
#endif
    if (LA_OIL_THIN_EDGE > 0.0005) {
        // saturate(): the factor is capped at 1, so a thinning film can only
        // go DARKER and toward the ink's hue, never brighter and paler. Left
        // uncapped it lifted every edge over bright ink into a milky lobe.
        float3 prod = oilC * saturate(0.30 + 1.10 * inkC);
        float  tt   = 1.0 - thk;
        oilThinC = prod;
        oilC = lerp(oilC, prod, saturate(tt * tt * LA_OIL_THIN_EDGE));
#ifdef OIL_LAMP
        float3 prodW = oilW * saturate(0.30 + 1.10 * inkC);
        oilThinW = prodW;
        oilW = lerp(oilW, prodW, saturate(tt * tt * LA_OIL_THIN_EDGE));
#endif
    }
#if defined(OIL_STAGETAP) && (OIL_STAGETAP == 17 || OIL_STAGETAP == 117)
    return OilTapLin((OIL_STAGETAP > 100) ? OIL_TAPW(oilC, oilW) : oilC);
#endif
    // ---- TRANSPARENT COLOURED FILM (oil_transparency) --------------------
    // Until now the oil was a fill: whatever the ink did under a disc, the
    // disc hid it. A real coloured oil is an ABSORBING film, so the ink's
    // marbling reads through it, tinted. Beer-Lambert per channel:
    //   T = exp(-absorb * thickness * (1 - oilHueNormalised))
    // An orange film has oilN ~= (1, 0.31, 0.18), so 1-oilN ~= (0, .69, .82):
    // red passes untouched, blue is eaten. A near-black oil normalises to ~0
    // and absorbs every channel -- which is right, it is a neutral-density
    // film, and that is why the approved black-oil family survives this key.
    //
    // The dish is BACKLIT, so the light reaching the eye has come THROUGH the
    // ink: over black ink nothing is transmitted and the disc would go black.
    // The scatter term oilC * (1 - T_avg) is the light scattered inside the
    // film itself -- it is what keeps thick oil reading as its own colour over
    // dead-black ink, and it vanishes at the rim where T -> 1.
    //
    // Thickness = the same proxy the edge work uses (thk: 0 at the rim, 1 deep
    // inside), times a slow fbm so the film has ISLANDS of thick and thin
    // instead of being one even pane of glass. thk -> 0 at the edge means the
    // thin edge is automatically the clearest part of the film, which is
    // exactly what oil_thin_edge was approximating by hand.
    float filmOp = 1.0;    // how much of this pixel is the OIL's own light
#ifdef OIL_LAMP
    float filmOpW = 1.0;
#endif
    if (LA_OIL_TRANSP > 0.0005) {
        float k = saturate(LA_OIL_TRANSP);
        float fb = saturate(AcidFbm(pp * 2.2 + float2(LA_TIME * 0.004,
                                                      -LA_TIME * 0.003)) * 1.143);
        float thick = thk * lerp(1.0, 0.35 + 1.30 * fb, saturate(LA_OIL_FILM_BUMP));
#ifdef OIL_LAMP2
        // Oil3D L3b (the stage table's culprit): T is the film's DYE, so it
        // is the WHITE track's chroma here, not the lamp-lit colour's. Lamp 2's
        // share mixes a second hue into the lit film; normalising THAT by its
        // peak channel read the mix's lower purity as a clearer film (more ink
        // through, less of its own light): open water's linear Y fell 11.2 %
        // here (live, 60 s, 0.6 / sof 0.9 / +100) under a sum that pinned it.
        // A lamp changes the light, never what the film absorbs.
        float  mx   = max(oilW.r, max(oilW.g, oilW.b));
        float3 oilN = (mx > 1e-4) ? saturate(oilW / mx) : float3(0.0, 0.0, 0.0);
#else
        float  mx   = max(oilC.r, max(oilC.g, oilC.b));
        float3 oilN = (mx > 1e-4) ? saturate(oilC / mx) : float3(0.0, 0.0, 0.0);
#endif
        float3 T    = exp(-max(LA_OIL_ABSORB, 0.0) * max(thick, 0.0) * (1.0 - oilN));
        float  Tav  = dot(T, float3(0.3333333, 0.3333333, 0.3333333));
        // inkC is already the REFRACTED (and, with oil_ink_blur, defocused)
        // ink at this pixel: the marbling under the film, seen through it.
#ifdef OIL_LAMP
        // Oil3D L1: the ink seen THROUGH the film is lit by the same lamp (its
        // hue comes with T, from the lamp-lit film; the level is the ratio)
        float3 glassC = inkC * (T * lampL.z) + oilC * (1.0 - Tav);
#else
        float3 glassC = inkC * T + oilC * (1.0 - Tav);
#endif
        oilC   = lerp(oilC, glassC, k);
        filmOp = lerp(1.0, saturate(1.0 - Tav), k);
#ifdef OIL_LAMP
        // the white track: its own Beer-Lambert (T follows the film's chroma:
        // the purer windowed film absorbs more of the ink and scatters more of
        // its own light, one of the terms the output cap exists for)
        float  mxW   = max(oilW.r, max(oilW.g, oilW.b));
        float3 oilNW = (mxW > 1e-4) ? saturate(oilW / mxW) : float3(0.0, 0.0, 0.0);
        float3 TW    = exp(-max(LA_OIL_ABSORB, 0.0) * max(thick, 0.0) * (1.0 - oilNW));
        float  TavW  = dot(TW, float3(0.3333333, 0.3333333, 0.3333333));
        oilW    = lerp(oilW, inkC * TW + oilW * (1.0 - TavW), k);
        filmOpW = lerp(1.0, saturate(1.0 - TavW), k);
#endif
    }
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
#if defined(OIL_STAGETAP) && (OIL_STAGETAP == 18 || OIL_STAGETAP == 118)
    return OilTapLin((OIL_STAGETAP > 100) ? OIL_TAPW(oilC, oilW) : oilC);
#endif
    // ---- surface relief: specular (oil_specular) + thin-film iridescence --
    // Kept deliberately weak: the references show almost no specular, because
    // the rig is BACKLIT. What little there is comes from the lens curvature
    // near the edge plus a slow thickness ripple, never a pin-point.
    float specAmt = 0.0;
    if (LA_OIL_SPECULAR > 0.0005 || LA_OIL_IRID > 0.0005) {
        float2 q  = pp * 4.3 + float2(LA_TIME * 0.006, -LA_TIME * 0.0045);
        float  f0 = AcidFbm(q);
        if (LA_OIL_SPECULAR > 0.0005) {
            const float he = 0.035;
            float2 bg = float2(AcidFbm(q + float2(he, 0.0)) - f0,
                               AcidFbm(q + float2(0.0, he)) - f0) / he;
            // dome normal: tilts OUTWARD (-grad) and hardest where the film is
            // thin, with the fbm adding a low relief across the flat middle.
            float2 nxy = (-grad / gl) * (1.0 - thk) * 0.85 - bg * 0.06;
            float3 nS  = normalize(float3(nxy, 1.0));
            float3 Lv  = float3(-0.406, -0.406, 0.819);   // top-left, ~55 deg up
            float3 Hv  = normalize(Lv + float3(0.0, 0.0, 1.0));
            float  sp  = pow(saturate(dot(nS, Hv)), 18.0);   // BROAD lobe
            float  fr  = pow(saturate(1.0 - nS.z), 3.0);     // Fresnel edge
            specAmt = (sp * 0.30 + fr * 0.12) * saturate(LA_OIL_SPECULAR);
#ifdef SPEC_TINT
            // T7 oil_spec_tint: the broad lobe (sp) takes the colour of the light
            // in the tank -- 1 the film's own colour here (after the lamp
            // windows), 2 lamp 1's window colour -- peak-normalised, so only its
            // colour changes; the Fresnel edge (fr) stays today's. The white
            // track takes it under white light (its own film / white).
#if SPEC_TINT == 2
            float3 stA = (laGlow.w > 0.5) ? DsToSrgb(max(laGlow.rgb, 0.0)) : float3(1.0, 1.0, 1.0);
#else
            float3 stA = oilC;
#endif
            float  stM = max(stA.r, max(stA.g, stA.b));
            float  frA = fr * 0.12 * saturate(LA_OIL_SPECULAR);
            oilC += (specAmt - frA) * ((stM > 1e-4) ? stA / stM : float3(1.0, 1.0, 1.0)) + frA;
#ifdef OIL_LAMP
#if SPEC_TINT == 2
            oilW += specAmt;
#else
            float stMW = max(oilW.r, max(oilW.g, oilW.b));
            oilW += (specAmt - frA) * ((stMW > 1e-4) ? oilW / stMW : float3(1.0, 1.0, 1.0)) + frA;
#endif
#endif
#else
            oilC += specAmt;
#ifdef OIL_LAMP
            oilW += specAmt;
#endif
#endif
        }
        if (LA_OIL_IRID > 0.0005) {
            float3 ir = 0.5 + 0.5 * cos(6.2831853 * (f0 * 3.0 + float3(0.0, 0.33, 0.67)));
            float  iw = saturate(LA_OIL_IRID) * lerp(0.30, 1.0, 1.0 - thk) * 0.45;
            oilC *= lerp(1.0, ir * 1.5, iw);
#ifdef OIL_LAMP
            oilW *= lerp(1.0, ir * 1.5, iw);
#endif
        }
    }
#if defined(OIL_STAGETAP) && (OIL_STAGETAP == 19 || OIL_STAGETAP == 119)
    return OilTapLin((OIL_STAGETAP > 100) ? OIL_TAPW(oilC, oilW) : oilC);
#endif
    // ---- rim variation (rim_vary / rim_ink_follow) -----------------------
    // The reference rim is NOT a uniform stroke: it thickens and brightens
    // where the ink under it is bright, and thins to nothing along other
    // stretches. Two multipliers, both EXACTLY 1.0 when the keys are 0, so
    // every shipped ini renders unchanged.
    float rimMul = 1.0, haloMul = 1.0;
    if (LA_RIM_VARY > 0.0005) {
        // low-frequency noise of POSITION (features ~1/3 of the frame) with a
        // slow drift, contrast-stretched so it really reaches 0 and 1
        float rn = AcidFbm(pp * 3.3 + float2(LA_TIME * 0.011, -LA_TIME * 0.008)) * 1.143;
        rn = saturate((rn - 0.5) * 1.9 + 0.5);
        float rv = saturate(LA_RIM_VARY);
        rimMul  = lerp(1.0, 0.40 + 1.50 * rn, rv);                  // 0.4x..1.9x width
        haloMul = lerp(1.0, smoothstep(0.18, 0.72, rn) * 1.55, rv); // dies / thickens
    }
    if (LA_RIM_INK_FOLLOW > 0.0005) {
        // The halo IS refracted ink, so it can be no brighter than the ink
        // just OUTSIDE the edge: one dye tap a few halo-widths along -grad
        // (the outward normal), so it doesn't collapse under the oil itself.
        float2 uvO = saturate(uv - (grad / gl) * (max(LA_MENISCUS_W, LA_RIM_WIDTH) * 8.0)
                                  * float2(1.0 / aspect, 1.0));
        float3 dO  = Dye.SampleLevel(linearClamp, uvO, 0).rgb;
        float  il  = saturate(max(dO.r, max(dO.g, dO.b)) * max(LA_INK_GAIN, 1.0));
        // Brightens where the ink outside is bright, and falls toward a floor
        // -- not to nothing -- over clear water, because the halo is also the
        // oil edge's own refraction (ink_mode=water is mostly clear water).
        haloMul *= lerp(1.0, 0.55 + 0.90 * smoothstep(0.03, 0.45, il), saturate(LA_RIM_INK_FOLLOW));
    }
    // ---- paired-annulus ordering (rim_order) -----------------------------
    // LA_RIM_ORDER = 0: the shipped placement — dark band centred at rim_inset,
    // bright halo centred at -meniscus_offset, wherever the ini put them.
    // LA_RIM_ORDER = 1: force the physical pairing (Micromachines 13(7):1021) —
    // the dark band one half-width INSIDE the isoline and the bright caustic
    // one half-width OUTSIDE it, so they are adjacent and never overlap.
    // ---- EMERGENT boundary (meniscus_from_ink) ---------------------------
    // The reference boundary is not a stroke of some chosen colour: it is the
    // INK, concentrated by the meniscus. Where the ink just outside is bright
    // it makes a WIDE, soft, grainy halo of that ink's own hue (ref 7's cyan)
    // with a dark hairline against it; where the ink is dark (refs 4/5/6, and
    // every mono-ink ini) both simply vanish and the oil softens into the
    // dark. One gate does both, so a foreign cyan line can never appear on
    // black ink again. laMen (meniscus_color) survives only as the legacy
    // fallback at meniscus_from_ink = 0.
    float3 menC   = laMen.rgb;
    float  menHWx = 1.0;      // halo half-width multiplier
    float  haloInk = 1.0;     // ink-brightness gate on halo AND hairline
    if (LA_MEN_FROM_INK > 0.0005) {
        float  k  = saturate(LA_MEN_FROM_INK);
        // The gate reads the ink OUTSIDE the isoline, not the ink under the
        // oil: a bright dye patch beneath a disc must not summon a halo, and
        // a hole punched into clear water must not get one either.
        float2 uvO = saturate(uv - (grad / gl) * (0.06 * lensR + LA_MENISCUS_W * 2.0)
                                  * float2(1.0 / aspect, 1.0));
        float3 dO  = Dye.SampleLevel(linearClamp, uvO, 0).rgb;
        float  ilo = saturate(max(dO.r, max(dO.g, dO.b)) * max(LA_INK_GAIN, 1.0));
        haloInk    = lerp(1.0, smoothstep(0.03, 0.40, ilo), k);
        float3 hs = AcidRgb2Hsv(inkC);     // lift: more saturated, brighter
        hs.y = saturate(hs.y * 1.35);
        hs.z = saturate(hs.z * 1.55 + 0.05);
        menC   = lerp(menC, AcidHsv2Rgb(hs), k);
        // a few percent of the LOCAL lens radius, never a 1-2 px line
        // ...but in CRISP mode the halo keeps its authored width: scaling it by
        // the local lens radius is what made a big hole's halo wide, and a
        // crisp edge wants the same thin meniscus at every size.
        menHWx = (LA_OIL_EDGE_MODE > 0.5) ? 1.0
               : lerp(1.0, max(1.0, (0.055 * lensR) / max(LA_MENISCUS_W, 1e-5)), k);
    }
    // ...both floored in px (band_min): the meniscus scaled by the lens
    // radius is exactly the "solid outline" the user photographed on a
    // medium droplet, and the dark rim is its twin.
    float rimHW = max(LA_RIM_WIDTH * rimMul, bmin * 1.5 * PX1440);
    float menHW = clamp(LA_MENISCUS_W * rimMul * menHWx, bmin * 3.5 * PX1440, 0.024);
    // A defocused hairline is a wider, fainter hairline: widths add in
    // quadrature, the way a Gaussian convolves with a Gaussian.
    if (soft > 1e-6) {
        rimHW = sqrt(rimHW * rimHW + soft * soft);
        menHW = sqrt(menHW * menHW + soft * soft);
    }
    float rimCtr = LA_RIM_ORDER > 0.5 ?  rimHW : LA_RIM_INSET;
    float menCtr = LA_RIM_ORDER > 0.5 ? -menHW : -LA_MENISCUS_OFF;
    if (LA_MEN_FROM_INK > 0.0005) menCtr = lerp(menCtr, -menHW * 0.55, saturate(LA_MEN_FROM_INK));
    // dark rim: a Gaussian band centred just INSIDE the boundary, forced to
    // zero deep inside a merged mass so no concentric rings appear there.
    float rimX = (sdf - rimCtr) / rimHW;
    float rimB = exp(-rimX * rimX)
               * (1.0 - smoothstep(thresh * 1.7, thresh * 3.4, field))
    // ...and the SAME |grad| gate the bright halo already carries. sdf =
    // (field - thresh) / |grad| is only a distance where the gradient is
    // strong; between two surfaces that are nearly merged (or nearly necked
    // apart) both the deficit and the gradient collapse, and the quotient
    // still lands inside the rim band -- painting a thin dark ARC along a
    // saddle with no isoline under it. That is the curved hairline the user
    // photographed below a big hole. The halo was gated for exactly this
    // reason and the dark twin never was.
               * smoothstep(0.5, 1.5, gl) * isoOk;
    // The hairline lives only where the halo does, and the soft thickness
    // edge has already taken over most of its job.
    float rimK = saturate(LA_RIM_DARK) * haloInk * (1.0 - 0.65 * saturate(LA_OIL_THIN_EDGE));
    oilC *= 1.0 - rimK * rimB;
#ifdef OIL_LAMP
    oilW *= 1.0 - rimK * rimB;
#endif
    // ---- the colour a RIM reflects (boundary_reflect_r, brief AJ) --------
    // Everything below that paints a droplet's boundary -- the bright-field
    // halo, mass_rim, the droplet lens's dome / meniscus / specular, oil_glow
    // and the swarm's caustics -- tints with oilR instead of oilC, and with
    // oilThinR instead of oilThinC. With boundary_reflect_r 0 rimHueD is 0 and
    // these are the same values, so every existing preset is byte-identical.
    // The film itself (the `col = lerp(inkC, oilC, alpha)` below) always keeps
    // its own colour: only the boundaries reach out.
    float3 oilR     = oilC;
    float3 oilThinR = oilThinC;
    [branch] if (abs(rimHueD) > 1e-4) {
        oilR     = AcidHueShift(oilC,     rimHueD);
        oilThinR = AcidHueShift(oilThinC, rimHueD);
    }
#ifdef OIL_LAMP
    float3 oilRW     = oilW;
    float3 oilThinRW = oilThinW;
    [branch] if (abs(rimHueD) > 1e-4) {
        oilRW     = AcidHueShift(oilW,     rimHueD);
        oilThinRW = AcidHueShift(oilThinW, rimHueD);
    }
#endif

)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
    // Film alpha: with oil_thin_edge the disc fades out over the thickness
    // band instead of over 1-2 px of coverage AA. Squared, because a lens
    // thins fast near its edge.
    float alpha = cov;
    if (LA_OIL_THIN_EDGE > 0.0005) {
        float aS = smoothstep(-edgeW * 0.35, edgeW, sdf);
        // the same S as the thickness ramp, or the disc's opacity would
        // arrive at the ink with the knee the colour no longer has.
        [branch] if (LA_OIL_EDGE_CURVE > 0.0005) {
            float t = saturate((sdf + edgeW * 0.35) / max(edgeW * 1.35, 1e-7));
            aS = lerp(aS, t * t * t * (t * (t * 6.0 - 15.0) + 10.0),
                      saturate(LA_OIL_EDGE_CURVE));
        }
        alpha = lerp(cov, aS * aS, saturate(LA_OIL_THIN_EDGE));
    }
    // ---- film_level (brief BK): the FLAT film's own light -----------------
    // The user: one element pitch black, one the main colour, one an accent.
    // This dims the sheet (and whatever of the ink its transparency shows)
    // but NOT the edge light: oilR / oilThinR were copied above, so mass_rim,
    // oil_glow, the bright-field halo and the lens highlights keep the full
    // palette -- brief BL's "brightness at the edges, the flat field dark".
    // The dye on the masses/droplets rides on inkC and is untouched.
    if (LA_FILM_LEVEL < 0.9995) {
        oilC *= LA_FILM_LEVEL;
        // brief BP: the dimmed film keeps its luma and gains chroma
        [branch] if (LA_DARK_SAT > 0.0005) oilC = DarkSat(oilC, LA_FILM_LEVEL);
#ifdef OIL_LAMP
        oilW *= LA_FILM_LEVEL;
        [branch] if (LA_DARK_SAT > 0.0005) oilW = DarkSat(oilW, LA_FILM_LEVEL);
#endif
    }
#if defined(OIL_STAGETAP) && (OIL_STAGETAP == 3 || OIL_STAGETAP == 103)
    return OilTapLin((OIL_STAGETAP > 100) ? OIL_TAPW(oilC, oilW) : oilC);
#endif
    float3 col = lerp(inkC, oilC, alpha);
#ifdef OIL_MASSDYE
    col += mdDelta * (1.0 - alpha);
#endif
#ifdef OIL_LAMP
    float3 wtCol = lerp(inkC, oilW, alpha);
#endif
#ifdef OIL_HEIGHT
    // ---- Oil3D Stage B: the floor light THROUGH the wax (oil3d_absorb) ----
    // A black mass is wax over the lit film: what reaches the eye there went
    // down through the wax to the film and back up, T^2, T = exp(-k h),
    // h = the BLOB field's own height (droplets never absorb: inside the
    // film a water droplet has fieldB above the isoline, so wM = 0 and it
    // keeps today's rendering untouched). In LINEAR light: the composite here
    // is display-gamma, so the transmitted film is lerped in through the
    // gamma (x^(1/2.2) of the linear factor). Under every rim / halo / lens
    // term below, which keep painting over it.
    [branch] if (LA_OIL3D_ABSORB > 0.0005) {
        float  hO, dO;
        float2 gO;
        OilHeightEval(thresh, fieldB, gradB, hessB, kapKB, kapXB, kapYB, hO, dO, gO);
        float  aaB = fwidth(fieldB) * LA_AA_SCALE + 1e-4;
        float  wM  = 1.0 - smoothstep(thresh - aaB, thresh + aaB, fieldB);
        float  tg  = exp(-(2.0 / 2.2) * LA_OIL3D_ABSORB * hO);
        // NO OUTLINE (B re-ruling): the rim's Fresnel darkening also holds
        // the transmission down at the contact line -- the rim profile
        // mirrored into the mass, (1 - rim_dark e^-(d/w)^2), d = the blob
        // field's smooth distance, w = the rim's own width x 2 -- so the light
        // rises from the rim minimum to ONE soft hump and decays over lambda.
        float  dIn = max(dO, 0.0) / max(2.0 * rimHW, 1e-5);
        tg *= 1.0 - saturate(LA_RIM_DARK) * exp(-dIn * dIn);
        // CORES CLEAN: T is forced to 0 where the wax is thick, so no film
        // texture ever reaches a mass's core whatever k is.
        tg *= 1.0 - smoothstep(0.55, 0.90, hO);
        col = lerp(col, oilC, tg * wM * (1.0 - alpha));
#ifdef OIL_LAMP
        wtCol = lerp(wtCol, oilW, tg * wM * (1.0 - alpha));
#endif
    }
#endif
#if defined(OIL_STAGETAP) && (OIL_STAGETAP == 4 || OIL_STAGETAP == 104)
    return OilTapLin((OIL_STAGETAP > 100) ? OIL_TAPW(col, wtCol) : col);
#endif
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
    // ---- brief BU: SPLIT TONE + LAMP GREY ---------------------------------
    // Both act on the composite right here, after the film and the dye are
    // laid down and BEFORE every rim/halo/lens/glow term below, so droplet
    // and mass edges keep their colour and paint over both. Both branches are
    // skipped at their defaults (zero add / GREY_K 0): byte-identical.
    //
    // SPLIT TONE (shadow_tone): the dark tones of the MASSES take the
    // complement of the main film hue. The CPU folds hue, saturation, lift,
    // the on/off gate and the amount into one add; the mask keeps it off the
    // film (alpha) and off anything already lit (luma above ~0.15, e.g. dyed
    // bodies). It lifts true black by at most shadow_tone_lift: the user's
    // call over the auditor's objection.
    [branch] if (LA_TONE_R + LA_TONE_G + LA_TONE_B > 0.0) {
        float tY = dot(col, float3(0.2126, 0.7152, 0.0722));
        float tS = smoothstep(0.02, 0.15, tY);
        // tone_balance (BU-b) scales the shadow band with the split point
        // (0.5 - 0.25 b) / 0.5; a separate branch so balance 0 keeps BU's
        // constant-folded smoothstep bit for bit.
        [branch] if (LA_TONE_BAL != 0.0) {
            float tK = 1.0 - 0.5 * LA_TONE_BAL;
            tS = smoothstep(0.02 * tK, 0.15 * tK, tY);
        }
        float tM = (1.0 - alpha) * (1.0 - tS);
        col += float3(LA_TONE_R, LA_TONE_G, LA_TONE_B) * tM;
#ifdef OIL_LAMP
        {
            float tYW = dot(wtCol, float3(0.2126, 0.7152, 0.0722));
            float tSW = smoothstep(0.02, 0.15, tYW);
            [branch] if (LA_TONE_BAL != 0.0) {
                float tKW = 1.0 - 0.5 * LA_TONE_BAL;
                tSW = smoothstep(0.02 * tKW, 0.15 * tKW, tYW);
            }
            wtCol += float3(LA_TONE_R, LA_TONE_G, LA_TONE_B) * ((1.0 - alpha) * (1.0 - tSW));
        }
#endif
    }
    // HIGHLIGHT TONE (brief BU-b, Lightroom split toning): the lit FILM
    // shifts colour toward the highlight tint at CONSTANT displayed
    // luminance (the shadow half above lifts; this half never adds light --
    // the film is most of the frame, and ABL). Worked in DISPLAY light:
    // d0 = what post_chroma, post_lift and the saturate() below will make of
    // this pixel, hT = the luminance the panel shows for it (BubLum: the
    // gamut stretch and its clamp included). Weight = alpha x smoothstep over
    // the encoded hT around the split (0.5 - 0.25 x tone_balance), 0.25 below
    // to 0.05 above it: the lit film (encoded ~0.5) takes ~85% at balance
    // 0, all of it at +0.5, ~40% at -0.5 (the first band, 0.40..0.05 below,
    // left balance >= 0 inert on this look). Then a white-balance multiply toward the tint
    // (lamp grey's construction), the film desaturation, a CAP (no channel
    // above the pixel's own max: magenta -> gold drives red, which already
    // sits at the clip), and the luminance put back EXACTLY: a scale down if
    // brighter, else a bisection toward the flat grey at that max (which
    // keeps the cap). Last, the exact inverse of post_lift / post_chroma
    // back into the composite. The CPU folds hue, sat, amount, desat and the
    // gate into LA_HTONE (0 = off, branch skipped).
    [branch] if (LA_HTONE_R + LA_HTONE_G + LA_HTONE_B > 0.0) {
#ifdef OIL_LAMP
        wtCol = OilHTone(wtCol, alpha);
#endif
        const float3 GW = float3(0.2126, 0.7152, 0.0722);
        float  hPc = max(LA_POST_CHROMA, 1e-3), hPl = max(LA_POST_LIFT, 1e-3);
        float  hY0 = dot(col, GW);
        float3 d0 = DsToLin(saturate(max(hY0 + (col - hY0) * hPc, 0.0) * hPl));
        float  hT = BubLum(d0);
        float  hSp = 0.5 - 0.25 * LA_TONE_BAL;
        float  hW = alpha * smoothstep(hSp - 0.25, hSp + 0.05, DsToSrgb(hT.xxx).x);
        [branch] if (hW > 0.0) {
            float3 hU = float3(LA_HTONE_R, LA_HTONE_G, LA_HTONE_B);
            float  hMag = dot(hU, GW);
            float3 hc = d0 * (hU / hMag);
            float  hY = dot(hc, GW);
            hc = hY + (hc - hY) * (1.0 - saturate(hMag - 1.0));
            float hM0 = max(max(d0.r, max(d0.g, d0.b)), hT);
            float hM1 = max(hc.r, max(hc.g, hc.b));
            if (hM1 > hM0) hc *= hM0 / hM1;
            float3 hx = lerp(d0, hc, hW);
            float  hL = BubLum(hx);
            [branch] if (hL >= hT) {
                hx *= hT / max(hL, 1e-8);
            } else {
                float lo = 0.0, hi = 1.0;
                [unroll] for (int it = 0; it < 7; it++) {
                    float mid = 0.5 * (lo + hi);
                    if (BubLum(lerp(hx, hM0.xxx, mid)) < hT) lo = mid; else hi = mid;
                }
                hx = lerp(hx, hM0.xxx, 0.5 * (lo + hi));
            }
            float3 he = DsToSrgb(hx);
            float  hy = dot(he, GW) / hPl;
            col = max(hy + (he / hPl - hy) / hPc, 0.0);
        }
    }
    // LAMP GREY (lamp_grey): "as if the dye far from the lamp gets less
    // light". A soft disc on the far corner (the CPU picks it and slides it
    // along the frame edge): full inside 0.35 x size, gone at size (screen
    // heights), and the corner is > 1 screen height from the frame centre,
    // so the centre never changes. LampGreyY (by DarkSat): Y-flat
    // desaturation about the LINEAR luminance, then a cool white balance
    // renormalised to the same Y, held through post_chroma. Applied after
    // the tone, so a tinted body in the grey corner greys with the rest.
    // (The centre is brief BX's grey heart, before the final trim below.)
    [branch] if (LA_GREY_K > 0.0) {
        float gS = max(LA_GREY_SIZE, 0.05);
        float gw = 1.0 - smoothstep(0.35 * gS, gS, length(pp - float2(LA_GREY_CX, LA_GREY_CY)));
        float gk = gw * LA_GREY_K;
#ifdef OIL_LAMP
        [branch] if (gk > 0.0) {
            col  = LampGreyY(col,  gk, gk * saturate(LA_GREY_COOL), 1.0);
            wtCol = LampGreyY(wtCol, gk, gk * saturate(LA_GREY_COOL), 1.0);
        }
#else
        [branch] if (gk > 0.0) col = LampGreyY(col, gk, gk * saturate(LA_GREY_COOL), 1.0);
#endif
    }
#if defined(OIL_STAGETAP) && (OIL_STAGETAP == 5 || OIL_STAGETAP == 105)
    return OilTapLin((OIL_STAGETAP > 100) ? OIL_TAPW(col, wtCol) : col);
#endif
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
    // ---- FLUORESCENT OIL UNDER THE LAMP (oil_fluor, brief BL) ------------
    // The user: "What if the lamp lights up the oil, and it's like
    // fluorescent? And the ambient light brightens the fluid?" -- refs
    // bl-biolum-ref-1/-2: water that GLOWS in its own electric colour,
    // brightest in the churned edges, everything else near black.
    // So the oil EMITS: its own hue at full saturation (no hue key -- the
    // palette rotates, the glow rotates with it), driven by the rig's lamp
    // (the same LAMP_X/Y the rim, the shadows and the specular use) with a
    // Gaussian falloff over oil_fluor_reach screen heights. The profile is
    // edge-weighted the way a fluorescent acrylic sheet is: the light it
    // makes is trapped inside and leaks out where the sheet ends, so the thin
    // film at a MASS edge glows most and the flat body only a little. The
    // edge is measured on the blob-only surface (fieldB/gradB), so the 950
    // droplet holes do not each grow a glowing ring ("too bubbly", BK) --
    // they sit in the body term. Added AFTER film_level, so film_level 0.3 +
    // oil_fluor = dark sheet, glowing lamp-side edges; weighted by alpha, so
    // the masses themselves emit nothing (the shadow pass below still
    // multiplies it: an occluded film is not excited). The edge band alone
    // gets HDR headroom further down (fluorM), which keeps the hot area small
    // for the panel's ABL. oil_fluor 0 skips all of it: byte-identical.
    float fluorM = 0.0;
    [branch] if (LA_OIL_FLUOR > 0.0005) {
        float2 lampF = float2(LA_LAMP_X * aspect, LA_LAMP_Y);
        float  dL    = length(pp - lampF) / max(LA_OIL_FLUOR_REACH, 0.05);
        float  fall  = exp(-dL * dL);
        float  glBf  = length(gradB) + 1e-6;
        float  sBf   = (fieldB - thresh) / glBf;
        float  wE    = max(edgeW * 1.5, bmin * 4.0 * PX1440);
        float  edgeP = exp(-max(sBf, 0.0) / wE) * smoothstep(0.5, 1.5, glBf) * isoOk;
        float  prof  = 0.20 + 0.80 * edgeP;
        // the film's own hue, value 1, saturation pushed: it glows, it does
        // not whiten (peak channel, the mass_rim convention)
        float3 fh = AcidRgb2Hsv(oilR);
        fh.y = saturate(fh.y * 1.25);
        fh.z = 1.0;
        float  k  = saturate(LA_OIL_FLUOR) * fall * alpha;
        col += AcidHsv2Rgb(fh) * (0.80 * k * prof);
#ifdef OIL_LAMP
        float3 fhW = AcidRgb2Hsv(oilRW);
        fhW.y = saturate(fhW.y * 1.25);
        fhW.z = 1.0;
        wtCol += AcidHsv2Rgb(fhW) * (0.80 * k * prof);
#endif
        fluorM = saturate(k * edgeP);
    }
#if defined(OIL_STAGETAP) && (OIL_STAGETAP == 6 || OIL_STAGETAP == 106)
    return OilTapLin((OIL_STAGETAP > 100) ? OIL_TAPW(col, wtCol) : col);
#endif
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
    // ---- BACKLIGHT PENUMBRA (oil_penumbra) -------------------------------
    // The lamp is under the middle of the dish: the oil right next to a black
    // mass receives less of it than the open sheet does, and a pigment lit
    // less shifts hue as well as level. A smootherstep band on the OIL side
    // of the isoline only -- it can never touch the black, because it is
    // multiplied by the coverage it sits on.
    [branch] if (LA_PENUMBRA > 0.0005) {
        float u = saturate(sdf / max(max(LA_PENUMBRA_W, bmin * 4.0 * PX1440), 1e-5));
        float b = 1.0 - u * u * u * (u * (u * 6.0 - 15.0) + 10.0);
        float k = saturate(LA_PENUMBRA) * b * cov * isoOk;
        float3 lit = CssHueRotate(col, LA_PENUMBRA_HUE) * (1.0 - saturate(LA_PENUMBRA_DARK));
        col = lerp(col, lit, k);
#ifdef OIL_LAMP
        wtCol = lerp(wtCol, CssHueRotate(wtCol, LA_PENUMBRA_HUE) * (1.0 - saturate(LA_PENUMBRA_DARK)), k);
#endif
    }
    // ---- BRIGHT-FIELD HALO ([post] halo / halo_px) -----------------------
    // The microscope look the user is after: every dark shape carries a soft
    // bright glow hugging its outside, with a faint darker echo further out --
    // the "double contour" of a phase-contrast image. "Very weak, but quite
    // wide", so this is a low-amplitude lift over a band many pixels across
    // with a smooth falloff, never a stroked line.
    //
    // Which side is the bright one is decided per pixel from the two colours
    // that meet here, so the halo always sits OUTSIDE the darker of the two:
    // over black ink it glows on the oil side of a hole, and it would flip by
    // itself under a dark oil on a bright ink.
    // Gated by the same |grad| and isoOk tests the meniscus carries, or a
    // shallow plateau that never crosses the threshold would grow a halo with
    // no surface under it.
    [branch] if (LA_HALO > 0.0005) {
        float hw = max(LA_HALO_W, 1e-5);
        // a bigger shape carries a slightly wider halo, as a lens does...
        hw *= lerp(1.0, clamp(lensR / 0.12, 0.6, 2.0), 0.5);
        // ...but never below the floor, and never wider than the frame can
        // read as a glow rather than a wash (band_min).
        hw = clamp(hw, bmin * 6.0 * PX1440, 24.0 * PX1440);
        float lo = dot(oilC, float3(0.2126, 0.7152, 0.0722));
        float li = dot(inkC, float3(0.2126, 0.7152, 0.0722));
        float sgnB = (lo >= li) ? 1.0 : -1.0;       // +1: the oil is the bright side
        float u  = (sdf * sgnB) / hw;               // distance into the BRIGHT side
        float br = exp(-u * u);                     // the glow, hugging the edge
        float ec = exp(-(u - 2.6) * (u - 2.6));     // the faint echo beyond it
        float k  = saturate(LA_HALO) * smoothstep(0.5, 1.5, gl) * isoOk;
        float3 bright = (sgnB > 0.0) ? oilR : inkC;
        col += lerp(bright, float3(1.0, 1.0, 1.0), 0.35) * (k * br * 0.60);
        col *= 1.0 - k * ec * 0.35;
#ifdef OIL_LAMP
        {   // the white track decides its own bright side
            float sgnW = (dot(oilW, float3(0.2126, 0.7152, 0.0722)) >= li) ? 1.0 : -1.0;
            float uW   = (sdf * sgnW) / hw;
            float ecW  = exp(-(uW - 2.6) * (uW - 2.6));
            wtCol += lerp((sgnW > 0.0) ? oilRW : inkC, float3(1.0, 1.0, 1.0), 0.35) * (k * exp(-uW * uW) * 0.60);
            wtCol *= 1.0 - k * ecW * 0.35;
        }
#endif
    }
    // ---- MASS RIM (mass_rim, brief AB) -----------------------------------
    // The user's lava-lamp photos: the wax mass is TRANSLUCENT and its edge is
    // a refracting boundary with a bright rim on the lit side (refs 5 and 7),
    // never a flat black cut-out. So a thin bright band just OUTSIDE the
    // isoline -- on the mass side, sdf < 0 -- and only where that edge faces
    // the rig's LAMP: the surface normal here is -grad(field), and its dot
    // with the lamp direction is how much of the lamp the boundary catches.
    // The far side of the same mass gets nothing, which is what stops this
    // reading as an outline and makes it read as a lit body.
    [branch] if (LA_MASS_RIM > 0.0005) {
        float  rw  = max(LA_MASS_RIM_W, bmin * 1.5 * PX1440);
        float  u2  = sdf / rw;                       // < 0 on the mass side
        float  band = exp(-(u2 + 1.15) * (u2 + 1.15) * 1.6);
        // which way this piece of edge faces, and how much lamp it catches
        float2 nrm = -grad / max(gl, 1e-5);
        float  lit = saturate(dot(nrm, lampD) * 0.5 + 0.5);
        lit = lit * lit;
        // Peak channel, never luminance: the film is a saturated magenta
        // whose luminance is a third of its red.
        float  src = max(oilR.r, max(oilR.g, oilR.b));
        float3 tint = lerp(oilR / max(src, 1e-4), float3(1.0, 1.0, 1.0), 0.45);
        col += tint * (saturate(LA_MASS_RIM) * band * lit * 0.28
                       * smoothstep(0.5, 1.5, gl) * isoOk * src);
#ifdef OIL_HEIGHT
        // B re-ruling cross-fade: hand ABS_SHARE of this fake band to the
        // transmission (0 unless oil3d_absorb > 0)
        col -= tint * (saturate(LA_MASS_RIM) * band * lit * 0.28
                       * smoothstep(0.5, 1.5, gl) * isoOk * src) * LA_ABS_SHARE;
#endif
#ifdef OIL_LAMP
        {   // peak-channel driven: a purer windowed film lights a brighter rim
            float  srcW  = max(oilRW.r, max(oilRW.g, oilRW.b));
            float3 tintW = lerp(oilRW / max(srcW, 1e-4), float3(1.0, 1.0, 1.0), 0.45);
            float3 rimW  = tintW * (saturate(LA_MASS_RIM) * band * lit * 0.28
                                    * smoothstep(0.5, 1.5, gl) * isoOk * srcW);
            wtCol += rimW;
#ifdef OIL_HEIGHT
            wtCol -= rimW * LA_ABS_SHARE;
#endif
        }
#endif
    }
    // ---- DROPLET LENS SHADING (droplet_lens, item X) ---------------------
    // The user, holding the oil-and-water reference beside our live frame: "I
    // was thinking of the oil boundary layer -- some mechanics of how it
    // should be gradual -- and it translated to it just being blurry. The
    // ratio of blurry to focused is off." In the reference every droplet down
    // to four pixels is CRISP at its edge and GRADUAL inside it. Ours were
    // soft-edged flat fills, which is the opposite trade.
    //
    // So: nothing here blurs anything. Four terms, all of them read off the
    // signed distance and the surface gradient the pass already has --
    //   (a) a radial interior gradient: a droplet is a plano-convex LENS over
    //       the backlight, so its middle is lighter than its shoulder;
    //   (b) a dark band just inside the boundary, on the oil side;
    //   (c) a thin BRIGHT refractive rim hugging the boundary -- the
    //       meniscus the reference shows on every droplet, crisp and the same
    //       few px wide at every size, as against the existing [post] halo,
    //       which is deliberately weak and 8-15 px wide;
    //   (d) a small specular, offset toward the RIG's lamp, so it swings when
    //       the lamp does instead of being painted on.
    //
    // All of it gated by smoothstep(gl) * isoOk, which is the cheap test for
    // "is there a CURVED surface here": a droplet's kernel stays steep right
    // through its middle, while deep inside a big merged mass the gradient
    // collapses. That is what keeps the film flat, keeps the OLED's black
    // black in the middle of a mass, and gives the lens treatment to the
    // things that are actually lenses.
    [branch] if (LA_LENS > 0.0005) {
        float k0  = saturate(LA_LENS);
        float lg  = smoothstep(0.35, 1.20, gl) * isoOk;
        float sIn = (sdf >= 0.0) ? 1.0 : -1.0;     // +1 in the oil, -1 in a hole
        float din = abs(sdf);                      // distance INTO whichever body
        // This lens's own radius, off the FULL gradient: lensR above is
        // deliberately blob-biased and floored at 0.02 p-units (~29 px), which
        // is right for the film's bands and useless for a 4-px droplet.
        float lensRD = clamp(0.78 / max(gl, 1e-3), 0.0012, 0.35);
        float u   = saturate(din / max(lensRD * 0.72, 1e-6));
        // (a) the interior: smootherstep so there is no knee at the rim and
        // none at the middle -- gradual is the whole point of the term.
        [branch] if (LA_LENS_CENTRE > 0.0005) {
            float dome = u * u * u * (u * (u * 6.0 - 15.0) + 10.0);
            // a hole is a lens too: its middle passes more of the film's own
            // light than its shoulder does, so it lifts toward the film
            // colour. An oil droplet lifts toward the backlight instead.
            float3 tgt = (sIn > 0.0) ? lerp(col, float3(1.0, 1.0, 1.0), 0.55) : oilR;
            col = lerp(col, tgt, k0 * saturate(LA_LENS_CENTRE) * dome * lg * 0.55);
#ifdef OIL_LAMP
            wtCol = lerp(wtCol, (sIn > 0.0) ? lerp(wtCol, float3(1.0, 1.0, 1.0), 0.55) : oilRW,
                        k0 * saturate(LA_LENS_CENTRE) * dome * lg * 0.55);
#endif
        }
        float bw = max(LA_LENS_BAND, bmin * 1.5 * PX1440);
        // (b) the dark band, on the OIL side only: inside a hole it would be
        // black on black, and this way the same term shades an oil droplet's
        // shoulder and the film's own lip at the edge of a hole.
        float xb  = (sdf - bw * 1.15) / bw;
        float bnd = exp(-xb * xb) * step(0.0, sdf);
        col *= 1.0 - k0 * 0.30 * bnd * lg;
#ifdef OIL_LAMP
        wtCol *= 1.0 - k0 * 0.30 * bnd * lg;
#endif
        // (c) the bright refractive rim, hugging the boundary just outside
        // the dark band. Authored in px and floored, never scaled by the
        // element's size, which is what makes a 4-px droplet carry the same
        // crisp rim a mass does instead of a sub-pixel smear.
        float xm  = (sdf + bw * 0.55) / (bw * 0.85);
        float men = exp(-xm * xm);
        col += lerp(oilR, float3(1.0, 1.0, 1.0), 0.45) * (k0 * 0.30 * men * lg);
#ifdef OIL_LAMP
        wtCol += lerp(oilRW, float3(1.0, 1.0, 1.0), 0.45) * (k0 * 0.30 * men * lg);
#endif
        // (d) the specular. nOut is the outward direction of whichever body
        // this pixel is inside, so one expression lights a droplet and a hole
        // alike, and both turn to face the lamp when the rig moves it.
        [branch] if (LA_DROP_SPEC > 0.0005) {
            float2 Lv = float2(LA_LAMP_X * aspect, LA_LAMP_Y) - pp;
            float2 Ld = Lv / max(length(Lv), 1e-6);
#if defined(DROP_LIFE) && !defined(DROP_RIGLAMP)
            // droplet_shadow on: a DROPLET's glint faces the lamp that throws
            // its shadow (Oil3D lamp 1), so the two sit on opposite sides. The
            // masses keep the rig's glint (the haze and the bloom read that
            // lamp): only pixels in a droplet turn, faded out over its rim.
            // (DROP_RIGLAMP, droplet_shadow_lamp 1: the shadow comes from the
            // rig lamp too, so the glint stays the rig's, as the masses'.)
            [branch] if (laDrop[1].z > 0.5 && dlRho < 1.4) {
                float2 Lm = lerp(Ld, -laDrop[1].xy, 1.0 - smoothstep(1.0, 1.4, dlRho));
                float  lm = length(Lm);
                Ld = (lm > 1e-3) ? Lm / lm : -laDrop[1].xy;
            }
#endif
            float2 nO = (-sIn / gl) * grad;
            float  f  = saturate(dot(nO, Ld));
            float  f2 = f * f; f2 = f2 * f2;              // ^4: a small hotspot
            float  w  = (u - 0.58) / 0.30;
            float  sp = f2 * exp(-w * w);
#ifdef SPEC_TINT
            // T7 oil_spec_tint: this glint is the broad grey-white band ~200 px
            // into a big mass on the lamp side (u ~ 0.58 of a lens radius capped
            // at 0.35 p) and the grey disc in a droplet. Today's colour is 80 %
            // white, added after the lamp windows (it skipped the colour). Here
            // it takes the light in the tank -- 1 the film colour the body sits
            // in (oilR: what it mirrors), 2 lamp 1's window colour -- at today's
            // peak channel (only the colour changes); the white track the same
            // under white light (its own film / white).
            {
                float3 g0 = lerp(oilR, float3(1.0, 1.0, 1.0), 0.80);
                float  gm = max(g0.r, max(g0.g, g0.b));
#if SPEC_TINT == 2
                float3 gA = (laGlow.w > 0.5) ? DsToSrgb(max(laGlow.rgb, 0.0)) : float3(1.0, 1.0, 1.0);
#else
                float3 gA = oilR;
#endif
                float  gAm = max(gA.r, max(gA.g, gA.b));
                col += ((gAm > 1e-4) ? gA * (gm / gAm) : g0) * (k0 * saturate(LA_DROP_SPEC) * sp * lg * 0.50);
#ifdef OIL_LAMP
                float3 gW = lerp(oilRW, float3(1.0, 1.0, 1.0), 0.80);
#if SPEC_TINT == 2
                wtCol += gW * (k0 * saturate(LA_DROP_SPEC) * sp * lg * 0.50);
#else
                float  gWm = max(gW.r, max(gW.g, gW.b)), oWm = max(oilRW.r, max(oilRW.g, oilRW.b));
                wtCol += ((oWm > 1e-4) ? oilRW * (gWm / oWm) : gW) * (k0 * saturate(LA_DROP_SPEC) * sp * lg * 0.50);
#endif
#endif
            }
#else
            col += lerp(oilR, float3(1.0, 1.0, 1.0), 0.80)
                 * (k0 * saturate(LA_DROP_SPEC) * sp * lg * 0.50);
#ifdef OIL_LAMP
            wtCol += lerp(oilRW, float3(1.0, 1.0, 1.0), 0.80)
                  * (k0 * saturate(LA_DROP_SPEC) * sp * lg * 0.50);
#endif
#endif
        }
    }
#ifdef DROP_LIFE
    // ---- droplet_core: the droplet as a lens of its own (2026-11-02) -----
    // A backlit oil drop in water: rays through its middle pass nearly
    // straight, so the film behind shows there, magnified and dimmed by the
    // oil; toward the edge they bend out of the lens's reach, so a DARK
    // refracting rim rings a lighter centre -- the panel showed the opposite
    // (a dark plateau inside a lighter ring: the halo, oil_glow and the
    // film's thin edge). (1) here: the lit centre, a soft dome over the inner
    // ~60 % of the disc in dlRho, nudged away from the lamp, toward the film
    // BEHIND the hole (dlFilm: lit, before any shadow or the thin edge) at
    // film_level x 0.6; (2) late, after the halo: the rim (see below).
    // Specks stay solid dark (the lens opens between a 3- and a 14-px radius);
    // solid holes only, so neither the film nor a mass is lifted.
    [branch] if (laDrop[2].x > 0.0005 && dlRho < 1.0 && sdf < 0.0) {
        float  szK = smoothstep(laDrop[2].y, laDrop[2].z, dlRv);
        float2 un  = dlQ / max(length(dlQ), 1e-6);
        float  rc  = length(un * dlRho - 0.12 * laDrop[1].xy);
        float  kC  = saturate(laDrop[2].x) * szK * saturate(dlK)
                   * (1.0 - smoothstep(0.10, 0.80, rc)) * 0.55;
        col = lerp(col, dlFilm * (min(LA_FILM_LEVEL, 1.0) * 0.60), kC);
#ifdef OIL_LAMP
        wtCol = lerp(wtCol, dlFilmW * (min(LA_FILM_LEVEL, 1.0) * 0.60), kC);
#endif
    }
#endif
    // ---- outside glow (oil_glow): the lens spills a little of its own
    // colour into the ink around it -- diffuse, never a line (refs 4/5).
    if (LA_OIL_GLOW > 0.0005) {
        float dOut = max(-sdf, 0.0) / max(edgeW * 1.3, 1e-5);
        float go   = exp(-dOut * dOut) * (1.0 - alpha) * smoothstep(0.5, 1.5, gl);
        col += oilThinR * (go * 0.45 * saturate(LA_OIL_GLOW));
#ifdef OIL_LAMP
        wtCol += oilThinRW * (go * 0.45 * saturate(LA_OIL_GLOW));
#endif
    }

)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
    // ---- bubble swarms ---------------------------------------------------
    // Two procedural cellular layers, each masked to one side of the oil
    // surface: dark water droplets trapped INSIDE the oil (ref 2, ref 3) and
    // oil droplets floating on the open ink (ref 3). Both creep and warp so
    // they are not glued to the screen.
    {
        float st = LA_TIME;
        float2 warp = float2(sin(pp.y * 4.1 + st * 0.11), cos(pp.x * 3.7 - st * 0.09));
        // DENSITY vs OPACITY: "denser here, sparser there" belongs in the
        // swarm's CELL DENSITY (and therefore inside AcidSwarm, decided per
        // cell), never in the composite alpha — fading the alpha tints every
        // droplet with the layer under it, which is why holes once came out
        // dark orange instead of ink-coloured. It must not be a per-PIXEL term
        // either, or the presence test flips part-way across a droplet and
        // slices a wedge out of it.
        // AA: fwidth() of a cellular min() jumps wherever the nearest cell
        // changes, which smeared droplets into short streaks along the cell
        // seams. p-space y spans 0..1 over the frame height, so one pixel is
        // texelSize.y — use that fixed width instead.
        float aaS = texelSize.y * 1.2;
        // swarm_lens: a trapped droplet is a HOLE IN THE FILM, not a punched
        // disc. The oil thins into it, so it gets the same soft edge and the
        // same thin-oil colour fringe as the outer boundary (ref 6), a
        // softened dark ring, and one small lens highlight offset toward the
        // light instead of a flat black interior.
        float lensK = saturate(LA_SWARM_LENS);
        if (LA_SWARM_HOLES > 0.002) {
            float2 sp = pp + warp * 0.030 + st * float2(0.0040, -0.0030);
            // two octaves 2.7x apart so the sizes run from ~1 px to ~2% of the
            // frame height; the fine one stays sparse or the oil reads as dust
            float2 sn1, sn2; float sr1, sr2;
            float  sda = AcidSwarm(sp, LA_SWARM_SCALE_HOLES, LA_SWARM_DENSITY, LA_SWARM_CLUMP, LA_SWARM_R_MIN, LA_SWARM_R_MAX, sn1, sr1);
            float  sdb = AcidSwarm(sp + 7.31, LA_SWARM_SCALE_HOLES * 2.7, LA_SWARM_DENSITY * 0.12, LA_SWARM_CLUMP,
                                   LA_SWARM_R_MIN, LA_SWARM_R_MAX, sn2, sr2);
            bool   wa = (sda <= sdb);
            float  sd = wa ? sda : sdb;
            float2 sn = wa ? sn1 : sn2;
            float  sr = max(wa ? sr1 : sr2, 1e-4);
            float  sw   = lerp(aaS, max(sr * 0.26, aaS), lensK);
            float  scov = 1.0 - smoothstep(-sw, sw, sd);
            float  sx = sd / max(lerp(aaS * 2.4, sw * 1.15, lensK), 1e-6);
            float  srim = exp(-sx * sx);
            float  mask = smoothstep(0.004, 0.030, sdf) * LA_SWARM_HOLES;   // inside the oil only
            if (lensK > 0.0005) {   // thin-oil fringe just OUTSIDE the droplet
                float fx = max(sd, 0.0) / max(sw * 1.8, 1e-5);
                col = lerp(col, oilThinR,
                           exp(-fx * fx) * (1.0 - scov) * mask * lensK * 0.75);
#ifdef OIL_LAMP
                wtCol = lerp(wtCol, oilThinRW,
                            exp(-fx * fx) * (1.0 - scov) * mask * lensK * 0.75);
#endif
            }
            // a trapped water droplet shows the INK through the oil film
            col = lerp(col, inkC * (1.0 - LA_SWARM_DARK * (1.0 - 0.40 * lensK)), scov * mask);
            col *= 1.0 - LA_SWARM_RIM_DARK * (1.0 - 0.55 * lensK) * srim * mask;
#ifdef OIL_LAMP
            wtCol = lerp(wtCol, inkC * (1.0 - LA_SWARM_DARK * (1.0 - 0.40 * lensK)), scov * mask);
            wtCol *= 1.0 - LA_SWARM_RIM_DARK * (1.0 - 0.55 * lensK) * srim * mask;
#endif
            if (lensK > 0.0005) {
                float2 vc = sn * (sd + sr);
                float  hd = length(vc - float2(-0.707, -0.707) * (0.42 * sr))
                          / max(sr * 0.30, 1e-5);
                col += oilR * (exp(-hd * hd) * scov * mask * lensK * 0.30);
#ifdef OIL_LAMP
                wtCol += oilRW * (exp(-hd * hd) * scov * mask * lensK * 0.30);
#endif
            }
        }
        if (LA_SWARM_DROPS > 0.002) {
            float2 sp = pp * 1.31 - warp * 0.024 + st * float2(-0.0031, 0.0042) + 41.7;
            float2 sn1, sn2; float sr1, sr2;
            float  sda = AcidSwarm(sp, LA_SWARM_SCALE_DROPS, LA_SWARM_DENSITY, LA_SWARM_CLUMP, LA_SWARM_R_MIN, LA_SWARM_R_MAX, sn1, sr1);
            float  sdb = AcidSwarm(sp + 3.17, LA_SWARM_SCALE_DROPS * 2.7, LA_SWARM_DENSITY * 0.12, LA_SWARM_CLUMP,
                                   LA_SWARM_R_MIN, LA_SWARM_R_MAX, sn2, sr2);
            bool   wa = (sda <= sdb);
            float  sd = wa ? sda : sdb;
            float2 sn = wa ? sn1 : sn2;
            float  sr = max(wa ? sr1 : sr2, 1e-4);
            float  sw   = lerp(aaS, max(sr * 0.26, aaS), lensK);
            float  scov = 1.0 - smoothstep(-sw, sw, sd);
            float  sx = sd / max(lerp(aaS * 2.4, sw * 1.15, lensK), 1e-6);
            float  srim = exp(-sx * sx);
            float  mask = smoothstep(0.004, 0.030, -sdf) * LA_SWARM_DROPS;  // open ink only
            col = lerp(col, laOil[3].rgb, scov * mask);
            col *= 1.0 - LA_SWARM_RIM_DARK * (1.0 - 0.55 * lensK) * srim * mask;
#ifdef OIL_LAMP
            wtCol = lerp(wtCol, laOil[3].rgb, scov * mask);
            wtCol *= 1.0 - LA_SWARM_RIM_DARK * (1.0 - 0.55 * lensK) * srim * mask;
#endif
            if (lensK > 0.0005) {
                // an oil droplet on open ink is a lens too: a faint caustic
                // just OUTSIDE it and one small highlight inside.
                float fx = max(sd, 0.0) / max(sw * 1.6, 1e-5);
                col += laOil[3].rgb * (exp(-fx * fx) * (1.0 - scov) * mask * lensK * 0.22);
#ifdef OIL_LAMP
                wtCol += laOil[3].rgb * (exp(-fx * fx) * (1.0 - scov) * mask * lensK * 0.22);
#endif
                float2 vc = sn * (sd + sr);
                float  hd = length(vc - float2(-0.707, -0.707) * (0.42 * sr))
                          / max(sr * 0.30, 1e-5);
                col += exp(-hd * hd) * scov * mask * lensK * 0.15;
#ifdef OIL_LAMP
                wtCol += exp(-hd * hd) * scov * mask * lensK * 0.15;
#endif
            }
        }
    }

    // ---- meniscus: thin BRIGHT ink-coloured halo just outside the rim ----
    // In the references this is the brightest thing in the frame (cyan on
    // ref 1) and it is what separates the oil from the ink. Painted with the
    // ramp's bright stop so it reads even over black ink.
    // ---- meniscus_film_mix, part 1: the ink-brightness GATE --------------
    // haloInk is the meniscus_from_ink gate: "where the ink just outside is
    // dark, the halo vanishes", so a foreign colour can never be stroked onto
    // black ink. In acid-rise-12 the ink outside IS black everywhere, so
    // haloInk is 0 in every pixel and this whole term -- the 0.85 at the top
    // of the ring stack -- paints nothing at all (measured: meniscus 0 is
    // byte-identical to meniscus 0.85). The gate is an INK-SOURCE gate: once
    // the colour comes from the FILM it is guarding against a danger that no
    // longer exists, so the mix opens it by the same amount it moves the
    // colour. Only the meniscus sees this; the dark hairline below keeps the
    // original haloInk. At mix 0, menInk == haloInk and nothing changes.
    float menInk = haloInk;
    [branch] if (LA_MEN_FILM_MIX > 0.0005) menInk = lerp(haloInk, 1.0, saturate(LA_MEN_FILM_MIX));
    float haloW = 0.0;
    if (LA_MENISCUS * haloMul * menInk > 0.002) {
        float hx = (sdf - menCtr) / menHW;
        float halo = exp(-hx * hx);
        // sdf = (field - thresh) / |grad| is only a distance where |grad| is
        // strong. On a broad low-gradient plateau that never reaches the
        // threshold, a tiny field deficit divided by a tiny gradient still
        // lands inside the halo band, painting a fuzzy blob-shaped glow with
        // no oil under it. Real blob surfaces have |grad| >~ 2 (it scales as
        // 1/radius, and the largest discs here are ~0.4), so gate on it.
        halo *= smoothstep(0.5, 1.5, gl) * isoOk;
        haloW = saturate(halo * LA_MENISCUS * haloMul * menInk);
        // ---- meniscus_film_mix, part 2: the COLOUR from the FILM ---------
        // menC above is derived from the INK (meniscus_from_ink), and this
        // band replaces up to `meniscus` (0.85 live) of the pixel -- so it
        // sits on top of, and hides, every FILM-hue feature. oilR is the film
        // colour the rim terms beside it already use, hue-rotated by the seam
        // reflect where there is a seam, so mixing toward it is what lets a
        // hue2 seam reach the halo. Lifted by the SAME factors the ink path
        // uses, so the halo stays a bright caustic and only its colour moves:
        // haloW (shape, width, amplitude) is computed above and untouched.
        float3 menCm = menC;
        [branch] if (LA_MEN_FILM_MIX > 0.0005) {
            float3 fs = AcidRgb2Hsv(oilR);
            fs.y = saturate(fs.y * 1.35);
            fs.z = saturate(fs.z * 1.55 + 0.05);
            menCm = lerp(menC, AcidHsv2Rgb(fs), saturate(LA_MEN_FILM_MIX));
        }
        col = lerp(col, menCm, haloW);
#ifdef OIL_LAMP
        float3 menCmW = menC;
        [branch] if (LA_MEN_FILM_MIX > 0.0005) {
            float3 fsW = AcidRgb2Hsv(oilRW);
            fsW.y = saturate(fsW.y * 1.35);
            fsW.z = saturate(fsW.z * 1.55 + 0.05);
            menCmW = lerp(menC, AcidHsv2Rgb(fsW), saturate(LA_MEN_FILM_MIX));
        }
        wtCol = lerp(wtCol, menCmW, haloW);
#endif
    }

    // ---- interface speckle: sparse cellular dots hugging the boundary ----
    if (LA_SPECKLE > 0.001) {
        float2 cell = floor(pp * LA_SPECKLE_SCALE);
        float  rnd  = AcidHash21(cell);
        float2 jit  = float2(AcidHash21(cell + 7.13), AcidHash21(cell + 13.71));
        float  dd   = length(frac(pp * LA_SPECKLE_SCALE) - (0.25 + 0.5 * jit));
        float  dot1 = step(dd, 0.08 + 0.26 * rnd) * step(0.88, rnd);
        float  sMask = saturate(smoothstep(-0.020, 0.0, sdf) * (1.0 - cov) * 1.2
                              + (1.0 - smoothstep(LA_RIM_WIDTH * 2.0, LA_RIM_WIDTH * 7.0, sdf)) * cov * 0.30);
        col = lerp(col, col * 0.18, dot1 * sMask * LA_SPECKLE);
#ifdef OIL_LAMP
        wtCol = lerp(wtCol, wtCol * 0.18, dot1 * sMask * LA_SPECKLE);
#endif
    }
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
#ifdef DROP_HAZERIM
    // post-night T4, droplet_haze_rim: the depth haze's share at this pixel
    // without the inside-the-drop test (the dark rim reaches past the
    // isoline), for the rim below; 0 without the haze
    float hzRim = 0.0;
#endif
#ifdef OIL_GRAIN
    // ---- Oil3D W3: the DEPTH HAZE on what sits in the water ---------------
    // After every droplet term (rim, meniscus, lens, halo, speckle), so a deep
    // droplet fades WHOLE. Its depth comes from the droplet loop's own weights
    // (depSum without the dye prior; z = 2 (depth - 0.5): 0 at the masses'
    // plane, 1 at the film) and it is seen through z of the murk: the pixel
    // keeps T = e^(-laGrain[0].z z) of its light and takes 1 - T of the lit
    // water's colour, which here is the film's own (oilC: the film block gave
    // it the veil, the grain and murk_haze_lift), in linear light. Never
    // inside a mass (the blobs-only field: the masses sit on the surface and
    // stay black, their film-coloured droplets too), never for a droplet
    // nearer than the masses' plane; plain film round a deep droplet already
    // is the film's colour, so only the droplet itself moves.
    [branch] if (laGrain[0].z > 0.0) {
        float dW  = depW - dyeW;
        float zD  = (dW > 1e-5) ? saturate(((depSum - dyeZ * dyeW) / dW - 0.5) * 2.0) : 0.0;
#ifdef DROP_HAZERIM
        hzRim = saturate(dW * 4.0) * (1.0 - exp(-laGrain[0].z * zD));
#endif
        float aaH = fwidth(fieldB) * LA_AA_SCALE + 1e-4;
        float wv  = saturate(dW * 4.0) * smoothstep(thresh - aaH, thresh + aaH, fieldB)
                  * (1.0 - exp(-laGrain[0].z * zD));
        [branch] if (wv > 1e-4) {
            col = pow(lerp(pow(max(col, 0.0), 2.2), pow(max(oilC, 0.0), 2.2), wv), 1.0 / 2.2);
#ifdef OIL_LAMP
            wtCol = pow(lerp(pow(max(wtCol, 0.0), 2.2), pow(max(oilW, 0.0), 2.2), wv), 1.0 / 2.2);
#endif
        }
    }
#endif
    // ---- MACRO "CELLULOSE" SURFACE TEXTURE (cellulose) -------------------
    // The user, on the Requiem microscope frame: the cell body is fibrous and
    // mottled, "macro-ish cellulose noise", and they want it "in the black oil
    // more than the oil". Two cheap fBms:
    //   * an ANISOTROPIC one -- the sample point is rotated into a slowly
    //     turning frame and squashed 5:1 along it, so the same noise reads as
    //     STRANDS instead of blobs -- run through 1-|2f-1| so the ridges
    //     become filaments rather than lumps;
    //   * a slow isotropic one three times coarser, for the mottling that
    //     makes the strands come in patches.
    // Both drift with the rise (LA_CELL_DRIFT = rise_speed * cellulose_drift), so
    // the texture belongs to the masses and not to the screen, and this sits
    // in the DISPLAY pass -- under the film grain, because the fibres are the
    // surface and the grain is the camera.
    //
    // On the BLACK side it is a THICKNESS effect, never a fill: the strands
    // are strongest where the black layer is thin (just inside its edge) and
    // fall off to nothing deep inside a mass, so the OLED's true black is
    // still true black over most of the frame. On the film it is a faint
    // multiplicative mottle of the colour, which cannot lift anything.
    [branch] if (LA_CELL_INK > 0.0005 || LA_CELL_OIL > 0.0005) {
        float  csc = max(LA_CELL_SCALE, 1e-4);
        float2 q0  = pp + float2(0.0, LA_CELL_DRIFT * LA_TIME);
        float  ang = 0.9 * AcidVNoise(q0 * 0.7) + LA_TIME * 0.010;
        float2 ca  = float2(cos(ang), sin(ang));
        float2 qr  = float2(dot(q0, ca), dot(q0, float2(-ca.y, ca.x))) / csc;
        qr.x *= 0.20;
        float  f   = saturate(AcidFbm(qr) * 1.143);
        float  str = 1.0 - abs(2.0 * f - 1.0);
        float  mot = saturate(AcidFbm(q0 / (csc * 3.5)) * 1.143);
        float  n   = (str - 0.5) * 0.75 + (mot - 0.5) * 0.45;
        // Thickness falloff into the black, Beer-Lambert rather than
        // Gaussian: a long tail is what makes the strands fade OUT of a mass
        // instead of ending on a rim of their own. The amplitudes below are
        // in the display's sRGB-ENCODED domain, where a lift of 0.08 over
        // black is ~1.5 nits at SDR white 240 -- "a few nits at most", and
        // the negative half of the noise is simply clamped away by the black.
        float  dIn = max(-sdf, 0.0);
        float  bw  = max(csc * 3.0, 24.0 * PX1440);
        float  th  = exp(-dIn / bw);
        // isoOk: on a plateau that never crosses the threshold sdf collapses
        // to nearly zero over a wide area, and without this gate the falloff
        // would print a mass-shaped grey cloud with no edge under it.
        col += n * (0.28 * LA_CELL_INK * (1.0 - alpha) * th * isoOk);
        col *= 1.0 + n * (0.45 * LA_CELL_OIL * alpha);
#ifdef OIL_LAMP
        wtCol += n * (0.28 * LA_CELL_INK * (1.0 - alpha) * th * isoOk);
        wtCol *= 1.0 + n * (0.45 * LA_CELL_OIL * alpha);
#endif
    }
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
    // ---- CAST SHADOWS (shadow_amt / shadow_len / shadow_soft / light_z) --
    // The user, brief BC: "idk if this is a feature, volumetric lighting, or
    // some implementation of it. The light should cast shadows essentially...
    // it can be from behind, or the bottom, or the top or side." Until now the
    // lamp drove the specular, the mass rim, the penumbra, the haze and the
    // bloom, and nothing in the frame blocked a single photon.
    //
    // WHAT CASTS. In this look the black "masses" are the NEGATIVE space of
    // the blob field -- the film is the blobs -- so a mass shadow cannot be
    // written per caster the way a rim can. It is SAMPLED instead: four taps
    // from this pixel TOWARD the lamp, each re-evaluating the blob field
    // alone (no gradient, no colour, no comb -- a shadow is low frequency and
    // none of that would survive the softening). A tap that lands on the dark
    // side of the isoline is a tap where the light never got through. The
    // droplets are the other caster and they are done ANALYTICALLY, in a walk
    // of the same 3x3 cells the field uses, because a droplet's shadow is
    // short: a capsule from its own centre is exact, and cheaper than four
    // more taps per droplet.
    //
    // LIGHT_Z is the lamp's stand-off from the plane of the dish:
    //   > 0  in front of / above it -- the shadow rakes away from the lamp,
    //        and the higher the lamp stands the shorter it is;
    //   = 0  in the plane -- the longest shadows this key can make;
    //   < 0  behind the dish: BACKLIT. There is no direction left to rake
    //        toward, so the shadow spills evenly round every caster onto the
    //        film in front of it, which is what a body lit from behind does
    //        (and the caster's own edge keeps the mass_rim / meniscus glow).
    //
    // WHERE. It MULTIPLIES the composed film, here -- after every shading
    // term, before the grain and before the post pass -- so halation, fog and
    // bloom all see a darker source and a shadow reads as LESS LIGHT rather
    // than as a grey overlay dropped on the picture. Over the black it is a
    // no-op by construction: a multiplier cannot darken a zero.
    float shadowMul = 1.0;
    [branch] if (LA_SHADOW_AMT > 0.0005) {
        float2 lampS = float2(LA_LAMP_X * aspect, LA_LAMP_Y);
        float2 toLv  = lampS - pp;
        float2 toLd  = toLv / max(length(toLv), 1e-5);
        float  lz    = clamp(LA_LIGHT_Z, -1.0, 1.0);
        // 1.0 at light_z 0.35 (a lamp a little in front of the dish), up to
        // 5x as the lamp sinks into the plane, half as it rises over it.
        float  elong = min(0.35 / max(lz, 0.07), 5.0);
        float  dirW  = saturate(lz * 4.0);            // 0 at and below zero
        float  sft   = saturate(LA_SHADOW_SOFT);
        float  L0    = max(LA_SHADOW_LEN, 0.0);
        float  lenD  = L0 * elong * dirW;             // the raked length
        float  lenI  = L0 * (1.0 - dirW) * 0.55;      // the backlit spill
        // Four taps, near to far. The near one is tight and counts most, the
        // far ones are wide and weak: "darkest and tightest at the caster,
        // fading and softening with distance".
        const float4 fk = float4(0.30, 0.58, 0.82, 1.00);
        const float4 wk = float4(1.00, 0.76, 0.52, 0.32);
        float2 o0 = toLd * (lenD * fk.x) + float2( 0.7071,  0.7071) * (lenI * fk.x);
        float2 o1 = toLd * (lenD * fk.y) + float2(-0.7071,  0.7071) * (lenI * fk.y);
        float2 o2 = toLd * (lenD * fk.z) + float2(-0.7071, -0.7071) * (lenI * fk.z);
        float2 o3 = toLd * (lenD * fk.w) + float2( 0.7071, -0.7071) * (lenI * fk.w);
        // One bounding disc round all four taps, so a blob nowhere near this
        // pixel's light path costs a dot product and nothing else.
        float2 cB = pp + toLd * (lenD * 0.64);
        float  rB = lenD * 0.64 + lenI + 1e-4;
        float4 ft = float4(0.0, 0.0, 0.0, 0.0);
        [loop]
        for (int si = 0; si < nb; si++) {
            AcidBlobGPU S = AcidBlobs[si];
            float2 sc  = float2(S.a.x * aspect, S.a.y);
            float  e   = 1.0 + S.b.w;
            float  sup = S.a.z * LA_SUPPORT_SCALE;
            float  reach = sup * e + rB;
            float2 qb  = cB - sc;
            if (dot(qb, qb) >= reach * reach) continue;
            float  s2 = sup * sup;
            float2 q; float d2, u;
            q = pp + o0 - sc; q.y /= e; d2 = dot(q, q);
            if (d2 < s2) { u = 1.0 - d2 / s2; ft.x += u * u * u * S.a.w; }
            q = pp + o1 - sc; q.y /= e; d2 = dot(q, q);
            if (d2 < s2) { u = 1.0 - d2 / s2; ft.y += u * u * u * S.a.w; }
            q = pp + o2 - sc; q.y /= e; d2 = dot(q, q);
            if (d2 < s2) { u = 1.0 - d2 / s2; ft.z += u * u * u * S.a.w; }
            q = pp + o3 - sc; q.y /= e; d2 = dot(q, q);
            if (d2 < s2) { u = 1.0 - d2 / s2; ft.w += u * u * u * S.a.w; }
        }
        // The far taps read the occluder through a WIDER threshold band --
        // that is the penumbra, and it costs one madd each. A weighted mean,
        // not a max: the max of four taps walks down four steps as the pixel
        // leaves a mass, and prints them as bands.
        float4 bk = thresh * (0.18 + sft * (0.35 + 1.9 * fk));
        float4 oc;
        oc.x = 1.0 - smoothstep(thresh - bk.x, thresh + bk.x, ft.x);
        oc.y = 1.0 - smoothstep(thresh - bk.y, thresh + bk.y, ft.y);
        oc.z = 1.0 - smoothstep(thresh - bk.z, thresh + bk.z, ft.z);
        oc.w = 1.0 - smoothstep(thresh - bk.w, thresh + bk.w, ft.w);
        float occ = dot(oc, wk) / dot(wk, float4(1.0, 1.0, 1.0, 1.0));
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
        // ---- the droplets, analytically ----------------------------------
        float dOcc = 0.0;
#ifdef DROP_LIFE
        // droplet_shadow on: its own shadows replace these (dOcc *= 0 below), so skip the walk
        [branch] if (LA_DROPS_ON > 0.5 && laDrop[1].w > 0.5) {
#else
        [branch] if (LA_DROPS_ON > 0.5) {
#endif
            const int gw2 = (int)LA_DROP_GRID_W, gh2 = (int)LA_DROP_GRID_H;
            float cellP = min(aspect / max((float)gw2, 1.0), 1.0 / max((float)gh2, 1.0));
            int cx2 = clamp((int)floor(uv.x * gw2), 0, gw2 - 1);
            int cy2 = clamp((int)floor(uv.y * gh2), 0, gh2 - 1);
            float2 sdir = -toLd;                       // away from the lamp
            [loop]
            for (int oy2 = -1; oy2 <= 1; oy2++) {
                int yy2 = cy2 + oy2;
                if (yy2 < 0 || yy2 >= gh2) continue;
                [loop]
                for (int ox2 = -1; ox2 <= 1; ox2++) {
                    int xx2 = cx2 + ox2;
                    if (xx2 < 0 || xx2 >= gw2) continue;
                    uint2 cl = DropCells[yy2 * gw2 + xx2];
                    [loop]
                    for (uint dj = 0; dj < cl.y; dj++) {
                        float4 D = AcidDrops[cl.x + dj];
                        float dqz = floor(D.w * 0.25);
                        float dwr = D.w - 4.0 * dqz;
                        float rng = (dwr > 1.5) ? 1.0 : 0.0;
                        float gate = dwr - 2.0 * rng;
                        if (gate <= 0.002) continue;
                        float R = abs(D.z);
                        // A droplet is a body, and its HEIGHT is its own
                        // radius: a 3-px speck throws a 3-px shadow and a big
                        // ring a long one. Capped at half a grid cell, which
                        // is as far as the 3x3 walk can see a caster at all.
                        float hN = saturate(R / 0.012);
                        float dl = min(lenD * hN, cellP * 0.55);
                        // The capsule starts just BEYOND the droplet's own
                        // rim, not at its centre. Centred, a droplet's shadow
                        // came out concentric -- a dark collar all round it,
                        // including on the side facing the lamp, which is the
                        // one side that cannot be in shadow. Pushed out by
                        // three quarters of the radius it leaves the lamp
                        // side clean and the lobe emerges on the far side,
                        // which is what makes it read as a shadow and not as
                        // a halo.
                        float2 dc = float2(D.x * aspect, D.y) + sdir * (R * 0.75);
                        float2 v = pp - dc;
                        float  t = clamp(dot(v, sdir), 0.0, dl);
                        float2 pe = v - sdir * t;
                        float  f  = (dl > 1e-6) ? (t / dl) : 0.0;
                        // A hollow droplet is a BUBBLE: its middle is film,
                        // so it shadows with its WALL and not with a disc.
                        float core  = rng * R;
                        float wallR = lerp(min(R, cellP * 0.5),
                                           max(R * saturate(LA_DROP_RING_WIDTH), 0.0008), rng);
                        float rd = length(pe) - core;
                        if (rng < 0.5) rd = max(rd, 0.0);
                        float rad = wallR * (0.90 + sft * (0.30 + 1.5 * f))
                                  + min(lenI * hN, cellP * 0.45);
                        // ...and a droplet is a LENS, not a plug: it bends
                        // the light aside rather than eating it, so it never
                        // throws the full umbra a mass does.
                        float s = 0.85 * exp(-2.0 * f) * gate
                                * exp(-(rd * rd) / max(rad * rad, 1e-12));
                        dOcc = max(dOcc, s);
                    }
                }
            }
        }
#ifdef DROP_LIFE
        dOcc *= laDrop[1].w;   // droplet_shadow on: its own lamp's shadows replace these
#endif
        shadowMul = 1.0 - saturate(LA_SHADOW_AMT) * saturate(max(occ, dOcc));
#ifdef OIL_HEIGHT
        if (LA_OIL_H_DEBUG > 4.5 && LA_OIL_H_DEBUG < 5.5) shadowMul = 1.0;   // the dump's film: UE casts its own
#endif
        col *= shadowMul;
#ifdef OIL_LAMP
        wtCol *= shadowMul;
#endif
    }
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
#ifdef DROP_LIFE
    // ---- droplet_core (2): the DARK refracting rim -------------------------
    // The outer part of the disc and the lighter ring just past its isoline
    // (the halo, oil_glow and the film's thin edge, all drawn above) go
    // darker, so the drop reads dark-rimmed round its lit centre. A band in
    // dlRho, so it scales with the drop: 0.5 .. 0.92 rising, flat to 1.0,
    // gone by 1.65.
    [branch] if (laDrop[2].x > 0.0005 && dlRho < 1.65) {
        float szR = smoothstep(laDrop[2].y, laDrop[2].z, dlRv);
        float rim = smoothstep(0.50, 0.92, dlRho) * (1.0 - smoothstep(1.00, 1.65, dlRho));
#ifdef DROP_HAZERIM
        rim *= 1.0 - saturate(laDrop[2].w) * hzRim;   // droplet_haze_rim: fades with the drop's haze
#endif
        float kR  = 1.0 - saturate(laDrop[2].x) * szR * saturate(dlK) * 0.50 * rim;
        col *= kR;
#ifdef OIL_LAMP
        wtCol *= kR;
#endif
    }
    // ---- droplet_shadow: the droplets float IN the tank (2026-11-02) ------
    // The user: "the black dots need more realism" -- flat discs, all alike,
    // nothing tying one to the film it floats over. Each droplet now throws a
    // soft shadow on the film AWAY from the Oil3D lamp (light_azim / _elev):
    // three taps toward the lamp, 5 / 12 / 22 px (x cot(elev) / cot(60)),
    // each asking whether that point lies inside a droplet's VISIBLE hole --
    // the summed kernels there (DropHoleSum) against the isoline's own value,
    // so a cluster of particles shadows as the one disc it looks like -- and
    // how long THAT droplet's shadow is: 0.9 R (1 + 1.5 t) x cot(elev) /
    // cot(60), t = 2 |depth - 0.5|, so a drop on the plane in focus throws a
    // short crisp crescent and one far from it a longer, fainter one (x (1 -
    // 0.5 t)): the shadow agrees with the depth of field. A tap counts while
    // its distance is inside that length. Never on the caster's own disc
    // (dlRho); it multiplies the composed film like shadow_amt's, so over the
    // black it is a no-op, and it replaces the old rig-lamp droplet shadow
    // (dOcc above).
    [branch] if (laDrop[0].x > 0.0005 && LA_DROPS_ON > 0.5) {
#ifdef DROP_RIGLAMP
        // post-night T4, droplet_shadow_lamp 1: toward the RIG lamp from this
        // pixel (the point the haze, the bloom and the mass glints read); the
        // length keeps lamp 1's elevation
        const float2 lvR = float2(LA_LAMP_X * aspect, LA_LAMP_Y) - pp;
        const float2 toL = lvR / max(length(lvR), 1e-6);
#else
        const float2 toL = -laDrop[1].xy;                       // toward the lamp, p-space
#endif
        const float  kE  = laDrop[0].y / 0.57735;               // cot(elev) / cot(60)
        float  F    = max(fieldB, thresh + 1e-3);
        float  iso  = (F - thresh) / (max(F, 1.0) * LA_DROP_WEIGHT);
        float  occ  = 0.0;
#ifdef DROP_HAZERIM
        float  hzS  = 0.0;                                      // the strongest caster's haze share
#endif
        [unroll]
        for (int tk = 0; tk < 3; tk++) {
            const float dPx = (tk == 0) ? 5.0 : ((tk == 1) ? 12.0 : 22.0);
            float  dlt = dPx * PX1440 * kE;
            float  dq0, rq0;
            float  sq  = DropHoleSum(pp + toL * dlt, aspect, dq0, rq0);
            float  tq  = saturate(2.0 * abs(dq0 - 0.5));
            float  len = 0.9 * rq0 * (1.0 + 1.5 * tq) * kE;     // that droplet's shadow length
            float  inW = smoothstep(0.60, 1.40, sq / max(iso, 1e-4));
            float  lnW = smoothstep(dlt - 4.0 * PX1440 * kE, dlt + 4.0 * PX1440 * kE, len);
#ifdef DROP_HAZERIM
            float  oT  = inW * lnW * (1.0 - 0.5 * tq);
            if (oT > occ) hzS = 1.0 - exp(-laGrain[0].z * saturate(2.0 * (dq0 - 0.5)));
            occ = max(occ, oT);
#else
            occ = max(occ, inW * lnW * (1.0 - 0.5 * tq));
#endif
        }
        occ *= smoothstep(0.85, 1.10, dlRho);                   // not on the caster's own disc
#ifdef DROP_HAZERIM
        occ *= 1.0 - saturate(laDrop[2].w) * hzS;               // droplet_haze_rim: its caster's haze
#endif
        float  lit = 1.0 - saturate(laDrop[0].x) * 0.75 * occ;
        col *= lit;
#ifdef OIL_LAMP
        wtCol *= lit;
#endif
    }
#endif
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
    // ---- ink-tinted toe (toe_tint) ---------------------------------------
    // The genre's darkest ink is #180808 / #2c1506 — a lifted, HUE-TINTED toe,
    // never a crush to neutral black. Lift only the bottom of the range, and
    // only toward a low-value version of the ink ramp's own mid hue, so the
    // tint always belongs to this palette (and follows the palette sweep).
    // LA_TOE_TINT = 0 leaves the shipped neutral toe untouched.
    if (LA_TOE_TINT > 0.001) {
        float3 inkHue = laInk[1].rgb / max(max(laInk[1].r, max(laInk[1].g, laInk[1].b)), 1e-3);
        float  toeL   = dot(col, float3(0.2126, 0.7152, 0.0722));
        col += inkHue * (0.12 * saturate(LA_TOE_TINT) * (1.0 - smoothstep(0.0, 0.22, toeL)));
#ifdef OIL_LAMP
        float  toeLW  = dot(wtCol, float3(0.2126, 0.7152, 0.0722));
        wtCol += inkHue * (0.12 * saturate(LA_TOE_TINT) * (1.0 - smoothstep(0.0, 0.22, toeLW)));
#endif
    }
#if defined(OIL_STAGETAP) && (OIL_STAGETAP == 7 || OIL_STAGETAP == 107)
    return OilTapLin((OIL_STAGETAP > 100) ? OIL_TAPW(col, wtCol) : col);
#endif
#ifdef OIL_LAMP2
    // ---- Oil3D L3b: THE TRIM HOLD (lamp 2) --------------------------------
    // The final trim works on ENCODED luma (post_chroma spreads the channels
    // about it and clamps at 0) and the gamut stretch re-weights the channels
    // (the BT.2020 / panel interpretation): both turn CHROMA into luminance.
    // Lamp 2's share changes the film's chroma at a luminance THE SUM pinned
    // (open water = today, a shadow = its physical ratio), so the windowed
    // composite is scaled first, in linear light, until its panel luminance
    // after the trim (OilTrimLum: the trim, the vignette, the stretch and the
    // output's clip, as the cap measures them) keeps the ratio to the white track's that
    // the two arrived with (three fixed-point steps; the metric is 1-homogeneous
    // but for the clip and the sRGB toe). Hue and chroma untouched.
    {
        const float3 GW = float3(0.2126, 0.7152, 0.0722);
        float3 cl = DsToLin(col);
        float  yC = dot(cl, GW), yWt = dot(DsToLin(wtCol), GW);
        [branch] if (yC > 1e-7 && yWt > 1e-7) {
            // (the ratio may be above 1 where a grade upstream lifted the
            // windowed film, equal load on a moved hue: the output cap takes
            // that back exactly; aiming at the white track instead undershoots
            // wherever the clip binds, measured -0.46 % on Coral's open water)
            // the vignette's factor here (applied to the encoded composite before
            // the stretch, so it decides what the output clip takes)
            float vg = 1.0;
            [branch] if (poP1.w > 0.0005) {
                float2 dv = i.pos.xy * texelSize - float2(poP1.x, poP1.y);
                float  rv = dot(dv, dv) * 2.0;
                vg = 1.0 - saturate(poP1.w) * 0.45 * rv * rv;
            }
            float tT = OilTrimLum(wtCol, vg) * (yC / yWt);
            float k  = 1.0;
            [unroll] for (int it = 0; it < 3; it++)
                k *= clamp(tT / max(OilTrimLum(DsToSrgb(cl * k), vg), 1e-9), 0.8, 1.25);
            col = DsToSrgb(cl * clamp(k, 0.8, 1.25));
        }
    }
#endif
    // (brief BX: the grey heart runs after the gamut stretch below, HeartGrey)
    // ---- final trim: post_chroma / post_lift -----------------------------
    // A transparent film costs perceptual chroma (10-20%) and a little
    // lightness against the same look opaque, measured in OKLab over the
    // non-dark pixels. Both of those are corrected here rather than in every
    // palette: chroma is scaled about the pixel's OWN luma, so hue and
    // luminance survive and only the distance from grey grows; lift is a
    // plain luma multiplier on top. 1 / 1 = untouched, and both are applied
    // to the whole composite, so ink, halo and film move together.
    if (abs(LA_POST_CHROMA - 1.0) > 0.001) {
        float pl = dot(col, float3(0.2126, 0.7152, 0.0722));
        col = max(pl.xxx + (col - pl.xxx) * LA_POST_CHROMA, 0.0);
#ifdef OIL_LAMP
        float plW = dot(wtCol, float3(0.2126, 0.7152, 0.0722));
        wtCol = max(plW.xxx + (wtCol - plW.xxx) * LA_POST_CHROMA, 0.0);
#endif
    }
#if defined(OIL_STAGETAP) && (OIL_STAGETAP == 8 || OIL_STAGETAP == 108)
    return OilTapLin((OIL_STAGETAP > 100) ? OIL_TAPW(col, wtCol) : col);
#endif
    if (abs(LA_POST_LIFT - 1.0) > 0.001) col *= LA_POST_LIFT;
#ifdef OIL_LAMP
    if (abs(LA_POST_LIFT - 1.0) > 0.001) wtCol *= LA_POST_LIFT;
#endif
#if defined(OIL_STAGETAP) && (OIL_STAGETAP == 9 || OIL_STAGETAP == 109)
    return OilTapLin((OIL_STAGETAP > 100) ? OIL_TAPW(col, wtCol) : col);
#endif
    // ---- coarse ANIMATED film grain over everything ----------------------
    // grain_shadow_weight (LA_GRAIN_SHADOW_W) biases the amplitude into the darks:
    // in every reference frame the ink is visibly noisy while the flat oil
    // discs are clean, which is what real high-ISO backlit macro looks like.
    // (1 - luma)^2 -- squared, so mid-tones already lose most of the grain.
    //
    // brief BD: this is the look's OWN second stock, independent of
    // [post] film_grain, and two things about it were wrong. It re-rolled
    // once per REFRESH (frac(time)), i.e. 240 fizz per second rather than a
    // film cadence -- it now quantises to film_grain_fps like every other
    // noise in the build. And when the image-space pass runs WITH a grain of
    // its own ([post] film_grain > 0) it is zeroed on the CPU, because it was
    // living in the texture that pass resamples: the lateral aberration was taking a first
    // difference of it on R and B and printing it as colour, which measured
    // as 72% of all the chroma noise inside a dark mass. One stock at a time.
    float grainAmp = LA_GRAIN;
    if (LA_GRAIN_SHADOW_W > 0.0005) {
        float gl2 = saturate(1.0 - dot(col, float3(0.2126, 0.7152, 0.0722)));
        grainAmp *= lerp(1.0, gl2 * gl2, saturate(LA_GRAIN_SHADOW_W));
    }
    // The reference halo band is visibly GRAINY (film grain over a bright,
    // thin, refracted band); the shadow weighting above would scrub it clean.
    grainAmp *= 1.0 + haloW * 1.2 * saturate(LA_MEN_FROM_INK);
    // ...and the pattern holds for one FRAME OF THE STOCK, not one refresh.
    float acidGq = frac(floor(LA_TIME * max(poP1.z, 1.0)) * 0.0731) * 913.7;
    col += (AcidHash21(floor(i.pos.xy / max(LA_GRAIN_SCALE, 1.0)) + acidGq) - 0.5)
         * grainAmp;
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
    // ---- [post] sdr_shoulder (HDR off only, shadow.z; 2026-11-02) --------
    // The film is driven far over white here (encoded up to ~2.6x) and the
    // saturate below clips each channel on its own, so a pale gradient prints
    // a contour where each channel crosses 1.0 (the user's pale-cyan photo).
    // The shoulder scales the WHOLE colour by one factor from its peak
    // channel -- a knee at 0.85, asymptotic to 1.0, slope 1 at the knee -- so
    // hue and saturation hold and no channel reaches the ceiling; shadow.z
    // mixes it in over the hard clip (0 = today, never entered).
    [branch] if (shadow.z > 0.0005) {
        float sm = max(col.r, max(col.g, col.b));
        float sf = (sm > 0.85) ? (0.85 + 0.15 * (1.0 - exp((0.85 - sm) / 0.15))) / sm : 1.0;
        col = lerp(saturate(col), col * sf, shadow.z);
    }
    C = saturate(col);
#ifdef OIL_LAMP
    float grainAmpW = LA_GRAIN;
    if (LA_GRAIN_SHADOW_W > 0.0005) {
        float gl2W = saturate(1.0 - dot(wtCol, float3(0.2126, 0.7152, 0.0722)));
        grainAmpW *= lerp(1.0, gl2W * gl2W, saturate(LA_GRAIN_SHADOW_W));
    }
    grainAmpW *= 1.0 + haloW * 1.2 * saturate(LA_MEN_FROM_INK);
    wtCol += (AcidHash21(floor(i.pos.xy / max(LA_GRAIN_SCALE, 1.0)) + acidGq) - 0.5)
          * grainAmpW;
    [branch] if (shadow.z > 0.0005) {   // sdr_shoulder, the white track likewise
        float smW = max(wtCol.r, max(wtCol.g, wtCol.b));
        float sfW = (smW > 0.85) ? (0.85 + 0.15 * (1.0 - exp((0.85 - smW) / 0.15))) / smW : 1.0;
        wtCol = lerp(saturate(wtCol), wtCol * sfW, shadow.z);
    }
    float3 CW = saturate(wtCol);
#endif
#if defined(OIL_STAGETAP) && (OIL_STAGETAP == 10 || OIL_STAGETAP == 110)
    return OilTapLin((OIL_STAGETAP > 100) ? OIL_TAPW(C, CW) : C);
#endif
    // HDR: the oil is a flat fill, so give it its own highlight level rather
    // than inheriting the ink's. Keep the hot part small (ABL): the rim band.
    // With oil_transparency the oil pixel is mostly TRANSMITTED ink, so it
    // must keep the ink's own highlight level; only the scattered part of the
    // film is driven to oil_hdr. filmOp is 1 when transparency is off.
    // lampG carries rise_bottom_light into the HDR level too: the base of the
    // lamp should be the hot part of the frame, not merely the pale part.
#ifdef OIL_LAMP
    float mW = m;     // the white track's HDR level (only filmOp differs)
#endif
#ifdef OIL_COLUMN
    // Oil3D C2: the column's shade lowers the film's HDR level with it (as
    // shadow_amt's shadowMul does; 1 in open water = today). (shaft_hdr, the
    // HDR lift inside the lit columns, was dropped in L1b: the T9 sweep showed
    // it does not separate the shafts from the shadow columns.)
    if (LA_OIL_HDR > 0.001) m = lerp(m, LA_OIL_HDR * lampG * shadowMul * wsShade, alpha * filmOp);
#ifdef OIL_LAMP
    if (LA_OIL_HDR > 0.001) mW = lerp(mW, LA_OIL_HDR * lampG * shadowMul * wsShade, alpha * filmOpW);
#endif
#else
    if (LA_OIL_HDR > 0.001) m = lerp(m, LA_OIL_HDR * lampG * shadowMul, alpha * filmOp);
#ifdef OIL_LAMP
    if (LA_OIL_HDR > 0.001) mW = lerp(mW, LA_OIL_HDR * lampG * shadowMul, alpha * filmOpW);
#endif
#endif
    if (LA_RIM_HDR > 0.001) m = max(m, LA_RIM_HDR * rimB * alpha);
    // brief BL: the fluorescent edge band runs into the HDR headroom (knee ->
    // capBright = up to peak_nits); shadowed film is not excited.
    [branch] if (fluorM > 0.0005) m = max(m, lerp(knee, capBright, fluorM * shadowMul));
    // a specular on a real oil surface is a highlight, not a paler fill
    if (LA_OIL_HDR > 0.001 && specAmt > 0.0005)
        m = max(m, min(LA_OIL_HDR * (1.0 + 2.5 * specAmt), 1.6) * alpha);
#ifdef OIL_LAMP
    if (LA_RIM_HDR > 0.001) mW = max(mW, LA_RIM_HDR * rimB * alpha);
    [branch] if (fluorM > 0.0005) mW = max(mW, lerp(knee, capBright, fluorM * shadowMul));
    if (LA_OIL_HDR > 0.001 && specAmt > 0.0005)
        mW = max(mW, min(LA_OIL_HDR * (1.0 + 2.5 * specAmt), 1.6) * alpha);
#endif
#endif

#if defined(LIQUID_ACID) && defined(OIL_HEIGHT)
    // --shot-dump film frame: the composite in display gamma, before the
    // vignette, the grain, the gamut stretch and the HDR gain (an albedo).
    [branch] if (LA_OIL_H_DEBUG > 4.5 && LA_OIL_H_DEBUG < 5.5) return float4(saturate(C), 1.0);
#endif
    // ======================= [post]: the last two steps ====================
    // Shared by every look and applied AFTER everything else (post_chroma /
    // post_lift included), in SCREEN space -- i.pos.xy, never the folded uv:
    // a lens and a film emulsion sit in front of the picture, so they do not
    // mirror with it. Both amounts default to 0 and neither branch is entered
    // then, so style=fluid is untouched down to the bit unless asked.
    // (aberration used to live here as a first-order expansion of the
    // composite about the pixel. It could not work: the offset direction came
    // from the LUMINANCE gradient, which always points at the brighter side,
    // so dR was positive and dB negative on BOTH sides of every droplet -- a
    // symmetric warm outline, never a split, at any slider value. And a 3-px
    // displacement is not something one derivative can express anyway. It is
    // now a real resample of the finished frame in kPostSrc, where the pixels
    // exist to be resampled. See item Z.)
    // A slight fall-off toward the corners: the field stop of a macro lens,
    // never a circle with an edge. This one stays here -- it is a shading of
    // the picture, not a displacement of it, so it needs nothing resampled.
    [branch] if (poP1.w > 0.0005) {
        // ...and its centre WANDERS with the rig's lens (item V3), so the
        // darkest corner turns over minutes instead of sitting in one corner
        // of the panel for hours. vignette_wander 0 puts it back at the
        // middle, which is the constant this line used to subtract.
        float2 d = i.pos.xy * texelSize - float2(poP1.x, poP1.y);
        float  r2 = dot(d, d) * 2.0;
        C *= 1.0 - saturate(poP1.w) * 0.45 * r2 * r2;
#ifdef OIL_LAMP
        CW *= 1.0 - saturate(poP1.w) * 0.45 * r2 * r2;
#endif
    }
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
    [branch] if (poP0.x > 0.0005) {
        // FILM GRAIN, luminance-weighted: the mids and darks carry it (that is
        // where emulsion noise lives and where the ink already looks grainy),
        // the peaks stay clean so a bright core never fizzes, and a pixel that
        // is TRUE BLACK gets only a whisper -- an off OLED pixel is the best
        // thing on this panel and grain would light the whole frame's floor.
        float2 gc  = floor(i.pos.xy / max(poP0.y, 0.25));
        // film_grain_fps patterns per second at speed 1 (24 by default: the
        // grain changes once per FRAME of the stock, not once per refresh);
        // lower speeds hold a pattern longer, the slow chatter of a big-grain
        // stock.
        float  tq  = floor(mrP0.w * max(poP1.z, 1.0) * max(poP0.z, 0.0));
        float2 tj  = frac(tq * float2(0.1031, 0.0973)) * 733.0;
        float  n0  = PostHash21(gc + tj);
        float3 nz  = float3(n0, n0, n0);
        [branch] if (poP0.w > 0.0005) {
            nz = lerp(nz, float3(n0, PostHash21(gc + tj + 37.71),
                                     PostHash21(gc + tj + 91.37)), saturate(poP0.w));
        }
        float lum = dot(C, float3(0.2126, 0.7152, 0.0722));
        float w   = (1.0 - smoothstep(0.55, 1.00, lum))
                  * lerp(0.15, 1.0, smoothstep(0.0, 0.06, lum));
        // brief BD: the same two knobs Emulsion() takes in the post pass, and
        // for the same reasons -- this is the copy of that grain that runs
        // when no image-space pass is on. mrP2.z = film_grain_chroma (1 = the
        // additive step this always was), mrP2.w = film_grain_density (0 =
        // the weight above). At the defaults the two lines below are the one
        // line they replaced.
        [branch] if (mrP2.w > 0.0005) {
            float wd = smoothstep(0.10, 0.32, lum) * (1.0 - smoothstep(0.55, 1.00, lum));
            w = lerp(w, wd, saturate(mrP2.w));
        }
        [branch] if (mrP2.z > 0.9995) {
            C = max(C + (nz - 0.5) * (poP0.x * 0.5 * w), 0.0);
        } else {
            float3 mul = max(C * (1.0 + (nz.x - 0.5) * (poP0.x * 0.6667 * w)), 0.0);
            float3 add = max(C + (nz - 0.5) * (poP0.x * 0.5 * w), 0.0);
            C = lerp(mul, add, saturate(mrP2.z));
        }
#ifdef OIL_LAMP
        {
            float lumW = dot(CW, float3(0.2126, 0.7152, 0.0722));
            float wW   = (1.0 - smoothstep(0.55, 1.00, lumW))
                       * lerp(0.15, 1.0, smoothstep(0.0, 0.06, lumW));
            [branch] if (mrP2.w > 0.0005) {
                float wdW = smoothstep(0.10, 0.32, lumW) * (1.0 - smoothstep(0.55, 1.00, lumW));
                wW = lerp(wW, wdW, saturate(mrP2.w));
            }
            [branch] if (mrP2.z > 0.9995) {
                CW = max(CW + (nz - 0.5) * (poP0.x * 0.5 * wW), 0.0);
            } else {
                float3 mulW = max(CW * (1.0 + (nz.x - 0.5) * (poP0.x * 0.6667 * wW)), 0.0);
                float3 addW = max(CW + (nz - 0.5) * (poP0.x * 0.5 * wW), 0.0);
                CW = lerp(mulW, addW, saturate(mrP2.z));
            }
        }
#endif
    }
#if defined(OIL_STAGETAP) && (OIL_STAGETAP == 11 || OIL_STAGETAP == 111)
    return OilTapLin((OIL_STAGETAP > 100) ? OIL_TAPW(C, CW) : C);
#endif
    float3 lin = SRGBToLinear(saturate(C));
    // Interpret the dye in a wider gamut and convert to the swap chain's 709
    // primaries. Out-of-gamut saturation comes out as negative components —
    // FP16 scRGB carries those to the display (QD-OLED shows them).
    if (gamut > 2.5) {          // HDR off: the configured gamut -> THIS panel's primaries
        lin = float3(
            dot(float3(fm2.y, fm2.z, 1.0 - fm2.y - fm2.z), lin),
            dot(float3(fm2.w, fmOff.x, 1.0 - fm2.w - fmOff.x), lin),
            dot(float3(fmOff.y, fmOff.z, 1.0 - fmOff.y - fmOff.z), lin));
    } else if (gamut > 1.5) {   // BT.2020 — full QD-OLED vividness
        lin = float3(
            dot(float3( 1.66049, -0.58764, -0.07285), lin),
            dot(float3(-0.12455,  1.13290, -0.00835), lin),
            dot(float3(-0.01815, -0.10058,  1.11873), lin));
    } else if (gamut > 0.5) {   // Display-P3 — parity with the WE original
        lin = float3(
            dot(float3( 1.22494, -0.22494,  0.0),     lin),
            dot(float3(-0.04206,  1.04206,  0.0),     lin),
            dot(float3(-0.01964, -0.07868,  1.09832), lin));
    }
#ifdef OIL_LAMP
    float3 linW = OilLampGamut(SRGBToLinear(saturate(CW)));
#endif
#if defined(OIL_STAGETAP) && (OIL_STAGETAP == 12 || OIL_STAGETAP == 112)
    return OilTapRaw((OIL_STAGETAP > 100) ? OIL_TAPW(lin, linW) : lin);
#endif
#ifdef LIQUID_ACID
    // ---- brief BX: GREY HEART (lamp_grey_heart) ----------------------------
    // The user: "have a true colour and make it look grayscale in the
    // middle". A CENTRED elliptical mask hm = 1 - smoothstep(0.3 S, S, r),
    // r = length(i.uv * 2 - 1) in half-frame units (edge midpoints 1, corners
    // 1.41), from the RAW screen interpolant (a mirror fold never moves it);
    // the grey follows sqrt(hm) (fitted to the user's Lightroom radial edit:
    // its colour returns more slowly than its brightness falls), the lift hm
    // x alpha (film only: masses never lift). HERE, on the finished linear
    // colour after the stretch, so nothing downstream in this pass
    // re-saturates it (HeartGrey). When the [post] pass runs, its bloom /
    // halation / glow / grain / stock would put colour back into the grey, so
    // the DESATURATION moves to the end of that pass (kPostSrc, BX_HEART) and
    // the CPU writes HEART_K = -amount: here only the film lift. 0 = skipped.
    [branch] if (LA_HEART_K != 0.0) {
        float2 hq = i.uv * 2.0 - 1.0;
        float  hm = 1.0 - smoothstep(0.3 * LA_HEART_SIZE, LA_HEART_SIZE, length(hq));
        [branch] if (hm > 0.0) {
            float liftMul = 1.0 + LA_HEART_LIFT * hm * alpha;
            [branch] if (LA_HEART_K > 0.0) lin = HeartGrey(lin, LA_HEART_K * sqrt(hm), liftMul);
            else                           lin *= liftMul;
#ifdef OIL_LAMP
            [branch] if (LA_HEART_K > 0.0) linW = HeartGrey(linW, LA_HEART_K * sqrt(hm), liftMul);
            else                           linW *= liftMul;
#endif
        }
    }
#endif
#if defined(OIL_STAGETAP) && (OIL_STAGETAP == 13 || OIL_STAGETAP == 113)
    return OilTapRaw((OIL_STAGETAP > 100) ? OIL_TAPW(lin, linW) : lin);
#endif

    // True HDR ("parity-plus"): the SDR-parity colour above is complete and
    // already clamped, so this gain scales ALL channels together in linear
    // light — hue and saturation are untouched, hot dye just gets brighter
    // than SDR white (a saturated red at 600 nits, not a white core). Driven
    // by the raw dye intensity with a soft knee: below `knee` exact parity,
    // reaching peakGain at the dye cap. Identity when peak_nits = 0.
#ifdef INK
    // ---- [ink] tone_chroma / tonemap ------------------------------------
    // Chroma hold first, in linear light, about the pixel's own luma — the
    // same trim [liquid_acid] post_chroma makes. Raising the white point
    // below must never read as a wash.
    if (abs(ikP5.w - 1.0) > 0.001) {
        // NOT clamped at 0 the way [liquid_acid] post_chroma is: with
        // gamut > 0 the out-of-gamut components are NEGATIVE on purpose and
        // the QD-OLED shows them, so clamping here would throw away exactly
        // the saturation this key exists to protect.
        float il = dot(lin, float3(0.2126, 0.7152, 0.0722));
        lin = il.xxx + (lin - il.xxx) * ikP5.w;
    }
    // HDR RANGE MAPPING. The parity-plus gain below spends headroom only on
    // dye that is nearly at the brightness CAP; the ink composite is an
    // absorption result that never gets there, so with `tonemap` 0 the whole
    // frame is squeezed into SDR white however much headroom the panel has.
    //
    // Here the composite's OWN 0..1 range is mapped onto [black, white] nits
    // instead, hue-preserving (one scale on all three channels, exactly like
    // the gain). `tone_knee` blends in a smoothstep S: the mids gain slope
    // (1.5x at the midpoint) and therefore more distinguishable steps, at the
    // cost of the extreme toe and shoulder. The parity-plus gain is then
    // retargeted ABOVE the new white, so hot moving cores (hdr_core, motion
    // gated) still run up toward peak_nits instead of stopping at white.
    if (ikP4.w > 0.5) {
        float v = max(lin.r, max(lin.g, lin.b));
        if (v > 1e-5) {
            float x = saturate(v);
            float s = lerp(x, x * x * (3.0 - 2.0 * x), saturate(ikP5.z));
            float o = ikP5.y + (ikP5.x - ikP5.y) * s;   // scRGB (1.0 = 80 nits)
            if (peakGain > 1.001) {
                float t = smoothstep(knee, max(capBright, knee + 0.01), m);
                o *= lerp(1.0, max(ikP6.y / max(ikP5.x, 1e-4), 1.0), t * t);
            }
            lin *= min(o, ikP6.y) / v;
        }
        return float4(lin, outCoc);
    }
#endif
    float gain = 1.0;
    if (peakGain > 1.001) {
        float t = smoothstep(knee, max(capBright, knee + 0.01), m);
        gain = lerp(1.0, peakGain, t * t);
    }
#ifdef OIL_LAMP
    // ---- Oil3D L1b: THE CAP, per pixel, in the OUTPUT space -------------------
    // (port chat cap ruling, binding) A window only REMOVES light: this pixel's
    // emitted luminance may never exceed the same pixel's under the white
    // state. Enforced HERE, against the white track carried through every
    // step above, because the steps between the film and the panel are not
    // luminance-neutral in chroma: the transparent film's Beer-Lambert T is
    // normalised by the peak channel, post_chroma spreads the channels in the
    // encoded domain, the mass rim is driven by the peak channel -- all of
    // which lift a PURER film above the mixed film it replaces at equal Y
    // (measured +3.5 % frame mean on the live config at W40 level 3 with the
    // film's own Y capped). Hue and chroma kept, luminance scaled; measured
    // the way the panel gets it: the scRGB value as is (wide-gamut negatives
    // carried: HDR on, what DWM sends in BT.2020, the PQ output, mean_lum), or
    // CLIPPED where something clips them first (fmOff.w: DWM with HDR off, or
    // the [post] aberration's max(d, 0) -- live runs it, and the purer film
    // gains MORE from a clipped negative channel: +1.6 % frame mean at W40
    // level 3 when the cap held only the unclipped Y). One criterion for both
    // would either miss the clip or dim an unclipped HDR frame for nothing.
    // A pixel that would exceed lands one FP16 step (2^-10) under today, so
    // the clamp survives the FP16 target's rounding; every other pixel is
    // untouched (a mass the window does not reach reads exactly today).
    {
        float gainW = 1.0;
        if (peakGain > 1.001) {
            float tW = smoothstep(knee, max(capBright, knee + 0.01), mW);
            gainW = lerp(1.0, peakGain, tW * tW);
        }
        const float3 YC = float3(0.2126, 0.7152, 0.0722);
#ifdef OIL_LAMP2
        // Oil3D L3b: lamp 2's open water is PINNED at today's Y (fence 0.00 +-
        // 0.05 %) and its windowed track meets the white track there to float
        // noise, so a one-FP16-step margin would be a -0.1 % bias on every
        // open-water pixel by itself: the scale is exact (the FP16 rounding
        // that is left is unbiased, <= half a step).
        const float  KEEP = 1.0;
#else
        const float  KEEP = 1.0 - 1.0 / 1024.0;
#endif
        float3 oN = lin * gain, oW = linW * gainW;
        float  yN = dot(oN, YC), yW = dot(oW, YC);
#if defined(OIL_STAGETAP) && (OIL_STAGETAP == 15 || OIL_STAGETAP == 115)
        return OilTapRaw(((OIL_STAGETAP > 100) ? oW : oN) * sdrScale);
#endif
#if defined(OIL_STAGETAP) && OIL_STAGETAP == 114
        return OilTapRaw(oW * sdrScale);
#endif
        [branch] if (fmOff.w > 0.5) {
            // the negatives are clipped on the way out: the white side as it
            // will show (clipped below, and at 1 too with HDR off), the
            // windowed side only below (saturate(s x) <= s max(x, 0) and
            // max(s x, 0) = s max(x, 0): the scale stays exact or conservative)
            yN = dot(max(oN, 0.0), YC);
            yW = dot((fmOff.w > 1.5) ? saturate(oW) : max(oW, 0.0), YC);
        }
        float  s  = 1.0;
        if (yN > yW) s = max(yW, 0.0) * KEEP / max(yN, 1e-12);
#if defined(OIL_STAGETAP) && OIL_STAGETAP == 99
        return float4(yN, yW, s, 1.0);
#endif
        lin *= s;
    }
#endif
#if defined(OIL_STAGETAP) && (OIL_STAGETAP == 14 || OIL_STAGETAP == 15 || OIL_STAGETAP == 99 || OIL_STAGETAP >= 114)
    return OilTapRaw(lin * sdrScale * gain);
#endif
    return float4(lin * sdrScale * gain, outCoc);
}
)hlsl";

// ===========================================================================
// [post] IMAGE-SPACE pass: the camera in front of the dish.
// The display pass above shades every edge analytically off a signed distance
// to its isoline, which is exact on a big mass and breaks on anything narrower
// than the band itself: a 2-px ring wall has no gradient at its centre, so the
// rim, meniscus and halo all gate themselves off within a pixel of it and the
// wall renders as a hard stroked line beside softly shaded droplets (the user:
// "these are pixel perfect circles", "whatever shading is in this photo needs
// to be everywhere"). A lens does not know what it is looking at. This pass
// takes the finished frame (linear scRGB, HDR gain and gamut already applied)
// and gives every feature, whatever its size, the same optics:
//   post_blur_px  a small disc defocus (the circle of confusion),
//   post_glow     a weak, wide veiling glare: the picture mixed with a wide
//                 blur of itself, so dark bleeds a little into bright and
//                 bright into dark around every edge -- the soft dark
//                 gradient the user liked on the medium droplet, everywhere,
//   film grain    re-applied here, AFTER the blur, so it stays crisp: the
//                 grain is in the emulsion, the blur is in the glass (the
//                 display pass zeroes its own grain when this pass runs).
// Runs only when blur or glow is non-zero, so style=fluid never enters it.
// Bound with the graphics root signature: b0 = 32 root constants, t0 = the
// display pass's target, s0 = the static linear clamp sampler.
// ===========================================================================
static const char* kPostSrc = R"hlsl(
cbuffer PostPassCB : register(b0) {
    float4 pp0;   // x 1/W        y 1/H        z blurPx (this res) w glow
    float4 pp1;   // x glowPx     y grain      z grainSize(this res) w grainSpeed
    float4 pp2;   // x grainColor y time       z sdrScale           w glowDark
    float4 pp3;   // x dust       y hairs      z scratches          w leak
    float4 pp4;   // x rate (s)   y noise      z noiseSize(this res) w H/1440
    float4 pp5;   // x stock      y fog        z bloom   w dofMaxPx(this res; 0 = alpha is not a CoC)
    float4 pp6;   // x fogReach(uv) y bloomPx(this res) z psfPx(this res) w grainFps
    float4 pp7;   // x dither  y halation  z halationPx(this res)  w warmth
};
// ---- THE RIG -------------------------------------------------------------
// One lamp and one lens on one body, stepped on the CPU (FluidRenderer::
// StepCameraRig) and handed over as a single block, so the haze, the bloom,
// the depth of field and -- later -- the lid ghosts, the flare and the
// vignette centre cannot disagree about where the rig is. rg2 is the lens's
// chromatic split, rg3 is reserved and rg4 is the lid; they are
// reserved for those. This is b3, which the display pass uses for its mirror
// fold and this pass has never bound.
cbuffer RigCB : register(b3) {
    float4 rg0;   // x lampX(uv, drifted) y lampY  z axisX(uv)  w axisY(uv)
    // rg1.xy used to be the tilt, which this pass never read (the display
    // pass gets it through its acid slot). They are now two packs of four
    // 6-bit fields (BDU6, most significant first; unused fields are 0),
    // fixed for briefs BO and BN so neither repacks the other:
    //   x  artefact_lum_gate : 6 | corner_warp : 6 | corner_warp_r : 6 | bloom_warmth : 6
    //   y  glass_streaks : 6 | halation_threshold : 6 | spare : 6 | spare : 6
    // corner_warp_r is 0.4 + 0.5 * field; it and glass_streaks are written 0
    // unless the warp / the lid is on (fluid.cpp). rg1.zw are brief BM's lid
    // scratches, packed like rg4.z. Both are exactly 0 when the scratches
    // are off, which is their only branch.
    //   z  amount : 8 | density : 8 | len : 8
    //   w  corner : 8 | soft : 8 | tint : 8
    float4 rg1;   // x BO/BN pack  y BN pack  z scratch A  w scratch B
    // rg2.w is brief BD's four knobs, PACKED the way the lid packs its own
    // (b0's 32 constants are full and the root signature is at the 64-DWORD
    // limit, so a new number has to fit in a slot that already exists):
    //   grainChroma : 6 | grainDensity : 6 | fogMassGate : 6 | aberrCoc : 6
    // Four 6-bit fields make an exact integer at most 2^24-1, which float32
    // carries losslessly. Six bits is 1/63 of a slider that is a taste knob
    // read once per pixel -- finer than the panel can show. The DEFAULTS are
    // what matters: 1 / 0 / 0 / 0 quantise to 63 / 0 / 0 / 0 and unpack to
    // exactly 1.0 / 0.0 / 0.0 / 0.0, so no preset moves by a bit because
    // this field exists.
    float4 rg2;   // x aberration y aberrPx(this res) z aberrField  w BD pack
    float4 rg3;   // x shimmer y shimmerPx(this res) z shiftX(uv) w shiftY(uv)
    // rg4 is THE LID (task V2), all of it, PACKED -- rg2 is the lens's
    // chromatic split and rg3 is spoken for, so the lid gets one float4 and
    // twelve numbers have to fit in it. Each float carries an exact integer
    // below 2^24, so a float32 holds it without loss.
    //   x  lidX : 12 | lidY : 12          the cover's own wander, uv
    //   y  lidRot : 12 | refractPx : 12   orientation, reflection wobble
    //   z  ghost : 8 | rings : 8 | sheen : 8
    //   w  glint : 7 | iris : 7 | sheenPx : 6 | ghostSpread : 4
    // The master is folded into the amplitudes on the CPU; all four floats
    // are exactly 0 when the lid is off, which is the block's only branch.
    float4 rg4;
};
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
#ifdef LAMP_GLOW
// ---- Oil3D L1b: LAMP 1's colour for the glows (the LAMP_GLOW post variants) --
// Fog is the lamp's light scattered by the water, bloom_warmth's veil is the
// lamp's colour: under a spectral window both take the WINDOW's light. The
// CPU (BuildLampLut) writes it at the end of the acid cbuffer (AcidParamsGPU
// .glow, byte 2848 = c178), bound at b1 for this pass only while the window
// is not white: linear rgb, white = (1, 1, 1), luminance = the window's share
// of the lamp's light x lamp1_level, never above 1 (a window removes light).
// Halation and bloom need nothing: they gather the display output, which is
// already the windowed film. The split tone is a grade, not light: untouched.
cbuffer AcidLampCB : register(b1) {
    float4 laLampGlow : packoffset(c178);
};
#endif
Texture2D Src : register(t0);
// The sim's own low-res velocity field, bound only when the thermal shimmer
// is on. It is what lets a post effect be moved BY THE FLUID instead of by
// its own clock -- the user's rule that a burst which shoves the oil should
// shove the heat above it too.
Texture2D<float4> VelLow : register(t3);
SamplerState linearClamp : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

// Value noise off the same hash: two smoothed lattice octaves are enough for
// a heat wobble, and it costs no texture and no table. (item V3)
float PVNoise(float2 p);
// same hash as the display pass's PostHash21, so the grain pattern is the one
// the user already approved
float PHash21(float2 p) {
    p = frac(p * float2(234.34, 435.345));
    p += dot(p, p + 34.23);
    return frac(p.x * p.y);
}
// ---- THE LID's packed constants (rg4) -----------------------------------
// Each field was quantised on the CPU and shifted into one exact integer.
// float32 carries 24 bits of mantissa, so every pack below is lossless and
// the unpack is a divide and a floor per field.
float2 LidU12(float v) {
    float a = floor(v * (1.0 / 4096.0));
    return float2(a, v - a * 4096.0) * (1.0 / 4095.0);
}
float3 LidU8(float v) {
    float a = floor(v * (1.0 / 65536.0));
    float r = v - a * 65536.0;
    float b = floor(r * (1.0 / 256.0));
    return float3(a, b, r - b * 256.0) * (1.0 / 255.0);
}
#ifdef BX_HEART
// brief BX (fix round): the grey heart's OKLab desaturation, the same maths
// as the display pass's HeartGrey (no lift here: the display pass lifted the
// film already), run on this pass's finished colour.
float3 HeartGreyP(float3 lin, float g) {
    const float3 W = float3(0.2126, 0.7152, 0.0722);
    float  yT  = dot(lin, W);
    float3 lms = float3(dot(float3(0.4122214708, 0.5363325363, 0.0514459929), lin),
                        dot(float3(0.2119034982, 0.6806995451, 0.1073969566), lin),
                        dot(float3(0.0883024619, 0.2817188376, 0.6299787005), lin));
    lms = sign(lms) * pow(abs(lms), 1.0 / 3.0);
    float3 lab = float3(dot(float3(0.2104542553,  0.7936177850, -0.0040720468), lms),
                        dot(float3(1.9779984951, -2.4285922050,  0.4505937099), lms),
                        dot(float3(0.0259040371,  0.7827717009, -0.8086757660), lms));
    lab.yz *= 1.0 - g;
    lms = float3(lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z,
                 lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z,
                 lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z);
    lms = lms * lms * lms;
    float3 o = float3(dot(float3( 4.0767416621, -3.3077115913,  0.2309699292), lms),
                      dot(float3(-1.2684380046,  2.6097574011, -0.3413193965), lms),
                      dot(float3(-0.0041960863, -0.7034186147,  1.7076147010), lms));
    float  y = dot(o, W);
    return (abs(y) > 1e-8) ? o * (yT / y) : o;
}
#endif
// brief BD's four 6-bit fields, most significant first:
// grainChroma | grainDensity | fogMassGate | aberrCoc.
float4 BDU6(float v) {
    float a = floor(v * (1.0 / 262144.0));
    float r = v - a * 262144.0;
    float b = floor(r * (1.0 / 4096.0));
    r -= b * 4096.0;
    float c = floor(r * (1.0 / 64.0));
    return float4(a, b, c, r - c * 64.0) * (1.0 / 63.0);
}
float4 LidU7764(float v) {
    float a = floor(v * (1.0 / 131072.0));
    float r = v - a * 131072.0;
    float b = floor(r * (1.0 / 1024.0));
    r -= b * 1024.0;
    float c = floor(r * (1.0 / 16.0));
    return float4(a * (1.0 / 127.0), b * (1.0 / 127.0),
                  c * (1.0 / 63.0), (r - c * 16.0) * (1.0 / 15.0));
}
float PVNoise(float2 p) {
    float2 i0 = floor(p), f = frac(p);
    float2 u  = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(PHash21(i0),                 PHash21(i0 + float2(1.0, 0.0)), u.x),
                lerp(PHash21(i0 + float2(0.0, 1.0)), PHash21(i0 + float2(1.0, 1.0)), u.x), u.y);
}
float3 ToSRGB(float3 c) {
    c = saturate(c);
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 1e-6), 1.0 / 2.4) - 0.055;
    return lerp(lo, hi, step(0.0031308, c));
}
float3 ToLinear(float3 c) {
    float3 lo = c / 12.92;
    float3 hi = pow((c + 0.055) / 1.055, 2.4);
    return lerp(lo, hi, step(0.04045, c));
}
// ONE noise layer, applied exactly the way the display pass applies its own
// grain: in an sRGB-ENCODED proxy of the pixel, luminance-weighted (the mids
// and darks carry it, the peaks stay clean, a true-black pixel gets only a
// whisper), converted back to linear and scaled by whatever HDR gain this
// pixel carries so it never fizzes on a hot core. Shared by the coarse film
// grain and the finer, faster film_noise.
//
// brief BD, measured: at the shipped keys this grain spanned 64-79 PQ10 codes
// inside a dark mass -- a 4-6x brightness ratio from pixel to pixel -- against
// 22 codes and 1.2x on the open film. Two reasons, and `chroma` and `density`
// are the two fixes. Both default to today's numbers and at those values the
// arithmetic below is character-for-character what it always was, so every
// preset that does not name them is byte-identical.
//   chroma 1  the grain is ADDED, the same encoded number on all three
//             channels. That is not luminance-only: an equal step on unequal
//             channels moves the pixel's saturation, and near black the
//             max(...,0) clips the negative half, which lifts the mean.
//   chroma 0  the grain SCALES the encoded pixel instead. The channel ratios
//             survive exactly, so it is luminance and nothing else; it cannot
//             lift a black pixel and it cannot clip. It is also what film
//             does -- grain is a fluctuation in DENSITY, i.e. in
//             transmittance, which is multiplicative.
//   density 0 today's weight: full from ~0.06 encoded upward, with a 0.15
//             FLOOR on absolute black.
//   density 1 the stock's own D-log-E hump: exactly zero in the dense shadow,
//             peak in the mid-tones, gone again on a clean highlight.
float3 Emulsion(float3 d, float3 nz, float amt, float sdr,
                float chroma, float density) {
    const float3 EW = float3(0.2126, 0.7152, 0.0722);
    float3 base = d / sdr;
    float3 e    = ToSRGB(base);
    float  lum  = dot(e, EW);
    float  w    = (1.0 - smoothstep(0.55, 1.00, lum))
                * lerp(0.15, 1.0, smoothstep(0.0, 0.06, lum));
    [branch] if (density > 0.0005) {
        float wd = smoothstep(0.10, 0.32, lum) * (1.0 - smoothstep(0.55, 1.00, lum));
        w = lerp(w, wd, saturate(density));
    }
    float3 e2;
    [branch] if (chroma > 0.9995) {
        e2 = max(e + (nz - 0.5) * (amt * 0.5 * w), 0.0);
    } else {
        // 0.6667 = 0.5 / 0.75: calibrated so a pixel at encoded 0.75 -- the
        // open film in the rising presets, the level the user has already
        // approved -- gets EXACTLY the swing the additive path gave it, and
        // everything darker gets proportionally less. That is the whole
        // point: the film keeps its grain, the shadow stops fizzing.
        float3 mul = max(e * (1.0 + (nz.x - 0.5) * (amt * 0.6667 * w)), 0.0);
        float3 add = max(e + (nz - 0.5) * (amt * 0.5 * w), 0.0);
        e2 = lerp(mul, add, saturate(chroma));
    }
    float3 l0   = ToLinear(e), l2 = ToLinear(e2);
    float  gain = clamp(dot(base, EW) / max(dot(l0, EW), 1e-4), 1.0, 16.0);
    return max(d + (l2 - l0) * gain * sdr, min(d, 0.0));
}

// Uniform disc, 31 taps (centre + 6/10/14 at 0.41/0.71/0.91 r): the circle of confusion
// of a defocused lens is a flat disc, not a Gaussian, which is why a defocused
// hairline becomes a soft band of the same darkness spread wider rather than
// a faint smear. Bilinear taps between texels make it smoother than its count.
// Three EQUAL-AREA annuli (radii sqrt(1/6), sqrt(3/6), sqrt(5/6)) with equal
// total weight each, plus a light centre tap: that is a flat disc sampled
// uniformly by area, where a ring pattern chosen by eye is not. It matters now
// that the radius is per-pixel and reaches ten-odd px on the far layer -- the
// old 19 taps printed a visible 12-pointed star round every out-of-focus
// highlight at that size. `jit` turns each pixel's tap angles by its own hash,
// so what banding is left dissolves into noise the grain is about to cover.
float3 Disc(float2 uv, float2 r, float jit) {
    float3 s = Src.SampleLevel(linearClamp, uv, 0).rgb * 0.06;
    float  w = 0.06;
    [unroll] for (int k = 0; k < 6; k++) {
        float a = (float)k * 1.0471976 + jit;
        s += Src.SampleLevel(linearClamp, uv + float2(cos(a), sin(a)) * (0.41 * r), 0).rgb * 0.0556;
        w += 0.0556;
    }
    [unroll] for (int j = 0; j < 10; j++) {
        float a = (float)j * 0.6283185 + jit * 1.7 + 0.31;
        s += Src.SampleLevel(linearClamp, uv + float2(cos(a), sin(a)) * (0.71 * r), 0).rgb * 0.0333;
        w += 0.0333;
    }
    [unroll] for (int m = 0; m < 14; m++) {
        float a = (float)m * 0.4487990 + jit * 2.3 + 0.73;
        s += Src.SampleLevel(linearClamp, uv + float2(cos(a), sin(a)) * (0.91 * r), 0).rgb * 0.0238;
        w += 0.0238;
    }
    return s / w;
}
// Wide, soft-shouldered blur, 37 taps on three rings with falling weights:
// the veiling glare of a real lens (scatter in the glass and the film base),
// which is what makes every edge in a macro photograph carry a faint wide
// gradient on both sides.
float3 Wide(float2 uv, float2 r) {
    float3 s = Src.SampleLevel(linearClamp, uv, 0).rgb;
    float  w = 1.0;
    [unroll] for (int k = 0; k < 8; k++) {
        float a = (float)k * 0.7853982 + 0.2;
        s += Src.SampleLevel(linearClamp, uv + float2(cos(a), sin(a)) * (0.33 * r), 0).rgb * 0.9;
        w += 0.9;
    }
    [unroll] for (int j = 0; j < 12; j++) {
        float a = (float)j * 0.5235988 + 0.1;
        s += Src.SampleLevel(linearClamp, uv + float2(cos(a), sin(a)) * (0.66 * r), 0).rgb * 0.55;
        w += 0.55;
    }
    [unroll] for (int m = 0; m < 16; m++) {
        float a = (float)m * 0.3926991;
        s += Src.SampleLevel(linearClamp, uv + float2(cos(a), sin(a)) * r, 0).rgb * 0.25;
        w += 0.25;
    }
    return s / w;
}
// brief BD: a 2x2 box of bilinear taps, half a texel out on each diagonal --
// a ~1.5 px average for the price of four samples. The lateral aberration
// reads the source through this, because the source still carries the display
// pass's own grain and a first difference of per-pixel noise taken on R and B
// alone is coloured speckle, not a lens.
float3 AvgTap(float2 uv, float2 t) {
    return (Src.SampleLevel(linearClamp, uv + float2( t.x,  t.y), 0).rgb
          + Src.SampleLevel(linearClamp, uv + float2(-t.x,  t.y), 0).rgb
          + Src.SampleLevel(linearClamp, uv + float2( t.x, -t.y), 0).rgb
          + Src.SampleLevel(linearClamp, uv + float2(-t.x, -t.y), 0).rgb) * 0.25;
}

)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
// ---- LID SCRATCHES (brief BM) ---------------------------------------------
// The WEAR on the cover: a scratched acrylic sheet under a lamp (refs
// lid-scratch-ref-1..4). A scratch is a thin groove, and a groove sends light
// to the eye only where it runs roughly PERPENDICULAR to the line from it to
// the light. That is why a scratched panel is invisible until the light
// catches it, why swirl marks form arcs round the lamp's reflection, and why
// the pattern breathes as the lamp drifts and re-aims. That one test is the
// whole effect; the rest is where the grooves are.
//
// Everything here is in LID space, px at 1440p: the caller has carried the
// pixel and the lamp through the lid's own drift and turn, so the grooves
// ride the lid and never the fluid.
//
// Two scales. MICRO: three layers of short, faintly curved grooves on long
// thin cells, one layer per 60 degrees; a slow noise field picks each
// region's dominant direction and a layer thins out as it turns away from it
// (never to nothing -- worn plastic has every direction in it). LONG: up to
// eight straight hairline gouges a few hundred px long, placed anywhere or
// pulled into the corners. Cost: 12 cells (one hash, one sincos, a few MADs
// each) + 8 slots, no marching.
float4 LidH42(float2 p) {
    float4 p4 = frac(p.xyxy * float4(0.1031, 0.1030, 0.0973, 0.1099));
    p4 += dot(p4, p4.wzxy + 33.33);
    return frac((p4.xxyz + p4.yzzw) * p4.zywx);
}
// how much of the lamp a groove with tangent T catches: 1 when it runs
// perpendicular to the direction to the light, falling off `lobe` (in cos)
// away from that to a small floor -- a groove's walls are rough, so every
// groove scatters a little whichever way it runs. Without the floor the lit
// set is all one direction in any one region, and it reads as rain.
float LidScrLit(float2 T, float2 toL, float lobe) {
    float c = dot(T, toL);
    return 0.12 + 0.88 * exp(-(c * c) / (lobe * lobe));
}
// P, Lp: the pixel and the lamp in lid space; F: the frame (px at 1440p);
// pxU: one OUTPUT pixel in those units (1 at 1440p, 2 at 720p).
float LidScratch(float2 P, float2 Lp, float2 F, float dens, float len,
                 float corner, float soft, float pxU) {
    float2 toL  = normalize(Lp - P + float2(1e-3, 0.0));
    float  wPx0 = 0.75 + 1.6 * soft;       // tent half-width: ~1 px at 1440p
    // Below 1440p a 1 px groove is thinner than a pixel and a point-sampled
    // tent would come out dotted; hold it at one output pixel and dim it by
    // the same ratio, so it keeps its energy and reads the same, only softer.
    float  wPx  = max(wPx0, 0.75 * pxU);
    float  wAmp = wPx0 / wPx;
    float  lobe = 0.30 + 0.30 * soft;
    // Where the wear is. The corners (lid_scratch_corner) of the lid's own
    // frame, the edge of that zone broken up by the same slow patchiness that
    // thins and thickens the wear everywhere.
    float  patch = PVNoise(P * (1.0 / 380.0) + 17.3);
    float2 e     = abs(P / F - 0.5) * 2.0;
    float  cm    = smoothstep(0.22, 0.80, e.x * e.y + (patch - 0.5) * 0.35);
    float  wgt   = lerp(1.0, cm, corner);
    float  pres  = lerp(0.10, 0.95, dens) * wgt * (0.55 + 0.9 * patch);
    float  acc   = 0.0;
    [branch] if (pres > 0.004) {
        float baseA = PVNoise(P * (1.0 / 640.0) + 3.1) * 9.42;
        // 64 px along x 28 across: with centres in the middle half of a cell
        // and every groove reaching at most half a cell each way, the 2x2
        // nearest cells are an exact lookup
        const float2 CS = float2(64.0, 28.0);
        [unroll] for (int ly = 0; ly < 3; ly++) {
            float  la = (float)ly * 1.0471976 + 0.35;
            float  cl = cos(la), sl = sin(la);
            float  lw = lerp(0.2, 1.0, saturate(0.5 + cos(2.0 * (baseA - la))));
            float2 Q  = float2(P.x * cl + P.y * sl, -P.x * sl + P.y * cl) / CS;
            float2 ci = floor(Q);
            float2 o  = step(0.5, Q - ci) - 1.0;
            [unroll] for (int k = 0; k < 4; k++) {
                float2 cc = ci + o + float2((float)(k & 1), (float)(k >> 1));
                float4 h  = LidH42(cc + float2(37.0 * (float)ly + 11.0, 5.0));
                if (h.x > pres * lw) continue;
                float  u  = frac(h.x * 97.31 + h.w * 13.7);
                float  v  = frac(h.y * 53.17 + h.z * 29.3);
                float  br = frac(h.z * 71.93 + h.x * 7.31);
                float2 c  = (cc + 0.25 + 0.5 * h.yz) * CS;
                float  a  = (h.w - 0.5) * 0.60;               // +-17 deg off the layer
                float2 T  = float2(cos(a), sin(a));
                float2 N  = float2(-T.y, T.x);
                float2 r  = Q * CS - c;
                float  t  = dot(r, T);
                float  hl = 6.0 + 20.0 * u * u;                // half length, px
                float  kc = (v - 0.5) * 0.020;                 // curvature, 1/px
                float  dv = kc * t;
                float  ds = abs(dot(r, N) - 0.5 * kc * t * t) * rsqrt(1.0 + dv * dv);
                float  ln = saturate(1.0 - ds / wPx)
                          * (1.0 - smoothstep(0.55, 1.0, abs(t) / hl));
                // the groove's own tangent here, turned back into lid space
                float2 Tt = normalize(T + N * dv);
                Tt = float2(Tt.x * cl - Tt.y * sl, Tt.x * sl + Tt.y * cl);
                acc += ln * LidScrLit(Tt, toL, lobe) * (0.30 + 0.70 * br * br);
            }
        }
    }
    // The long gouges: `len` is the share of the eight slots in use. A
    // straight groove lights only round the foot of the perpendicular from
    // the lamp, so its lobe is wider or it would never show at all.
    [branch] if (len > 0.004) {
        [unroll] for (int k = 0; k < 8; k++) {
            float4 h = LidH42(float2((float)k * 7.13 + 3.1, 91.7));
            if (h.x > len) continue;
            float4 g  = LidH42(float2((float)k * 3.71 + 11.9, 23.3));
            float2 cc = float2(g.w < 0.5 ? 0.04 + h.y * 0.30 : 0.96 - h.y * 0.30,
                               frac(g.w * 2.0) < 0.5 ? 0.04 + h.z * 0.30
                                                     : 0.96 - h.z * 0.30);
            float2 c  = lerp(h.yz, cc, corner) * F;
            float  a  = h.w * 6.2831853;
            float2 T  = float2(cos(a), sin(a));
            float2 r  = P - c;
            float  t  = dot(r, T);
            float  hl = 160.0 + 380.0 * g.x;
            if (abs(t) > hl) continue;
            float  ds = abs(r.x * T.y - r.y * T.x);
            float  ln = saturate(1.0 - ds / (wPx * (1.0 + 0.5 * g.y)))
                      * (1.0 - smoothstep(0.6, 1.0, abs(t) / hl));
            acc += ln * LidScrLit(T, toL, lobe * 1.6) * (0.55 + 0.45 * g.z);
        }
    }
    return acc * wAmp * rsqrt(wPx0 / 0.75); // a wider groove is a dimmer one
}
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
#ifdef BN_OPTICS
// ---- INSTRUMENT OPTICS (brief BN) -----------------------------------------
// The user, on the LAPD microscope sheet: "the corners need to almost look
// oddly distorted, like a real microscope ... but underneath it should still
// look pretty clear, specifically in the centre."
//
// CORNER WARP. A microscope's field is flat and sharp in the middle and goes
// strange toward the edge of the field stop: off-axis the image is stretched
// ALONG the arcs round the axis (the tangential image of an astigmatic field)
// while the radius stays put. Here the frame is treated as a square with its
// corners at (+-1, +-1); outside `r0` (1 = a corner) each pixel reads from a
// point turned toward the nearest diagonal, by an angle that grows as
// ((r - r0) / (1 - r0))^2. That is a tangential MAGNIFICATION on each
// diagonal -- the corner content smears out along its arc -- paid for by a
// slight squeeze near the middle of each edge, and it is continuous all the
// way round because sin(4 theta) is. The radius is never changed, so nothing
// is ever read from outside the frame.
//
// It is tied to the FRAME, not to the drifting lens axis: the field stop is
// the eyepiece's, and the whole point is a centre that is untouched by
// construction. At r <= r0 the pixel's uv is returned as it came in -- the
// middle 40% of the frame lies inside r = 0.4, the lowest r0 the slider
// allows, so it is bit-identical at every setting. Nothing here adds light;
// the fluid moving through the warp is what keeps it off a fixed pixel.
float2 CornerWarp(float2 uv, float k, float r0) {
    float2 s  = uv * 2.0 - 1.0;
    float  rn = length(s) * 0.70710678;
    [branch] if (rn <= r0) return uv;
    float  t  = saturate((rn - r0) / max(1.0 - r0, 1e-3));
    // 0.20 rad at k = 1 on the diagonal's neighbourhood: a tangential
    // magnification of 1 / (1 - 0.8 k) right at the corner (5x at k = 1)
    float  dA = (0.20 * k * t * t) * sin(4.0 * atan2(s.y, s.x));
    float  c  = cos(dA), sn = sin(dA);
    return float2(s.x * c - s.y * sn, s.x * sn + s.y * c) * 0.5 + 0.5;
}
// HALATION with a movable knee (brief BN halation_threshold): the halation
// block's own two-ring gather, taps and weights unchanged, knee lo..hi on
// the peak channel. Only called when the key is on; returns the normalised sum.
float3 HalGatherT(float2 ctr, float2 rad, float jt, float sdrH, float lo, float hi) {
    float3 hot = float3(0.0, 0.0, 0.0);
    float  hw  = 0.0;
    [unroll] for (int k = 0; k < 8; k++) {
        float a2 = (float)k * 0.7853982 + jt;
        float3 sp = Src.SampleLevel(linearClamp, ctr + float2(cos(a2), sin(a2)) * (0.55 * rad), 0).rgb;
        hot += sp * smoothstep(lo, hi, max(sp.r, max(sp.g, sp.b)) / sdrH); hw += 1.0;
    }
    [unroll] for (int m = 0; m < 12; m++) {
        float a2 = (float)m * 0.5235988 + jt + 0.2617994;
        float3 sp = Src.SampleLevel(linearClamp, ctr + float2(cos(a2), sin(a2)) * rad, 0).rgb;
        hot += sp * smoothstep(lo, hi, max(sp.r, max(sp.g, sp.b)) / sdrH) * 0.6; hw += 0.6;
    }
    return hot / max(hw, 1e-5);
}
// GLASS STREAKS. The LAPD sheet's thin bright wavy lines across the glass:
// light caught along a fold or a smear line in the cover, not in the picture.
// Two lines, near-horizontal, each a ~1.7 px (FWHM, 1440p) core with a very
// faint 6 px sheen, brighter along wisps and brightest where they pass the
// lamp's reflection in the cover. They live in LID space exactly like the
// scratches -- carried by a third of the lid's wander and turned by a tenth
// of its rotation about its optical centre -- so they drift with the ghosts
// and the sheen, swing when the rig readjusts, and their waves travel slowly
// along them on their own clock: nothing sits on a pixel.
// uv: the pixel (OLED orbit only), ctrL/lidO/lidA/lampR: the lid block's own
// numbers, pxU: one OUTPUT pixel in 1440p px. Returns the line strength.
float GlassStreaks(float2 uv, float2 ctrL, float2 lidO, float lidA,
                   float2 lampR, float aspL, float t, float pxU) {
    float2 F  = float2(1440.0 * aspL, 1440.0);
    float2 P  = uv * F - ctrL * F - lidO * (0.35 * F);
    float  th = 0.10 * lidA;
    float  c  = cos(th), s = sin(th);
    float2 Q  = float2(P.x * c + P.y * s, -P.x * s + P.y * c);  // along, across
    // below 1440p hold the core at one output pixel and dim it by the same
    // ratio (the scratches' rule), so it is never dotted and keeps its energy
    float  w0 = 1.0;
    float  w  = max(w0, 0.75 * pxU);
    float  wA = w0 / w;
    float  acc = 0.0;
    [unroll] for (int k = 0; k < 2; k++) {
        float  fk = (float)k;
        float  y0 = (k == 0) ? -0.085 * F.y : 0.060 * F.y;
        float  f1 = (k == 0) ? 0.0041 : 0.0033;
        float  f2 = (k == 0) ? 0.0107 : 0.0089;
        float  a1 = (k == 0) ? 26.0 : 34.0;
        float  a2 = (k == 0) ? 7.0 : 9.0;
        float  p1 = Q.x * f1 + fk * 2.1 + t * 0.011;
        float  p2 = Q.x * f2 + fk * 4.7 - t * 0.017;
        float  cv = a1 * sin(p1) + a2 * sin(p2);
        float  dv = a1 * f1 * cos(p1) + a2 * f2 * cos(p2);
        float  ds = abs(Q.y - y0 - cv) * rsqrt(1.0 + dv * dv);
        float  ln = exp(-(ds * ds) / (w * w)) * wA
                  + 0.06 * exp(-(ds * ds) / 36.0);
        // wisps: the line comes and goes along its length, slowly
        float  ws = PVNoise(float2(Q.x * (1.0 / 260.0) + fk * 13.1 + t * 0.006, fk * 7.3));
        float  al = 0.25 + 0.75 * smoothstep(0.30, 0.70, ws);
        acc += ln * al * ((k == 0) ? 1.0 : 0.75);
    }
    float  dl = length((uv - lampR) * float2(aspL, 1.0));
    return acc * (0.30 + 0.70 / (1.0 + dl * dl * 6.0));
}
#endif // BN_OPTICS
#ifdef POST_BLACK
// dither_auto's HDR leg: ST 2084 (PQ) for y = nits / 10000, and back
float3 PqEnc(float3 y) {
    float3 p = pow(saturate(y), 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), 78.84375);
}
float3 PqDec(float3 e) {
    float3 p = pow(max(e, 0.0), 1.0 / 78.84375);
    return pow(max(p - 0.8359375, 0.0) / max(18.8515625 - 18.6875 * p, 1e-6), 1.0 / 0.1593017578125);
}
// mass_black's reach test: how much LIT film is there within ~64 px (at
// 1440p) of this pixel? Eight taps on two rings (26 / 64 px), judged by their
// PEAK channel (a deep blue film is plainly lit and weighs next to nothing in
// luminance), the inner ring weighted double. The rings are turned per pixel
// AND per frame by interleaved gradient noise: an edge crossing a fixed tap
// would switch the gate by an eighth at one exact distance and print a contour
// line parallel to the edge; turned, the eighths dissolve into a fine dither.
// Returns the gate: 1 with film close by, falling over ~40..64 px to 0.
#ifdef POST_BLACK_SOFT
// [post] mass_black_soft (post-night T5): the eight taps still printed the
// IGN pattern as diagonal hatching in the 40..64 px band of a still frame
// (each tap moved the gate by an eighth). Here 24 taps on a golden-angle
// spiral, radius 0.3 .. 1 of the reach spread evenly (no ring at one
// distance), weighted 1.3 .. 0.6 with the radius (the near film counts more,
// as the 2:1 rings did), turned by the same per-pixel, per-frame jitter, and
// a wider smoothstep: one tap moves the gate by ~1/24.
float PbLitNear(float2 uv, float2 r64, float jit, float sdr) {
    float s = 0.0, wS = 0.0;
    [unroll] for (int k = 0; k < 24; k++) {
        float  a = (float)k * 2.3999632 + jit;
        float  r = 0.3 + 0.7 * ((float)k + 0.5) / 24.0;
        float  w = 1.6 - r;
        float3 e = ToSRGB(max(Src.SampleLevel(linearClamp, uv + float2(cos(a), sin(a)) * (r64 * r), 0).rgb, 0.0) / sdr);
        s  += max(e.r, max(e.g, e.b)) * w;
        wS += w;
    }
    return smoothstep(0.015, 0.22, s / wS);
}
#else
float PbLitNear(float2 uv, float2 r64, float jit, float sdr) {
    float s = 0.0;
    [unroll] for (int k = 0; k < 8; k++) {
        float  a = (float)k * 0.7853982 + jit;
        float  r = (k & 1) ? 1.0 : 0.4;
        float3 e = ToSRGB(max(Src.SampleLevel(linearClamp, uv + float2(cos(a), sin(a)) * (r64 * r), 0).rgb, 0.0) / sdr);
        s += max(e.r, max(e.g, e.b)) * ((k & 1) ? 1.0 : 2.0);
    }
    return smoothstep(0.02, 0.18, s * (1.0 / 12.0));
}
#endif // POST_BLACK_SOFT
#endif // POST_BLACK
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
float4 PSMain(VSOut i) : SV_Target {
    const float3 W = float3(0.2126, 0.7152, 0.0722);
    // brief BD's four knobs, unpacked once: x grain chroma (1 = today),
    // y grain density curve (0 = today), z fog mass gate (0 = today),
    // w aberration fades with defocus (0 = today's uniform split).
    const float4 bdK = BDU6(rg2.w);
    float2 uv = i.uv;
#ifdef POST_BLACK
    // ---- the POST_BLACK variant (2026-11-02): [post] mass_black + dither_auto
    // Built on demand while either key is on (FluidRenderer::RunPostPass), as
    // BN_OPTICS + BX_HEART + this, so every other preset keeps its bytecode.
    // The dither slot carries all three numbers here, NEGATIVE so the plain
    // dither branch at the end never runs in this variant:
    //   pp7.x = -(1 + dq + 64 mode + 256 mbq), dq = dither x 4 (0..32),
    //   mode 0 today's dither / 1 dither_auto HDR off / 2 dither_auto HDR on,
    //   mbq = mass_black x 63. Exact integers below 2^14: lossless in float.
    float pbP  = -pp7.x - 1.0;
    float pbMb = floor(pbP * (1.0 / 256.0));
    pbP -= pbMb * 256.0;
    float pbMd = floor(pbP * (1.0 / 64.0));
    float pbDq = (pbP - pbMd * 64.0) * 0.25;
    pbMb *= 1.0 / 63.0;
    float pbG  = 1.0;   // the mass_black gate: 1 = today, -> 0 deep in a mass (set with massDeep)
#endif
#ifdef BN_OPTICS
    // brief BN corner_warp (rg1.x field 2) and corner_warp_r (field 3,
    // 0.4..0.9): FIRST, so every tap below reads the warped frame; the grain,
    // the dither and the film overlay are in SV_Position and stay unwarped.
    // All BN code is compiled only into the second post PSO (BN_OPTICS, see
    // FluidRenderer::RunPostPass), so with every BN key at 0 the post pass
    // runs today's exact bytecode: merely adding untaken branches to one
    // shader moved 1-2 pixels by one code on the identity presets (the
    // driver's own ISA compile, not the HLSL, is what changed).
    [branch] if (rg1.x >= 4096.0) {
        const float4 bnW = BDU6(rg1.x);
        [branch] if (bnW.y > 0.0005) uv = CornerWarp(uv, bnW.y, 0.4 + 0.5 * bnW.z);
    }
#endif
    // ======================= V3: MOTION ====================================
    // The user's rule for this whole family: nothing may sit at a fixed screen
    // position on an OLED, ever. Their motion model is specific -- a SLIGHT,
    // SLOW drift all the time, plus occasional readjustments where everything
    // moves at once. Both of these shift the coordinate the finished frame is
    // READ from, so every effect downstream (defocus, aberration, halation,
    // haze, bloom) travels with the picture as one piece, while the grain and
    // the dither stay in screen space -- which is right: the film moves, the
    // sensor does not.
    [branch] if (abs(rg3.z) > 1e-9 || abs(rg3.w) > 1e-9) {
        // OLED PIXEL-SHIFT ORBIT: a couple of px on a many-minute closed path,
        // stepped by a few thousandths of a pixel per frame. Invisible, and
        // still moving, which is the whole trick.
        uv += float2(rg3.z, rg3.w);
    }
    [branch] if (rg3.x > 0.0005) {
        // THERMAL SHIMMER: the air above a lamp. A very fine refractive
        // wobble, a pixel or two at most, strongest near the lamp and fading
        // to nothing away from it -- and ADVECTED BY THE SIM, not by its own
        // clock: the low-res velocity field offsets the noise coordinates, so
        // a burst that shoves the oil shoves the heat above it as well.
        float  aspS = pp0.y / max(pp0.x, 1e-9);
        float2 qs   = float2((uv.x - rg0.x) * aspS, uv.y - rg0.y);
        float  near = exp(-length(qs) * 1.35);
        // CLAMPED on purpose: this texture holds the sim's velocity in the
        // sim's own units, which a burst can drive arbitrarily high, and an
        // unbounded offset into a noise field is not advection -- it is white
        // noise that flickers. Bounded, a burst leans the heat and a calm
        // frame leaves it alone, which is the behaviour that was wanted.
        float2 vel  = clamp(VelLow.SampleLevel(linearClamp, uv, 0).xy * 0.5, -1.5, 1.5);
        float  t    = pp2.y;
        // two octaves, drifting upward off the lamp (heat rises) and carried
        // sideways by the fluid
        float2 np   = float2(uv.x * aspS, uv.y) * 11.0
                    + vel * 1.5 + float2(0.0, -t * 0.09);
        float  n1   = PVNoise(np);
        float  n2   = PVNoise(np * 2.17 + 31.7 - float2(0.0, t * 0.05));
        float2 warp = float2(n1 - 0.5, n2 - 0.5) * (2.0 * rg3.y * rg3.x * near);
        uv += warp * pp0.xy;
    }
    float4 c4 = Src.SampleLevel(linearClamp, uv, 0);
    float3 c  = c4.rgb;
    float3 d  = c;
    // ---- DEPTH OF FIELD: one defocus radius PER PIXEL --------------------
    // post_blur_px is the lens's own softness, the floor under everything;
    // the display pass has written this pixel's circle of confusion into the
    // source's ALPHA (px at 1440p), so an element far from the plane of focus
    // gets a wider disc than the one sitting in it. Gather-style: the radius
    // comes from the CENTRE pixel, which is what makes it one texture read
    // and no second pass, at the price of a sharp element bleeding very
    // slightly into a blurred neighbour's disc -- invisible at these radii,
    // and cheaper than any scatter that would fix it. pp7.x = 0 means no look
    // wrote a CoC and alpha is the plain 1.0, so it is never read.
    // The user, on the first depth-of-field frame: "what's in focus is
    // EXTREMELY in focus." The dreaminess is supposed to come from the haze,
    // the bloom and the elements that are OFF the plane -- not from a global
    // softness sitting on the sharp slice too. So with a CoC in hand
    // post_blur_px stops being a floor under the whole frame and becomes the
    // lens's softness on pixels that have ALREADY left focus, faded in with
    // the CoC itself: on the plane the radius is exactly zero and the Disc is
    // never entered, which is as sharp as the renderer can be.
    float rPx = pp0.z;
    [branch] if (pp5.w > 0.0005) {
        float coc = min(c4.a * pp4.w, pp5.w);
        rPx = max(coc, pp0.z * saturate(coc));
    }
    // DIFFRACTION (item W). The floor, and it does not care about focus: the
    // sharpest a lens can draw a point is its own point spread, so at
    // psf_px >= 1 nothing in the frame is ever pixel-limited and an in-focus
    // isoline can never come out as the stair-stepped coverage AA the user
    // photographed.
    rPx = max(rPx, pp6.z);
    [branch] if (rPx > 0.01) {
        float jit = 6.2831853 * PHash21(i.pos.xy * 0.0173 + 0.31);
        d = Disc(uv, rPx * pp0.xy, jit);
    }
    [branch] if (pp0.w > 0.0005) {
        float3 g = Wide(uv, pp1.x * pp0.xy);
        // glowDark > 0 leans the glare toward the DARK side: dark features
        // bleed into the bright film more than the bright film bleeds into
        // the black (the backlit dish: a droplet's shadow is a real thing,
        // the film's light scattering into the ink is fainter).
        float wg = pp0.w;
        if (pp2.w > 0.0005) {
            float darker = saturate(dot(d, W) - dot(g, W));   // this pixel is brighter than its surround
            wg *= lerp(1.0, 1.0 + 1.5 * step(1e-5, darker), saturate(pp2.w));
        }
        d = lerp(d, g, saturate(wg));
    }
    // brief BD: ONE "am I deep inside a dye mass?" test, shared by the haze
    // below and by the lid's sheen and glint halo further down. Four taps
    // 40 px out: if the WIDE surroundings are dark too, this pixel is inside
    // something and a lift has no business here; if they take in bright film,
    // it is the water beside a mass or the LIGHTER BAND along its edge, and
    // everything stays exactly as it was. The test is spatial, not a test of
    // the pixel's own level, which is the whole reason it cannot darken that
    // band (the user, on the panel: "that low-key should stay, it has a cool
    // effect"). A shape narrower than 40 px never reads as a mass at all, so
    // droplets keep their reflections too.
    //
    // brief BO moved it up here, above the aberration, because the lens split
    // is now one of the things it gates. It only reads Src (the unaberrated
    // frame) at the final uv, so the value the haze and the lid see is the
    // one they always saw.
    //
    // artefact_lum_gate (brief BO, rg1.x's first 6-bit field). The user, on
    // the 1440p dye frames: the grain, the lens split and the cover's sheen
    // read as fake on a PURE BLACK mass and as natural on a coloured one. So
    // inside a mass those artefacts follow the mass's own brightness and
    // colour: g = max(massDeep, satLift), where satLift judges the SAME ring
    // by its peak channel when it is saturated (a deep blue dye weighs next
    // to nothing in luminance and is still plainly a colour). Every artefact
    // is scaled by lerp(1, g, gate); the sheen and the glint halo already
    // carried massDeep, and for them g REPLACES it (lerp(massDeep, g, gate))
    // rather than stacking, so a dyed mass gets its sheen back and a black
    // one stays clean. The glint's core keeps its own rules. At 0 lumG is
    // exactly 1 and lidG exactly massDeep: today's frame, bit for bit.
    const float4 bnK = BDU6(rg1.x);
    float massDeep = 1.0;
    float lumG = 1.0;      // grain, aberration, iridescence
    float lidG = 1.0;      // sheen and glint halo (was massDeep)
#ifdef POST_BLACK
    [branch] if (bdK.z > 0.0005 || rg4.z > 0.5 || rg4.w > 0.5 || bnK.x > 0.0005 || pbMb > 0.0005) {
#else
    [branch] if (bdK.z > 0.0005 || rg4.z > 0.5 || rg4.w > 0.5 || bnK.x > 0.0005) {
#endif
        float  sdrM = max(pp2.z, 1e-3);
        float2 mr   = (40.0 * pp4.w) * pp0.xy;
        float3 sw   = (Src.SampleLevel(linearClamp, uv + float2(mr.x, 0.0), 0).rgb
                     + Src.SampleLevel(linearClamp, uv - float2(mr.x, 0.0), 0).rgb
                     + Src.SampleLevel(linearClamp, uv + float2(0.0, mr.y), 0).rgb
                     + Src.SampleLevel(linearClamp, uv - float2(0.0, mr.y), 0).rgb) * 0.25;
        massDeep = smoothstep(0.02, 0.18, dot(ToSRGB(max(sw, 0.0) / sdrM), W));
        lidG = massDeep;
        [branch] if (bnK.x > 0.0005) {
            float3 eS  = ToSRGB(max(sw, 0.0) / sdrM);
            float  mxS = max(eS.r, max(eS.g, eS.b));
            float  mnS = min(eS.r, min(eS.g, eS.b));
            float  stS = (mxS - mnS) / max(mxS, 1e-4);
            float  g   = max(massDeep, smoothstep(0.02, 0.18, mxS)
                                     * smoothstep(0.15, 0.50, stS));
            lumG = lerp(1.0, g, saturate(bnK.x));
            lidG = lerp(massDeep, g, saturate(bnK.x));
        }
#ifdef POST_BLACK
        // ---- mass_black (user 2026-11-02: "On this black oil, the grey reads
        // insanely obvious and low quality"; the rim at the edge is liked).
        // PbLitNear: lit film within ~64 px = 1 (the water beside a mass and
        // the band along its edge keep every bit of today's light), none = 0
        // (deep in a mass, or in water the film has gone dark around). The
        // wide wash (bloom, 400 px: the grey-blue fill of a big mass), the
        // haze and the lid's ghosts and rings (the grey arcs) scale by
        // lerp(1, gate, mass_black). Halation (60 px, the glow AT the edge)
        // and everything in the display pass are not touched.
        [branch] if (pbMb > 0.0005) {
            float2 jq = i.pos.xy + frac(pp2.y * float2(41.0, 67.0)) * 57.0;
            float  jb = 6.2831853 * frac(52.9829189 * frac(dot(jq, float2(0.06711056, 0.00583715))));
            pbG = lerp(1.0, PbLitNear(uv, (64.0 * pp4.w) * pp0.xy, jb, sdrM), pbMb);
        }
#endif
    }
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
    // ---- LATERAL CHROMATIC ABERRATION (item Z) ---------------------------
    // A real lens focuses red and blue at slightly different MAGNIFICATIONS,
    // so the three channels land on the sensor at slightly different scales:
    // red pushed out from the optical axis, blue pulled in, green where it
    // belongs. On a frame that is a radial split which is nothing at the
    // centre and a couple of pixels at the corners -- exactly the LAPD sheet's
    // colour fringing.
    //
    // This used to be attempted in the display pass as a first-order expansion
    // about the pixel, and it could not work: the offset direction came from
    // the LUMINANCE gradient, which always points at the brighter side, so red
    // was added and blue subtracted on BOTH sides of every droplet. A
    // symmetric warm outline, never a split, at any slider value -- which is
    // why the user could not see it. A lateral split needs the sign to FLIP
    // across an edge, and no derivative taken about one pixel can do that.
    // Here the finished frame exists as a texture, so the channels are simply
    // resampled where they actually landed.
    //
    // The optical centre is the RIG's lens centre, not the middle of the
    // screen: it drifts and it re-aims with everything else, so the fringing
    // never has a fixed null point burnt into one spot on the panel.
    [branch] if (rg2.x > 0.0005) {
        float  aspZ = pp0.y / max(pp0.x, 1e-9);
        float2 qz   = float2((uv.x - rg0.z) * aspZ, uv.y - rg0.w);
        float  rz   = length(qz);
        float2 nz   = qz / max(rz, 1e-5);
        // 0.60 in this space is about a 16:9 corner, so rn is ~1 there. The
        // floor keeps a trace of it mid-frame -- a lens is never perfect in
        // the middle either -- and aberration_field bends how fast it grows.
        float  rn   = saturate(rz / 0.60);
        float  ramp = lerp(0.12, 1.0, pow(rn, 1.0 + saturate(rg2.z * 0.5) * 2.0));
        float  sp   = rg2.y * ramp * saturate(rg2.x);        // px at this res
        float2 duv  = nz * (sp * pp0.y) * float2(1.0 / aspZ, 1.0);
        // Applied as the DIFFERENCE the displacement makes, not as a raw
        // resample: `d` already carries the defocus and the glare, and those
        // are not in Src.
        //
        // brief BD, two corrections to what this block used to claim and do:
        //
        // (1) The comment here said the fringe "correctly fades out with the
        //     blur, which is what a real lens does". It did not. Both taps
        //     came from Src, which is the SHARP display output, while `d` is
        //     the blurred picture -- so a bokeh disc kept a razor-sharp
        //     colour fringe, exactly backwards. The split is now scaled by
        //     this pixel's own defocus radius against the diffraction floor,
        //     so on the sharp slice it is what the user approved on
        //     2026-09-19 and on an out-of-focus element it goes away.
        //
        // (2) Both taps are now a ~1.5 px average (AvgTap) rather than a
        //     single texel. Src still carries the display pass's own grain,
        //     and a first difference of per-pixel noise taken on R and B and
        //     never on G is not a lens -- it is coloured speckle, and it
        //     measured as 72% of all the chroma noise inside a dark mass.
        //     Averaging first leaves the SHAPES, which are what a lateral
        //     split is supposed to act on, and costs eight extra samples.
        // The reference is the diffraction floor, or one texel where there is
        // none, so "in focus" means ab = 1 and nothing about the approved
        // sharp-slice look changes. aberration_coc 0 is the uniform split the
        // user approved on 2026-09-19 and is the default, so this costs no
        // preset anything until one asks for it.
        float  ab = 1.0;
        [branch] if (bdK.w > 0.0005) {
            float abR = max(pp6.z, 1.0);
            ab = lerp(1.0, abR / max(rPx, abR), saturate(bdK.w));
        }
        // brief BO: artefact_lum_gate (lumG = 1 when the gate is 0)
        ab *= lumG;
        float2 ts = pp0.xy * 0.75;
        float3 s0 = AvgTap(uv, ts);
        d.r += (AvgTap(uv + duv, ts).r - s0.r) * ab;
        d.b += (AvgTap(uv - duv, ts).b - s0.b) * ab;
        d = max(d, 0.0);
    }
#if defined(POST_STAGETAP) && POST_STAGETAP == 1
    return float4(d, 1.0);
#endif
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
    // ---- HALATION (item V1) ----------------------------------------------
    // CineStill 800T is ordinary cinema stock with the anti-halation backing
    // removed: light from a highlight goes through the emulsion, reflects off
    // the film base and comes back a few pixels out, reddened by the layers it
    // has now crossed twice. So it is NOT the wide weak wash `bloom` already
    // does. It is tight (a dozen px), it is taken only from what is genuinely
    // bright, and it lands in the DARK around a highlight rather than on the
    // highlight itself -- which is the difference between the film glowing and
    // the picture simply being over-exposed.
    //
    // Two jittered rings of taps around a centre pushed toward the rig's LAMP,
    // so the glow pools on the lamp side and the whole field's asymmetry turns
    // when the lamp wanders. Nothing here is ever at a fixed screen position.
    [branch] if (pp7.y > 0.0005) {
        float  sdrH = max(pp2.z, 1e-3);
        float  asp  = pp0.y / max(pp0.x, 1e-9);
        float2 toL  = float2((rg0.x - uv.x) * asp, rg0.y - uv.y);
        float2 lean = toL / max(length(toL), 1e-5) * float2(1.0 / asp, 1.0);
        float2 rad  = pp7.z * pp0.xy;
        float2 ctr  = uv + lean * rad * 0.30;
        // brief BD: this jitter used to be the FULL circle per pixel. Twenty
        // taps of a spatially varying quantity, angled at random per pixel and
        // weighted into the dark, is a noisy estimator -- it measured as part
        // of the 27% of the dark masses' luminance noise that the post lifts
        // own, and it arrives in this block's warm tint, which is where the
        // grey-brown speckle came from. The taps already cover the circle at
        // 45 degrees; a jitter of HALF a tap spacing fills the gaps between
        // them and leaves nothing else to vary.
        float  jt   = 0.3926991 * (PHash21(i.pos.xy * 0.0271 + 7.13) - 0.5);
        float3 hot  = float3(0.0, 0.0, 0.0);
        float  hw   = 0.0;
        [unroll] for (int k = 0; k < 8; k++) {
            float a2 = (float)k * 0.7853982 + jt;
            float3 sp = Src.SampleLevel(linearClamp, ctr + float2(cos(a2), sin(a2)) * (0.55 * rad), 0).rgb;
            // Only what is REALLY bright contributes, with a soft knee so a
            // drifting highlight does not switch the glow on in one frame.
            // The test is on the PEAK CHANNEL, not on luminance: this film is
            // a saturated magenta whose luminance is a third of its red
            // channel, so a luminance threshold called the brightest thing in
            // the frame dark and the whole effect evaluated to exactly zero.
            // Physically the peak channel is also the right question -- the
            // film base scatters whatever light reached it, and a saturated
            // red highlight halates hard however little it weighs in Y.
            float  ex = smoothstep(0.55, 1.05, max(sp.r, max(sp.g, sp.b)) / sdrH);
            hot += sp * ex; hw += 1.0;
        }
        [unroll] for (int m = 0; m < 12; m++) {
            float a2 = (float)m * 0.5235988 + jt + 0.2617994;
            float3 sp = Src.SampleLevel(linearClamp, ctr + float2(cos(a2), sin(a2)) * rad, 0).rgb;
            float  ex = smoothstep(0.55, 1.05, max(sp.r, max(sp.g, sp.b)) / sdrH);
            hot += sp * ex * 0.6; hw += 0.6;
        }
        hot /= max(hw, 1e-5);
        // brief BN halation_threshold (rg1.y field 2): 0 = today's knee, and
        // the gather above is left exactly as it was (a runtime-selected knee
        // measurably moved default frames). Above 0 the same gather is redone
        // with the knee lowered toward 0.05..0.25 of SDR white, so the
        // mid-tones scatter too and the halation becomes the diffusion haze of
        // a pro-mist filter -- the sharp picture is never blurred, only veiled
        // by a wide, weak, brightness-weighted copy of itself, still leaning
        // toward the lamp.
#ifdef BN_OPTICS
        const float hThr = BDU6(rg1.y).y;
        [branch] if (hThr > 0.0005)
            hot = HalGatherT(ctr, rad, jt, sdrH, 0.55 - 0.50 * hThr, 1.05 - 0.80 * hThr);
#endif
        // reddened by the two passes through the emulsion
        float3 tint = lerp(float3(1.0, 1.0, 1.0), float3(1.00, 0.34, 0.13), saturate(pp7.w));
        // ...and it only shows where this pixel is DARKER than what it is
        // gathering: on the bright film itself there is nothing to see.
        float  own  = max(d.r, max(d.g, d.b)) / sdrH;   // same metric as the gather
        float  into = 1.0 - smoothstep(0.30, 0.95, own);
        d += hot * tint * (saturate(pp7.y) * 0.55 * into);
    }
#if defined(POST_STAGETAP) && POST_STAGETAP == 2
    return float4(d, 1.0);
#endif
    // ---- film grain, after the glass ------------------------------------
    // The display pass's grain lives in the sRGB-encoded domain before the HDR
    // gain; the frame here is linear scRGB with the gain applied. The same
    // luminance weighting and amplitude are reproduced on an sRGB proxy of
    // the pixel and the resulting linear delta is scaled by whatever gain the
    // pixel carries, so the grain reads the same in the SDR mids and never
    // fizzes on a hot HDR core.
    // ---- LIGHT IN THE WATER: volumetric haze + a wide, weak bloom -------
    // The user's reference is a glass of cloudy water with a lamp under it:
    // the WATER glows, brightest near the lamp and fading with distance, so
    // the dark side is never pure black near the light but a soft milky murk,
    // and the bright film bleeds a very wide, very weak wash into the black.
    // Both hang off ONE off-view light position, and that position never sits
    // still: a sum of slow sines, seconds to a minute, so the frame has an
    // idle animation of its own instead of a fixed gradient.
    // (massDeep, the "am I deep inside a dye mass?" test the haze gate reads,
    // is computed above the aberration block since brief BO.)
    [branch] if (pp5.y > 0.0005 || pp5.z > 0.0005) {
        float  tq  = pp2.y;
        float  asp = pp0.y / max(pp0.x, 1e-9);           // W/H
        // the lamp, already drifted, straight off the rig -- same sines as
        // before, evaluated once on the CPU instead of once per pixel
        float2 L   = rg0.xy;
        float2 qL  = float2((uv.x - L.x) * asp, uv.y - L.y);
        float  dl  = length(qL);
        float  sdrL = max(pp2.z, 1e-3);
        float  lum0 = dot(ToSRGB(d / sdrL), W);
        // (a) HAZE. Only into the DARK, and it reaches EXACTLY zero a little
        // way out (the exponential has its own floor subtracted), so far from
        // the lamp an off OLED pixel is still off -- the whole frame is never
        // lifted, which is the one thing that would ruin this panel.
        [branch] if (pp5.y > 0.0005) {
            float x  = dl / max(pp6.x, 1e-4);
            float hz = max(exp(-x) - 0.050, 0.0) / 0.950;
            float wd = 1.0 - smoothstep(0.02, 0.50, lum0);
            // brief BD, fog_mass_gate. "Dark" is not the same question as
            // "water". A dark pixel beside the bright film IS the water and
            // should glow; a dark pixel deep inside a dye mass is looking at
            // the mass, which floats in FRONT of the water, and the haze has
            // no business shining through it. massDeep above is that test. At
            // 0 the gate does not exist and the haze is what it always was.
            float mg = lerp(1.0, massDeep, saturate(bdK.z));
#ifdef POST_BLACK
            mg = min(mg, pbG);   // mass_black
#endif
#ifdef LAMP_GLOW
            // Oil3D L1b: fog = lamp x medium -- the medium's warm white times
            // the lamp window's light (white = today)
            d += float3(1.00, 0.93, 0.86) * laLampGlow.rgb
               * (saturate(pp5.y) * 0.16 * hz * hz * wd * mg * sdrL);
#else
            d += float3(1.00, 0.93, 0.86)
               * (saturate(pp5.y) * 0.16 * hz * hz * wd * mg * sdrL);
#endif
        }
        // (b) BLOOM. Rings of taps at a radius of 100+ px at 1440p: too few
        // taps for a clean blur, which does not matter at a few percent, and
        // a half-tap-spacing angular jitter breaks up what banding there
        // would be without turning the gather itself into noise (brief BD).
        // The radius breathes, the gather centre creeps, and the wash is
        // stronger on the lamp's side, so it drifts with the light instead of
        // sitting still.
        [branch] if (pp5.z > 0.0005) {
            float  rad = pp6.y * (1.0 + 0.12 * sin(tq * 0.0197 + 0.9));
            float2 ctr = uv + float2(sin(tq * 0.0131), cos(tq * 0.0173))
                            * (rad * 0.10 * pp0.xy);
            // The user photographed faint concentric RINGS around every bright
            // droplet sitting in the black: two rings of taps at fixed radii
            // ARE two circles, and a point source lights each of them up. So
            // the taps now cover the whole disc -- four radii with Gaussian
            // weights, jittered in ANGLE by half a tap spacing -- and a point
            // source blooms into a smooth wash, never a halo with edges.
            // brief BD: both of these used to vary over their whole range
            // per pixel. Twenty-four taps spread over 140 px with a per-pixel
            // RADIUS as well as a per-pixel angle is a very noisy estimate of
            // a very wide average, and it is weighted into the dark, so what
            // it printed on a mass was speckle in the film's own colour. The
            // angle keeps half a tap spacing of jitter, which is all that is
            // needed to fill the gaps between the six taps; the radius is
            // fixed. The wash still breathes and still creeps -- those are
            // `rad` and `ctr` above, and they are per FRAME, not per pixel.
            float  jit = 0.5235988 * (PHash21(i.pos.xy * 0.37) - 0.5);
            float  rj  = 1.0;
            float3 bl  = float3(0.0, 0.0, 0.0);
            float  bw  = 0.0;
            [unroll] for (int m = 0; m < 6; m++) {
                float  an = (float)m * 1.0471976 + jit;
                float2 un = float2(cos(an), sin(an));
                [unroll] for (int k = 0; k < 4; k++) {
                    float  fr = (0.18 + 0.27 * (float)k) * rj;
                    float  w  = exp(-fr * fr * 1.6);
                    bl += max(Src.SampleLevel(linearClamp, ctr + un * (fr * rad * pp0.xy), 0).rgb, 0.0) * w;
                    bw += w;
                }
            }
            bl /= max(bw, 1e-4);
            float wd = 1.0 - smoothstep(0.10, 0.75, lum0);
            float lw = 0.75 + 0.50 * exp(-dl / max(pp6.x * 1.5, 1e-4));
#ifdef BN_OPTICS
            // brief BN bloom_warmth (rg1.x field 4): the LAPD tile's veil is
            // the LAMP's colour, not the film's -- hot yellow-white on the
            // lamp side, falling to red away from it. The wash keeps its own
            // luminance exactly (the tint is normalised to Y = 1), so this is
            // a hue move and nothing else: mean_lum and ABL do not change.
            const float bWm = BDU6(rg1.x).w;
            [branch] if (bWm > 0.0005) {
                float  nl  = exp(-dl / max(pp6.x * 1.5, 1e-4));
                float3 wc  = lerp(float3(1.00, 0.34, 0.20), float3(1.00, 0.82, 0.55), nl);
#ifdef LAMP_GLOW
                // Oil3D L1b: the lamp's colour through its window (hue only:
                // the wash keeps the luminance it gathered)
                wc *= laLampGlow.rgb;
                wc /= max(dot(wc, W), 1e-6);
#else
                wc /= dot(wc, W);
#endif
#ifdef POST_BLOOM_AMB
                // T7 bloom_ambient: inside the dark bodies (wd, the wash's own
                // dark-pixel weight: the droplets' inner discs, the masses near
                // their edge) the veil keeps the colour it gathered -- the film
                // round them -- instead of the lamp's warm hue; the lit film
                // keeps bloom_warmth as today
                bl = lerp(bl, dot(bl, W) * wc, saturate(bWm) * (1.0 - wd));
#else
                bl = lerp(bl, dot(bl, W) * wc, saturate(bWm));
#endif
            }
#endif
#ifdef POST_BLACK
            bl *= pbG;           // mass_black: the wide wash stays out of a mass
#endif
            d += bl * (saturate(pp5.z) * 0.20 * wd * lw);
        }
    }

)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
    // =====================================================================
    // THE LID (task V2)  -- self-contained; ONE packed constant (rg4), own
    // branch, nothing above or below it touched. The user's constraints are
    // NEGATIVE ones: no visible instrument, no dish rim, no grid, no UI
    // marks, no dark corners -- the oil stays full-bleed. So there is no mask
    // and no border anywhere in here. Every term is ADDITIVE and full-frame.
    //
    // The excuse for all of it is a sheet of glass or plastic lying over the
    // dish: what you see is the bright film reflected INSIDE that sheet
    // (ghosts and ring ghosts), the lamp smeared across its surface (sheen
    // and glint), the interference colours of the thin oil film on it (iris),
    // and the wobble a cheap sheet gives its own reflections (refract).
    // Reference: the LAPD optics sheet, endgoal-ref-13.
    //
    // NOTHING HERE IS AT A FIXED SCREEN POSITION. Every position is built
    // from the rig: the lamp (rg0.xy, always drifting), the lens axis
    // (rg0.zw) and the lid's own wander (rg4.x, four periods between three
    // and twelve minutes). At each of the rig's occasional readjustments the
    // lid is shoved to a new resting offset on the same spring the focus
    // rides, so the ghosts, the sheen and the glint all arrive together --
    // never one effect moving alone.
    // =====================================================================
#ifdef BN_OPTICS
    [branch] if (rg4.z > 0.5 || rg4.w > 0.5 || rg1.z > 0.5 || rg1.y >= 262144.0) {
#else
    [branch] if (rg4.z > 0.5 || rg4.w > 0.5 || rg1.z > 0.5) {
#endif
        float  sdrL = max(pp2.z, 1e-3);
        float  aspL = pp0.y / max(pp0.x, 1e-9);            // W/H
        // unpack: see the rg4 comment in the cbuffer above
        float2 lidO = LidU12(rg4.x) - 0.5;                 // uv, +-0.5
        float2 lidB = LidU12(rg4.y);
        float  lidA = lidB.x * 8.0 - 4.0;                  // rad
        float  lidW = lidB.y * 24.0;                       // refract, px here
        float3 lidC = LidU8(rg4.z);                        // ghost rings sheen
        float4 lidD = LidU7764(rg4.w);                     // glint iris shPx spread
        float  lidS = 8.0 + lidD.z * 1392.0;               // sheen px, this res
        float  lidP = lidD.w * 2.0;                        // ghost spread
        // The master is already folded into every amplitude on the CPU.
        const float M = 1.0;
        // Where the sheet's optical centre sits: the lens axis, carried by
        // the lid's own wander.
        float2 ctrL = float2(rg0.z, rg0.w) + lidO * 0.45;
        // ...and where the lamp's reflection lands in it: the lamp mirrored
        // through that centre and pulled back inside the frame, so it is
        // always somewhere on screen and always moving with the lamp.
        float2 lampR = ctrL - (float2(rg0.x, rg0.y) - ctrL) * 0.55 + lidO * 0.75;
        // The peak channel, never luminance: this film is a saturated
        // magenta whose luminance is a third of its red, and a luminance
        // test calls the brightest thing in the frame dark.
        float  ownL = max(d.r, max(d.g, d.b)) / sdrL;
        // Reflections in a cover show up in the DARK; on the bright film
        // itself there is nothing to see. Not a hard gate -- a lean.
        float  into = lerp(0.30, 1.0, 1.0 - smoothstep(0.25, 0.95, ownL));

        // ---- the sheet is not flat: its reflections wobble --------------
        // Only the REFLECTIONS wobble, not the transmitted picture: warping
        // the picture would mean resampling the frame and undoing the depth
        // of field the camera pass just computed. Physically it is also the
        // right half -- light bounced inside the sheet crosses its uneven
        // faces twice, transmitted light barely at all.
        float2 wob = float2(sin(uv.y * 7.3 + lidA * 3.1 + pp2.y * 0.047),
                            sin(uv.x * 6.1 - lidA * 2.3 + pp2.y * 0.031))
                   * (lidW * pp0.xy);

        // ---- GHOSTS: offset, dimmed, slightly magnified copies ----------
        // The classic lens-flare chain: each internal bounce puts a copy of
        // the bright field on the line from the lamp's reflection through the
        // optical centre, at its own scale. Tinted by the coating it bounced
        // off -- amber, green, magenta, which is what the LAPD sheet shows.
        [branch] if (lidC.x > 0.0005) {
            float3 gsum = float3(0.0, 0.0, 0.0);
            [unroll] for (int gi = 0; gi < 3; gi++) {
                float  fk = lerp(-0.42, 0.80, (float)gi * 0.5) * (0.4 + lidP);
                float2 guv = ctrL + (uv - ctrL) * (1.0 + fk * 0.55)
                           - (lampR - ctrL) * fk + wob;
                float3 sp  = Src.SampleLevel(linearClamp, guv, 0).rgb;
                // Same knee as halation, and on the PEAK CHANNEL for the same
                // reason: only what is genuinely bright bounces.
                float  ex  = smoothstep(0.55, 1.15,
                                        max(sp.r, max(sp.g, sp.b)) / sdrL);
                // fade where the copy runs off the plate, or the clamp would
                // paint a static-looking band along the edge
                float2 fe = smoothstep(0.0, 0.05, guv) * smoothstep(1.0, 0.95, guv);
                // Desaturated on purpose. A fully saturated green bounce over
                // a magenta film comes out OLIVE and the black masses turn to
                // mud; these are coatings, not filters.
                float3 tint = (gi == 0) ? float3(1.00, 0.70, 0.38)
                           : ((gi == 1) ? float3(0.60, 0.95, 0.80)
                                        : float3(0.95, 0.66, 1.00));
                // ...and the green bounce is the weakest of the three, for
                // the same reason.
                float  gw   = (gi == 0) ? 1.00 : ((gi == 1) ? 0.55 : 0.40);
                gsum += sp * (ex * fe.x * fe.y * gw) * tint;
            }
#ifdef POST_BLACK
            gsum *= pbG;         // mass_black
#endif
            d += gsum * (M * lidC.x * 0.085 * into);
        }

        // ---- RING GHOSTS: the concentric coloured arcs ------------------
        // A reflection off a curved element images the field as a ring, not
        // as a copy. Three annuli of different width and colour about a
        // centre that is the far end of the ghost chain, so they travel with
        // the ghosts. Weighted into the dark, never a drawn circle over the
        // oil.
        [branch] if (lidC.y > 0.0005) {
            float2 rc = ctrL - (lampR - ctrL) * (0.55 + 0.30 * lidP);
            float2 q  = (uv - rc) * float2(aspL, 1.0);
            float  rr = length(q) / max(0.34 + 0.22 * lidP, 1e-4);
            float  b1 = exp(-64.0 * (rr - 0.55) * (rr - 0.55));
            float  b2 = exp(-192.0 * (rr - 0.74) * (rr - 0.74));
            float  b3 = exp(-36.0 * (rr - 0.96) * (rr - 0.96));
            float3 rcol = float3(0.30, 1.00, 0.68) * b1
                        + float3(1.00, 0.80, 0.30) * b2
                        + float3(1.00, 0.26, 0.12) * b3;
#ifdef POST_BLACK
            rcol *= pbG;         // mass_black: no ring drawn across a black mass
#endif
            d += rcol * (M * lidC.y * 0.030 * into * sdrL);
        }

        // ---- SHEEN: the broad soft specular off the cover ---------------
        // A wide anisotropic smear about the lamp's reflection, its long axis
        // turning with the sheet. This is the "warm veiling flare across the
        // field" of the reference -- deliberately enormous and deliberately
        // weak, so it reads as glass in front of the picture rather than as a
        // light in it.
        float sheenE = 0.0;
        [branch] if (lidC.z > 0.0005 || lidD.y > 0.0005) {
            float  rs = max(lidS * pp0.y, 1e-4);          // px at this res -> uv
            float2 q  = (uv - lampR) * float2(aspL, 1.0);
            float  ca = cos(lidA), sa = sin(lidA);
            float2 e2 = float2(q.x * ca + q.y * sa, -q.x * sa + q.y * ca);
            e2.x /= 2.7;                                   // drawn out along the sheet
            sheenE = exp(-dot(e2, e2) / (rs * rs));
            // brief BD: `massDeep` here is new. The sheen was the one lid
            // term with no fade to black at all -- a flat warm floor across
            // the whole frame, masses included, measured at about a third of
            // a nit of warm lift on pixels that are meant to be black (a
            // coloured dye is brief AG, a separate feature). It keeps every
            // bit of itself on the film and along a mass edge and gives up
            // only the deep interior, where there is nothing for a reflection
            // to sit on.
            d += float3(1.00, 0.88, 0.70)
               * (M * lidC.z * 0.075 * sheenE * sdrL * lidG);   // BO: lidG = massDeep at gate 0
        }

        // ---- IRIDESCENCE: the thin oil film on the cover ----------------
        // Interference colours: a slowly turning phase ramp read through a
        // cosine palette, living only where the sheet is catching light, so
        // it appears as a colour sweep across the sheen and nowhere else.
        [branch] if (lidD.y > 0.0005) {
            float2 q  = (uv - ctrL) * float2(aspL, 1.0);
            float  ph = 7.0 * (q.x * cos(lidA) + q.y * sin(lidA))
                      + 2.6 * sin(q.y * 3.3 - lidA * 1.7)
                      + pp2.y * 0.021;
            float3 ir = 0.5 + 0.5 * cos(ph + float3(0.0, 2.0944, 4.1888));
            d += (ir - 0.333) * (M * lidD.y * 0.055 * sheenE * sdrL * lumG);   // BO
        }

        // ---- GLINT: the lamp itself, seen in the cover ------------------
        // A soft core with a wide amber halo. The core is kept soft and
        // modest on purpose: it is the brightest thing this block adds and it
        // lives on an OLED, so it is a blob that wanders hundreds of pixels
        // over the minutes, never a star pinned to a pixel.
        [branch] if (lidD.x > 0.0005) {
            float  dd   = length((uv - lampR) * float2(aspL, 1.0));
            float  core = exp(-(dd * dd) / (0.030 * 0.030));
            float  halo = exp(-dd / 0.19);
            // brief BD: the CORE keeps its own rules -- it is the lamp seen
            // in the cover, a small bright thing, and the user asked for more
            // of the lid's reflections, not less (brief AF). It is the wide
            // amber HALO that was the lift: ~2 nits of warm haze over a third
            // of the frame with nothing to stop it inside a mass, so that one
            // gives up the deep interior and nothing else.
            d += (float3(1.00, 0.90, 0.72) * (core * 0.55)
                + float3(1.00, 0.56, 0.20) * (halo * 0.13 * lidG))   // BO: lidG, see above
                 * (M * lidD.x * sdrL);
        }

        // ---- SCRATCHES: the wear on the cover (brief BM) ----------------
        // Lit by the lamp as the cover sees it (lampR, the glint's position)
        // and carried by the lid: shifted by a third of its wander and turned
        // by a sliver of its rotation about its optical centre, so they drift
        // with the ghosts and are shoved with them at a readjust. The pixel
        // comes in through the OLED orbit only, NOT the shimmer warp above --
        // that one is moved by the fluid, and the lid never is. Not scaled by
        // the lid master: they are the sheet's wear, not a reflection in it.
        [branch] if (rg1.z > 0.5) {
            float3 sA  = LidU8(rg1.z);                     // amount density len
            float3 sB  = LidU8(rg1.w);                     // corner soft tint
            float2 F   = float2(1440.0 * aspL, 1440.0);
            float2 C   = ctrL * F;
            float2 off = lidO * (0.35 * F);
            float  th  = 0.06 * lidA;
            float  cth = cos(th), sth = sin(th);
            float2 Ps  = (i.uv + float2(rg3.z, rg3.w)) * F - C - off;
            float2 Ls  = lampR * F - C - off;
            float2 Pl  = float2(Ps.x * cth + Ps.y * sth, -Ps.x * sth + Ps.y * cth) + C;
            float2 Ll  = float2(Ls.x * cth + Ls.y * sth, -Ls.x * sth + Ls.y * cth) + C;
            float  sc  = saturate(LidScratch(Pl, Ll, F, sA.y, sA.z, sB.x, sB.y,
                                              1.0 / max(pp4.w, 1e-4)));
            // brightest near the lamp, never gone far from it
            float  dl  = length((i.uv - lampR) * float2(aspL, 1.0));
            float  fal = 0.15 + 0.85 / (1.0 + dl * dl * 8.0);
            // clear over the dark, a whisper over the bright film
            float  ovr = lerp(0.10, 1.0, 1.0 - smoothstep(0.12, 0.80, ownL));
            // tint 0: neutral white. 1: the colour of the film under the
            // groove (light scattered out of the picture it lies over);
            // over black there is no colour to take, so it stays white.
            float  mx  = max(d.r, max(d.g, d.b));
            float3 fc  = max(d, 0.0) / max(mx, 1e-4);
            float3 col = lerp(float3(1.0, 1.0, 1.0), fc,
                              sB.z * smoothstep(0.02, 0.10, mx / sdrL));
            d += col * (sA.x * 0.60 * sc * fal * ovr * sdrL);
        }
#ifdef BN_OPTICS
        // ---- GLASS STREAKS (brief BN, rg1.y field 1) --------------------
        // Deliberately NOT gated by massDeep / lidG: they are on the glass,
        // in front of everything, and a gate would leave them inert over the
        // masses that fill most of the frame. Not ADDED either: the first
        // cut was an add leaned off the bright pixels, and on a frame that is
        // 90% saturated film it was inert too. A line of light on glass
        // WHITENS what is behind it, so the pixel is pulled toward a pale
        // warm target at 1.2x SDR white, per channel and never downward
        // (max): over black it is a bright line, over the magenta film a
        // paler one, and over a core already brighter than the target
        // nothing -- the frame's peak cannot rise. Written 0 by the CPU
        // unless the lid is on, so lidO / lidA are always real here.
        [branch] if (rg1.y >= 262144.0) {
            float  gsA = BDU6(rg1.y).x;
            float  gs  = GlassStreaks(i.uv + float2(rg3.z, rg3.w), ctrL, lidO, lidA,
                                      lampR, aspL, pp2.y, 1.0 / max(pp4.w, 1e-4));
            float3 tg  = float3(1.00, 0.90, 0.86) * (1.2 * sdrL);
            d = max(d, lerp(d, tg, saturate(gsA * gs)));
        }
#endif
    }
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
#if defined(POST_STAGETAP) && POST_STAGETAP == 3
    return float4(d, 1.0);
#endif
    // ---- FILM OVERLAY ARTEFACTS -----------------------------------------
    // Hairs caught in the gate, dust, fine scratches and the odd light leak.
    // Everything is authored in px at 1440p (P below) and is therefore the
    // same size relative to the picture at any output resolution, and
    // everything is ADDITIVE and weighted toward the DARK pixels: a speck on
    // the black is a little star, the same speck on the film is nothing.
    //
    // Nothing sits still. Each artefact's seed is hashed from a QUANTISED
    // time -- floor(t / rate) for the hairs, slower multiples of it for the
    // scratches and the leak, the 24 fps "film frame" for the dust -- so the
    // dust flickers frame to frame, a hair sticks for a few seconds and is
    // gone, and a scratch persists for a stretch, drifts and disappears.
    [branch] if (pp3.x > 0.0005 || pp3.y > 0.0005 ||
                 pp3.z > 0.0005 || pp3.w > 0.0005) {
        float  sc  = max(pp4.w, 1e-4);
        float2 P   = i.pos.xy / sc;                       // px at 1440p
        float  Hy  = 1440.0;
        float  Wx  = 1440.0 * (pp0.y / max(pp0.x, 1e-9)); // frame width, same units
        float  t   = pp2.y;
        float  per = max(pp4.x, 0.25);
        float  ff  = floor(t * 24.0);                     // the film frame
        float3 add = float3(0.0, 0.0, 0.0);

        // DUST: sparse bright points 1-3 px across at 1440p, a new population
        // every film frame. A cell of a coarse grid may carry one speck,
        // placed well inside it so a single-cell lookup is exact.
        [branch] if (pp3.x > 0.0005) {
            const float CELL = 42.0;
            float2 cid = floor(P / CELL);
            float2 fp  = P - cid * CELL;
            float  h0  = PHash21(cid * 1.7 + frac(ff * 0.0371) * 511.0);
            [branch] if (h0 > 1.0 - 0.10 * saturate(pp3.x)) {
                float  h1 = PHash21(cid + 19.3 + h0 * 37.0);
                float  h2 = PHash21(cid + 71.9 + h0 * 11.0);
                float  h3 = PHash21(cid + 3.71 + h0 * 53.0);
                float2 ct = float2(0.2 + 0.6 * h1, 0.2 + 0.6 * h2) * CELL;
                float  r  = 0.6 + 1.4 * h3;
                float  dd = length(fp - ct) / r;
                add += (0.30 * saturate(pp3.x) * (0.35 + 0.65 * h3))
                     * exp(-dd * dd * 1.6);
            }
        }
        // HAIRS: three slots, each either empty or carrying one curly strand
        // for this reel. The strand is the set of points a fixed distance
        // from a wobbling line (two sines in the along-strand coordinate),
        // with the distance divided by sqrt(1 + slope^2) so the hair keeps
        // its thickness through every curl.
        [branch] if (pp3.y > 0.0005) {
            float tq  = floor(t / per);
            float ph  = frac(t / per);
            float env = smoothstep(0.0, 0.12, ph) * (1.0 - smoothstep(0.78, 1.0, ph));
            [unroll] for (int k = 0; k < 3; k++) {
                float fk = (float)k;
                float s0 = PHash21(float2(tq * 1.13 + 5.7, fk * 9.31 + 2.1));
                if (s0 > saturate(pp3.y)) continue;
                float2 C  = float2(PHash21(float2(tq + 3.3, fk * 7.7)) * Wx,
                                   PHash21(float2(tq + 7.1, fk * 5.9)) * Hy);
                float  an = PHash21(float2(tq + 11.3, fk * 2.7)) * 6.2831853;
                float  L  = 30.0 + 120.0 * PHash21(float2(tq + 17.9, fk * 4.1));
                float2 dr = float2(cos(an), sin(an));
                float2 nr = float2(-dr.y, dr.x);
                float2 rl = P - C;
                float  tt = dot(rl, dr);
                if (abs(tt) > L * 0.5 + 3.0) continue;
                float  w1 = 0.055 + 0.050 * s0;
                float  a1 = 3.0 + 6.0 * PHash21(float2(tq + 23.1, fk * 6.3));
                float  a2 = 5.0 + 9.0 * PHash21(float2(tq + 29.7, fk * 8.7));
                float  cp = PHash21(float2(tq + 37.3, fk * 1.9)) * 6.2831853;
                float  cv = a1 * sin(tt * w1 + cp) + a2 * sin(tt * 0.021 - cp * 0.7);
                float  dv = a1 * w1 * cos(tt * w1 + cp)
                          + a2 * 0.021 * cos(tt * 0.021 - cp * 0.7);
                float  ds = abs(dot(rl, nr) - cv) * rsqrt(1.0 + dv * dv);
                float  wd = 0.6 + 0.6 * PHash21(float2(tq + 41.7, fk * 3.1));
                // the gate flutters: the strand breathes a little every frame
                float  jt = (PHash21(float2(ff, fk * 13.1)) - 0.5) * 0.7;
                float  en = 1.0 - smoothstep(L * 0.40, L * 0.5, abs(tt));
                add += (1.0 - smoothstep(wd, wd + 1.5, ds + jt)) * en * env
                     * (0.09 * (0.5 + 0.5 * saturate(pp3.y)));
            }
        }
        // SCRATCHES: four slots on a slower clock, near-vertical, faintly
        // wavy, drifting sideways, each covering part of the height.
        [branch] if (pp3.z > 0.0005) {
            float tq  = floor(t / (per * 3.0));
            float ph  = frac(t / (per * 3.0));
            float env = smoothstep(0.0, 0.10, ph) * (1.0 - smoothstep(0.82, 1.0, ph));
            [unroll] for (int j = 0; j < 4; j++) {
                float fj = (float)j;
                float s0 = PHash21(float2(tq * 1.7 + 3.3, fj * 7.1 + 1.3));
                if (s0 > saturate(pp3.z)) continue;
                // The sideways drift runs on the time INSIDE this slot's window (up to
                // +-3 px/s over per*3 s). It used to run on the total uptime t, so the
                // offset grew to +-3t px and the scratches left the frame for good:
                // all on screen in a short shot, ~12% after an hour, ~0.5% after a day
                // (review 2026-10-04 R13).
                float xc = PHash21(float2(tq + 13.7, fj * 2.9)) * Wx
                         + (PHash21(float2(tq + 19.1, fj * 5.3)) - 0.5) * 6.0 * (ph * per * 3.0)
                         + 2.0 * sin(P.y * 0.004 + s0 * 6.2831853);
                float wd = 0.35 + 0.80 * PHash21(float2(tq + 23.3, fj * 3.7));
                float y0 = PHash21(float2(tq + 29.9, fj * 11.3)) * Hy * 0.6;
                float y1 = y0 + Hy * (0.35 + 0.65 * PHash21(float2(tq + 31.1, fj * 4.7)));
                float yv = smoothstep(y0 - 40.0, y0 + 40.0, P.y)
                         * (1.0 - smoothstep(y1 - 60.0, y1 + 60.0, P.y));
                // The user, on the first version: "no film has a line like
                // that" -- a clean 1-px rule down the frame. A real gate
                // scratch is faint, its edges are soft, and it is BROKEN along
                // its length: the emulsion is torn in stretches, not cut, so
                // it reads as a flickering dashed thread, never a ruled line.
                float seg = PHash21(float2(floor(P.y / 14.0) + tq * 3.1, fj * 9.7 + ff * 0.13));
                float brk = smoothstep(0.35, 0.75, seg);           // ~55% of segments lit
                add += (1.0 - smoothstep(wd, wd + 2.6, abs(P.x - xc))) * yv * env * brk
                     * (0.5 + 0.5 * PHash21(float2(ff, fj * 17.3)))
                     * (0.022 * (0.4 + 0.6 * saturate(pp3.z)));
            }
        }
        // LIGHT LEAK: a COLOURED wash entering from one edge -- a warm core
        // (red through orange to yellow) with a cool green/teal fringe just
        // outside it, in soft bands along the edge -- swelling and dying on
        // its own slow clock, and not returning every time.
        [branch] if (pp3.w > 0.0005) {
            float tq  = floor(t / (per * 5.0));
            float ph  = frac(t / (per * 5.0));
            float s0  = PHash21(float2(tq * 2.7 + 1.9, 4.21));
            float on  = step(s0, saturate(pp3.w * 0.8 + 0.2));
            float env = sin(saturate(ph) * 3.14159265);
            env = on * env * env;
            float sd  = PHash21(float2(tq + 5.1, 8.3));
            float2 q  = float2(P.x / Wx, P.y / Hy);
            float  u  = (sd < 0.25) ? q.x : ((sd < 0.5) ? 1.0 - q.x
                      : ((sd < 0.75) ? q.y : 1.0 - q.y));
            float  v  = (sd < 0.5) ? q.y : q.x;                  // along the edge
            float  bd = 0.75 + 0.25 * sin(v * 9.0 + t * 0.05 + s0 * 6.2831853)
                             * sin(v * 3.0 - t * 0.03);
            float  core = exp(-u * u * 34.0);                    // warm, on the edge
            float  frng = exp(-(u - 0.16) * (u - 0.16) * 60.0);  // cool, just outside
            float3 warm = lerp(float3(1.00, 0.22, 0.06), float3(1.00, 0.72, 0.18),
                               PHash21(float2(tq + 9.7, 2.3)));
            float3 cool = float3(0.10, 0.85, 0.65);
            add += (warm * core + cool * (frng * 0.45))
                 * (bd * env * 0.20 * saturate(pp3.w));
        }
        // The whole overlay leans into the dark: near-invisible on the film.
        // brief BD: the lean used to have a FLOOR of 0.10, so a speck still
        // landed at a tenth strength on a pixel that was meant to be black.
        // The floor is now zero -- on the bright film there is nothing to see,
        // which is what this block's own help text always claimed.
        float sdrA = max(pp2.z, 1e-3);
        float lumA = dot(ToSRGB(d / sdrA), W);
        d += add * (sdrA * (1.0 - smoothstep(0.20, 0.80, lumA)));
    }
#if defined(POST_STAGETAP) && POST_STAGETAP == 4
    return float4(d, 1.0);
#endif
    // ---- film grain, after the glass ------------------------------------
    [branch] if (pp1.y > 0.0005) {
        float2 gc  = floor(i.pos.xy / max(pp1.z, 0.25));
        // film_grain_fps patterns per second (24 by default: once per frame of
        // the stock, a whole number of panel refreshes each) times the speed.
        float  tq  = floor(pp2.y * max(pp6.w, 1.0) * max(pp1.w, 0.0));
        float2 tj  = frac(tq * float2(0.1031, 0.0973)) * 733.0;
        float  n0  = PHash21(gc + tj);
        float3 nz  = float3(n0, n0, n0);
        [branch] if (pp2.x > 0.0005) {
            nz = lerp(nz, float3(n0, PHash21(gc + tj + 37.71),
                                     PHash21(gc + tj + 91.37)), saturate(pp2.x));
        }
        d = Emulsion(d, nz, pp1.y * lumG, max(pp2.z, 1e-3), bdK.x, bdK.y);   // BO: lumG
    }
    // ---- film_noise: the finer, faster layer under the stock's grain -----
    // A new pattern every FRAME OF THE STOCK (film_grain_fps, ignoring the
    // grain speed multiplier), one px at 1440p by default: the emulsion's
    // fizz as against the stock's grain structure, on the same film cadence.
    [branch] if (pp4.y > 0.0005) {
        float2 nc = floor(i.pos.xy / max(pp4.z, 0.25));
        float  tq = floor(pp2.y * max(pp6.w, 1.0));
        float2 tj = frac(tq * float2(0.0891, 0.1237)) * 557.0;
        float  n0 = PHash21(nc + tj + 211.7);
        d = Emulsion(d, float3(n0, n0, n0), pp4.y * lumG, max(pp2.z, 1e-3), bdK.x, bdK.y);   // BO: lumG
    }
#if defined(POST_STAGETAP) && POST_STAGETAP == 5
    return float4(d, 1.0);
#endif
    // ---- film_stock: the stock's own colour ------------------------------
    // Lifted teal shadows, warm highlights, a slightly different curve per
    // channel -- the cross-process feel. Done on the sRGB proxy so it reads
    // as a grade and not as a gain, and scaled by the pixel's own HDR gain so
    // a hot core keeps its level. The shadow lift is deliberately tiny: at
    // the shipped strength it is a fraction of a nit on a black pixel.
    [branch] if (pp5.x > 0.0005) {
        float  k    = saturate(pp5.x);
        float  sdrS = max(pp2.z, 1e-3);
        float3 base = d / sdrS;
        float3 e    = ToSRGB(base);
        float  l    = dot(e, W);
        // ...but a pixel that is TRUE BLACK keeps its black: the lift ramps in
        // from just above zero, so an off OLED pixel stays off and only the
        // shadows that already carry light get the teal.
        float3 sh   = float3(-0.004, 0.008, 0.022) * (1.0 - smoothstep(0.0, 0.40, l))
                    * smoothstep(0.004, 0.060, l);
        float3 hi   = float3( 0.045, 0.012, -0.030) * smoothstep(0.30, 1.0, l);
        float3 e2   = max(e + (sh + hi * e) * k, 0.0);
        float3 l0   = ToLinear(e), l2 = ToLinear(e2);
        float  gain = clamp(dot(base, W) / max(dot(l0, W), 1e-4), 1.0, 16.0);
        d = max(d + (l2 - l0) * gain * sdrS, min(d, 0.0));
    }
#if defined(POST_STAGETAP) && POST_STAGETAP == 6
    return float4(d, 1.0);
#endif
#ifdef BX_HEART
    // ---- brief BX: GREY HEART, the desaturation (fix round) --------------
    // The last colour op of the frame (only the dither follows), so nothing
    // re-saturates the grey: rg1.y fields 3/4 = amount, (size - 0.8) / 1.4.
    // Same mask as the display pass (which lifted the film already).
    {
        const float4 hxK = BDU6(rg1.y);
        [branch] if (hxK.z > 0.0) {
            float  hS = 0.8 + 1.4 * hxK.w;
            float2 hq = i.pos.xy * pp0.xy * 2.0 - 1.0;
            float  hm = 1.0 - smoothstep(0.3 * hS, hS, length(hq));
            [branch] if (hm > 0.0) d = HeartGreyP(d, hxK.z * sqrt(hm));
        }
    }
#endif
#if defined(POST_STAGETAP) && POST_STAGETAP == 7
    return float4(d, 1.0);
#endif
    // ---- OUTPUT DITHER (item V0) -----------------------------------------
    // The frame leaves here as FP16, but the panel quantises it to 10 bits in
    // a perceptual domain, and on a big saturated flat -- which is most of
    // this look -- the user can see the steps. So the noise goes in where the
    // quantiser is: on the sRGB-encoded proxy, half an LSB of a 10-bit signal,
    // converted back to linear and scaled by whatever HDR gain this pixel
    // carries so a hot core is dithered by the same half step as a mid tone.
    //
    // INTERLEAVED-GRADIENT noise rather than a hash: its spectrum is close to
    // blue, so the pattern sits above the eye's peak sensitivity instead of
    // clumping the way white noise does, and it costs three instructions. A
    // new phase every frame, and a different one per channel, because the
    // channels band independently on a saturated colour.
    //
    // It FADES OUT into true black. A dither that lifted an off OLED pixel
    // would be a far worse bug than the band it fixed.
    [branch] if (pp7.x > 0.0005) {
        float  sdrD = max(pp2.z, 1e-3);
        float3 base = d / sdrD;
        float3 e    = ToSRGB(base);
        float2 jt   = frac(pp2.y * float2(37.0, 61.0)) * 61.0;
        float3 n;
        n.x = frac(52.9829189 * frac(dot(i.pos.xy + jt,          float2(0.06711056, 0.00583715))));
        n.y = frac(52.9829189 * frac(dot(i.pos.xy + jt + 23.71,  float2(0.06711056, 0.00583715))));
        n.z = frac(52.9829189 * frac(dot(i.pos.xy + jt + 51.37,  float2(0.06711056, 0.00583715))));
        float  lq = dot(e, W);
        // ...and the fade starts well ABOVE the first code, not at it. Banding
        // is a mid-tone and flat-highlight problem; the deep shadow has
        // nothing to dither and everything to lose, so below ~0.2 nits this
        // term is exactly zero and the OLED's off pixels stay off.
        float  amp = pp7.x * (1.0 / 1023.0) * smoothstep(0.010, 0.040, lq);
        float3 e2 = max(e + (n - 0.5) * amp, 0.0);
        float3 l0 = ToLinear(e), l2 = ToLinear(e2);
        float  g  = clamp(dot(base, W) / max(dot(l0, W), 1e-4), 1.0, 16.0);
        d = max(d + (l2 - l0) * g * sdrD, min(d, 0.0));
    }
)hlsl"
// (split: MSVC caps a single string literal at 16380 bytes)
R"hlsl(
#ifdef POST_BLACK
    // ---- [post] POST_BLACK variant: the dither, re-read from its packed slot
    // (this variant only; pp7.x < 0 here, so the branch above never runs).
    // Mode 0 is the dither above, line for line; modes 1 / 2 are dither_auto.
    [branch] if (pbDq > 0.0005 && pbMd < 0.5) {
        float  sdrD = max(pp2.z, 1e-3);
        float3 base = d / sdrD;
        float3 e    = ToSRGB(base);
        float2 jt   = frac(pp2.y * float2(37.0, 61.0)) * 61.0;
        float3 n;
        n.x = frac(52.9829189 * frac(dot(i.pos.xy + jt,          float2(0.06711056, 0.00583715))));
        n.y = frac(52.9829189 * frac(dot(i.pos.xy + jt + 23.71,  float2(0.06711056, 0.00583715))));
        n.z = frac(52.9829189 * frac(dot(i.pos.xy + jt + 51.37,  float2(0.06711056, 0.00583715))));
        float  lq = dot(e, W);
        float  amp = pbDq * (1.0 / 1023.0) * smoothstep(0.010, 0.040, lq);
        float3 e2 = max(e + (n - 0.5) * amp, 0.0);
        float3 l0 = ToLinear(e), l2 = ToLinear(e2);
        float  g  = clamp(dot(base, W) / max(dot(l0, W), 1e-4), 1.0, 16.0);
        d = max(d + (l2 - l0) * g * sdrD, min(d, 0.0));
    }
    [branch] if (pbDq > 0.0005 && pbMd > 0.5) {
        // ---- dither_auto: the dither sized to the REAL output (2026-11-02) --
        // The user, HDR off: "get rid of this stuff (the color banding that's
        // grey)". With Windows HDR OFF, DWM composes the desktop at EIGHT bits,
        // sRGB-encoded: the half-LSB-of-10-bit above is an eighth of that
        // step, and its fade (full only above code ~10) leaves the dark
        // gradients of a mass -- codes 2..20 -- undithered, so they print as
        // grey contour steps. Mode 1 (HDR off): one LSB = 1/255 of the sRGB
        // code; mode 2 (HDR on): one LSB = 1/1023 of the PQ code the panel is
        // sent. pbDq = the dither key, 1 = TPDF of +-1 LSB, the textbook
        // amount that leaves the quantising error's mean AND variance
        // independent of the signal (no band, no noise pumping).
        // The noise is the same interleaved gradient noise remapped to a
        // TRIANGULAR PDF (it keeps its blue-ish spectrum), a new phase every
        // frame and per channel. Per CHANNEL it fades in from half an LSB to
        // 2.5 LSB of that channel, so a true-black pixel (and the zero channel
        // of a saturated colour) stays exactly 0, and an SDR channel past
        // white (which DWM clips anyway) is left alone.
        bool   pqD  = pbMd > 1.5;
        float  lsb  = pqD ? (1.0 / 1023.0) : (1.0 / 255.0);
        float  sdrD = max(pp2.z, 1e-3);
        float2 jt   = frac(pp2.y * float2(37.0, 61.0)) * 61.0;
        float3 u;
        u.x = frac(52.9829189 * frac(dot(i.pos.xy + jt,          float2(0.06711056, 0.00583715))));
        u.y = frac(52.9829189 * frac(dot(i.pos.xy + jt + 23.71,  float2(0.06711056, 0.00583715))));
        u.z = frac(52.9829189 * frac(dot(i.pos.xy + jt + 51.37,  float2(0.06711056, 0.00583715))));
        float3 n  = lerp(sqrt(2.0 * u) - 1.0, 1.0 - sqrt(2.0 - 2.0 * u), step(0.5, u));   // uniform -> triangular [-1, 1]
        float3 dp = max(d, 0.0);
        // scRGB (1.0 = 80 nits) -> the code the quantiser sees, per channel
        float3 e  = pqD ? PqEnc(dp * (80.0 / 10000.0)) : ToSRGB(dp / sdrD);
        float3 a  = pbDq * lsb * smoothstep(0.5 * lsb, 2.5 * lsb, e);
        if (!pqD) a *= step(dp / sdrD, 1.0);
        float3 e2 = max(e + n * a, 0.0);
        // the CHANGE goes back in linear (exactly 0 where a = 0)
        float3 dl = pqD ? (PqDec(e2) - PqDec(e)) * (10000.0 / 80.0)
                        : (ToLinear(e2) - ToLinear(e)) * sdrD;
        d = max(d + dl, min(d, 0.0));
    }
#endif
    return float4(d, 1.0);
}
)hlsl";

// The M1 HDR diagnostic gradient, kept behind the --gradient flag.
static const char* kGradientSrc = R"hlsl(
cbuffer Consts : register(b0) {
    float2 res;
    float  time;
    float  hdrOn;
    float  page;    // 0 = M1 gradient; 1..3 = calibration quiz pages
};

float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float3 HueToRGB(float h) {
    float r = abs(h * 6.0 - 3.0) - 1.0;
    float g = 2.0 - abs(h * 6.0 - 2.0);
    float b = 2.0 - abs(h * 6.0 - 4.0);
    return saturate(float3(r, g, b));
}

// Quiz page 1 — black level. Ten vertical strips at near-black gray levels
// (scRGB linear; 1.0 = 80 nits). Bottom edge of each strip shows strip-number
// notches. User reports the lowest-numbered strip visible -> shadow_floor.
float4 PageBlackLevel(float2 uv) {
    float lv[10] = { 0.002, 0.004, 0.006, 0.008, 0.010,
                     0.015, 0.020, 0.030, 0.050, 0.080 };
    int i = min(9, (int)(uv.x * 10.0));
    float fx = frac(uv.x * 10.0);
    float v = (fx > 0.04 && fx < 0.96) ? lv[i] : 0.0;
    if (uv.y < 0.04) {   // notch row: strip index+1 white cells out of 10
        int cell = (int)(frac(uv.x * 10.0) * 10.0);
        v = (cell < i + 1) ? 0.5 : 0.0;
    }
    return float4(v, v, v, 1.0);
}

// Quiz page 2 — saturation. Rows = hues, columns = CSS-saturate factor
// applied in gamma space (like the app's post filter), then gamma-decoded.
// User reports the most pleasing column (1-6, left to right).
float4 PageSaturation(float2 uv) {
    float hues[4] = { 0.0, 0.08, 0.33, 0.60 };
    float sats[6] = { 0.8, 1.0, 1.2, 1.4, 1.6, 2.0 };
    int r = min(3, (int)(uv.y * 4.0));
    int c = min(5, (int)(uv.x * 6.0));
    float3 col = HueToRGB(hues[r]) * 0.6;
    float l = dot(col, float3(0.2126, 0.7152, 0.0722));
    col = max(l + (col - l) * sats[c], 0.0);
    if (frac(uv.x * 6.0) < 0.02 || frac(uv.y * 4.0) < 0.04) col = 0.0;
    return float4(pow(col, 2.2), 1.0);
}

// Quiz page 3 — blowout ladder. Horizontal bands of near-white at rising
// nits (scRGB: 1.0 = 80 nits). Band 1 is at the BOTTOM. User reports the
// "almost blown but not painful" band -> peak_nits / max_brightness.
float4 PageBlowout(float2 uv) {
    float nits[10] = { 80, 150, 240, 300, 400, 500, 600, 800, 1000, 1500 };
    int i = min(9, (int)(uv.y * 10.0));
    float v = nits[i] / 80.0;
    if (hdrOn < 0.5) v = min(v, 1.0);
    if (frac(uv.y * 10.0) < 0.03) v = 0.0;
    return float4(v, v * 0.98, v * 0.95, 1.0);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target {
    float2 uv = pos.xy / res;
    int p = (int)(page + 0.5);
    if (p == 1) return PageBlackLevel(uv);
    if (p == 2) return PageSaturation(uv);
    if (p == 3) return PageBlowout(uv);
    if (uv.y > 0.88) {
        float levels[5] = { 1.0, 2.0, 4.0, 8.0, 12.5 };
        int i = min(4, (int)(uv.x * 5.0));
        float fx = frac(uv.x * 5.0);
        float v = (fx > 0.02 && fx < 0.98) ? levels[i] : 0.0;
        if (hdrOn < 0.5) v /= 12.5;
        return float4(v, v, v, 1.0);
    }
    float y = uv.y / 0.88;
    float hue = frac(uv.x + time * 0.03);
    float3 c = HueToRGB(hue);
    float ramp = lerp(4.0, 0.02, pow(y, 1.5));
    float band = 0.5 + 0.5 * sin(6.28318 * (y * 2.0 - time * 0.25));
    float brightness = ramp * (0.6 + 0.8 * band * band);
    if (hdrOn < 0.5) brightness /= 5.6;
    return float4(c * brightness, 1.0);
}
)hlsl";

// ===========================================================================
// brief BY PHOTO-STAGE: the photo stage's display pass -- a FOURTH PSO in its
// own literal (pre-flight 9), so kDisplaySrc and the three look PSOs built
// from it are untouched (dxbc-cmp IDENTICAL by construction). Bound with the
// graphics root signature as it is: b0 = param 0's 32 root constants, t0 =
// the photo's SRV through param 1's table, s0 = the static linear clamp
// sampler; params 2-8 are left unbound (pre-flight 10).
// No gamut matrix (pre-flight 14): scRGB is linear BT.709, the sRGB primaries,
// so a JPEG's colours are already in the output space; the display pass's
// matrices EXPAND colour on purpose and would push sRGB red out of gamut.
// HDR off is this same shader with sdrScale 1 (pre-flight 15): SDR white of
// the file lands exactly on SDR white, never above it (no peak gain).
// Output = SRGBToLinear(texel) * alpha * sdrScale*fade inside the rect, and
// exactly 0 outside it (letterbox / pillarbox).
// ===========================================================================
static const char* kPhotoSrc = R"hlsl(
cbuffer PhotoCB : register(b0) {
    float4 ph0;   // x rect x0 (px)  y rect y0 (px)  z 1/rect width  w 1/rect height
    float4 ph1;   // x sdrScale*fade  y 1 = composite the file's alpha over black
                  // z 1 = the rect is the texture 1:1 (Load, no filtering)  w unused
};
Texture2D<float4> Photo : register(t0);
SamplerState Samp : register(s0);

float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// the piecewise sRGB decode, as kDisplaySrc's SRGBToLinear
float3 SRGBToLinear(float3 c) {
    float3 lo = c / 12.92;
    float3 hi = pow((c + 0.055) / 1.055, 2.4);
    return lerp(lo, hi, step(0.04045, c));
}

float4 PSMain(float4 pos : SV_Position) : SV_Target {
    float2 uv = (pos.xy - ph0.xy) * ph0.zw;
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x >= 1.0 || uv.y >= 1.0) return float4(0.0, 0.0, 0.0, 1.0);
    float4 t;
    if (ph1.z > 0.5) t = Photo.Load(int3(int2(pos.xy - ph0.xy), 0));
    else             t = Photo.SampleLevel(Samp, uv, 0.0);
    float a = ph1.y > 0.5 ? saturate(t.a) : 1.0;
    return float4(SRGBToLinear(saturate(t.rgb)) * (a * ph1.x), 1.0);
}
)hlsl";
