// Project-authored SDR screen-space supplements; no copied game/third-party shader.
// The native controller owns activation and the shared Original/Upscaled toggle.
uniform bool TQDepthReady < source = "bufready_depth"; > = false;
uniform bool TQDepthFlipY < ui_category = "Depth setup"; ui_label = "Flip depth vertically"; > = false;
uniform bool TQDepthReverse < ui_category = "Depth setup"; ui_label = "Reverse depth"; > = false;
uniform float TQDepthNear < ui_category = "Depth setup"; ui_label = "Approximate perspective near plane"; ui_min = 0.001; ui_max = 1000.0; > = 0.1;
uniform float TQDepthFar < ui_category = "Depth setup"; ui_label = "Approximate perspective far plane"; ui_min = 1.0; ui_max = 1000000.0; > = 1000.0;
uniform float TQDepthFOV < ui_category = "Depth setup"; ui_label = "Approximate vertical FOV"; ui_min = 20.0; ui_max = 150.0; > = 75.0;
uniform float TQDepthPixelRadius < ui_category = "Depth setup"; ui_label = "Maximum probe radius (pixels)"; ui_min = 4.0; ui_max = 96.0; > = 48.0;
uniform float TQAOStrength < ui_category = "Ambient occlusion"; ui_type = "slider"; ui_label = "AO strength"; ui_min = 0.0; ui_max = 2.0; > = 1.0;
uniform float TQAORadius < ui_category = "Ambient occlusion"; ui_label = "AO radius"; ui_min = 0.05; ui_max = 5.0; > = 1.0;
uniform float TQAOBias < ui_category = "Ambient occlusion"; ui_label = "AO surface bias"; ui_min = 0.0; ui_max = 0.3; > = 0.04;
uniform int TQAOQuality < ui_category = "Ambient occlusion"; ui_type = "combo"; ui_label = "AO quality"; ui_items = "Fast (4 probes)\0Balanced (8)\0High (16)\0"; > = 1;
uniform float TQContactStrength < ui_category = "Contact shadows"; ui_type = "slider"; ui_label = "Contact shadow strength"; ui_min = 0.0; ui_max = 0.85; > = 0.4;
uniform float TQContactLength < ui_category = "Contact shadows"; ui_label = "Screen-space ray length"; ui_min = 0.05; ui_max = 5.0; > = 1.5;
uniform float TQContactThickness < ui_category = "Contact shadows"; ui_label = "Occluder thickness"; ui_min = 0.01; ui_max = 2.0; > = 0.3;
uniform float TQContactAngle < ui_category = "Contact shadows"; ui_label = "Approximate light angle"; ui_min = 0.0; ui_max = 360.0; > = 225.0;
uniform float TQContactElevation < ui_category = "Contact shadows"; ui_label = "Light direction toward camera"; ui_min = 0.05; ui_max = 2.0; > = 0.6;
uniform int TQContactQuality < ui_category = "Contact shadows"; ui_type = "combo"; ui_label = "Contact quality"; ui_items = "Fast (6 steps)\0Balanced (10)\0High (16)\0"; > = 1;
uniform int TQDepthView < ui_category = "Depth diagnostic"; ui_type = "combo"; ui_label = "Depth view"; ui_items = "Raw depth\0Scene / raw split\0Depth edges\0"; > = 2;
uniform float TQDepthScale < ui_category = "Depth diagnostic"; ui_label = "Raw depth contrast scale"; ui_min = 0.0; ui_max = 65536.0; > = 1.0;
uniform float TQDepthOffset < ui_category = "Depth diagnostic"; ui_label = "Raw depth contrast offset"; ui_min = -65536.0; ui_max = 65536.0; > = 0.0;

