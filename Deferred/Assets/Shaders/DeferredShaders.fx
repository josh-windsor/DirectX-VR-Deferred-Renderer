///////////////////////////////////////////////////////////////////////////////
// Simple Mesh Shader
///////////////////////////////////////////////////////////////////////////////


cbuffer PerFrameCB : register(b0)
{
	float4x4 matProjection;
	float4x4 matView;
	float4x4 matViewProjection;
	float4x4 matInverseProjection;
	float4x4 matInverseView;
	float  time;
	//float  padding[3];
};

cbuffer PerDrawCB : register(b1)
{
    matrix matMVP;
};

Texture2D texture0 : register(t0);

SamplerState linearMipSampler : register(s0);


struct VertexInput
{
    float3 pos   : POSITION;
    float4 color : COLOUR;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD;
};

struct VertexOutput
{
    float4 vpos  : SV_POSITION;
    float4 color : COLOUR;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
};

VertexOutput VS_Geometry(VertexInput input)
{
    VertexOutput output;
    output.vpos  = mul(float4(input.pos, 1.0f), matMVP);
    output.color = input.color;
    output.normal = input.normal;
    output.uv = input.uv;

    return output;
}

//geometry rendering
struct GBufferOut {
	float4 vColourSpec : SV_TARGET0;
	float4 vNormalPow : SV_TARGET1;
};

GBufferOut PS_Geometry(VertexOutput input) : SV_TARGET
{
	GBufferOut gbuffer;
	float lightIntensity = dot(normalize(float3(1,1,1)), input.normal);

	// Albido.
 	gbuffer.vColourSpec.rgb = texture0.Sample(linearMipSampler, input.uv).rgb;
	gbuffer.vColourSpec.a = 0.f;

 	gbuffer.vNormalPow.rgb = normalize(input.normal);
	gbuffer.vNormalPow.a = 0.f;

 	return gbuffer;
}

// Gbuffer textures for lighting and Debug passes.
Texture2D gBufferColourSpec : register(t0);
Texture2D gBufferNormalPow : register(t1);
Texture2D gBufferDepth : register(t2);

VertexOutput VS_Passthrough(VertexInput input)
{
    VertexOutput output;
    output.vpos  = float4(input.pos.xyz, 1.0f);
    output.color = input.color;
    output.normal = input.normal.xyz;
    output.uv = input.uv.xy;

    return output;
}

float4 PS_GBufferDebug_Albido(VertexOutput input) : SV_TARGET
{

 	float4 vColourSpec = gBufferColourSpec.Sample(linearMipSampler, input.uv);
 //	float4 vNormalPow = gBufferNormalPow.Sample(linearMipSampler, input.uv);
 //	float fDepth = gBufferDepth.Sample(linearMipSampler, input.uv).r

 	return float4(vColourSpec.xyz, 1.0f);

}

float4 PS_GBufferDebug_Normals(VertexOutput input) : SV_TARGET
{

 //	float4 vColourSpec = gBufferColourSpec.Sample(linearMipSampler, input.uv);
 	float4 vNormalPow = gBufferNormalPow.Sample(linearMipSampler, input.uv);
 //	float fDepth = gBufferDepth.Sample(linearMipSampler, input.uv).r;

 	return float4(vNormalPow.xyz * 0.5f + 0.5f, 1.0f);

}

float4 PS_GBufferDebug_Specular(VertexOutput input) : SV_TARGET
{

 	float4 vColourSpec = gBufferColourSpec.Sample(linearMipSampler, input.uv);
 	float4 vNormalPow = gBufferNormalPow.Sample(linearMipSampler, input.uv);
 //	float fDepth = gBufferDepth.Sample(linearMipSampler, input.uv).r;

 	return float4(vColourSpec.w, vNormalPow.w, 0.0f, 1.0f);

}

float4 PS_GBufferDebug_Position(VertexOutput input) : SV_TARGET
{


 //	float4 vColourSpec = gBufferColourSpec.Sample(linearMipSampler, input.uv);
 //	float4 vNormalPow = gBufferNormalPow.Sample(linearMipSampler, input.uv);
 	float fDepth = gBufferDepth.Sample(linearMipSampler, input.uv).r;

 	// discard fragments we didn't write in the Geometry pass.
 	clip(0.99999f - fDepth);

 	float2 flipUV = input.uv.xy * float2(1,-1) + float2(0,1) ;

 	float4 clipPos = float4(flipUV * 2.0f - 1.0f, fDepth, 1.0f);
 	float4 viewPos = mul(clipPos, matInverseProjection);
 	viewPos /= viewPos.w;
 	float4 worldPos = mul(viewPos, matInverseView);
 	//worldPos /= worldPos.w;

 	return float4(worldPos.xyz, 1.0f);

}

