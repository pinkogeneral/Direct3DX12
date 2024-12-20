//***************************************************************************************
// ShapesApp.cpp by Frank Luna (C) 2015 All Rights Reserved.
//
// Shows how to draw a box in Direct3D 12.
//
// Controls:
//   Hold the left mouse button down and move the mouse to rotate.
//   Hold the right mouse button down and move the mouse to zoom in and out.
//***************************************************************************************

#include "ClientApp.h"

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance,
                   PSTR cmdLine, int showCmd)
{
    // Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try
    {
        ClientMain theApp(hInstance);
        if (!theApp.Initialize())
            return 0;

        return theApp.Run();
    }
    catch (DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}

ClientMain::ClientMain(HINSTANCE hInstance)
    : D3DApp(hInstance)
{
	mSceneBounds.Center = XMFLOAT3(0.0f, 0.0f, 0.0f);
	mSceneBounds.Radius = sqrtf(10.0f * 10.0f + 15.0f * 15.0f);
}

ClientMain::~ClientMain()
{
}

bool ClientMain::Initialize()
{
    if (!D3DApp::Initialize())
        return false;

    // 초기화 명령들을 기록하기 위해 커맨드 리스트를 리셋합니다.
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));
	
    // Get the increment size of a descriptor in this heap type.  This is hardware specific, 
    // so we have to query this information.
	mCbvSrvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	mCamera.SetPosition(0.0f, 2.0f, -15.0f);
    mShadowMap = std::make_unique<ShadowMap>(md3dDevice.Get(), 2048, 2048);
	mSsao = std::make_unique<Ssao>(md3dDevice.Get(), mCommandList.Get(), mClientWidth, mClientHeight);

    LoadTexture();

    BuildRootSignature();
	BuildSsaoRootSignature();

    BuildDescriptorHeaps();

    BuildShadersAndInputLayout();
    BuildShapeGeometry();

    BuildMaterials();
    BuildSkyRenderItems(); 
    BuildRenderItems();

    BuildFrameResources();
    BuildPSOs();

	mSsao->SetPSOs(mPSOs["ssao"].Get(), mPSOs["ssaoBlur"].Get());

    // 초기화 명령들을 실행시킵니다.
    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdLists[] = {mCommandList.Get()};
    mCommandQueue->ExecuteCommandLists(1, cmdLists);

    // 초기화가 종료될 때가지 기다립니다.
    FlushCommandQueue();

    return true;
}

void ClientMain::CreateRtvAndDsvDescriptorHeaps()
{
	// Add +6 RTV for cube render target.
	// Add +1 for screen normal map, +2 for ambient maps.

	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
	rtvHeapDesc.NumDescriptors = SwapChainBufferCount + 3;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
		&rtvHeapDesc, IID_PPV_ARGS(mRtvHeap.GetAddressOf())));

	// Add +1 DSV for shadow map.
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
	dsvHeapDesc.NumDescriptors = 2;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
		&dsvHeapDesc, IID_PPV_ARGS(mDsvHeap.GetAddressOf())));
}

void ClientMain::OnResize()
{
    D3DApp::OnResize();

    mCamera.SetLens(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);

	if (mSsao != nullptr)
	{
		mSsao->OnResize(mClientWidth, mClientHeight);

		// Resources changed, so need to rebuild descriptors.
		mSsao->RebuildDescriptors(mDepthStencilBuffer.Get());
	}
}

void ClientMain::Update(const GameTimer& gt)
{
    OnKeyboardInput(gt);
    UpdateCamera(gt);

    // 다음 프레임 리소스의 자원을 얻기위해 순환합니다.
    mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
    mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

    // 현재 프레임 리소스에 대한 명령들이 GPU에서 처리 되었습니까?
    // 처리되지 않았다면 커맨드들의 펜스 지점까지 GPU가 처리할 때까지 기다려야합니다.
    if (mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
    {
        HANDLE eventHandle = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }

	mLightRotationAngle += 0.1f * gt.DeltaTime();

	XMMATRIX R = XMMatrixRotationY(mLightRotationAngle);
	for (int i = 0; i < 3; ++i)
	{
		XMVECTOR lightDir = XMLoadFloat3(&mBaseLightDirections[i]);
		lightDir = XMVector3TransformNormal(lightDir, R);
		XMStoreFloat3(&mRotatedLightDirections[i], lightDir);
	}

    AnimateMaterials(gt);
    UpdateObjectCBs(gt);
    UpdateMaterialCBs(gt);
	UpdateShadowTransform(gt);
    UpdateMainPassCB(gt);
    UpdateReflectedPassCB(gt);
	UpdateShadowPassCB(gt);
	UpdateSsaoCB(gt);

}

void ClientMain::Draw(const GameTimer& gt)
{
    auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

    // 커맨드 기록을 위한 메모리를 재활용 합니다.
    // 제출한 커맨드들이 GPU에서 모두 끝났을때 리셋할 수 있습니다.
    ThrowIfFailed(cmdListAlloc->Reset());

    // ExecuteCommandList를 통해 커맨드 큐에 제출한 다음에 커맨드 리스트를 리셋할 수 있습니다.
    ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mPSOs["opaque"].Get()));

	ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
    mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

	auto matBuffer = mCurrFrameResource->MaterialCB->Resource();
	mCommandList->SetGraphicsRootShaderResourceView(2, matBuffer->GetGPUVirtualAddress());

	// Bind null SRV for shadow map pass.
	mCommandList->SetGraphicsRootDescriptorTable(3, mNullSrv);

	// Bind all the textures used in this scene.  Observe
	// that we only have to specify the first descriptor in the table.  
	// The root signature knows how many descriptors are expected in the table.
	mCommandList->SetGraphicsRootDescriptorTable(4, mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

	DrawSceneToShadowMap();

	DrawNormalsAndDepth();

	mCommandList->SetGraphicsRootSignature(mSsaoRootSignature.Get());
	mSsao->ComputeSsao(mCommandList.Get(), mCurrFrameResource, 3);
	

	mCommandList->SetGraphicsRootSignature(mRootSignature.Get());


	// Bind all the materials used in this scene.  For structured buffers, we can bypass the heap and 
	// set as a root descriptor.

	matBuffer = mCurrFrameResource->MaterialCB->Resource();
	mCommandList->SetGraphicsRootShaderResourceView(2, matBuffer->GetGPUVirtualAddress());


	mCommandList->RSSetViewports(1, &mScreenViewport);
	mCommandList->RSSetScissorRects(1, &mScissorRect);

	// 리소스의 상태를 렌더링을 할 수 있도록 변경합니다.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_PRESENT,
		D3D12_RESOURCE_STATE_RENDER_TARGET));

	// 백 버퍼와 뎁스 버퍼를 클리어 합니다.
	mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::LightSteelBlue, 0, nullptr);
	mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

	// 어디에 렌더링을 할지 설정합니다.
	mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

	auto passCB = mCurrFrameResource->PassCB->Resource();
	mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

	//auto matBuffer = mCurrFrameResource->MaterialCB->Resource();
	//mCommandList->SetGraphicsRootShaderResourceView(2, matBuffer->GetGPUVirtualAddress());

	CD3DX12_GPU_DESCRIPTOR_HANDLE skyTexDescriptor(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	skyTexDescriptor.Offset(mSkyTexHeapIndex, mCbvSrvDescriptorSize);
	mCommandList->SetGraphicsRootDescriptorTable(3, skyTexDescriptor);

	mCommandList->SetGraphicsRootDescriptorTable(4, mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

    UINT passCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));

    // 불투명한 항목 (바닥, 벽, 상자등을 그린다.)
	mCommandList->SetPipelineState(mPSOs["opaque"].Get());
    DrawRenderItems(mCommandList.Get(), mRenderItems[(int)RenderLayer::Opaque]);

	mCommandList->SetPipelineState(mPSOs["sky"].Get());
	DrawRenderItems(mCommandList.Get(), mRenderItems[(int)RenderLayer::Sky]);

	mCommandList->SetPipelineState(mPSOs["debug"].Get());
	DrawRenderItems(mCommandList.Get(), mRenderItems[(int)RenderLayer::Debug]);

    // 가시적 거울 픽셀들을 스텐실 버퍼 1로 표시해 둔다.
    mCommandList->OMSetStencilRef(1);
    mCommandList->SetPipelineState(mPSOs["markStencilMirrors"].Get());
    DrawRenderItems(mCommandList.Get(), mRenderItems[(int)RenderLayer::Mirrors]);

    // 반사상을 거울 영역에만 그린다. (스텐실 버퍼 항목이 1인 픽셀들만 그려지게 한다) 이전과 다른 패스별 살수 버퍼를 지정해야 함을 주목하자.
    // 거울 평면에 대해 반사된 광원 설정을 담은 패스별 상수 버퍼를 지정한다.
    //mCommandList->SetGraphicsRootConstantBufferView(0, passCB->GetGPUVirtualAddress() + 1 * passCBByteSize);
    mCommandList->SetPipelineState(mPSOs["drawStencilReflections"].Get());
    DrawRenderItems(mCommandList.Get(), mRenderItems[(int)RenderLayer::Reflected]);

    // Restore main pass constants and stencil ref.
    mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());
    mCommandList->OMSetStencilRef(0);

    // Draw mirror with transparency so reflection blends through.
    mCommandList->SetPipelineState(mPSOs["transparent"].Get());
    DrawRenderItems(mCommandList.Get(), mRenderItems[(int)RenderLayer::Transparent]);


    mCommandList->SetPipelineState(mPSOs["alphaTested"].Get());
    DrawRenderItems(mCommandList.Get(), mRenderItems[(int)RenderLayer::AlphaTested]);


    // 리소스의 상태를 출력할 수 있도록 변경합니다.
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT));

    // 커맨드 기록을 종료합니다.
    ThrowIfFailed(mCommandList->Close());

    // 커맨드 리스트의 실행을 위해 큐에 제출합니다.
    ID3D12CommandList* cmdLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(1, cmdLists);

    // 백 버퍼와 프론트 버퍼를 교체합니다.
    ThrowIfFailed(mSwapChain->Present(0, 0));
    mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

    // 이 펜스 지점까지 커맨드들을 표시하기 위해 펜스 값을 증가합니다,
    mCurrFrameResource->Fence = ++mCurrentFence;

    // 새 펜스 지점을 설정하는 인스트럭션을 커맨드 큐에 추가합니다.
    // 어플리케이션은 GPU 시간축에 있지 않기 때문에,
    // GPU가 모든 커맨드들의 처리가 완료되기 전까지 Signal()을 처리하지 않습니다.
    mCommandQueue->Signal(mFence.Get(), mCurrentFence);
}

