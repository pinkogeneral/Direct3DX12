#include "common.hlsl"

// mul: 행렬 수학을 사용하여 x, y를 곱한다. 내부 차원 x열과 y행은 같아야 한다. 
// pow: 지정된 거듭제곱에 대해 발생하는 지정된 값을 반환한다. 
struct VertexIn
{
	float3 PosL		: POSITION;
    float3 NormalL	: NORMAL;
	float2 TexC		: TEXCOORD;
	float3 TangentU : TANGENT; 
};

struct VertexOut
{
	float4 PosH		: SV_POSITION;
	float4 ShadowPosH : POSITION0;
    float3 PosW		: POSITION1;
    float3 NormalW	: NORMAL;
	float3 TangentW : TANGENT; 
	float2 TexC		: TEXCOORD;
};

VertexOut VS(VertexIn vin)
{
	VertexOut vout = (VertexOut)0.0f;
	
   	MaterialData matData = gMaterialData[gMaterialIndex];
	
    // Transform to world space.
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW = posW.xyz;
	
    // Assumes nonuniform scaling; otherwise, need to use inverse-transpose of world matrix.
    vout.NormalW = mul(vin.NormalL, (float3x3)gWorld);
	
	vout.TangentW = mul(vin.TangentU, (float3x3)gWorld); 
	
    // Transform to homogeneous clip space.
    vout.PosH = mul(posW, gViewProj);
	
	// Output vertex attributes for interpolation across triangle.
	float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
	vout.TexC = mul(texC, matData.MatTransform).xy;

	// Generate projective tex-coords to project shadow map onto scene.
    vout.ShadowPosH = mul(posW, gShadowTransform);

    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
   	// Fetch the material data.
	MaterialData matData = gMaterialData[gMaterialIndex];
	float4 diffuseAlbedo = matData.DiffuseAlbedo;
	float3 fresnelR0 = matData.FresnelR0;
	float  roughness = matData.Roughness;
	uint diffuseTexIndex = matData.DiffuseMapIndex;
	uint normalMapIndex = matData.NormalMapIndex;

	// 법선을 보간하면 단위 길이가 아니게 될 수 있으므로 다시 정규화한다. 
	 pin.NormalW = normalize(pin.NormalW);

	float4 normalMapSample = gDiffuseMap[normalMapIndex].Sample(gsamAnisotropicWrap, pin.TexC);
	float3 bumpedNormalW = NormalSampleToWorldSpace(normalMapSample.rgb, pin.NormalW, pin.TangentW);

	// Dynamically look up the texture in the array.
	diffuseAlbedo *= gDiffuseMap[diffuseTexIndex].Sample(gsamAnisotropicWrap, pin.TexC);
	
#ifdef ALPHA_TEST
	// Discard pixel if texture alpha < 0.1.  We do this test as soon 
	// as possible in the shader so that we can potentially exit the
	// shader early, thereby skipping the rest of the shader code.
	clip(diffuseAlbedo.a - 0.1f);
#endif

   // Interpolating normal can unnormalize it, so renormalize it.
   pin.NormalW = normalize(pin.NormalW);
//
//   // Vector from point being lit to eye. 
	float3 toEyeW = gEyePosW - pin.PosW;
	float distToEye = length(toEyeW);
	toEyeW /= distToEye; // normalize

   // Light terms.
   float4 ambient = gAmbientLight*diffuseAlbedo;

       // Only the first light casts a shadow.
    float3 shadowFactor = float3(1.0f, 1.0f, 1.0f);
    shadowFactor[0] = CalcShadowFactor(pin.ShadowPosH);

   const float shininess = (1.0f - roughness) *  normalMapSample.a;
   Material mat = { diffuseAlbedo, fresnelR0, shininess };
  
   float4 directLight = ComputeLighting(gLights, mat, pin.PosW,
       bumpedNormalW, toEyeW, shadowFactor);

   float4 litColor = ambient + directLight;

#ifdef FOG
	float fogAmount = saturate((distToEye - gFogStart) / gFogRange);
	litColor = lerp(litColor, gFogColor, fogAmount);
#endif

	// 반영 반사항을 추가한다. 
	float3 r = reflect(-toEyeW, bumpedNormalW);
	float4 reflectionColor = gCubeMap.Sample(gsamLinearWrap, r);
	float3 fresnelFactor = SchlickFresnel(fresnelR0, bumpedNormalW, r);
   litColor.rgb += shininess * fresnelFactor * reflectionColor.rgb;

   // Common convention to take alpha from diffuse albedo.
   litColor.a = diffuseAlbedo.a;

   //return directLight;
   //return float4(shadowFactor.x,shadowFactor.y,shadowFactor.z,1 );
  return litColor;
   // return float4(1,1,1,1);
}