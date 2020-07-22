//--------------------------------------------------------------------------------------
// File: BasicCompute11.cpp
//
// Demonstrates the basics to get DirectX 11 Compute Shader (aka DirectCompute) up and
// running by implementing Array A + Array B
//
// Copyright (c) Microsoft Corporation. All rights reserved.
//--------------------------------------------------------------------------------------

#include <stdio.h>
#include <crtdbg.h>
#include <d3dcommon.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <array>
#include <assert.h>
#include <chrono>

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p)      { if (p) { (p)->Release(); (p)=nullptr; } }
#endif

// Comment out the following line to use raw buffers instead of structured buffers
#define USE_STRUCTURED_BUFFERS

// If defined, then the hardware/driver must report support for double-precision CS 5.0 shaders or the sample fails to run
//#define TEST_DOUBLE

// The number of elements in a buffer to be tested
const UINT NUM_ELEMENTS = 1024;


//--------------------------------------------------------------------------------------
// Forward declarations 
//--------------------------------------------------------------------------------------
HRESULT CreateComputeDevice( _Outptr_ ID3D11Device** ppDeviceOut, _Outptr_ ID3D11DeviceContext** ppContextOut, _In_ bool bForceRef );
HRESULT CreateComputeShader( _In_z_ LPCWSTR pSrcFile, _In_z_ LPCSTR pFunctionName, 
                             _In_ ID3D11Device* pDevice, _Outptr_ ID3D11ComputeShader** ppShaderOut );
HRESULT CreateStructuredBuffer( _In_ ID3D11Device* pDevice, _In_ UINT uElementSize, _In_ UINT uCount,
                                _In_reads_(uElementSize*uCount) void* pInitData,
                                _Outptr_ ID3D11Buffer** ppBufOut );
HRESULT CreateRawBuffer( _In_ ID3D11Device* pDevice, _In_ UINT uSize, _In_reads_(uSize) void* pInitData, _Outptr_ ID3D11Buffer** ppBufOut );
HRESULT CreateConstBuffer(_In_ ID3D11Device* pDevice, _In_ UINT uSize, _In_reads_(uSize) void* pInitData, _Outptr_ ID3D11Buffer** ppBufOut);
HRESULT CreateBufferSRV( _In_ ID3D11Device* pDevice, _In_ ID3D11Buffer* pBuffer, _Outptr_ ID3D11ShaderResourceView** ppSRVOut );
HRESULT CreateBufferUAV( _In_ ID3D11Device* pDevice, _In_ ID3D11Buffer* pBuffer, _Outptr_ ID3D11UnorderedAccessView** pUAVOut );
ID3D11Buffer* CreateAndCopyToDebugBuf( _In_ ID3D11Device* pDevice, _In_ ID3D11DeviceContext* pd3dImmediateContext, _In_ ID3D11Buffer* pBuffer );
void RunComputeShader( _In_ ID3D11DeviceContext* pd3dImmediateContext,
                       _In_ ID3D11ComputeShader* pComputeShader,
                       _In_ UINT nNumViews, _In_reads_(nNumViews) ID3D11ShaderResourceView** pShaderResourceViews, 
                       _In_opt_ ID3D11Buffer* pCBCS, _In_reads_opt_(dwNumDataBytes) void* pCSData, _In_ DWORD dwNumDataBytes,
                       _In_ ID3D11UnorderedAccessView* pUnorderedAccessView,
					   _In_ ID3D11Buffer* pConstantBuffer,
                       _In_ UINT X, _In_ UINT Y, _In_ UINT Z );
HRESULT FindDXSDKShaderFileCch( _Out_writes_(cchDest) WCHAR* strDestPath,
                                _In_ int cchDest, 
                                _In_z_ LPCWSTR strFilename );

//--------------------------------------------------------------------------------------
// Global variables
//--------------------------------------------------------------------------------------
ID3D11Device*               g_pDevice = nullptr;
ID3D11DeviceContext*        g_pContext = nullptr;
ID3D11ComputeShader*        g_pCS = nullptr;