texture2D TQScene : COLOR;
texture2D TQDiagnosticDepth : DEPTH;
sampler2D TQScenePoint { Texture = TQScene; AddressU = CLAMP; AddressV = CLAMP; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; SRGBTexture = false; };
sampler2D TQDepthSampler { Texture = TQDiagnosticDepth; AddressU = CLAMP; AddressV = CLAMP; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; SRGBTexture = false; };
texture2D TQAOField { Width = (BUFFER_WIDTH + 1) / 2; Height = (BUFFER_HEIGHT + 1) / 2; Format = R16F; };
texture2D TQContactField { Width = (BUFFER_WIDTH + 1) / 2; Height = (BUFFER_HEIGHT + 1) / 2; Format = R16F; };
sampler2D TQAOSampler { Texture = TQAOField; AddressU = CLAMP; AddressV = CLAMP; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler2D TQContactSampler { Texture = TQContactField; AddressU = CLAMP; AddressV = CLAMP; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
static const float2 TQPixel = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);
static const float2 TQHalfSize = float2((BUFFER_WIDTH + 1) / 2, (BUFFER_HEIGHT + 1) / 2);
static const float2 TQHalfPixel = 1.0 / TQHalfSize;
static const float TQAspect = float(BUFFER_WIDTH) / float(BUFFER_HEIGHT);
void TQFullscreenVS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD0)
{ uv = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0); position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0); }
// Screen and half-resolution fields are level-zero buffers. Explicit LOD avoids
// implicit derivatives inside bounded, depth-dependent probe loops.
float4 TQSample0(sampler2D field, float2 uv) { return tex2Dlod(field, float4(uv, 0.0, 0.0)); }
bool TQUnitColor(float3 rgb) { return all(rgb >= 0.0) && all(rgb <= 1.0); }
bool TQSDR(float3 rgb) {
#if BUFFER_COLOR_SPACE >= 2
    return false;
#else
    return TQUnitColor(rgb);
#endif
}
float3 TQLinear(float3 rgb) { return pow(saturate(rgb), 2.2); }
float3 TQEncoded(float3 rgb) { return pow(saturate(rgb), 1.0 / 2.2); }
float TQLuma(float3 rgb) { return dot(rgb, float3(0.2126, 0.7152, 0.0722)); }
bool TQCameraValid() { return TQDepthNear >= 0.001 && TQDepthNear <= 1000.0 && TQDepthFar > TQDepthNear && TQDepthFar <= 1000000.0 && TQDepthFOV >= 20.0 && TQDepthFOV <= 150.0 && TQDepthPixelRadius >= 4.0 && TQDepthPixelRadius <= 96.0; }
bool TQReadRawDepth(float2 uv, out float raw) {
    raw = 0.0;
    if (!TQDepthReady || !all(uv >= 0.0) || !all(uv <= 1.0)) return false;
    raw = TQSample0(TQDepthSampler, float2(uv.x, TQDepthFlipY ? 1.0 - uv.y : uv.y)).r;
    if (!(raw >= 0.0 && raw <= 1.0)) return false;
    raw = TQDepthReverse ? 1.0 - raw : raw; return true;
}
bool TQReadPosition(float2 uv, out float3 viewPosition) {
    viewPosition = 0.0;
    if (!TQCameraValid() || !TQDepthReady || !all(uv >= 0.0) || !all(uv <= 1.0)) return false;
    // Snap before sampling: a fractional UV on a depth-texel boundary may be
    // rounded differently by hardware point filtering and shader floor().
    // Read depth and reconstruct position from the same unambiguous centre.
    float2 depthSize = float2(tex2Dsize(TQDepthSampler, 0));
    if (!all(depthSize >= 1.0)) return false;
    float2 sampledUV = float2(uv.x, TQDepthFlipY ? 1.0 - uv.y : uv.y);
    sampledUV = (min(floor(sampledUV * depthSize), depthSize - 1.0) + 0.5) / depthSize;
    uv = float2(sampledUV.x, TQDepthFlipY ? 1.0 - sampledUV.y : sampledUV.y);
    float raw;
    if (!TQReadRawDepth(uv, raw) || !(raw > 0.000001 && raw < 0.999999)) return false;
    float z = TQDepthNear * TQDepthFar / (TQDepthFar - raw * (TQDepthFar - TQDepthNear));
    float tangent = tan(TQDepthFOV * 0.00872664626);
    viewPosition = float3((uv.x * 2.0 - 1.0) * TQAspect * tangent * z, (1.0 - uv.y * 2.0) * tangent * z, z);
    return all(viewPosition >= -10000000.0) && all(viewPosition <= 10000000.0) && z > 0.0;
}
bool TQGeometry(float2 uv, out float3 p, out float3 n) {
    n = float3(0.0, 0.0, -1.0); if (!TQReadPosition(uv, p)) return false;
    float3 l, r, a, b;
    bool vl = TQReadPosition(uv - float2(TQPixel.x, 0.0), l), vr = TQReadPosition(uv + float2(TQPixel.x, 0.0), r);
    bool va = TQReadPosition(uv - float2(0.0, TQPixel.y), a), vb = TQReadPosition(uv + float2(0.0, TQPixel.y), b);
    if (!(vl || vr) || !(va || vb)) return false;
    float3 dx = vr && (!vl || abs(r.z - p.z) < abs(l.z - p.z)) ? r - p : p - l;
    float3 dy = vb && (!va || abs(b.z - p.z) < abs(a.z - p.z)) ? b - p : p - a;
    float3 value = cross(dx, dy); float magnitude = dot(value, value);
    if (!(magnitude > 0.000000000000000000000001)) return false;
    n = value * rsqrt(magnitude); if (dot(n, p) > 0.0) n = -n; return true;
}
// Convert a conservative raw-depth quantization step to view-space error.
// dz/dd = z*z*(far-near)/(near*far). This is a tolerance, not extra occlusion.
float TQSurfaceError(float z) { return max(z * 0.00035, z * z * (TQDepthFar - TQDepthNear) / max(TQDepthNear * TQDepthFar, 0.000001) * 0.0000001192092896); }
bool TQOcclusionGeometry(float2 uv, out float3 p, out float3 n) {
    n = float3(0.0, 0.0, -1.0); if (!TQReadPosition(uv, p)) return false;
    float3 l, r, a, b; float2 step = TQPixel * 2.0;
    bool vl = TQReadPosition(uv - float2(step.x, 0.0), l), vr = TQReadPosition(uv + float2(step.x, 0.0), r);
    bool va = TQReadPosition(uv - float2(0.0, step.y), a), vb = TQReadPosition(uv + float2(0.0, step.y), b);
    if (!(vl || vr) || !(va || vb)) return false;
    float3 dx = vr && (!vl || abs(r.z - p.z) < abs(l.z - p.z)) ? r - p : p - l;
    float3 dy = vb && (!va || abs(b.z - p.z) < abs(a.z - p.z)) ? b - p : p - a;
    float local = max(p.z * 0.02, TQSurfaceError(p.z) * 4.0);
    if (vl && vr && max(abs(l.z - p.z), abs(r.z - p.z)) < local) dx = r - l;
    if (va && vb && max(abs(a.z - p.z), abs(b.z - p.z)) < local) dy = b - a;
    float3 value = cross(dx, dy); float magnitude = dot(value, value);
    if (!(magnitude > 0.000000000000000000000001)) return false;
    n = value * rsqrt(magnitude); if (dot(n, p) > 0.0) n = -n; return true;
}
// A fixed spatial phase: no frame/time input and no temporal history to smear.
float TQSpatialPhase(float2 uv) {
    uint2 texel = uint2(floor(uv * TQHalfSize));
    uint tile = (texel.x & 3u) + ((texel.y & 3u) << 2u);
    return (float((tile * 5u + 3u) & 15u) + 0.5) / 16.0;
}
float2 TQRotate(float2 direction, float phase) {
    float angle = phase * 6.28318530718, c = cos(angle), s = sin(angle);
    return float2(direction.x * c - direction.y * s, direction.x * s + direction.y * c);
}
int TQProbeCount(int quality) { return quality == 0 ? 4 : quality == 1 ? 8 : 16; }
float2 TQDirection(int index) {
    int i = index % 8;
    if (i == 0) return float2(1.0, 0.0); if (i == 1) return float2(0.0, 1.0);
    if (i == 2) return float2(-1.0, 0.0); if (i == 3) return float2(0.0, -1.0);
    if (i == 4) return float2(0.70710678, 0.70710678); if (i == 5) return float2(-0.70710678, 0.70710678);
    if (i == 6) return float2(-0.70710678, -0.70710678); return float2(0.70710678, -0.70710678);
}
float2 TQProbeRadius(float radius, float z) { float y = radius / max(z * tan(TQDepthFOV * 0.00872664626) * 2.0, 0.00001); return min(float2(y / TQAspect, y), TQPixel * TQDepthPixelRadius); }
bool TQAOValid() { return TQAOStrength >= 0.0 && TQAOStrength <= 2.0 && TQAORadius >= 0.05 && TQAORadius <= 5.0 && TQAOBias >= 0.0 && TQAOBias <= 0.3 && TQAOQuality >= 0 && TQAOQuality <= 2; }
float4 TQAOEvaluatePS(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 p, n; if (!TQAOValid() || TQAOStrength == 0.0 || !TQOcclusionGeometry(uv, p, n)) return float4(1.0, 0.0, 0.0, 0.0);
    float occlusion = 0.0; int count = TQProbeCount(TQAOQuality); float2 radius = TQProbeRadius(TQAORadius, p.z);
    float phase = TQSpatialPhase(uv), surfaceError = TQSurfaceError(p.z);
    if (surfaceError >= TQAORadius * 0.5) return float4(1.0, 0.0, 0.0, 0.0);
    [loop] for (int i = 0; i < 16; ++i) {
        if (i >= count) break; float fraction = count == 16 ? (i < 8 ? 0.35 : 0.8) : 0.55; float3 q;
        fraction *= 0.85 + 0.3 * frac(phase + float(i) * 0.61803398875);
        if (!TQReadPosition(uv + TQRotate(TQDirection(i), phase) * radius * fraction, q)) continue;
        float tolerance = max(surfaceError, TQSurfaceError(q.z));
        if (dot(n, q - p) <= tolerance) continue;
        float3 d = q - (p + n * tolerance); float distance = length(d);
        if (!(distance > 0.00001 && distance < TQAORadius)) continue;
        float falloff = 1.0 - distance / TQAORadius;
        occlusion += max(dot(n, d / distance) - TQAOBias, 0.0) * falloff * falloff;
    }
    return float4(1.0 - min(occlusion * (2.0 * TQAOStrength / float(count)), 0.6), 0.0, 0.0, 0.0);
}
// Depth-aware upsampling of four neighboring half-resolution values.
float4 TQBilateral(sampler2D field, float2 uv, float z, float4 neutral) {
    float2 at = uv * TQHalfSize - 0.5, fraction = frac(at), baseUV = (floor(at) + 0.5) * TQHalfPixel;
    float4 sum = 0.0; float total = 0.0;
    [loop] for (int i = 0; i < 4; ++i) {
        float2 corner = float2(i % 2, i / 2), sampleUV = baseUV + corner * TQHalfPixel; float3 q;
        if (!TQReadPosition(sampleUV, q)) continue;
        float2 w = lerp(1.0 - fraction, fraction, corner);
        float weight = w.x * w.y * saturate(1.0 - abs(q.z - z) / max(0.02, z * 0.02));
        sum += TQSample0(field, sampleUV) * weight; total += weight;
    }
    return total > 0.00001 ? sum / total : neutral;
}
// Resolve only neighboring samples compatible with the receiver's local plane.
// Depth alone can reject alternate rows of a sloped plane; plane distance avoids
// that while a separate range bound keeps foreground/background edges apart.
float TQResolveOcclusion(sampler2D field, float2 uv, float3 p, float3 n) {
    float2 base = floor(uv * TQHalfSize), centre = (base + 0.5) * TQHalfPixel;
    float sum = 0.0, total = 0.0, error = TQSurfaceError(p.z);
    [loop] for (int i = 0; i < 9; ++i) {
        float2 offset = float2(i % 3 - 1, i / 3 - 1), sampleUV = centre + offset * TQHalfPixel; float3 q;
        if (!TQReadPosition(sampleUV, q)) continue;
        float2 spatial = max(1.5 - abs((sampleUV - uv) * TQHalfSize), 0.0);
        float plane = saturate(1.0 - abs(dot(n, q - p)) / max(error * 4.0, p.z * 0.001));
        float range = saturate(1.0 - abs(q.z - p.z) / max(error * 8.0, p.z * 0.08));
        float weight = spatial.x * spatial.y * plane * range;
        sum += TQSample0(field, sampleUV).r * weight; total += weight;
    }
    return total > 0.00001 ? sum / total : 1.0;
}
float4 TQAOCompositePS(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 original = TQSample0(TQScenePoint, uv); float3 p, n;
    if (!TQSDR(original.rgb) || !TQAOValid() || TQAOStrength == 0.0 || !TQOcclusionGeometry(uv, p, n)) return original;
    float amount = saturate(TQResolveOcclusion(TQAOSampler, uv, p, n));
    if (amount >= 0.999999) return original; return float4(TQEncoded(TQLinear(original.rgb) * amount), original.a);
}
bool TQContactValid() { return TQContactStrength >= 0.0 && TQContactStrength <= 0.85 && TQContactLength >= 0.05 && TQContactLength <= 5.0 && TQContactThickness >= 0.01 && TQContactThickness <= 2.0 && TQContactAngle >= 0.0 && TQContactAngle <= 360.0 && TQContactElevation >= 0.05 && TQContactElevation <= 2.0 && TQContactQuality >= 0 && TQContactQuality <= 2; }
float4 TQContactEvaluatePS(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 p, n; if (!TQContactValid() || TQContactStrength == 0.0 || !TQOcclusionGeometry(uv, p, n)) return float4(1.0, 0.0, 0.0, 0.0);
    float angle = TQContactAngle * 0.01745329252;
    float3 light = normalize(float3(cos(angle), sin(angle), -TQContactElevation));
    float bias = max(0.005, TQSurfaceError(p.z) * 2.0), phase = TQSpatialPhase(uv);
    if (dot(n, light) <= 0.02 || bias >= TQContactThickness) return float4(1.0, 0.0, 0.0, 0.0);
    int count = TQContactQuality == 0 ? 6 : TQContactQuality == 1 ? 10 : 16;
    float tangent = tan(TQDepthFOV * 0.00872664626), shadow = 0.0;
    float rayLength = min(TQContactLength, TQDepthPixelRadius * TQPixel.y * p.z * tangent * 2.0);
    [loop] for (int i = 0; i < 16; ++i) {
        if (i >= count) break; float t = (float(i) + 0.25 + phase * 0.75) / float(count); float3 ray = p + n * bias + light * (rayLength * t);
        if (ray.z <= TQDepthNear) break;
        float2 sampleUV = float2(ray.x / (ray.z * tangent * TQAspect) * 0.5 + 0.5, 0.5 - ray.y / (ray.z * tangent) * 0.5);
        if (any(abs(sampleUV - uv) > TQPixel * TQDepthPixelRadius)) break;
        float3 q; if (!TQReadPosition(sampleUV, q)) continue;
        if (dot(n, q - p) <= max(bias, TQSurfaceError(q.z) * 2.0)) continue;
        float gap = ray.z - q.z;
        if (gap > bias && gap < TQContactThickness) { shadow = (1.0 - t * 0.5) * saturate(gap / (bias * 2.0)); break; }
    }
    return float4(1.0 - shadow * TQContactStrength, 0.0, 0.0, 0.0);
}
float4 TQContactCompositePS(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 original = TQSample0(TQScenePoint, uv); float3 p, n;
    if (!TQSDR(original.rgb) || !TQContactValid() || TQContactStrength == 0.0 || !TQOcclusionGeometry(uv, p, n)) return original;
    float amount = saturate(TQResolveOcclusion(TQContactSampler, uv, p, n));
    if (amount >= 0.999999) return original; return float4(TQEncoded(TQLinear(original.rgb) * amount), original.a);
}
float4 TQDepthDiagnosticPS(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 original = TQSample0(TQScenePoint, uv);
    if (!(TQDepthView >= 0 && TQDepthView <= 2 && TQDepthScale >= 0.0 && TQDepthScale <= 65536.0 && TQDepthOffset >= -65536.0 && TQDepthOffset <= 65536.0)) return original;
    if (TQDepthView == 1 && uv.x < 0.5) return original; float raw; if (!TQReadRawDepth(uv, raw)) return original;
    if (TQDepthView < 2) { float value = saturate(raw * TQDepthScale + TQDepthOffset); return float4(value, value, value, original.a); }
    float3 p, l, r, a, b;
    if (!TQReadPosition(uv, p) || !TQReadPosition(uv - float2(TQPixel.x, 0.0), l) || !TQReadPosition(uv + float2(TQPixel.x, 0.0), r) || !TQReadPosition(uv - float2(0.0, TQPixel.y), a) || !TQReadPosition(uv + float2(0.0, TQPixel.y), b)) return original;
    float edge = max(max(abs(p.z - l.z), abs(p.z - r.z)), max(abs(p.z - a.z), abs(p.z - b.z)));
    float opacity = saturate(edge / max(0.01, p.z * 0.02)) * 0.85;
    return opacity == 0.0 ? original : float4(lerp(original.rgb, float3(0.0, 1.0, 1.0), opacity), original.a);
}
// No enabled/key annotation: controller owns technique state, users own requests.
technique TQAmbientOcclusion {
    pass Evaluate { VertexShader = TQFullscreenVS; PixelShader = TQAOEvaluatePS; RenderTarget = TQAOField; }
    pass Composite { VertexShader = TQFullscreenVS; PixelShader = TQAOCompositePS; BlendEnable = false; SRGBWriteEnable = false; }
}
technique TQContactShadows {
    pass Evaluate { VertexShader = TQFullscreenVS; PixelShader = TQContactEvaluatePS; RenderTarget = TQContactField; }
    pass Composite { VertexShader = TQFullscreenVS; PixelShader = TQContactCompositePS; BlendEnable = false; SRGBWriteEnable = false; }
}
technique TQDepthDiagnostic { pass Diagnostic { VertexShader = TQFullscreenVS; PixelShader = TQDepthDiagnosticPS; BlendEnable = false; SRGBWriteEnable = false; } }