float4 PS_GBufferDebug_Depth(VertexOutput input) : SV_TARGET
{

 //	float4 vColourSpec = gBufferColourSpec.Sample(linearMipSampler, input.uv);
 //	float4 vNormalPow = gBufferNormalPow.Sample(linearMipSampler, input.uv);
 	float fDepth = gBufferDepth.Sample(linearMipSampler, input.uv).r;
 	float fScaledDepth = (fDepth - 0.9) * 4.0f;
 	return float4(fScaledDepth,fScaledDepth,fScaledDepth, 1.0f);

}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
cbuffer LightInfo : register(b2)
{
	float4 vLightPosition; // w == 0 then directional
	float4 vLightDirection; // for directional and spot , w == 0 then point.
	float4 vLightColour; // all types
	float4 vLightAtt; // light attenuation factors spot and point.
	// various spot params... to be added.
};


float4 PS_DirectionalLight(VertexOutput input) : SV_TARGET
{

 	float4 vColourSpec = gBufferColourSpec.Sample(linearMipSampler, input.uv);
 	float4 vNormalPow = gBufferNormalPow.Sample(linearMipSampler, input.uv);
 	//float fDepth = gBufferDepth.Sample(linearMipSampler, input.uv).r;
 	
 	// decode the gbuffer.
 	float3 materialColour = vColourSpec.rgb;
	float3 N = vNormalPow.xyz;


	float kDiffuse = max(dot(vLightDirection.xyz, N),0); 
 	float3 diffuseColour = kDiffuse * materialColour * vLightColour.rgb;

 	return float4(diffuseColour, 1.f);
}

struct LightVolumeVertexOutput
{
    float4 vpos  : SV_POSITION;
    float4 vScreenPos : TEXCOORD0;
};

LightVolumeVertexOutput VS_LightVolume(VertexInput input)
{
    LightVolumeVertexOutput output;
    output.vpos  = mul(float4(input.pos.xyz, 1.0f), matMVP);
    output.vScreenPos = output.vpos;
    return output;
}

float4 PS_PointLight(LightVolumeVertexOutput input) : SV_TARGET
{
	float2 ScreenUV = (input.vScreenPos.xy / input.vScreenPos.w * 0.5 + 0.5) * float2(1, -1) + float2(0, 1);

 	float4 vColourSpec = gBufferColourSpec.Sample(linearMipSampler, ScreenUV);
 	float4 vNormalPow = gBufferNormalPow.Sample(linearMipSampler, ScreenUV);
 	float fDepth = gBufferDepth.Sample(linearMipSampler, ScreenUV).r;
 	 
 	// discard fragments we didn't write in the Geometry pass.
 	clip(0.99999f - fDepth);

 	// decode the gbuffer.
 	float3 materialColour = vColourSpec.xyz;
	float3 N = vNormalPow.xyz;

	// Decode world position for uv
 	float4 clipPos = float4(input.vScreenPos.xy / input.vScreenPos.w, fDepth, 1.0f);
 	float4 viewPos = mul(clipPos, matInverseProjection);
 	viewPos /= viewPos.w;
 	float4 worldPos = mul(viewPos, matInverseView);

 	// obtain vector to point light
 	float3 vToLight = vLightPosition.xyz - worldPos.xyz;
 	float3 lightDir = normalize(vToLight);
 	float lightDistance = length(vToLight);

 	//Compute light attenuation
	float kAtt = 1.0 / (vLightAtt.x + vLightAtt.y*lightDistance + vLightAtt.z*lightDistance*lightDistance);
	kAtt *= 1.0f - smoothstep(vLightAtt.w - 0.25f, vLightAtt.w, lightDistance);

	// clip outside of volume radius.
	clip(vLightAtt.w - lightDistance);

	float kDiffuse = max(dot(lightDir, N),0) * kAtt; 

 	float3 diffuseColour = kDiffuse * materialColour * vLightColour.rgb;

 	return float4(diffuseColour.xyz, 1.f);

 	//return float4(1,0,0,1);
}

///////////////////////////////////////////////////////////////////////////////