// Particle self shadowing task
// X - forward
// Y - up
// Z - right

ID3D11Buffer* particlesBuffer = nullptr;
ID3D11Buffer* shadowBuffer = nullptr;
ID3D11Buffer* constBuffer = nullptr;
ID3D11ShaderResourceView* particlesBufferSRV = nullptr;
ID3D11UnorderedAccessView*  shadowBufferUAV = nullptr;

struct Pos {
	float x, y, z;
};

struct Particle {
	Pos pos;
	float radius;
	float opacity;
};

float Smoothstep(float edge0, float edge1, float value) {
	const float t = min(max((value - edge0) / (edge1 - edge0), 0.0f), 1.0f);
	return t * t * (3.0 - 2.0 * t);
}

float Overlap(float dir[4], const Particle & caster, const Particle & receiver) {
	
	const float dReceiver{ dir[0] * receiver.pos.x + dir[1] * receiver.pos.y + dir[2] * receiver.pos.z };
	const float dCaster{ dir[0] * caster.pos.x + dir[1] * caster.pos.y + dir[2] * caster.pos.z };

	if (dCaster < dReceiver) {
		return 0.0f;
	}

	const Pos posReceiever{ receiver.pos.x - dir[0] * dReceiver, receiver.pos.y - dir[1] * dReceiver, receiver.pos.z - dir[2] * dReceiver };
	const Pos posCaster{ caster.pos.x - dir[0] * dCaster, caster.pos.y - dir[1] * dCaster, caster.pos.z - dir[2] * dCaster};

	const float dist = sqrtf(
		(posReceiever.x - posCaster.x) * (posReceiever.x - posCaster.x) +
		(posReceiever.y - posCaster.y) * (posReceiever.y - posCaster.y) +
		(posReceiever.z - posCaster.z) * (posReceiever.z - posCaster.z));

	return caster.opacity * min(caster.radius * caster.radius / (receiver.radius * receiver.radius), 1.0f) * Smoothstep(receiver.radius + caster.radius, abs(receiver.radius - caster.radius), dist);
}


#define THREAD_X 32
#define THREAD_Y 32

std::array<Particle, THREAD_X * THREAD_Y> particlesArr;
float sunDir[4];

void CreateIOBuffers();
void SetUniforms();
void TestOverlapHost();
void TestResult(float result[THREAD_X * THREAD_Y]);

//--------------------------------------------------------------------------------------
// Entry point to the program
//--------------------------------------------------------------------------------------
int __cdecl main()
{
	printf("Test covering function...");
	TestOverlapHost();
	printf("done\n");

    printf( "Creating device..." );
    if ( FAILED( CreateComputeDevice( &g_pDevice, &g_pContext, false ) ) )
        return 1;
    printf( "done\n" );

    printf( "Creating Compute Shader..." );
    if ( FAILED( CreateComputeShader( L"compute.hlsl", "csComputeSelfShadowing", g_pDevice, &g_pCS ) ) )
        return 1;
    printf( "done\n" );

    printf( "Creating buffers and filling them with initial data..." );

	CreateIOBuffers();

    printf( "done\n" );

    printf( "Running Compute Shader..." );
    ID3D11ShaderResourceView* aRViews[1] = { particlesBufferSRV };

	SetUniforms();
    RunComputeShader( g_pContext, g_pCS, 1, aRViews, nullptr, nullptr, 0, shadowBufferUAV, constBuffer, 1, 1, 1 );
    printf( "done\n" );

	// Testing
    {
        ID3D11Buffer* debugbuf = CreateAndCopyToDebugBuf( g_pDevice, g_pContext, shadowBuffer );
        D3D11_MAPPED_SUBRESOURCE MappedResource; 
        float *p;
        g_pContext->Map( debugbuf, 0, D3D11_MAP_READ, 0, &MappedResource );
        p = (float*)MappedResource.pData;

        printf( "Verifying against CPU result..." );
        
		TestResult(p);

		printf("done\n");

        g_pContext->Unmap( debugbuf, 0 );

        SAFE_RELEASE( debugbuf );
    }
    
    printf( "Cleaning up...\n" );
	SAFE_RELEASE(particlesBufferSRV);
	SAFE_RELEASE(shadowBufferUAV);
	SAFE_RELEASE(particlesBuffer);
	SAFE_RELEASE(shadowBuffer);
    SAFE_RELEASE( g_pCS );
    SAFE_RELEASE( g_pContext );
    SAFE_RELEASE( g_pDevice );

	printf("done\n");

	std::getchar();
    return 0;
}