void ClientMain::OnMouseDown(WPARAM btnState, int x, int y)
{
    mLastMousePos.x = x;
    mLastMousePos.y = y;

    SetCapture(mhMainWnd);
}

void ClientMain::OnMouseUp(WPARAM btnState, int x, int y)
{
    ReleaseCapture();
}

void ClientMain::OnMouseMove(WPARAM btnState, int x, int y)
{
    if ((btnState & MK_LBUTTON) != 0)
    {
        // 마우스 한 픽셀의 이동을 0.25도에 대응시킵니다.
        float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
        float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));

		mCamera.Pitch(dy);
		mCamera.RotateY(dx);
    }
    else if ((btnState & MK_RBUTTON) != 0)
    {
        // 마우스 한 픽셀의 이동을 0.005 단위에 대응시킵니다.
        float dx = 0.2f * static_cast<float>(x - mLastMousePos.x);
        float dy = 0.2f * static_cast<float>(y - mLastMousePos.y);
    }

    mLastMousePos.x = x;
    mLastMousePos.y = y;
}

void ClientMain::OnKeyboardInput(const GameTimer& gt)
{
	const float dt = gt.DeltaTime();

	if (GetAsyncKeyState('W') & 0x8000)
		mCamera.Walk(10.0f * dt);

	if (GetAsyncKeyState('S') & 0x8000)
		mCamera.Walk(-10.0f * dt);

	if (GetAsyncKeyState('A') & 0x8000)
		mCamera.Strafe(-10.0f * dt);

	if (GetAsyncKeyState('D') & 0x8000)
		mCamera.Strafe(10.0f * dt);

	mCamera.UpdateViewMatrix();


}

void ClientMain::UpdateCamera(const GameTimer& gt)
{
}

void ClientMain::AnimateMaterials(const GameTimer& gt)
{
}

void ClientMain::UpdateObjectCBs(const GameTimer& gt)
{
    auto currObjectCB = mCurrFrameResource->ObjectCB.get();
    for (auto& e : mAllRitems)
    {
        // 상수들이 바뀌었 때만 상수 버퍼 데이터를 업데이트 합니다.
        // 이것은 매 프레임 자원마다 수행해야 합니다.
        if (e->NumFramesDirty > 0)
        {
            XMMATRIX world = XMLoadFloat4x4(&e->World);
			XMMATRIX texTransform = XMLoadFloat4x4(&e->TexTransform);

            ObjectConstants objConstants;
            XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
			XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));
			objConstants.MaterialIndex = e->Mat->MatCBIndex;
            currObjectCB->CopyData(e->ObjCBIndex, objConstants);

            // 다음 프레임 리소스도 마찬가지로 업데이트 되어야 합니다.
            e->NumFramesDirty--;
        }
    }
}

void ClientMain::UpdateMaterialCBs(const GameTimer& gt)
{
    auto currMaterialCB = mCurrFrameResource->MaterialCB.get();

    for (auto& e : mMaterials)
    {
        Material* mat = e.second.get();
        if (mat->NumFramesDirty > 0)
        {
            XMMATRIX matTransform = XMLoadFloat4x4(&mat->MatTransform); 

			MaterialData matData;
			matData.DiffuseAlbedo = mat->DiffuseAlbedo;
			matData.FresnelR0 = mat->FresnelR0;
			matData.Roughness = mat->Roughness;
			XMStoreFloat4x4(&matData.MatTransform, XMMatrixTranspose(matTransform));
			matData.DiffuseMapIndex = mat->DiffuseSrvHeapIndex;
			matData.NormalMapIndex = mat->NormalSrvHeapIndex;


            currMaterialCB->CopyData(mat->MatCBIndex, matData);

            mat->NumFramesDirty--; 
        }
    }
}

void ClientMain::UpdateShadowTransform(const GameTimer& gt)
{
	// Only the first "main" light casts a shadow.
	XMVECTOR lightDir = XMLoadFloat3(&mRotatedLightDirections[0]);
	XMVECTOR lightPos = -2.0f * mSceneBounds.Radius * lightDir;
	XMVECTOR targetPos = XMLoadFloat3(&mSceneBounds.Center);
	XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
	XMMATRIX lightView = XMMatrixLookAtLH(lightPos, targetPos, lightUp);

	XMStoreFloat3(&mLightPosW, lightPos);

	// Transform bounding sphere to light space.
	XMFLOAT3 sphereCenterLS;
	XMStoreFloat3(&sphereCenterLS, XMVector3TransformCoord(targetPos, lightView));

	// Ortho frustum in light space encloses scene.
	float l = sphereCenterLS.x - mSceneBounds.Radius;
	float b = sphereCenterLS.y - mSceneBounds.Radius;
	float n = sphereCenterLS.z - mSceneBounds.Radius;
	float r = sphereCenterLS.x + mSceneBounds.Radius;
	float t = sphereCenterLS.y + mSceneBounds.Radius;
	float f = sphereCenterLS.z + mSceneBounds.Radius;

	mLightNearZ = n;
	mLightFarZ = f;
	XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(l, r, b, t, n, f);

	// Transform NDC space [-1,+1]^2 to texture space [0,1]^2
	XMMATRIX T(
		0.5f, 0.0f, 0.0f, 0.0f,
		0.0f, -0.5f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.5f, 0.5f, 0.0f, 1.0f);

	XMMATRIX S = lightView * lightProj * T;
	XMStoreFloat4x4(&mLightView, lightView);
	XMStoreFloat4x4(&mLightProj, lightProj);
	XMStoreFloat4x4(&mShadowTransform, S);
}

void ClientMain::UpdateMainPassCB(const GameTimer& gt)
{
    XMMATRIX view = mCamera.GetView();
    XMMATRIX proj = mCamera.GetProj();

    XMMATRIX viewProj = XMMatrixMultiply(view, proj);
    XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
    XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
    XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);
	XMMATRIX shadowTransform = XMLoadFloat4x4(&mShadowTransform);

    XMStoreFloat4x4(&mMainPassCB.View, XMMatrixTranspose(view));
    XMStoreFloat4x4(&mMainPassCB.InvView, XMMatrixTranspose(invView));
    XMStoreFloat4x4(&mMainPassCB.Proj, XMMatrixTranspose(proj));
    XMStoreFloat4x4(&mMainPassCB.InvProj, XMMatrixTranspose(invProj));
    XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(viewProj));
    XMStoreFloat4x4(&mMainPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
	XMStoreFloat4x4(&mMainPassCB.ShadowTransform, XMMatrixTranspose(shadowTransform));


    mMainPassCB.EyePosW = mCamera.GetPosition3f();
    mMainPassCB.RenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
    mMainPassCB.RenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
    mMainPassCB.NearZ = 1.0f;
    mMainPassCB.FarZ = 1000.0f;
    mMainPassCB.TotalTime = gt.TotalTime();
    mMainPassCB.DeltaTime = gt.DeltaTime();
	mMainPassCB.AmbientLight = { 0.25f, 0.25f, 0.35f, 1.0f };
	mMainPassCB.mLight[0].Direction = mRotatedLightDirections[0];
	mMainPassCB.mLight[0].Strength = { 0.9f, 0.8f, 0.7f };
	mMainPassCB.mLight[1].Direction = mRotatedLightDirections[1];
	mMainPassCB.mLight[1].Strength = { 0.45f, 0.45f, 0.45f };
	mMainPassCB.mLight[2].Direction = mRotatedLightDirections[2];
	mMainPassCB.mLight[2].Strength = { 0.35f, 0.35f, 0.15f };


    auto currPassCB = mCurrFrameResource->PassCB.get();
    currPassCB->CopyData(0, mMainPassCB);
}

void ClientMain::UpdateReflectedPassCB(const GameTimer& gt)
{
	mReflectedPassCB = mMainPassCB;

	XMVECTOR mirrorPlane = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f); // xy plane
	XMMATRIX R = XMMatrixReflect(mirrorPlane);

	// Reflect the lighting.
	for (int i = 0; i < 3; ++i)
	{
		XMVECTOR lightDir = XMLoadFloat3(&mMainPassCB.mLight[i].Direction);
		XMVECTOR reflectedLightDir = XMVector3TransformNormal(lightDir, R);
		XMStoreFloat3(&mReflectedPassCB.mLight[i].Direction, reflectedLightDir);
	}

	// Reflected pass stored in index 1
	auto currPassCB = mCurrFrameResource->PassCB.get();
	currPassCB->CopyData(1, mReflectedPassCB);
}

void ClientMain::UpdateShadowPassCB(const GameTimer& gt)
{
	XMMATRIX view = XMLoadFloat4x4(&mLightView);
	XMMATRIX proj = XMLoadFloat4x4(&mLightProj);

	XMMATRIX viewProj = XMMatrixMultiply(view, proj);
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
	XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
	XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

	UINT w = mShadowMap->Width();
	UINT h = mShadowMap->Height();

	XMStoreFloat4x4(&mShadowPassCB.View, XMMatrixTranspose(view));
	XMStoreFloat4x4(&mShadowPassCB.InvView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&mShadowPassCB.Proj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&mShadowPassCB.InvProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&mShadowPassCB.ViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&mShadowPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
	mShadowPassCB.EyePosW = mLightPosW;
	mShadowPassCB.RenderTargetSize = XMFLOAT2((float)w, (float)h);
	mShadowPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / w, 1.0f / h);
	mShadowPassCB.NearZ = mLightNearZ;
	mShadowPassCB.FarZ = mLightFarZ;

	auto currPassCB = mCurrFrameResource->PassCB.get();
	currPassCB->CopyData(1, mShadowPassCB);
}

