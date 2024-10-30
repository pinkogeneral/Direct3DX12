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

// CPU���� �� �������� �׸��� ���� Ŀ�ǵ���� ����ϱ� ���� ���ҽ����� �����մϴ�.
struct FrameResource
{
public:
	FrameResource(ID3D12Device* device, UINT passCount, UINT objectCount);
	FrameResource(const FrameResource& rhs) = delete;
	FrameResource& operator=(const FrameResource& rhs) = delete;
	~FrameResource();

	// GPU�� ��� ���ɵ��� ó���ϱ� ������ ���� �Ҵ��ڸ� ������ �� �����ϴ�.
	// �׷��Ƿ� �� �����Ӹ��� ���� �Ҵ��ڰ� �ʿ��մϴ�.
	Microsoft::WRL::ComPtr<ID3D12CommandAllocator> CmdListAlloc;

	// GPU�� ��� ���ɵ��� ó���ϱ� ������ ��� ���۸� ������Ʈ �� �� �����ϴ�.
	// �׷��Ƿ� �� �����Ӹ��� ��� ���۰� �ʿ��մϴ�.
	std::unique_ptr<UploadBuffer<PassConstants>> PassCB = nullptr;
	std::unique_ptr<UploadBuffer<MaterialConstants>> MaterialCB = nullptr;
	std::unique_ptr<UploadBuffer<ObjectConstants>> ObjectCB = nullptr;

	// �潺 ���� ���� �潺 ���������� ���ɵ��� ǥ���մϴ�.
	// �� ���� ���� GPU�� ���ؼ� �ڿ����� ����ϴ��� �˻��� �� �ְ� ���ݴϴ�.
	UINT64 Fence = 0;
};