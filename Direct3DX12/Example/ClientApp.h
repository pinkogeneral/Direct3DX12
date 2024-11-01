#pragma once

#include "../Common/d3dApp.h"
#include "../Common/GeometryGenerator.h"
#include "FrameResource.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

const int gNumFrameResources = 3;

// 도형을 그리는데 필요한 파라미터들을 저장한 가벼운 구조체입니다.
// 이 구조체는 앱마다 굉장히 다를것 입니다.
struct RenderItem
{
	RenderItem() = default;

	// 월드 공간에서 도형의 위치, 회전, 스케일을 정의한 메트릭스입니다.
	XMFLOAT4X4 World = MathHelper::Identity4x4();
	XMFLOAT4X4 TexTransform = MathHelper::Identity4x4();

	// 오브젝트의 데이터가 변경됬는지를 나타내는 더티 플레그입니다.
	// 더티 플레그가 활성화되어 있으면 상수 버퍼를 업데이트 해줘야 합니다.
	// 매 프레임 자원마다 오브젝트 상수 버퍼가 존재하기 때문에
	// 오브젝트의 데이터가 변경됬을때 NumFramesDirty = gNumFrameResource로
	// 설정해야합니다. 이렇게 하므로써 모든 프레임 리소스가 업데이트가 됩니다.
	int NumFramesDirty = gNumFrameResources;

	// 렌더 아이템에 해당하는 물체 상수 버퍼의 인덱스 입니다.
	UINT ObjCBIndex = -1;

	MeshGeometry* Geo = nullptr;
	Material* Mat = nullptr; 

	// 도형 토폴로지입니다.
	D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

	// DrawIndexedInstance 파라미터들 입니다.
	UINT IndexCount = 0;
	UINT StartIndexLocation = 0;
	int BaseVertexLocation = 0;
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

	void LoadTexture(); 
	void BuildDescriptorHeaps();
	void BuildConstantBufferViews();
	void BuildRootSignature();
	void BuildShadersAndInputLayout();
	void BuildShapeGeometry();
	void BuildPSOs();
	void BuildFrameResources();
	void BuildRenderItems();
	void BuildMaterials();
	void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);

	// 정적 표본추출기 : 셰이더에서 쓰인다. 셰이더에서 표본추출기를 사용하려면 표본추출기 객체에 대한 서술자를
	// 원하는 셰이더에 묶어야 한다. 
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

	// 렌더 아이템 목록.
	std::vector<std::unique_ptr<RenderItem>> mAllRitems;

	// PSO에 의해 나눠진 렌더 아이템 목록.
	std::vector<RenderItem*> mOpaqueRitems;

	PassConstants mMainPassCB;

	UINT mPassCbvOffset = 0;

	bool mIsWireframe = false;

	XMFLOAT3 mEyePos = { 0.0f, 0.0f, 0.0f };
	XMFLOAT4X4 mView = MathHelper::Identity4x4();
	XMFLOAT4X4 mProj = MathHelper::Identity4x4();

	float mTheta = 1.5f * XM_PI;
	float mPhi = 0.2f * XM_PI;
	float mRadius = 15.0f;

	POINT mLastMousePos;
};