void ClientMain::UpdateSsaoCB(const GameTimer& gt)
{
	SsaoConstants ssaoCB;

	XMMATRIX P = mCamera.GetProj();

	// Transform NDC space [-1,+1]^2 to texture space [0,1]^2
	XMMATRIX T(
		0.5f, 0.0f, 0.0f, 0.0f,
		0.0f, -0.5f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.5f, 0.5f, 0.0f, 1.0f);

	ssaoCB.Proj = mMainPassCB.Proj;
	ssaoCB.InvProj = mMainPassCB.InvProj;
	XMStoreFloat4x4(&ssaoCB.ProjTex, XMMatrixTranspose(P * T));

	mSsao->GetOffsetVectors(ssaoCB.OffsetVectors);

	auto blurWeights = mSsao->CalcGaussWeights(2.5f);
	ssaoCB.BlurWeights[0] = XMFLOAT4(&blurWeights[0]);
	ssaoCB.BlurWeights[1] = XMFLOAT4(&blurWeights[4]);
	ssaoCB.BlurWeights[2] = XMFLOAT4(&blurWeights[8]);

	ssaoCB.InvRenderTargetSize = XMFLOAT2(1.0f / mSsao->SsaoMapWidth(), 1.0f / mSsao->SsaoMapHeight());

	// Coordinates given in view space.
	ssaoCB.OcclusionRadius = 0.5f;
	ssaoCB.OcclusionFadeStart = 0.2f;
	ssaoCB.OcclusionFadeEnd = 1.0f;
	ssaoCB.SurfaceEpsilon = 0.05f;

	auto currSsaoCB = mCurrFrameResource->SsaoCB.get();
	currSsaoCB->CopyData(0, ssaoCB);
}

void ClientMain::LoadTexture()
{
	std::vector<std::string> texNames =
	{
		"bricks",
        "brick2",
        "tile",
		"stone",
		"checkboard",
		"WireFence",
        "ice",
		"white1x1",
        "bricks_nmap",
        "bricks2_nmap",
        "tile_nmap",
        "default_nmap",
        "skyCubeMap",
	};

	std::vector<std::wstring> texFilenames =
	{
		L"../Textures/bricks.dds",
        L"../Textures/bricks2.dds",
        L"../Textures/tile.dds",
		L"../Textures/stone.dds",
		L"../Textures/checkboard.dds",
		L"../Textures/WireFence.dds", 
        L"../Textures/ice.dds",
		L"../Textures/white1x1.dds",
		L"../Textures/bricks_nmap.dds",
		L"../Textures/bricks2_nmap.dds",
		L"../Textures/tile_nmap.dds",
        L"../Textures/default_nmap.dds",

		L"../Textures/grasscube1024.dds",
	};

	for (int i = 0; i < (int)texNames.size(); ++i)
	{
		auto texMap = std::make_unique<Texture>();
		texMap->Name = texNames[i];
		texMap->Filename = texFilenames[i];
		ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(),
			mCommandList.Get(), texMap->Filename.c_str(),
			texMap->Resource, texMap->UploadHeap));

		mTextures[texMap->Name] = std::move(texMap);
	}
}

void ClientMain::BuildDescriptorHeaps()
{
    // 텍스처 자원을 성공적으로 생성했다면, SRV서술자를 생성해야한다. 
    // 셰이더 프로그램이 사용할 루트 서명 매개변수 슬롯에 설정할 수 있다. 

	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
	srvHeapDesc.NumDescriptors = 20;
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvDescriptorHeap)));

    //D3D12_SHADER_RESOURCE_VIEW_DESC 이구조체는 자원의 용도와 기타 정보 (형식, 차원, 밉맵 개수등)를 서술하는 역할을 한다. 
    // 힙의 시작을 가리키는 포인터를 얻는다. 
	CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> tex2DList =
    {
		mTextures["bricks"]->Resource,          // 0
		mTextures["brick2"]->Resource,          // 1
		mTextures["tile"]->Resource,            // 2
		mTextures["stone"]->Resource,           // 3
		mTextures["checkboard"]->Resource,      // 4
		mTextures["WireFence"]->Resource,       // 5
		mTextures["ice"]->Resource,             // 6
		mTextures["white1x1"]->Resource,        // 7
		mTextures["bricks_nmap"]->Resource,     // 8
		mTextures["bricks2_nmap"]->Resource,    // 9
		mTextures["tile_nmap"]->Resource,       // 10
        mTextures["default_nmap"]->Resource     // 11
    };
    auto skyCubeMap = mTextures["skyCubeMap"]->Resource;

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

	for (UINT i = 0; i < (UINT)tex2DList.size(); ++i)
	{
		srvDesc.Format = tex2DList[i]->GetDesc().Format;
		srvDesc.Texture2D.MipLevels = tex2DList[i]->GetDesc().MipLevels;
		md3dDevice->CreateShaderResourceView(tex2DList[i].Get(), &srvDesc, hDescriptor);

		// next descriptor
		hDescriptor.Offset(1, mCbvSrvDescriptorSize);
	}

	// -- (7)
	// sky : 입방체 맵 텍스처 자원에 대한 SRV ()
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
	srvDesc.TextureCube.MostDetailedMip = 0;
	srvDesc.TextureCube.MipLevels = skyCubeMap->GetDesc().MipLevels;
	srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
	srvDesc.Format = skyCubeMap->GetDesc().Format;
	md3dDevice->CreateShaderResourceView(skyCubeMap.Get(), &srvDesc, hDescriptor);

	mSkyTexHeapIndex = static_cast<UINT>(tex2DList.size());

    // Shader4ComponentMapping : 셰이더에서 텍스처의 표본을 추출하면 지정된 텍스처 좌표에 잇는 텍스처 자료를 담은 벡터가 반환된다.
    // Format : 자원의 형식 DXGI_FORMAT
    // ViewDimension : 자원의 차원 2차원 , 3차원 등등 .. 
    // MostDetailedMip : 이 뷰에 대해 가장 세부적인 밉맵 수준의 인덱스를 지정한다. 
    // ResourceMinLODClamp : 접근 가능한 최소 밉맵 수준을 지정한다. 

    mShadowMapHeapIndex = mSkyTexHeapIndex + 1;
	mSsaoHeapIndexStart = mShadowMapHeapIndex + 1;
	mSsaoAmbientMapIndex = mSsaoHeapIndexStart + 3;
	mNullCubeSrvIndex = mSsaoHeapIndexStart + 5;
	mNullTexSrvIndex1 = mNullCubeSrvIndex + 1;
	mNullTexSrvIndex2 = mNullTexSrvIndex1 + 1;

	auto srvCpuStart = mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
	auto srvGpuStart = mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
	auto dsvCpuStart = mDsvHeap->GetCPUDescriptorHandleForHeapStart();

    //CD3DX12_CPU_DESCRIPTOR_HANDLE: 초기화되지 않은 새 인스턴스를 만든다.
    // _In_ const D3D12_CPU_DESCRIPTOR_HANDLE &other : 첫 핸들
    // INT offsetInDescriptors : 오프셋 인덱스
    // UINT descriptorIncrementSize : 서술자의 바이트 크기

	auto nullSrv = GetCpuSrv(mNullCubeSrvIndex);
	mNullSrv = GetGpuSrv(mNullCubeSrvIndex);

	md3dDevice->CreateShaderResourceView(nullptr, &srvDesc, nullSrv);
	nullSrv.Offset(1, mCbvSrvUavDescriptorSize);

	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
	md3dDevice->CreateShaderResourceView(nullptr, &srvDesc, nullSrv);

	mShadowMap->BuildDescriptors(
		CD3DX12_CPU_DESCRIPTOR_HANDLE(srvCpuStart, mShadowMapHeapIndex, mCbvSrvUavDescriptorSize),
		CD3DX12_GPU_DESCRIPTOR_HANDLE(srvGpuStart, mShadowMapHeapIndex, mCbvSrvUavDescriptorSize),
		CD3DX12_CPU_DESCRIPTOR_HANDLE(dsvCpuStart, 1, mDsvDescriptorSize));

	mSsao->BuildDescriptors(
		mDepthStencilBuffer.Get(),
		GetCpuSrv(mSsaoHeapIndexStart),
		GetGpuSrv(mSsaoHeapIndexStart),
		GetRtv(SwapChainBufferCount),
		mCbvSrvUavDescriptorSize,
		mRtvDescriptorSize);
}

void ClientMain::BuildConstantBufferViews()
{
}

