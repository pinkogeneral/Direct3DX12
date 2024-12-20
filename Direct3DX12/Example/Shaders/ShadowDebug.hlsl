#include "common.hlsl"

struct VertexIn
{
	float3 PosL    : POSITION;
	float2 TexC    : TEXCOORD;
};

struct VertexOut
{
	float4 PosH    : SV_POSITION;
	float2 TexC    : TEXCOORD;
};

VertexOut VS(VertexIn vin)
{
	VertexOut vout = (VertexOut)0.0f;

    // Already in homogeneous clip space.
    vout.PosH = float4(vin.PosL, 1.0f);
	
	vout.TexC = vin.TexC;
	
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
	//float shadowValue = gShadowMap.Sample(gsamLinearWrap, pin.TexC).r; 
	//float ssaoValue = gSsaoMap.Sample(gsamLinearWrap, pin.TexC).r; 

	//float combinedValue = shadowValue + ssaoValue; 
    //return float4(combinedValue,combinedValue,combinedValue, 1.0f);
	return float4(gSsaoMap.Sample(gsamLinearWrap, pin.TexC).rrr, 1.0f);
}


