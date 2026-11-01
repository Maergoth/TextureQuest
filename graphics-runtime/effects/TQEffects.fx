// TextureQuest optional effects. Project-authored source; no game shader code.
// TQBloom is a brightness-based glow, not dynamic/emissive scene lighting.
// The add-on owns activation and the shared Original/Upscaled shortcut.

uniform float TQGlowStrength <
    ui_type = "slider";
    ui_label = "Glow strength";
    ui_tooltip = "Subtle glow around bright pixels. Does not add world lights.";
    ui_min = 0.0; ui_max = 0.5; ui_step = 0.01;
> = 0.12;

uniform float TQGlowThreshold <
    ui_type = "slider";
    ui_label = "Glow threshold";
    ui_tooltip = "Linear brightness required for glow. Higher values affect fewer highlights.";
    ui_min = 0.1; ui_max = 1.0; ui_step = 0.01;
> = 0.65;

uniform float TQGlowRadius <
    ui_type = "slider";
    ui_label = "Glow radius";
    ui_tooltip = "Blur spacing in half-resolution pixels.";
    ui_min = 0.5; ui_max = 3.0; ui_step = 0.1;
> = 1.5;

texture2D TQScene : COLOR;
sampler2D TQSceneSampler
{
    Texture = TQScene;
    AddressU = CLAMP; AddressV = CLAMP;
    MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT;
    SRGBTexture = false;
};

// Round odd dimensions up and retain at least one pixel on very small targets.
texture2D TQGlowA
{
    Width = (BUFFER_WIDTH + 1) / 2;
    Height = (BUFFER_HEIGHT + 1) / 2;
    Format = RGBA16F;
};
texture2D TQGlowB
{
    Width = (BUFFER_WIDTH + 1) / 2;
    Height = (BUFFER_HEIGHT + 1) / 2;
    Format = RGBA16F;
};
sampler2D TQGlowASampler
{
    Texture = TQGlowA;
    AddressU = CLAMP; AddressV = CLAMP;
    MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT;
};
sampler2D TQGlowBSampler
{
    Texture = TQGlowB;
    AddressU = CLAMP; AddressV = CLAMP;
    MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT;
};

static const float2 TQPixelSize = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);
static const float2 TQHalfPixelSize = float2(2.0 / (BUFFER_WIDTH + (BUFFER_WIDTH % 2)),
                                          2.0 / (BUFFER_HEIGHT + (BUFFER_HEIGHT % 2)));

void TQFullscreenVS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD0)
{
    // A full-screen triangle with no vertex buffer.
    uv = float2((id == 2) ? 2.0 : 0.0, (id == 1) ? 2.0 : 0.0);
    position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}

float3 TQLinear(float3 encoded)
{
    // This first effect targets SDR. Do not reinterpret an HDR signal here.
    return pow(saturate(encoded), 2.2);
}

float3 TQHighlight(float3 encoded)
{
    float3 linearColor = TQLinear(encoded);
    float brightness = dot(linearColor, float3(0.2126, 0.7152, 0.0722));
    float threshold = clamp(TQGlowThreshold, 0.1, 1.0);
    const float knee = 0.05;
    float soft = clamp(brightness - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float amount = max(brightness - threshold, soft) / max(brightness, 0.00001);
    return linearColor * saturate(amount);
}

float4 TQExtractPS(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float2 offset = TQPixelSize * 0.5;
    float3 glow = TQHighlight(tex2D(TQSceneSampler, uv + float2(-offset.x, -offset.y)).rgb);
    glow += TQHighlight(tex2D(TQSceneSampler, uv + float2(offset.x, -offset.y)).rgb);
    glow += TQHighlight(tex2D(TQSceneSampler, uv + float2(-offset.x, offset.y)).rgb);
    glow += TQHighlight(tex2D(TQSceneSampler, uv + float2(offset.x, offset.y)).rgb);
    return float4(glow * 0.25, 0.0);
}

float4 TQBlurHorizontalPS(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float2 step = float2(TQHalfPixelSize.x * clamp(TQGlowRadius, 0.5, 3.0), 0.0);
    float3 glow = tex2D(TQGlowASampler, uv).rgb * 0.375;
    glow += (tex2D(TQGlowASampler, uv - step).rgb + tex2D(TQGlowASampler, uv + step).rgb) * 0.25;
    glow += (tex2D(TQGlowASampler, uv - 2.0 * step).rgb + tex2D(TQGlowASampler, uv + 2.0 * step).rgb) * 0.0625;
    return float4(glow, 0.0);
}

float4 TQBlurVerticalPS(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float2 step = float2(0.0, TQHalfPixelSize.y * clamp(TQGlowRadius, 0.5, 3.0));
    float3 glow = tex2D(TQGlowBSampler, uv).rgb * 0.375;
    glow += (tex2D(TQGlowBSampler, uv - step).rgb + tex2D(TQGlowBSampler, uv + step).rgb) * 0.25;
    glow += (tex2D(TQGlowBSampler, uv - 2.0 * step).rgb + tex2D(TQGlowBSampler, uv + 2.0 * step).rgb) * 0.0625;
    return float4(glow, 0.0);
}

float4 TQCompositePS(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 original = tex2D(TQSceneSampler, uv);
#if BUFFER_COLOR_SPACE >= 2
    // scRGB / HDR10 need their own transfer function and calibrated glow.
    return original;
#else
    float strength = clamp(TQGlowStrength, 0.0, 0.5);
    if (strength == 0.0 || any(original.rgb < 0.0) || any(original.rgb > 1.0))
        return original;
    float3 glow = max(tex2D(TQGlowASampler, uv).rgb, 0.0) * strength;
    if (all(glow == 0.0))
        return original;
    // A bounded screen blend keeps white highlights white without lowering RGB.
    float3 linearColor = TQLinear(original.rgb);
    float3 combined = 1.0 - (1.0 - linearColor) * exp(-glow);
    return float4(pow(saturate(combined), 1.0 / 2.2), original.a);
#endif
}

// No enabled or keyboard annotation: TextureQuest's controller owns this state.
technique TQBloom
{
    pass Extract
    {
        VertexShader = TQFullscreenVS;
        PixelShader = TQExtractPS;
        RenderTarget = TQGlowA;
    }
    pass BlurHorizontal
    {
        VertexShader = TQFullscreenVS;
        PixelShader = TQBlurHorizontalPS;
        RenderTarget = TQGlowB;
    }
    pass BlurVertical
    {
        VertexShader = TQFullscreenVS;
        PixelShader = TQBlurVerticalPS;
        RenderTarget = TQGlowA;
    }
    pass Composite
    {
        VertexShader = TQFullscreenVS;
        PixelShader = TQCompositePS;
        SRGBWriteEnable = false;
    }
}