void ClientMain::BuildRootSignature()
{
    // ** CD3DX12_DESCRIPTOR_RANGE ** 
	// - UINT numDescriptors : 범위의 설명자 수 
	// - UINT baseShaderRegister: 범위의 기본 셰이더 레지스터이다. 1인 경우 (t1)
	// - UINT registerSpace = 0
	// - UINT offsetInDescriptorsFromTableStart

	CD3DX12_DESCRIPTOR_RANGE texTable0;
	texTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0); // t0 ~ t1

	CD3DX12_DESCRIPTOR_RANGE texTable1;
	texTable1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 20, 3, 0); // baseShaderRegister: t2

	// Root parameter can be a table, root descriptor or root constants.
	CD3DX12_ROOT_PARAMETER slotRootParameter[5];


    // 퍼포먼스 TIP : 가장 자주 발생하는 것 부터 가장 적게 발생하는 것 순으로 정렬한다.
    // 루트 CBV를 생성합니다.
	slotRootParameter[0].InitAsConstantBufferView(0); // register (b0) --> cbPerObject
	slotRootParameter[1].InitAsConstantBufferView(1); // register (b1) --> CbPass
	slotRootParameter[2].InitAsShaderResourceView(0, 1);
	slotRootParameter[3].InitAsDescriptorTable(1, &texTable0, D3D12_SHADER_VISIBILITY_PIXEL);
	slotRootParameter[4].InitAsDescriptorTable(1, &texTable1, D3D12_SHADER_VISIBILITY_PIXEL);

    auto staticSamplers = GetStaticSamplers(); //(s0 ~ s6)

    // 루트 시그네쳐는 루트 파라미터 배열입니다.

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, slotRootParameter,
        (UINT)staticSamplers.size(), staticSamplers.data(),
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);


    // 한 개의 상수 버퍼로 구성된 디스크립터 레인지를 가르키고 있는
    // 두 개의 슬롯으로 구성되어있는 루트 시그네쳐를 생성합니다.
    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                             serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

    if (errorBlob != nullptr)
    {
        OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    }
    ThrowIfFailed(hr);

    ThrowIfFailed(md3dDevice->CreateRootSignature(
        0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(mRootSignature.GetAddressOf())));
}

void ClientMain::BuildSsaoRootSignature()
{
	CD3DX12_DESCRIPTOR_RANGE texTable0;
	texTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0);

	CD3DX12_DESCRIPTOR_RANGE texTable1;
	texTable1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 0);

	// Root parameter can be a table, root descriptor or root constants.
	CD3DX12_ROOT_PARAMETER slotRootParameter[4];

	// Perfomance TIP: Order from most frequent to least frequent.
	slotRootParameter[0].InitAsConstantBufferView(0);
	slotRootParameter[1].InitAsConstants(1, 1);
	slotRootParameter[2].InitAsDescriptorTable(1, &texTable0, D3D12_SHADER_VISIBILITY_PIXEL);
	slotRootParameter[3].InitAsDescriptorTable(1, &texTable1, D3D12_SHADER_VISIBILITY_PIXEL);

	const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
		0, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
		1, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC depthMapSam(
		2, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressW
		0.0f,
		0,
		D3D12_COMPARISON_FUNC_LESS_EQUAL,
		D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE);

	const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
		3, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	std::array<CD3DX12_STATIC_SAMPLER_DESC, 4> staticSamplers =
	{
		pointClamp, linearClamp, depthMapSam, linearWrap
	};

	// A root signature is an array of root parameters.
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(4, slotRootParameter,
		(UINT)staticSamplers.size(), staticSamplers.data(),
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
	{
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(mSsaoRootSignature.GetAddressOf())));
}