//--------------------------------------------------------------------------------------
// Create the D3D device and device context suitable for running Compute Shaders(CS)
//--------------------------------------------------------------------------------------
_Use_decl_annotations_
HRESULT CreateComputeDevice( ID3D11Device** ppDeviceOut, ID3D11DeviceContext** ppContextOut, bool bForceRef )
{    
    *ppDeviceOut = nullptr;
    *ppContextOut = nullptr;
    
    HRESULT hr = S_OK;

    UINT uCreationFlags = D3D11_CREATE_DEVICE_SINGLETHREADED;
#ifdef _DEBUG
    uCreationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL flOut;
    static const D3D_FEATURE_LEVEL flvl[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    
    bool bNeedRefDevice = false;
    if ( !bForceRef )
    {
        hr = D3D11CreateDevice( nullptr,                        // Use default graphics card
                                D3D_DRIVER_TYPE_HARDWARE,    // Try to create a hardware accelerated device
                                nullptr,                        // Do not use external software rasterizer module
                                uCreationFlags,              // Device creation flags
                                flvl,
                                sizeof(flvl) / sizeof(D3D_FEATURE_LEVEL),
                                D3D11_SDK_VERSION,           // SDK version
                                ppDeviceOut,                 // Device out
                                &flOut,                      // Actual feature level created
                                ppContextOut );              // Context out
        
        if ( SUCCEEDED( hr ) )
        {
            // A hardware accelerated device has been created, so check for Compute Shader support

            // If we have a device >= D3D_FEATURE_LEVEL_11_0 created, full CS5.0 support is guaranteed, no need for further checks
            if ( flOut < D3D_FEATURE_LEVEL_11_0 )            
            {
#ifdef TEST_DOUBLE
                bNeedRefDevice = true;
                printf( "No hardware Compute Shader 5.0 capable device found (required for doubles), trying to create ref device.\n" );
#else
                // Otherwise, we need further check whether this device support CS4.x (Compute on 10)
                D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS hwopts;
                (*ppDeviceOut)->CheckFeatureSupport( D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &hwopts, sizeof(hwopts) );
                if ( !hwopts.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x )
                {
                    bNeedRefDevice = true;
                    printf( "No hardware Compute Shader capable device found, trying to create ref device.\n" );
                }
#endif
            }

#ifdef TEST_DOUBLE
            else
            {
                // Double-precision support is an optional feature of CS 5.0
                D3D11_FEATURE_DATA_DOUBLES hwopts;
                (*ppDeviceOut)->CheckFeatureSupport( D3D11_FEATURE_DOUBLES, &hwopts, sizeof(hwopts) );
                if ( !hwopts.DoublePrecisionFloatShaderOps )
                {
                    bNeedRefDevice = true;
                    printf( "No hardware double-precision capable device found, trying to create ref device.\n" );
                }
            }
#endif
        }
    }
    
    if ( bForceRef || FAILED(hr) || bNeedRefDevice )
    {
        // Either because of failure on creating a hardware device or hardware lacking CS capability, we create a ref device here

        SAFE_RELEASE( *ppDeviceOut );
        SAFE_RELEASE( *ppContextOut );
        
        hr = D3D11CreateDevice( nullptr,                        // Use default graphics card
                                D3D_DRIVER_TYPE_REFERENCE,   // Try to create a hardware accelerated device
                                nullptr,                        // Do not use external software rasterizer module
                                uCreationFlags,              // Device creation flags
                                flvl,
                                sizeof(flvl) / sizeof(D3D_FEATURE_LEVEL),
                                D3D11_SDK_VERSION,           // SDK version
                                ppDeviceOut,                 // Device out
                                &flOut,                      // Actual feature level created
                                ppContextOut );              // Context out
        if ( FAILED(hr) )
        {
            printf( "Reference rasterizer device create failure\n" );
            return hr;
        }
    }

    return hr;
}

//--------------------------------------------------------------------------------------
// Compile and create the CS
//--------------------------------------------------------------------------------------
_Use_decl_annotations_
HRESULT CreateComputeShader( LPCWSTR pSrcFile, LPCSTR pFunctionName, 
                             ID3D11Device* pDevice, ID3D11ComputeShader** ppShaderOut )
{
    if ( !pDevice || !ppShaderOut )
        return E_INVALIDARG;

    // Finds the correct path for the shader file.
    // This is only required for this sample to be run correctly from within the Sample Browser,
    // in your own projects, these lines could be removed safely
    WCHAR str[MAX_PATH];
    HRESULT hr = FindDXSDKShaderFileCch( str, MAX_PATH, pSrcFile );
    if ( FAILED(hr) )
        return hr;
    
    DWORD dwShaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    // Set the D3DCOMPILE_DEBUG flag to embed debug information in the shaders.
    // Setting this flag improves the shader debugging experience, but still allows 
    // the shaders to be optimized and to run exactly the way they will run in 
    // the release configuration of this program.
    dwShaderFlags |= D3DCOMPILE_DEBUG;

    // Disable optimizations to further improve shader debugging
    dwShaderFlags |= D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    const D3D_SHADER_MACRO defines[] = 
    {
#ifdef USE_STRUCTURED_BUFFERS
        "USE_STRUCTURED_BUFFERS", "1",
#endif

#ifdef TEST_DOUBLE
        "TEST_DOUBLE", "1",
#endif
        nullptr, nullptr
    };

    // We generally prefer to use the higher CS shader profile when possible as CS 5.0 is better performance on 11-class hardware
    LPCSTR pProfile = ( pDevice->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 ) ? "cs_5_0" : "cs_4_0";

    ID3DBlob* pErrorBlob = nullptr;
    ID3DBlob* pBlob = nullptr;
    hr = D3DCompileFromFile( str, defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, pFunctionName, pProfile, 
                             dwShaderFlags, 0, &pBlob, &pErrorBlob );
    if ( FAILED(hr) )
    {
        if ( pErrorBlob )
            OutputDebugStringA( (char*)pErrorBlob->GetBufferPointer() );

        SAFE_RELEASE( pErrorBlob );
        SAFE_RELEASE( pBlob );    

        return hr;
    }    

    hr = pDevice->CreateComputeShader( pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, ppShaderOut );

    SAFE_RELEASE( pErrorBlob );
    SAFE_RELEASE( pBlob );

#if defined(_DEBUG) || defined(PROFILE)
    if ( SUCCEEDED(hr) )
    {
        (*ppShaderOut)->SetPrivateData( WKPDID_D3DDebugObjectName, lstrlenA(pFunctionName), pFunctionName );
    }
#endif

    return hr;
}

//--------------------------------------------------------------------------------------
// Create Structured Buffer
//--------------------------------------------------------------------------------------
_Use_decl_annotations_
HRESULT CreateStructuredBuffer( ID3D11Device* pDevice, UINT uElementSize, UINT uCount, void* pInitData, ID3D11Buffer** ppBufOut )
{
    *ppBufOut = nullptr;

    D3D11_BUFFER_DESC desc = {};
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    desc.ByteWidth = uElementSize * uCount;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = uElementSize;

    if ( pInitData )
    {
        D3D11_SUBRESOURCE_DATA InitData;
        InitData.pSysMem = pInitData;
        return pDevice->CreateBuffer( &desc, &InitData, ppBufOut );
    } else
        return pDevice->CreateBuffer( &desc, nullptr, ppBufOut );
}

//--------------------------------------------------------------------------------------
// Create Raw Buffer
//--------------------------------------------------------------------------------------
_Use_decl_annotations_
HRESULT CreateRawBuffer( ID3D11Device* pDevice, UINT uSize, void* pInitData, ID3D11Buffer** ppBufOut )
{
    *ppBufOut = nullptr;

    D3D11_BUFFER_DESC desc = {};
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_INDEX_BUFFER | D3D11_BIND_VERTEX_BUFFER;
    desc.ByteWidth = uSize;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;

    if ( pInitData )
    {
        D3D11_SUBRESOURCE_DATA InitData;
        InitData.pSysMem = pInitData;
        return pDevice->CreateBuffer( &desc, &InitData, ppBufOut );
    } else
        return pDevice->CreateBuffer( &desc, nullptr, ppBufOut );
}

HRESULT CreateConstBuffer(ID3D11Device * pDevice, UINT uSize, void * pInitData, ID3D11Buffer ** ppBufOut)
{
	// Fill in a buffer description.
	D3D11_BUFFER_DESC cbDesc;
	cbDesc.ByteWidth = uSize;
	cbDesc.Usage = D3D11_USAGE_DYNAMIC;
	cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	cbDesc.MiscFlags = 0;
	cbDesc.StructureByteStride = 0;

	// Fill in the subresource data.
	D3D11_SUBRESOURCE_DATA InitData;
	InitData.pSysMem = pInitData;
	InitData.SysMemPitch = 0;
	InitData.SysMemSlicePitch = 0;

	// Create the buffer.
	return pDevice->CreateBuffer(&cbDesc, &InitData, ppBufOut);
}

//--------------------------------------------------------------------------------------
// Create Shader Resource View for Structured or Raw Buffers
//--------------------------------------------------------------------------------------
_Use_decl_annotations_
HRESULT CreateBufferSRV( ID3D11Device* pDevice, ID3D11Buffer* pBuffer, ID3D11ShaderResourceView** ppSRVOut )
{
    D3D11_BUFFER_DESC descBuf = {};
    pBuffer->GetDesc( &descBuf );

    D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
    desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    desc.BufferEx.FirstElement = 0;

    if ( descBuf.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS )
    {
        // This is a Raw Buffer

        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        desc.BufferEx.NumElements = descBuf.ByteWidth / 4;
    } else
    if ( descBuf.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED )
    {
        // This is a Structured Buffer

        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.BufferEx.NumElements = descBuf.ByteWidth / descBuf.StructureByteStride;
    } else
    {
        return E_INVALIDARG;
    }

    return pDevice->CreateShaderResourceView( pBuffer, &desc, ppSRVOut );
}

//--------------------------------------------------------------------------------------
// Create Unordered Access View for Structured or Raw Buffers
//-------------------------------------------------------------------------------------- 
_Use_decl_annotations_
HRESULT CreateBufferUAV( ID3D11Device* pDevice, ID3D11Buffer* pBuffer, ID3D11UnorderedAccessView** ppUAVOut )
{
    D3D11_BUFFER_DESC descBuf = {};
    pBuffer->GetDesc( &descBuf );
        
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc = {};
    desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    desc.Buffer.FirstElement = 0;

    if ( descBuf.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS )
    {
        // This is a Raw Buffer

        desc.Format = DXGI_FORMAT_R32_TYPELESS; // Format must be DXGI_FORMAT_R32_TYPELESS, when creating Raw Unordered Access View
        desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        desc.Buffer.NumElements = descBuf.ByteWidth / 4; 
    } else
    if ( descBuf.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED )
    {
        // This is a Structured Buffer

        desc.Format = DXGI_FORMAT_UNKNOWN;      // Format must be must be DXGI_FORMAT_UNKNOWN, when creating a View of a Structured Buffer
        desc.Buffer.NumElements = descBuf.ByteWidth / descBuf.StructureByteStride; 
    } else
    {
        return E_INVALIDARG;
    }
    
    return pDevice->CreateUnorderedAccessView( pBuffer, &desc, ppUAVOut );
}

//--------------------------------------------------------------------------------------
// Create a CPU accessible buffer and download the content of a GPU buffer into it
// This function is very useful for debugging CS programs
//-------------------------------------------------------------------------------------- 
_Use_decl_annotations_
ID3D11Buffer* CreateAndCopyToDebugBuf( ID3D11Device* pDevice, ID3D11DeviceContext* pd3dImmediateContext, ID3D11Buffer* pBuffer )
{
    ID3D11Buffer* debugbuf = nullptr;

    D3D11_BUFFER_DESC desc = {};
    pBuffer->GetDesc( &desc );
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    if ( SUCCEEDED(pDevice->CreateBuffer(&desc, nullptr, &debugbuf)) )
    {
#if defined(_DEBUG) || defined(PROFILE)
        debugbuf->SetPrivateData( WKPDID_D3DDebugObjectName, sizeof( "Debug" ) - 1, "Debug" );
#endif

        pd3dImmediateContext->CopyResource( debugbuf, pBuffer );
    }

    return debugbuf;
}

//--------------------------------------------------------------------------------------
// Run CS
//-------------------------------------------------------------------------------------- 
_Use_decl_annotations_
void RunComputeShader( ID3D11DeviceContext* pd3dImmediateContext,
                      ID3D11ComputeShader* pComputeShader,
                      UINT nNumViews, ID3D11ShaderResourceView** pShaderResourceViews, 
                      ID3D11Buffer* pCBCS, void* pCSData, DWORD dwNumDataBytes,
                      ID3D11UnorderedAccessView* pUnorderedAccessView,
					  ID3D11Buffer* pConstBuffer,
                      UINT X, UINT Y, UINT Z )
{
    pd3dImmediateContext->CSSetShader( pComputeShader, nullptr, 0 );
    pd3dImmediateContext->CSSetShaderResources( 0, nNumViews, pShaderResourceViews );
    pd3dImmediateContext->CSSetUnorderedAccessViews( 0, 1, &pUnorderedAccessView, nullptr );
	pd3dImmediateContext->CSSetConstantBuffers(0, 1, &pConstBuffer);
    if ( pCBCS && pCSData )
    {
        D3D11_MAPPED_SUBRESOURCE MappedResource;
        pd3dImmediateContext->Map( pCBCS, 0, D3D11_MAP_WRITE_DISCARD, 0, &MappedResource );
        memcpy( MappedResource.pData, pCSData, dwNumDataBytes );
        pd3dImmediateContext->Unmap( pCBCS, 0 );
        ID3D11Buffer* ppCB[1] = { pCBCS };
        pd3dImmediateContext->CSSetConstantBuffers( 0, 1, ppCB );
    }

	//auto begin = std::chrono::high_resolution_clock::now();
	LARGE_INTEGER start, stop, freq;
	QueryPerformanceCounter(&start);

	//
	// 1. Create an event query from the current device
	D3D11_QUERY_DESC queryDesc1;
	queryDesc1.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
	queryDesc1.MiscFlags = 0;
	ID3D11Query * pQuery1;

	D3D11_QUERY_DESC queryDesc2;
	queryDesc2.Query = D3D11_QUERY_TIMESTAMP;
	queryDesc2.MiscFlags = 0;
	ID3D11Query * pQuery2;

	D3D11_QUERY_DESC queryDesc3;
	queryDesc3.Query = D3D11_QUERY_TIMESTAMP;
	queryDesc3.MiscFlags = 0;
	ID3D11Query * pQuery3;

	HRESULT res;
	res = g_pDevice->CreateQuery(&queryDesc1, &pQuery1);
	res = g_pDevice->CreateQuery(&queryDesc2, &pQuery2);
	res = g_pDevice->CreateQuery(&queryDesc3, &pQuery3);

	pd3dImmediateContext->Begin(pQuery1);
	pd3dImmediateContext->End(pQuery2);

    pd3dImmediateContext->Dispatch( X, Y, Z );

	pd3dImmediateContext->End(pQuery3);
	pd3dImmediateContext->End(pQuery1);

	D3D11_QUERY_DATA_TIMESTAMP_DISJOINT queryData1;
	UINT64 queryData2, queryData3;

	while (true)
	{
		res = pd3dImmediateContext->GetData(pQuery1, &queryData1, sizeof(queryData1), 0);
		if (res == S_OK) {
			break;
		}
	}
	res = pd3dImmediateContext->GetData(pQuery2, &queryData2, sizeof(queryData2), 0);
	res = pd3dImmediateContext->GetData(pQuery3, &queryData3, sizeof(queryData3), 0);

	printf("elapsed GPU time: %d microsecods\n", ((queryData3 - queryData2) * 1000000) / queryData1.Frequency);

    pd3dImmediateContext->CSSetShader( nullptr, nullptr, 0 );

    ID3D11UnorderedAccessView* ppUAViewnullptr[1] = { nullptr };
    pd3dImmediateContext->CSSetUnorderedAccessViews( 0, 1, ppUAViewnullptr, nullptr );

    ID3D11ShaderResourceView* ppSRVnullptr[2] = { nullptr, nullptr };
    pd3dImmediateContext->CSSetShaderResources( 0, 2, ppSRVnullptr );

    ID3D11Buffer* ppCBnullptr[1] = { nullptr };
    pd3dImmediateContext->CSSetConstantBuffers( 0, 1, ppCBnullptr );
}

//--------------------------------------------------------------------------------------
// Tries to find the location of the shader file
// This is a trimmed down version of DXUTFindDXSDKMediaFileCch.
//--------------------------------------------------------------------------------------
_Use_decl_annotations_
HRESULT FindDXSDKShaderFileCch( WCHAR* strDestPath,
                                int cchDest, 
                                LPCWSTR strFilename )
{
    if( !strFilename || strFilename[0] == 0 || !strDestPath || cchDest < 10 )
        return E_INVALIDARG;

    // Get the exe name, and exe path
    WCHAR strExePath[MAX_PATH] =
    {
        0
    };
    WCHAR strExeName[MAX_PATH] =
    {
        0
    };
    WCHAR* strLastSlash = nullptr;
    GetModuleFileName( nullptr, LPSTR(strExePath), MAX_PATH );
    strExePath[MAX_PATH - 1] = 0;
    strLastSlash = wcsrchr( strExePath, TEXT( '\\' ) );
    if( strLastSlash )
    {
        wcscpy_s( strExeName, MAX_PATH, &strLastSlash[1] );

        // Chop the exe name from the exe path
        *strLastSlash = 0;

        // Chop the .exe from the exe name
        strLastSlash = wcsrchr( strExeName, TEXT( '.' ) );
        if( strLastSlash )
            *strLastSlash = 0;
    }

    // Search in directories:
    //      .\
    //      %EXE_DIR%\..\..\%EXE_NAME%

    wcscpy_s( strDestPath, cchDest, strFilename );
    if( GetFileAttributes( LPSTR(strDestPath) ) != 0xFFFFFFFF )
        return S_OK;

    swprintf_s( strDestPath, cchDest, L"%s\\..\\..\\%s\\%s", strExePath, strExeName, strFilename );
    if( GetFileAttributes( LPSTR(strDestPath) ) != 0xFFFFFFFF )
        return S_OK;    

    // On failure, return the file as the path but also return an error code
    wcscpy_s( strDestPath, cchDest, strFilename );

    return E_FAIL;
}

void CreateIOBuffers()
{

#define frand() (static_cast <float> (rand()) / static_cast <float> (RAND_MAX))

	const float revLen = 1.0f / sqrtf(0.5f * 0.5f + 0.2f * 0.2f + 0.3f * 0.3f);

	sunDir[0] = 0.5f * revLen;
	sunDir[1] = 0.2f * revLen;
	sunDir[2] = 0.3f * revLen;
	sunDir[3] = 0.0f;

	const float sizeX{ 10.0f }, sizeY{ 10.0f }, sizeZ{ 10.0f };


	for (auto & particle : particlesArr) {

		particle.pos.x = (frand() - 0.5f) * sizeX;
		particle.pos.y = (frand() - 0.5f) * sizeY;
		particle.pos.z = (frand() - 0.5f) * sizeZ;
		particle.radius = frand();
		particle.opacity = frand();
	}

	CreateStructuredBuffer(g_pDevice, sizeof(Particle), particlesArr.size() , &particlesArr[0], &particlesBuffer);
	CreateStructuredBuffer(g_pDevice, sizeof(float), particlesArr.size(), nullptr, &shadowBuffer);
	CreateBufferSRV( g_pDevice, particlesBuffer, &particlesBufferSRV );
	CreateBufferUAV(g_pDevice, shadowBuffer, &shadowBufferUAV);
}

void SetUniforms()
{
	CreateConstBuffer(g_pDevice, sizeof(sunDir), &sunDir[0], &constBuffer);
}

void TestOverlapHost()
{
	const float diff{ 1e-6f };
	{
		float sunSir[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
		Particle receiver{ {0.0f, 0.0f, 0.0f}, 1.0f, 1.0f };
		Particle caster{ {2.0f, 0.0f, 0.0f}, 0.5f, 0.5f };

		const float result = Overlap(sunSir, caster, receiver);
		const float expected = 0.5f * (0.5*0.5 / 1.0f * 1.0f);

		assert(abs( result - expected ) < diff, "Full intercetion (caster < receiver)");
	}
	{
		float sunSir[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
		Particle receiver{ {0.0f, 0.0f, 0.0f}, 0.9f, 1.0f };
		Particle caster{ {2.0f, 0.0f, 0.0f}, 1.0f, 0.5f };

		const float result = Overlap(sunSir, caster, receiver);
		const float expected = 0.5f;

		assert(abs(result - expected) < diff, "Full intersection (cater > receiver)");
	}
	{
		float sunSir[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
		Particle receiver{ {0.0f, 0.0f, 0.0f}, 1.0f, 1.0f };
		Particle caster{ {2.0f, 0.0f, 0.0f}, 0.5f, 0.5f };

		const float result = Overlap(sunSir, caster, receiver);
		const float expected = 0.0f;

		assert(abs(result - expected) < diff, "No intersection");
	}

	{
		float sunSir[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
		Particle receiver{ {0.0f, 0.0f, 0.0f}, 1.0f, 1.0f };
		Particle caster{ {2.0f, 1.0f, 0.0f}, 1.0f, 0.5f };

		const float result = Overlap(sunSir, caster, receiver);
		const float expected = 0.5f * 0.5f;

		assert(abs(result - expected) < diff, "Part intersection");
	}

}

void TestResult(float result[THREAD_X * THREAD_Y])
{
	static std::array<float, THREAD_X * THREAD_Y> expected;

	const float diff{ 1e-5f };

	auto begin = std::chrono::high_resolution_clock::now();
	for (size_t i = 0; i < expected.size(); ++i) {
		expected[i] = 1.0f;
		for (size_t j = 0; j < expected.size(); ++j) {

			if (i != j) {
				expected[i] *= 1.0f - Overlap(sunDir, particlesArr[j], particlesArr[i]);
			}
		}
	}
	printf("elapsed CPU time: %d milliseconds\n",
		(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - begin)).count());

	for (size_t i = 0; i < expected.size(); ++i) {
		assert(abs(result[i] - expected[i]) < diff);
	}
}
