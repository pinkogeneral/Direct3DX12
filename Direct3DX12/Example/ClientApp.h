#pragma once

#include "../Common/d3dApp.h"
#include "../Common/GeometryGenerator.h"
#include "../Common/Camera.h"
#include "FrameResource.h"
#include "ShadowMap.h"


using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

const int gNumFrameResources = 3;

// ������ �׸��µ� �ʿ��� �Ķ���͵��� ������ ������ ����ü�Դϴ�.
// �� ����ü�� �۸��� ������ �ٸ��� �Դϴ�.
struct RenderItem
{
	RenderItem() = default;

	// ���� �������� ������ ��ġ, ȸ��, �������� ������ ��Ʈ�����Դϴ�.
	XMFLOAT4X4 World = MathHelper::Identity4x4();
	XMFLOAT4X4 TexTransform = MathHelper::Identity4x4();

	// ������Ʈ�� �����Ͱ� ���������� ��Ÿ���� ��Ƽ �÷����Դϴ�.
	// ��Ƽ �÷��װ� Ȱ��ȭ�Ǿ� ������ ��� ���۸� ������Ʈ ����� �մϴ�.
	// �� ������ �ڿ����� ������Ʈ ��� ���۰� �����ϱ� ������
	// ������Ʈ�� �����Ͱ� ��������� NumFramesDirty = gNumFrameResource��
	// �����ؾ��մϴ�. �̷��� �ϹǷν� ��� ������ ���ҽ��� ������Ʈ�� �˴ϴ�.
	int NumFramesDirty = gNumFrameResources;

	// ���� �����ۿ� �ش��ϴ� ��ü ��� ������ �ε��� �Դϴ�.
	UINT ObjCBIndex = -1;

	MeshGeometry* Geo = nullptr;
	Material* Mat = nullptr; 

	// ���� ���������Դϴ�.
	D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

	// DrawIndexedInstance �Ķ���͵� �Դϴ�.
	UINT IndexCount = 0;
	UINT StartIndexLocation = 0;
	int BaseVertexLocation = 0;
};

enum class RenderLayer : int
{
	Opaque = 0,
	opaque_wireframe,
	Transparent,
	AlphaTested,
	Mirrors,
	Reflected,
	Sky,
	Count
};

class ClientMain : public D3DApp
{
public:
	ClientMain(HINSTANCE hInstance);
	ClientMain(const ClientMain& rhs) = delete;
	ClientMain& operator=(const ClientMain& rhs) = delete;
	~ClientMain();

	virtual bool Initialize() override;

private:
	virtual void OnResize()override;
	virtual void Update(const GameTimer& gt) override;
	virtual void Draw(const GameTimer& gt) override;

	virtual void OnMouseDown(WPARAM btnState, int x, int y) override;
	virtual void OnMouseUp(WPARAM btnState, int x, int y) override;
	virtual void OnMouseMove(WPARAM btnState, int x, int y) override;

	void OnKeyboardInput(const GameTimer& gt);
	void UpdateCamera(const GameTimer& gt);
	void AnimateMaterials(const GameTimer& gt);
	void UpdateObjectCBs(const GameTimer& gt);
	void UpdateMaterialCBs(const GameTimer& gt);
	void UpdateMainPassCB(const GameTimer& gt);
	void UpdateReflectedPassCB(const GameTimer& gt);

	void LoadTexture(); 
	void BuildDescriptorHeaps();
	void BuildConstantBufferViews();
	void BuildRootSignature();
	void BuildShadersAndInputLayout();
	void BuildShapeGeometry();
	void BuildPSOs();
	void BuildFrameResources();
	void BuildRenderItems();
	void BuildSkyRenderItems();
	void BuildMaterials();
	void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);
	void DrawSceneToShadowMap();


	// ���� ǥ������� : ���̴����� ���δ�. ���̴����� ǥ������⸦ ����Ϸ��� ǥ������� ��ü�� ���� �����ڸ�
	// ���ϴ� ���̴��� ����� �Ѵ�. 
	std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> GetStaticSamplers();

private:
	std::vector<std::unique_ptr<FrameResource>> mFrameResources;
	FrameResource* mCurrFrameResource = nullptr;
	int mCurrFrameResourceIndex = 0;

	ComPtr<ID3D12RootSignature> mRootSignature = nullptr;
	ComPtr<ID3D12DescriptorHeap> mCbvHeap = nullptr;

	UINT mCbvSrvDescriptorSize = 0;
	ComPtr<ID3D12DescriptorHeap> mSrvDescriptorHeap = nullptr;

	std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
	std::unordered_map<std::string, std::unique_ptr<Material>> mMaterials;
	std::unordered_map<std::string, std::unique_ptr<Texture>> mTextures; 
	std::unordered_map<std::string, ComPtr<ID3DBlob>> mShaders;
	std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> mPSOs;

	std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

	std::unique_ptr<ShadowMap> mShadowMap;

	// ���� ������ ���.
	std::vector<std::unique_ptr<RenderItem>> mAllRitems;

	// PSO�� ���� ������ ���� ������ ���.
	std::vector<RenderItem*> mRenderItems[(int)RenderLayer::Count];

	XMFLOAT3 mReflectTranslation = { 0.0f, 1.0f, -5.0f };

	UINT mSkyTexHeapIndex = 0; //D3D12_DESCRIPTOR_HEAP_DESC ���⼭ ����� �ε��� ..
	UINT mShadowMapHeapIndex = 0;
	UINT mNullCubeSrvIndex = 0; 
	UINT mNullTexSrvIndex = 0;
	
	CD3DX12_GPU_DESCRIPTOR_HANDLE mNullSrv;

	PassConstants mMainPassCB;
	PassConstants mReflectedPassCB;

	UINT mPassCbvOffset = 0;
	RenderItem* mReflectedSkullRitem = nullptr;

	Camera mCamera;

	POINT mLastMousePos;
};