void ClientMain::BuildShadersAndInputLayout()
{
	const D3D_SHADER_MACRO defines[] =
	{
		"FOG", "1",
		NULL, NULL
	};

	const D3D_SHADER_MACRO alphaTestDefines[] =
	{
		"FOG", "1",
		"ALPHA_TEST", "1",
		NULL, NULL
	};

    mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\default.hlsl", nullptr, "VS", "vs_5_1");
    mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\default.hlsl", defines, "PS", "ps_5_1");
	mShaders["alphaTestedPS"] = d3dUtil::CompileShader(L"Shaders\\default.hlsl", alphaTestDefines, "PS", "ps_5_1");

	mShaders["skyVS"] = d3dUtil::CompileShader(L"Shaders\\sky.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["skyPS"] = d3dUtil::CompileShader(L"Shaders\\sky.hlsl", nullptr, "PS", "ps_5_1");

	mShaders["shadowVS"] = d3dUtil::CompileShader(L"Shaders\\Shadows.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["shadowOpaquePS"] = d3dUtil::CompileShader(L"Shaders\\Shadows.hlsl", nullptr, "PS", "ps_5_1");

	mShaders["debugVS"] = d3dUtil::CompileShader(L"Shaders\\ShadowDebug.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["debugPS"] = d3dUtil::CompileShader(L"Shaders\\ShadowDebug.hlsl", nullptr, "PS", "ps_5_1");

	mShaders["drawNormalsVS"] = d3dUtil::CompileShader(L"Shaders\\DrawNormals.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["drawNormalsPS"] = d3dUtil::CompileShader(L"Shaders\\DrawNormals.hlsl", nullptr, "PS", "ps_5_1");

	mShaders["ssaoVS"] = d3dUtil::CompileShader(L"Shaders\\Ssao.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["ssaoPS"] = d3dUtil::CompileShader(L"Shaders\\Ssao.hlsl", nullptr, "PS", "ps_5_1");

	mShaders["ssaoBlurVS"] = d3dUtil::CompileShader(L"Shaders\\SsaoBlur.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["ssaoBlurPS"] = d3dUtil::CompileShader(L"Shaders\\SsaoBlur.hlsl", nullptr, "PS", "ps_5_1");

	mShaders["skyVS"] = d3dUtil::CompileShader(L"Shaders\\Sky.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["skyPS"] = d3dUtil::CompileShader(L"Shaders\\Sky.hlsl", nullptr, "PS", "ps_5_1");


	mShaders["shadowAlphaTestedPS"] = d3dUtil::CompileShader(L"Shaders\\Shadows.hlsl", alphaTestDefines, "PS", "ps_5_1");


    mInputLayout =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
}

void ClientMain::BuildShapeGeometry()
{
    GeometryGenerator geoGen;
    GeometryGenerator::MeshData box = geoGen.CreateBox(1.5f, 1.5f, 1.5f, 3);
    GeometryGenerator::MeshData grid = geoGen.CreateGrid(23.0f, 23.0f, 60, 40);
    GeometryGenerator::MeshData sphere = geoGen.CreateSphere(0.5f, 20, 20);
    GeometryGenerator::MeshData cylinder = geoGen.CreateCylinder(0.5f, 0.3f, 3.0f, 20, 20);
    GeometryGenerator::MeshData wall = geoGen.CreateWall(30.0f, 30.0f);
	GeometryGenerator::MeshData quad = geoGen.CreateQuad(-1.0f, +1.0f, 0.4f, 0.4f, 0.0f); // depth shadow render
	GeometryGenerator::MeshData quad2 = geoGen.CreateQuad(-0.59f, +1.0f, 0.4f, 0.4f, 0.0f); // depth shadow render

    //
    // 모든 지오메트리를 하나의 큰 버텍스/인덱스 버퍼에 연결해서 저장합니다.
    // 그러므로 각각의 서브메쉬가 버퍼에서 차지하는 영역을 정의합니다.
    //

    // 연결된 정점 버퍼에서 각 오브젝트의 버텍스 오프셋들을 캐시합니다.
    UINT boxVertexOffset = 0;
    UINT gridVertexOffset = (UINT)box.Vertices.size();
    UINT sphereVertexOffset = gridVertexOffset + (UINT)grid.Vertices.size();
    UINT cylinderVertexOffset = sphereVertexOffset + (UINT)sphere.Vertices.size();
    UINT wallVertexOffset = cylinderVertexOffset + (UINT)cylinder.Vertices.size();
	UINT quadVertexOffset = wallVertexOffset + (UINT)wall.Vertices.size();
	UINT quad2VertexOffset = quadVertexOffset + (UINT)quad.Vertices.size();


    UINT boxIndexOffset = 0;
    UINT gridIndexOffset = (UINT)box.Indices32.size();
    UINT sphereIndexOffset = gridIndexOffset + (UINT)grid.Indices32.size();
    UINT cylinderIndexOffset = sphereIndexOffset + (UINT)sphere.Indices32.size();
    UINT wallIndexOffset = cylinderIndexOffset + (UINT)cylinder.Indices32.size();;
	UINT quadIndexOffset = wallIndexOffset + (UINT)wall.Indices32.size();
	UINT quad2IndexOffset = quadIndexOffset + (UINT)quad.Indices32.size();

    SubmeshGeometry boxSubmesh;
    boxSubmesh.IndexCount = (UINT)box.Indices32.size();
    boxSubmesh.StartIndexLocation = boxIndexOffset;
    boxSubmesh.BaseVertexLocation = boxVertexOffset;

    SubmeshGeometry gridSubmesh;
    gridSubmesh.IndexCount = (UINT)grid.Indices32.size();
    gridSubmesh.StartIndexLocation = gridIndexOffset;
    gridSubmesh.BaseVertexLocation = gridVertexOffset;

    SubmeshGeometry sphereSubmesh;
    sphereSubmesh.IndexCount = (UINT)sphere.Indices32.size();
    sphereSubmesh.StartIndexLocation = sphereIndexOffset;
    sphereSubmesh.BaseVertexLocation = sphereVertexOffset;

    SubmeshGeometry cylinderSubmesh;
    cylinderSubmesh.IndexCount = (UINT)cylinder.Indices32.size();
    cylinderSubmesh.StartIndexLocation = cylinderIndexOffset;
    cylinderSubmesh.BaseVertexLocation = cylinderVertexOffset;

	SubmeshGeometry WallSubmesh;
    WallSubmesh.IndexCount = (UINT)wall.Indices32.size();
    WallSubmesh.StartIndexLocation = wallIndexOffset;
    WallSubmesh.BaseVertexLocation = wallVertexOffset;

	SubmeshGeometry quadSubmesh;
	quadSubmesh.IndexCount = (UINT)quad.Indices32.size();
	quadSubmesh.StartIndexLocation = quadIndexOffset;
	quadSubmesh.BaseVertexLocation = quadVertexOffset;

	SubmeshGeometry quad2Submesh;
	quad2Submesh.IndexCount = (UINT)quad2.Indices32.size();
	quad2Submesh.StartIndexLocation = quad2IndexOffset;
	quad2Submesh.BaseVertexLocation = quad2VertexOffset;

    auto totalVertexCount =
        box.Vertices.size() +
        grid.Vertices.size() +
        sphere.Vertices.size() +
        cylinder.Vertices.size() + 
        wall.Vertices.size() + 
        quad.Vertices.size()  +
		quad2.Vertices.size();

    std::vector<Vertex> vertices(totalVertexCount);

    UINT k = 0;
    for (size_t i = 0; i < box.Vertices.size(); ++i, ++k)
    {
        vertices[k].Pos = box.Vertices[i].Position;
        vertices[k].Normal = box.Vertices[i].Normal;
        vertices[k].TexC = box.Vertices[i].TexC;
        vertices[k].TangentU = box.Vertices[i].TangentU; 
    }

    for (size_t i = 0; i < grid.Vertices.size(); ++i, ++k)
    {
        vertices[k].Pos = grid.Vertices[i].Position;
        vertices[k].Normal = grid.Vertices[i].Normal;
        vertices[k].TexC = grid.Vertices[i].TexC;
		vertices[k].TangentU = grid.Vertices[i].TangentU;

    }

    for (size_t i = 0; i < sphere.Vertices.size(); ++i, ++k)
    {
        vertices[k].Pos = sphere.Vertices[i].Position;
        vertices[k].Normal = sphere.Vertices[i].Normal;
        vertices[k].TexC = sphere.Vertices[i].TexC;
		vertices[k].TangentU = sphere.Vertices[i].TangentU;
    }

    for (size_t i = 0; i < cylinder.Vertices.size(); ++i, ++k)
    {
        vertices[k].Pos = cylinder.Vertices[i].Position;
        vertices[k].Normal = cylinder.Vertices[i].Normal;
        vertices[k].TexC = cylinder.Vertices[i].TexC;
        vertices[k].TangentU = cylinder.Vertices[i].TangentU;
    }

	for (size_t i = 0; i < wall.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = wall.Vertices[i].Position;
		vertices[k].Normal = wall.Vertices[i].Normal;
		vertices[k].TexC = wall.Vertices[i].TexC;
        vertices[k].TangentU = wall.Vertices[i].TangentU;
	}

	for (int i = 0; i < quad.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = quad.Vertices[i].Position;
		vertices[k].Normal = quad.Vertices[i].Normal;
		vertices[k].TexC = quad.Vertices[i].TexC;
		vertices[k].TangentU = quad.Vertices[i].TangentU;
	}

	for (int i = 0; i < quad2.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = quad2.Vertices[i].Position;
		vertices[k].Normal = quad2.Vertices[i].Normal;
		vertices[k].TexC = quad2.Vertices[i].TexC;
		vertices[k].TangentU = quad2.Vertices[i].TangentU;
	}

    std::vector<std::uint16_t> indices;
    indices.insert(indices.end(), std::begin(box.GetIndices16()), std::end(box.GetIndices16()));
    indices.insert(indices.end(), std::begin(grid.GetIndices16()), std::end(grid.GetIndices16()));
    indices.insert(indices.end(), std::begin(sphere.GetIndices16()), std::end(sphere.GetIndices16()));
    indices.insert(indices.end(), std::begin(cylinder.GetIndices16()), std::end(cylinder.GetIndices16()));
    indices.insert(indices.end(), std::begin(wall.GetIndices16()), std::end(wall.GetIndices16()));
	indices.insert(indices.end(), std::begin(quad.GetIndices16()), std::end(quad.GetIndices16()));
	indices.insert(indices.end(), std::begin(quad2.GetIndices16()), std::end(quad2.GetIndices16()));

    const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
    const UINT ibByteSize = (UINT)indices.size()  * sizeof(std::uint16_t);

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "shapeGeo";

    ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
    CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

    ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
    CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

    geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
                                                        mCommandList.Get(),
                                                        vertices.data(), vbByteSize,
                                                        geo->VertexBufferUploader);

    geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
                                                       mCommandList.Get(),
                                                       indices.data(), ibByteSize,
                                                       geo->IndexBufferUploader);

    geo->VertexByteStride = sizeof(Vertex);
    geo->VertexBufferByteSize = vbByteSize;
    geo->IndexFormat = DXGI_FORMAT_R16_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    geo->DrawArgs["box"] = boxSubmesh;
    geo->DrawArgs["grid"] = gridSubmesh;
    geo->DrawArgs["sphere"] = sphereSubmesh;
    geo->DrawArgs["cylinder"] = cylinderSubmesh;
    geo->DrawArgs["wall"] = WallSubmesh;    
	geo->DrawArgs["quad"] = quadSubmesh;
	geo->DrawArgs["quad2"] = quad2Submesh;
    mGeometries[geo->Name] = std::move(geo);
}

void ClientMain::BuildPSOs()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC basePsoDesc;

    //
    // 불투명 오브젝트를 위한 PSO 생성.
    //
    ZeroMemory(&basePsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
    basePsoDesc.InputLayout = {mInputLayout.data(), (UINT)mInputLayout.size()};
    basePsoDesc.pRootSignature = mRootSignature.Get();
    basePsoDesc.VS =
    {
        reinterpret_cast<BYTE*>(mShaders["standardVS"]->GetBufferPointer()),
        mShaders["standardVS"]->GetBufferSize()
    };
    basePsoDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["opaquePS"]->GetBufferPointer()),
        mShaders["opaquePS"]->GetBufferSize()
    };
    basePsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    basePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    basePsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    basePsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    basePsoDesc.SampleMask = UINT_MAX;
    basePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    basePsoDesc.NumRenderTargets = 1;
    basePsoDesc.RTVFormats[0] = mBackBufferFormat;
    basePsoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
    basePsoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
    basePsoDesc.DSVFormat = mDepthStencilFormat;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&basePsoDesc, IID_PPV_ARGS(&mPSOs["opaque"])));


	D3D12_GRAPHICS_PIPELINE_STATE_DESC transparentPsoDesc = basePsoDesc;

	D3D12_RENDER_TARGET_BLEND_DESC transparencyBlendDesc;
	transparencyBlendDesc.BlendEnable = true;
	transparencyBlendDesc.LogicOpEnable = false;
	transparencyBlendDesc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
	transparencyBlendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	transparencyBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
	transparencyBlendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
	transparencyBlendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
	transparencyBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	transparencyBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
	transparencyBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	transparentPsoDesc.BlendState.RenderTarget[0] = transparencyBlendDesc;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&transparentPsoDesc, IID_PPV_ARGS(&mPSOs["transparent"])));


    // 스텐실 거울 영역 표시용 pso
	CD3DX12_BLEND_DESC mirrorBlendState(D3D12_DEFAULT);
	mirrorBlendState.RenderTarget[0].RenderTargetWriteMask = 0;

	D3D12_DEPTH_STENCIL_DESC mirrorDSS;
	mirrorDSS.DepthEnable = true;
	mirrorDSS.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	mirrorDSS.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
	mirrorDSS.StencilEnable = true;
	mirrorDSS.StencilReadMask = 0xff;
	mirrorDSS.StencilWriteMask = 0xff;

	mirrorDSS.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
	mirrorDSS.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	mirrorDSS.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
	mirrorDSS.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

    // 후면 다각형은 렌더링하지 않으므로 다음 설정들은 중요x
    mirrorDSS.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
	mirrorDSS.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	mirrorDSS.BackFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
	mirrorDSS.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC markMirrorsPsoDesc = basePsoDesc;
	markMirrorsPsoDesc.BlendState = mirrorBlendState;
	markMirrorsPsoDesc.DepthStencilState = mirrorDSS;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&markMirrorsPsoDesc, IID_PPV_ARGS(&mPSOs["markStencilMirrors"])));

	//
	// PSO for stencil reflections.
	//

	D3D12_DEPTH_STENCIL_DESC reflectionsDSS;
	reflectionsDSS.DepthEnable = true;
	reflectionsDSS.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	reflectionsDSS.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
	reflectionsDSS.StencilEnable = true;
	reflectionsDSS.StencilReadMask = 0xff;
	reflectionsDSS.StencilWriteMask = 0xff;

	reflectionsDSS.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
	reflectionsDSS.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	reflectionsDSS.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
	reflectionsDSS.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;

    // 후면 다각형은 렌더링하지 않으므로 다음 설정들은 중요x
	reflectionsDSS.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
	reflectionsDSS.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	reflectionsDSS.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
	reflectionsDSS.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC drawReflectionsPsoDesc = basePsoDesc;
	drawReflectionsPsoDesc.DepthStencilState = reflectionsDSS;
	drawReflectionsPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
	drawReflectionsPsoDesc.RasterizerState.FrontCounterClockwise = true;
    drawReflectionsPsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["alphaTestedPS"]->GetBufferPointer()),
		mShaders["alphaTestedPS"]->GetBufferSize()
	};

	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&drawReflectionsPsoDesc, IID_PPV_ARGS(&mPSOs["drawStencilReflections"])));


	D3D12_GRAPHICS_PIPELINE_STATE_DESC alphaTestedPsoDesc = basePsoDesc;
	alphaTestedPsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["alphaTestedPS"]->GetBufferPointer()),
		mShaders["alphaTestedPS"]->GetBufferSize()
	};
	alphaTestedPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&alphaTestedPsoDesc, IID_PPV_ARGS(&mPSOs["alphaTested"])));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueWireframePsoDesc = basePsoDesc;
    opaqueWireframePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaqueWireframePsoDesc, IID_PPV_ARGS(&mPSOs["opaque_wireframe"])));

    
    // - sky pso 만들기 
    // 개별적인 셰이더 프로그램들이 쓰이므로 따로 만들어준다. 

	D3D12_GRAPHICS_PIPELINE_STATE_DESC skyPsoDesc = basePsoDesc;

    // - CullMode: 카메라가 구의 내부에 있기 때문에 후면 선별을 비활성화해야한다. 
	skyPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    // - DepthFunc: LESS_EQUAL 하늘 구가 깊이 판정을 통과한다. 
	skyPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	skyPsoDesc.pRootSignature = mRootSignature.Get();
	skyPsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["skyVS"]->GetBufferPointer()),
		mShaders["skyVS"]->GetBufferSize()
	};
	skyPsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["skyPS"]->GetBufferPointer()),
		mShaders["skyPS"]->GetBufferSize()
	};
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&skyPsoDesc, IID_PPV_ARGS(&mPSOs["sky"])));


	//
