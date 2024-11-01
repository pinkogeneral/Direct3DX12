#pragma once

#include "../Common/d3dUtil.h"
#include "../Common/MathHelper.h"
#include "../Common/UploadBuffer.h"

struct Vertex
{
	DirectX::XMFLOAT3 Pos;
	DirectX::XMFLOAT3 Normal;
};

struct ObjectConstants
{
	DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4();
	DirectX::XMFLOAT4X4 TexTransform = MathHelper::Identity4x4();
};

struct PassConstants
{
	DirectX::XMFLOAT4X4 View = MathHelper::Identity4x4();
	DirectX::XMFLOAT4X4 InvView = MathHelper::Identity4x4();
	DirectX::XMFLOAT4X4 Proj = MathHelper::Identity4x4();
	DirectX::XMFLOAT4X4 InvProj = MathHelper::Identity4x4();
	DirectX::XMFLOAT4X4 ViewProj = MathHelper::Identity4x4();
	DirectX::XMFLOAT4X4 InvViewProj = MathHelper::Identity4x4();
	DirectX::XMFLOAT3 EyePosW = { 0.0f, 0.0f, 0.0f };
	float cbPerObjectPad1 = 0.0f;
	DirectX::XMFLOAT2 RenderTargetSize = { 0.0f, 0.0f };
	DirectX::XMFLOAT2 InvRenderTargetSize = { 0.0f, 0.0f };
	float NearZ = 0.0f;
	float FarZ = 0.0f;
	float TotalTime = 0.0f;
	float DeltaTime = 0.0f;

	DirectX::XMFLOAT4 AmbientLight = { 0.0f, 0.0f, 0.0f, 1.0f }; 

	Light mLight[MaxLights];
};


// CPU에서 한 프레임을 그리기 위한 커맨드들을 기록하기 위한 리소스들을 저장합니다.
struct FrameResource
{
public:
	FrameResource(ID3D12Device* device, UINT passCount, UINT objectCount, UINT materialCount);
	FrameResource(const FrameResource& rhs) = delete;
	FrameResource& operator=(const FrameResource& rhs) = delete;
	~FrameResource();

	// GPU가 모든 명령들을 처리하기 전까지 명령 할당자를 리셋할 수 없습니다.
	// 그러므로 매 프레임마다 명령 할당자가 필요합니다.
	Microsoft::WRL::ComPtr<ID3D12CommandAllocator> CmdListAlloc;

	// GPU가 모든 명령들을 처리하기 전까지 상수 버퍼를 업데이트 할 수 없습니다.
	// 그러므로 매 프레임마다 상수 버퍼가 필요합니다.
	std::unique_ptr<UploadBuffer<PassConstants>> PassCB = nullptr;
	std::unique_ptr<UploadBuffer<MaterialConstants>> MaterialCB = nullptr;
	std::unique_ptr<UploadBuffer<ObjectConstants>> ObjectCB = nullptr;

	// 펜스 값은 현재 펜스 지점까지의 명령들을 표시합니다.
	// 이 값은 아직 GPU에 의해서 자원들이 사용하는지 검사할 수 있게 해줍니다.
	UINT64 Fence = 0;
};