// PSO for shadow map pass.
//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC smapPsoDesc = basePsoDesc;
	smapPsoDesc.RasterizerState.DepthBias = 100000; // 깊이 편향 
	smapPsoDesc.RasterizerState.DepthBiasClamp = 0.0f;
	smapPsoDesc.RasterizerState.SlopeScaledDepthBias = 1.0f;
	smapPsoDesc.pRootSignature = mRootSignature.Get();
	smapPsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["shadowVS"]->GetBufferPointer()),
		mShaders["shadowVS"]->GetBufferSize()
	};
	smapPsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["shadowOpaquePS"]->GetBufferPointer()),
		mShaders["shadowOpaquePS"]->GetBufferSize()
	};

	// Shadow map pass does not have a render target.
	smapPsoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
	smapPsoDesc.NumRenderTargets = 0;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&smapPsoDesc, IID_PPV_ARGS(&mPSOs["shadow_opaque"])));

	//
	// PSO for debug layer.
	//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC debugPsoDesc = basePsoDesc;
	debugPsoDesc.pRootSignature = mRootSignature.Get();
	debugPsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["debugVS"]->GetBufferPointer()),
		mShaders["debugVS"]->GetBufferSize()
	};
	debugPsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["debugPS"]->GetBufferPointer()),
		mShaders["debugPS"]->GetBufferSize()
	};
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&debugPsoDesc, IID_PPV_ARGS(&mPSOs["debug"])));


	//
	// PSO for drawing normals.
	//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC drawNormalsPsoDesc = basePsoDesc;
	drawNormalsPsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["drawNormalsVS"]->GetBufferPointer()),
		mShaders["drawNormalsVS"]->GetBufferSize()
	};
	drawNormalsPsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["drawNormalsPS"]->GetBufferPointer()),
		mShaders["drawNormalsPS"]->GetBufferSize()
	};
	drawNormalsPsoDesc.RTVFormats[0] = Ssao::NormalMapFormat;
	drawNormalsPsoDesc.SampleDesc.Count = 1;
	drawNormalsPsoDesc.SampleDesc.Quality = 0;
	drawNormalsPsoDesc.DSVFormat = mDepthStencilFormat;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&drawNormalsPsoDesc, IID_PPV_ARGS(&mPSOs["drawNormals"])));

	//
	// PSO for SSAO.
	//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC ssaoPsoDesc = basePsoDesc;
	ssaoPsoDesc.InputLayout = { nullptr, 0 };
	ssaoPsoDesc.pRootSignature = mSsaoRootSignature.Get();
	ssaoPsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["ssaoVS"]->GetBufferPointer()),
		mShaders["ssaoVS"]->GetBufferSize()
	};
	ssaoPsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["ssaoPS"]->GetBufferPointer()),
		mShaders["ssaoPS"]->GetBufferSize()
	};

	// SSAO effect does not need the depth buffer.
	ssaoPsoDesc.DepthStencilState.DepthEnable = false;
	ssaoPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	ssaoPsoDesc.RTVFormats[0] = Ssao::AmbientMapFormat;
	ssaoPsoDesc.SampleDesc.Count = 1;
	ssaoPsoDesc.SampleDesc.Quality = 0;
	ssaoPsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&ssaoPsoDesc, IID_PPV_ARGS(&mPSOs["ssao"])));

	//
	// PSO for SSAO blur.
	//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC ssaoBlurPsoDesc = ssaoPsoDesc;
	ssaoBlurPsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["ssaoBlurVS"]->GetBufferPointer()),
		mShaders["ssaoBlurVS"]->GetBufferSize()
	};
	ssaoBlurPsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["ssaoBlurPS"]->GetBufferPointer()),
		mShaders["ssaoBlurPS"]->GetBufferSize()
	};
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&ssaoBlurPsoDesc, IID_PPV_ARGS(&mPSOs["ssaoBlur"])));

}

void ClientMain::BuildFrameResources()
{
    for (int i = 0; i < gNumFrameResources; ++i)
    {
        mFrameResources.push_back(std::make_unique<FrameResource>(
            md3dDevice.Get(), 1, (UINT)mAllRitems.size(), (UINT)mMaterials.size()));
    }
}

void ClientMain::BuildRenderItems()
{
    auto boxRitem = std::make_unique<RenderItem>();
    XMStoreFloat4x4(&boxRitem->World, XMMatrixScaling(2.0f, 2.0f, 2.0f) * XMMatrixTranslation(0.0f, 1.0f, 0.0f));
	XMStoreFloat4x4(&boxRitem->TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
    boxRitem->ObjCBIndex = 0;
    boxRitem->Geo = mGeometries["shapeGeo"].get();
    boxRitem->Mat = mMaterials["WireFence"].get();
    boxRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    boxRitem->IndexCount = (UINT)boxRitem->Geo->DrawArgs["box"].IndexCount;
    boxRitem->StartIndexLocation = boxRitem->Geo->DrawArgs["box"].StartIndexLocation;
    boxRitem->BaseVertexLocation = boxRitem->Geo->DrawArgs["box"].BaseVertexLocation;
    mRenderItems[(int)RenderLayer::AlphaTested].push_back(boxRitem.get()); 

	// Reflected skull will have different world matrix, so it needs to be its own render item.
	auto reflectedboxRitem = std::make_unique<RenderItem>();
	*reflectedboxRitem = *boxRitem;
    reflectedboxRitem->ObjCBIndex = 1;
    mReflectedSkullRitem = reflectedboxRitem.get();

	auto reflectedleftCylRitem = std::make_unique<RenderItem>();
    XMMATRIX skullScale = XMMatrixScaling(2.0f, 2.0f, 2.0f);
	XMMATRIX ReflectRotate = XMMatrixRotationY(0.5f * MathHelper::Pi);
	XMMATRIX ReflectOffset = XMMatrixTranslation(15.0f, boxRitem->World._42, boxRitem->World._43);
	XMMATRIX ReflectWorld = ReflectRotate * skullScale * ReflectOffset;

	XMVECTOR mirrorPlane = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f); // xy plane
	XMMATRIX R = XMMatrixReflect(mirrorPlane);
    
    XMStoreFloat4x4(&reflectedboxRitem->World, ReflectWorld * R);

    mRenderItems[(int)RenderLayer::Reflected].push_back(reflectedboxRitem.get());
    mAllRitems.push_back(std::move(boxRitem));
    mAllRitems.push_back(std::move(reflectedboxRitem));


    auto gridRitem = std::make_unique<RenderItem>();
    gridRitem->World = MathHelper::Identity4x4();
    XMStoreFloat4x4(&gridRitem->World, XMMatrixScaling(1.3f, 1.0f, 1.2f));
    gridRitem->World._41 += 6.f;
	XMStoreFloat4x4(&gridRitem->TexTransform, XMMatrixScaling(8.0f, 8.0f, 1.0f));
    gridRitem->ObjCBIndex = 2;
    gridRitem->Geo = mGeometries["shapeGeo"].get();
    gridRitem->Mat = mMaterials["tile"].get();
    gridRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    gridRitem->IndexCount = gridRitem->Geo->DrawArgs["grid"].IndexCount;
    gridRitem->StartIndexLocation = gridRitem->Geo->DrawArgs["grid"].StartIndexLocation;
    gridRitem->BaseVertexLocation = gridRitem->Geo->DrawArgs["grid"].BaseVertexLocation;
    mRenderItems[(int)RenderLayer::Opaque].push_back(gridRitem.get());
    mAllRitems.push_back(std::move(gridRitem));

	auto wallRitem = std::make_unique<RenderItem>();
    wallRitem->World = MathHelper::Identity4x4();
    XMStoreFloat4x4(&wallRitem->World, XMMatrixTranslation(-0.8f, 0.0f, 0.0f));
    wallRitem->ObjCBIndex = 3;
    wallRitem->Geo = mGeometries["shapeGeo"].get();
    wallRitem->Mat = mMaterials["brick2"].get();
    wallRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    wallRitem->IndexCount = wallRitem->Geo->DrawArgs["wall"].IndexCount;
    wallRitem->StartIndexLocation = wallRitem->Geo->DrawArgs["wall"].StartIndexLocation;
    wallRitem->BaseVertexLocation = wallRitem->Geo->DrawArgs["wall"].BaseVertexLocation;


	auto iceRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&iceRitem->World, XMMatrixScaling(0.4f, 0.4f, 0.4f) + XMMatrixTranslation(+3.1f, 0.0f, 0.0f));
	XMStoreFloat4x4(&iceRitem->TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
	iceRitem->ObjCBIndex = 4;
	iceRitem->Geo = mGeometries["shapeGeo"].get();
	iceRitem->Mat = mMaterials["ice"].get();
	iceRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	iceRitem->IndexCount = iceRitem->Geo->DrawArgs["wall"].IndexCount;
	iceRitem->StartIndexLocation = iceRitem->Geo->DrawArgs["wall"].StartIndexLocation;
	iceRitem->BaseVertexLocation = iceRitem->Geo->DrawArgs["wall"].BaseVertexLocation;
	mRenderItems[(int)RenderLayer::Mirrors].push_back(iceRitem.get());
    mRenderItems[(int)RenderLayer::Transparent].push_back(iceRitem.get());
	mAllRitems.push_back(std::move(iceRitem));

	XMMATRIX brickTexTransform = XMMatrixScaling(1.0f, 1.0f, 1.0f);
    UINT objCBIndex = 5;
    for (int i = 0; i < 5; ++i)
    {
        auto leftCylRitem = std::make_unique<RenderItem>();
        auto rightCylRitem = std::make_unique<RenderItem>();
        auto leftSphereRitem = std::make_unique<RenderItem>();
        auto rightSphereRitem = std::make_unique<RenderItem>();

        XMMATRIX leftCylWorld = XMMatrixTranslation(-5.0f, 1.5f, -10.0f + i * 5.0f);
        XMMATRIX rightCylWorld = XMMatrixTranslation(+5.0f, 1.5f, -10.0f + i * 5.0f);

        XMMATRIX leftSphereWorld = XMMatrixTranslation(-5.0f, 3.5f, -10.0f + i * 5.0f);
        XMMATRIX rightSphereWorld = XMMatrixTranslation(+5.0f, 3.5f, -10.0f + i * 5.0f);

        XMStoreFloat4x4(&leftCylRitem->World, rightCylWorld);
        XMStoreFloat4x4(&leftCylRitem->TexTransform, brickTexTransform);
        leftCylRitem->ObjCBIndex = objCBIndex++;
        leftCylRitem->Geo = mGeometries["shapeGeo"].get();
        leftCylRitem->Mat = mMaterials["bricks"].get();
        leftCylRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        leftCylRitem->IndexCount = leftCylRitem->Geo->DrawArgs["cylinder"].IndexCount;
        leftCylRitem->StartIndexLocation = leftCylRitem->Geo->DrawArgs["cylinder"].StartIndexLocation;
        leftCylRitem->BaseVertexLocation = leftCylRitem->Geo->DrawArgs["cylinder"].BaseVertexLocation;
        mRenderItems[(int)RenderLayer::Opaque].push_back(leftCylRitem.get());
		
        auto reflectedleftCylRitem = std::make_unique<RenderItem>();
		XMMATRIX ReflectRotate = XMMatrixRotationY(0.5f * MathHelper::Pi);
		XMMATRIX ReflectOffset = XMMatrixTranslation(10.0f , leftCylRitem->World._42 , leftCylRitem->World._43);
		XMMATRIX ReflectWorld = ReflectRotate * ReflectOffset;
		XMVECTOR mirrorPlane = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f); // xy plane
		XMMATRIX R = XMMatrixReflect(mirrorPlane);

		*reflectedleftCylRitem = *leftCylRitem;
        reflectedleftCylRitem->ObjCBIndex = objCBIndex++;
        reflectedleftCylRitem->Mat = mMaterials["bricks"].get();
		//XMStoreFloat4x4(&reflectedleftCylRitem->World, ReflectWorld);
        XMStoreFloat4x4(&reflectedleftCylRitem->World, ReflectWorld * R);
        mRenderItems[(int)RenderLayer::Reflected].push_back(reflectedleftCylRitem.get());

        //=============================================================================================================//

        XMStoreFloat4x4(&rightCylRitem->World, leftCylWorld);
		XMStoreFloat4x4(&rightCylRitem->TexTransform, brickTexTransform);
        rightCylRitem->ObjCBIndex = objCBIndex++;
        rightCylRitem->Geo = mGeometries["shapeGeo"].get();
        rightCylRitem->Mat = mMaterials["bricks"].get();
        rightCylRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        rightCylRitem->IndexCount = rightCylRitem->Geo->DrawArgs["cylinder"].IndexCount;
        rightCylRitem->StartIndexLocation = rightCylRitem->Geo->DrawArgs["cylinder"].StartIndexLocation;
        rightCylRitem->BaseVertexLocation = rightCylRitem->Geo->DrawArgs["cylinder"].BaseVertexLocation;
        mRenderItems[(int)RenderLayer::Opaque].push_back(rightCylRitem.get());

		auto reflectedrightCylRitem = std::make_unique<RenderItem>();
		*reflectedrightCylRitem = *rightCylRitem;
		reflectedrightCylRitem->ObjCBIndex = objCBIndex++;
		reflectedrightCylRitem->Mat = mMaterials["bricks"].get();

		ReflectOffset = XMMatrixTranslation(20.0f, rightCylRitem->World._42, rightCylRitem->World._43 );
		ReflectWorld = ReflectRotate * ReflectOffset;
		XMStoreFloat4x4(&reflectedrightCylRitem->World, ReflectWorld * R);
		mRenderItems[(int)RenderLayer::Reflected].push_back(reflectedrightCylRitem.get());

		//=============================================================================================================//

		XMStoreFloat4x4(&leftSphereRitem->World, leftSphereWorld);
		XMStoreFloat4x4(&leftSphereRitem->TexTransform, brickTexTransform);
		leftSphereRitem->ObjCBIndex = objCBIndex++;
		leftSphereRitem->Geo = mGeometries["shapeGeo"].get();
		leftSphereRitem->Mat = mMaterials["white1x1"].get();
		leftSphereRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		leftSphereRitem->IndexCount = leftSphereRitem->Geo->DrawArgs["sphere"].IndexCount;
		leftSphereRitem->StartIndexLocation = leftSphereRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
		leftSphereRitem->BaseVertexLocation = leftSphereRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;
		mRenderItems[(int)RenderLayer::Opaque].push_back(leftSphereRitem.get());

		auto reflectedleftSphereRitem = std::make_unique<RenderItem>();
		*reflectedleftSphereRitem = *leftSphereRitem;
		reflectedleftSphereRitem->ObjCBIndex = objCBIndex++;
		reflectedleftSphereRitem->Mat = mMaterials["white1x1"].get();

		ReflectOffset = XMMatrixTranslation(10.0f, leftSphereRitem->World._42, leftSphereRitem->World._43);
		ReflectWorld = ReflectRotate * ReflectOffset;
		XMStoreFloat4x4(&reflectedleftSphereRitem->World, ReflectWorld);
		mRenderItems[(int)RenderLayer::Reflected].push_back(reflectedleftSphereRitem.get());

		XMStoreFloat4x4(&rightSphereRitem->World, rightSphereWorld);
		XMStoreFloat4x4(&rightSphereRitem->World, rightSphereWorld);
		rightSphereRitem->ObjCBIndex = objCBIndex++;
		rightSphereRitem->Geo = mGeometries["shapeGeo"].get();
		rightSphereRitem->Mat = mMaterials["white1x1"].get();
		rightSphereRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		rightSphereRitem->IndexCount = rightSphereRitem->Geo->DrawArgs["sphere"].IndexCount;
		rightSphereRitem->StartIndexLocation = rightSphereRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
		rightSphereRitem->BaseVertexLocation = rightSphereRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;
		mRenderItems[(int)RenderLayer::Opaque].push_back(rightSphereRitem.get());

		auto reflectedrightSphereRitem = std::make_unique<RenderItem>();
		*reflectedrightSphereRitem = *rightSphereRitem;
		reflectedrightSphereRitem->ObjCBIndex = objCBIndex++;
		reflectedrightSphereRitem->Mat = mMaterials["white1x1"].get();

		ReflectOffset = XMMatrixTranslation(20.0f, rightSphereRitem->World._42,  rightSphereRitem->World._43 );
		ReflectWorld = ReflectRotate * ReflectOffset;
		XMStoreFloat4x4(&reflectedrightSphereRitem->World, ReflectWorld);
		mRenderItems[(int)RenderLayer::Reflected].push_back(reflectedrightSphereRitem.get());

        mAllRitems.push_back(std::move(leftCylRitem));
        mAllRitems.push_back(std::move(rightCylRitem));
		mAllRitems.push_back(std::move(leftSphereRitem));
		mAllRitems.push_back(std::move(rightSphereRitem));
		mAllRitems.push_back(std::move(reflectedleftCylRitem));
		mAllRitems.push_back(std::move(reflectedrightCylRitem));
		mAllRitems.push_back(std::move(reflectedleftSphereRitem));
		mAllRitems.push_back(std::move(reflectedrightSphereRitem));
    }

}

void ClientMain::BuildSkyRenderItems()
{
	auto skyRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&skyRitem->World, XMMatrixScaling(5000.0f, 5000.0f, 5000.0f));
	skyRitem->TexTransform = MathHelper::Identity4x4();
	skyRitem->ObjCBIndex = 45;
	skyRitem->Mat = mMaterials["sky"].get();
	skyRitem->Geo = mGeometries["shapeGeo"].get();
	skyRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	skyRitem->IndexCount = skyRitem->Geo->DrawArgs["sphere"].IndexCount;
	skyRitem->StartIndexLocation = skyRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
	skyRitem->BaseVertexLocation = skyRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;

    mRenderItems[(int)RenderLayer::Sky].push_back(skyRitem.get());
	mAllRitems.push_back(std::move(skyRitem));


	auto quadRitem = std::make_unique<RenderItem>();
	quadRitem->World = MathHelper::Identity4x4();
	quadRitem->TexTransform = MathHelper::Identity4x4();
	quadRitem->ObjCBIndex = 46;
	quadRitem->Mat = mMaterials["bricks"].get();
	quadRitem->Geo = mGeometries["shapeGeo"].get();
	quadRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	quadRitem->IndexCount = quadRitem->Geo->DrawArgs["quad"].IndexCount;
	quadRitem->StartIndexLocation = quadRitem->Geo->DrawArgs["quad"].StartIndexLocation;
	quadRitem->BaseVertexLocation = quadRitem->Geo->DrawArgs["quad"].BaseVertexLocation;

	mRenderItems[(int)RenderLayer::Debug].push_back(quadRitem.get());
	mAllRitems.push_back(std::move(quadRitem));

	quadRitem = std::make_unique<RenderItem>();
	quadRitem->ObjCBIndex = 47;
	quadRitem->Mat = mMaterials["bricks"].get();
	quadRitem->Geo = mGeometries["shapeGeo"].get();
	quadRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	quadRitem->IndexCount = quadRitem->Geo->DrawArgs["quad2"].IndexCount;
	quadRitem->StartIndexLocation = quadRitem->Geo->DrawArgs["quad2"].StartIndexLocation;
	quadRitem->BaseVertexLocation = quadRitem->Geo->DrawArgs["quad2"].BaseVertexLocation;

	mRenderItems[(int)RenderLayer::Debug].push_back(quadRitem.get());
	mAllRitems.push_back(std::move(quadRitem));
}

void ClientMain::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
{
    UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
    auto objectCB = mCurrFrameResource->ObjectCB->Resource();

    // 각 렌더 항목에 대해서...
    for (size_t i = 0; i < ritems.size(); ++i)
    {
        auto ri = ritems[i];

        cmdList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
        cmdList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
        cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

        // 서술자를 셰이더 프로그램이 사용할 루트 서명의 한 매개변수에 묶는 방법 
		CD3DX12_GPU_DESCRIPTOR_HANDLE tex(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
		tex.Offset(ri->Mat->DiffuseSrvHeapIndex, mCbvSrvDescriptorSize);

        D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize; 

		cmdList->SetGraphicsRootConstantBufferView(0, objCBAddress);

        cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
    }
}

void ClientMain::DrawSceneToShadowMap()
{
    // - viewport : 3d 렌더링된 콘텐츠가 실제로 화면에 표시되는 영역을 정의한다. viewport는 
    // 3d월드에서 카메라를 통해 캡처된 장면이 윈도우나 화면 상의 어느 부분에 보여질 것인지를 결정하는 설정이다.
	// 	mViewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f }; width 2048, height 2048 
	mCommandList->RSSetViewports(1, &mShadowMap->Viewport());
	// - ScissorRect : 특정 픽셀들을 선별하는 용도로 쓰인다. 후면 버퍼를 기준으로 가위 직사각형을 정의,설정하면, 렌더링 시 시저
    // 렉트의 바깥에 있는 픽셀들은 후면 버퍼에 래스터화 되지 않는다. 
    mCommandList->RSSetScissorRects(1, &mShadowMap->ScissorRect());

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap->Resource(),
		D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE));
	UINT passCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));
	mCommandList->ClearDepthStencilView(mShadowMap->Dsv(),
		D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
    // 랜더 타겟을 세팅하지 않는다. 우리는 오직 뎁스 버퍼만 그린다. 랜더 타겟을 세팅하지 않는 것은 컬러를 쓰지 않는다는 것이다. 
    // pso에 적어라 또한 명확히해라 랜더 타겟이 0인것을.
	mCommandList->OMSetRenderTargets(0, nullptr, false, &mShadowMap->Dsv());

	// Bind the pass constant buffer for the shadow map pass.
	auto passCB = mCurrFrameResource->PassCB->Resource();
	D3D12_GPU_VIRTUAL_ADDRESS passCBAddress = passCB->GetGPUVirtualAddress() + 1 * passCBByteSize;
	mCommandList->SetGraphicsRootConstantBufferView(1, passCBAddress);

	mCommandList->SetPipelineState(mPSOs["shadow_opaque"].Get());

	DrawRenderItems(mCommandList.Get(), mRenderItems[(int)RenderLayer::Opaque]);
	DrawRenderItems(mCommandList.Get(), mRenderItems[(int)RenderLayer::Reflected]);
	DrawRenderItems(mCommandList.Get(), mRenderItems[(int)RenderLayer::AlphaTested]);


	// Change back to GENERIC_READ so we can read the texture in a shader.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap->Resource(),
		D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_GENERIC_READ));
}

void ClientMain::DrawNormalsAndDepth()
{
	mCommandList->RSSetViewports(1, &mScreenViewport);
	mCommandList->RSSetScissorRects(1, &mScissorRect);

	auto normalMap = mSsao->NormalMap();
	auto normalMapRtv = mSsao->NormalMapRtv();

	// Change to RENDER_TARGET.
	// ResourceBarrier : 리소스 상태를 변경하는 역할을 한다. 
	// normalMap: 상태를 전환할 대상 리소스 
	// D3D12_RESOURCE_STATE_GENERIC_READ: 리소스의 현재 상태 
	// D3D12_RESOURCE_STATE_RENDER_TARGET 전환하려는 새로운 상태 
	// 리소스가 렌더 타겟으로 사용될 준비가 되었음을 나타낸다. 이 상태에서 GPU가 해당 리소스에 렌더일 데이터를 기록할 수 있다.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(normalMap, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

	// Clear the screen normal map and depth buffer.
	float clearValue[] = { 0.0f, 0.0f, 1.0f, 0.0f };
	mCommandList->ClearRenderTargetView(normalMapRtv, clearValue, 0, nullptr);
	mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

	// Specify the buffers we are going to render to.
	mCommandList->OMSetRenderTargets(1, &normalMapRtv, true, &DepthStencilView());

	// Bind the constant buffer for this pass.
	auto passCB = mCurrFrameResource->PassCB->Resource();
	mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

	mCommandList->SetPipelineState(mPSOs["drawNormals"].Get());

	DrawRenderItems(mCommandList.Get(), mRenderItems[(int)RenderLayer::Opaque]);
	DrawRenderItems(mCommandList.Get(), mRenderItems[(int)RenderLayer::Reflected]);
	DrawRenderItems(mCommandList.Get(), mRenderItems[(int)RenderLayer::AlphaTested]);

	// Change back to GENERIC_READ so we can read the texture in a shader.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(normalMap,
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
}


CD3DX12_CPU_DESCRIPTOR_HANDLE ClientMain::GetCpuSrv(int index)const
{
	auto srv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
	srv.Offset(index, mCbvSrvUavDescriptorSize);
	return srv;
}

CD3DX12_GPU_DESCRIPTOR_HANDLE ClientMain::GetGpuSrv(int index)const
{
	auto srv = CD3DX12_GPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	srv.Offset(index, mCbvSrvUavDescriptorSize);
	return srv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE ClientMain::GetDsv(int index)const
{
	auto dsv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mDsvHeap->GetCPUDescriptorHandleForHeapStart());
	dsv.Offset(index, mDsvDescriptorSize);
	return dsv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE ClientMain::GetRtv(int index)const
{
	auto rtv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
	rtv.Offset(index, mRtvDescriptorSize);
	return rtv;
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 7> ClientMain::GetStaticSamplers()
{
	const CD3DX12_STATIC_SAMPLER_DESC pointWrap(
		0, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
		1, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

    // D3D12_FILTER_MIN_MAG_MIP_LINEAR : 축소, 확대, mipmap 수준 전환 모두에서 선형 필터링을 수행하는 설정
    // wrap(반복)모드는 텍스처 좌표가 범위를 벗어나면, 텍스처를 타일처럼 반복하는 샘플링
	const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
		2, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
		3, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(
		4, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressW
		0.0f,                             // mipLODBias
		8);                               // maxAnisotropy

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(
		5, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressW
		0.0f,                              // mipLODBias
		8);                                // maxAnisotropy


	const CD3DX12_STATIC_SAMPLER_DESC shadow(
		6, // shaderRegister
		D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressW
		0.0f,                               // mipLODBias
		16,                                 // maxAnisotropy
		D3D12_COMPARISON_FUNC_LESS_EQUAL,
		D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK);

    return {
        pointWrap, pointClamp,
        linearWrap, linearClamp,
        anisotropicWrap, anisotropicClamp, shadow };
}


void ClientMain::BuildMaterials()
{
	std::vector<std::string> texNames =
	{
		"bricks",
		"brick2",
		"tile",
		"stone",
        "WireFence", 
        "ice",
        "white1x1",
        "sky", 
	};

    auto SetMaterial = [&](
        std::string pName, 
        int pMatCBIndex, 
        int pDiffuseSrvHeapIndex,
        int pNormalSrvHeapIndex, 
        DirectX::XMFLOAT4 pDiffuseAlbedo,
        DirectX::XMFLOAT3 pFresnelR0, 
        float pRoughness )
    {
		auto aTemp = std::make_unique<Material>();
        aTemp->Name = pName;
        aTemp->MatCBIndex = pMatCBIndex;
        aTemp->DiffuseSrvHeapIndex = pDiffuseSrvHeapIndex;
        aTemp->NormalSrvHeapIndex = pNormalSrvHeapIndex; 
        aTemp->DiffuseAlbedo = pDiffuseAlbedo;
        aTemp->FresnelR0 = pFresnelR0;
        aTemp->Roughness = pRoughness;

		mMaterials[texNames[pMatCBIndex]] = std::move(aTemp);
    };

    SetMaterial("bricks",       0,  0,  8,   XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),   XMFLOAT3(0.1f, 0.12f, 0.12f),    0.1f);
	SetMaterial("brick2",       1,  1,  9,   XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),   XMFLOAT3(0.05f, 0.05f, 0.05f),    0.3f);
	SetMaterial("tile",         2,  2,  10,  XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),   XMFLOAT3(0.02f, 0.02f, 0.02f),    0.2f);
	SetMaterial("stone",        3,  3,  11,   XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),   XMFLOAT3(0.1f, 0.1f, 0.1f),       0.25f);
	SetMaterial("WireFence",    4,  5,  11,   XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),   XMFLOAT3(0.02f, 0.02f, 0.02f),    0.1f);
    SetMaterial("ice",          5,  6,  11,  XMFLOAT4(1.0f, 1.0f, 1.0f, 0.3f),   XMFLOAT3(0.78f, 0.78f, 0.78f),    0.4f);


	SetMaterial("white1x1",     6,  7,  11,  XMFLOAT4(0.0f, 0.0f, 0.0f, 0.5f),   XMFLOAT3(0.98f, 0.97f, 0.95f), 0.1f);
	SetMaterial("sky",          7,  11, 11,   XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),   XMFLOAT3(0.1f, 0.1f, 0.1f),    1.0f);
}

