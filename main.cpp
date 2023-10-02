// CComPtr
#include <atlbase.h>
#include <cassert>
#include <cstdint>
#include <dxcapi.h>
#include <dxgidebug.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <fstream>
#define _USE_MATH_DEFINES
#include <Math.h> 
#include <string>
#include <sstream>
#include <Windows.h>
#include <format>

#include "DirectXTex.h"

#include "ConvertString.h"
#include "mat4x4.h"
#include "Transform.h"

#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
//#include <wrl.h>


#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"dxguid.lib")
#pragma comment(lib,"dxcompiler.lib")


struct Vector4 {
	float x;
	float y;
	float z;
	float w;
};

struct Vector2 {
	float u;
	float v;
};

struct VertexData {
	Vector4 position;
	Vector2 texcoord;
	Vector3 normal;
};

struct Matrix3x3 {
	float m[3][3];
};

struct Matrix4x4 {
	float m[4][4];
};

struct TransformationMatrix {
	mat4x4 WVP;
	mat4x4 world;
};

struct Material {
	Vector4 color;
	int32_t enableLighting;
	float padding[3];
	mat4x4 uvTransform;
};

struct DirectionalLight {
	Vector4 color; //!< ライトの色
	Vector3 direction; //!< ライトの色
	float intensiy; //!< 輝度
};

struct MaterialData {
	std::string textureFilePath;
};

struct ModelData {
	std::vector<VertexData> vertices;
	MaterialData material;
};


// デバックレイヤー
void EnableShaderBasedValidation() {
	CComPtr<ID3D12Debug> spDebugController0;
	CComPtr<ID3D12Debug1> spDebugController1;
	assert(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&spDebugController0))));
	assert(SUCCEEDED(spDebugController0->QueryInterface(IID_PPV_ARGS(&spDebugController1))));
	spDebugController1->SetEnableGPUBasedValidation(true);
}

#pragma region WindowProc
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
		return true;
	}
	//メッセージに応じてゲーム固有の処理を行う
	switch (msg) {
		//ウィンドウが破棄された
	case WM_DESTROY:
		//OSに対して、アプリの終了を伝える
		PostQuitMessage(0);
		return 0;
	}


	//標準のメッセージ処理を行う
	return DefWindowProc(hwnd, msg, wparam, lparam);
}
#pragma endregion WindowProc

#pragma region CompileShader関数
IDxcBlob* CompileShader(
	//ConpilerするShaderファイルへのパス
	const std::wstring& filePath,
	//Compilerに使用するProfile
	const wchar_t* profile,
	//初期化で生成したのもを3つ
	IDxcUtils* dxcUtils,
	IDxcCompiler3* dxcCompiler,
	IDxcIncludeHandler* includeHandler
)
{
	//これからシェーダーをコンパイルする旨をログに出す
	Log(ConvertString(std::format(L"Begin CompilerShader, Path:{},profile:{}\n", filePath, profile)));
	//hlslファイルを読み込む
	IDxcBlobEncoding* shaderSource = nullptr;
	HRESULT hr = dxcUtils->LoadFile(filePath.c_str(), nullptr, &shaderSource);
	//読めなかったら止める
	assert(SUCCEEDED(hr));
	//読み込んだファイルの内容を設定する
	DxcBuffer shaderSourceBuffer;
	shaderSourceBuffer.Ptr = shaderSource->GetBufferPointer();
	shaderSourceBuffer.Size = shaderSource->GetBufferSize();
	shaderSourceBuffer.Encoding = DXC_CP_UTF8;//UTF8の文字コード

	LPCWSTR arguments[] = {
		filePath.c_str(),//コンパイル対象のhlslファイル名
		L"-E",L"main",//エントリーポイントの指定。基本的にmain以外はしない
		L"-T",profile,//ShaderProfileの設定
		L"-Zi",L"-Qembed_debug",//デバック用の情報を埋め込む
		L"-Od",//最適化を外しておく
		L"-Zpr",//メモリレイアウトは行優先
	};
	//実際にShaderをコンパイルする
	IDxcResult* shaderResult = nullptr;
	hr = dxcCompiler->Compile(
		&shaderSourceBuffer,//読み込んだファイル
		arguments,//コンパイラオプション
		_countof(arguments),//コンパイラオプションの数
		includeHandler,//includeが含まれた諸々
		IID_PPV_ARGS(&shaderResult)//コンパイラ結果
	);
	//コンパイルエラーではなくdxcが起動できないなど致命的な状況
	assert(SUCCEEDED(hr));

	//警告、エラーが出たらログを出して止める
	IDxcBlobUtf8* shaderError = nullptr;
	shaderResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&shaderError), nullptr);
	if (shaderError != nullptr && shaderError->GetStringLength() != 0) {
		Log(shaderError->GetStringPointer());
		//警告、エラーダメ
		assert(false);
	}
	//コンパイル結果から実行用のバイナリ部分を取得
	IDxcBlob* shaderBlob = nullptr;
	hr = shaderResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shaderBlob), nullptr);
	assert(SUCCEEDED(hr));
	//成功したログを出す
	Log(ConvertString(std::format(L"Compile Succeeded,path:{},profile:{}\n", filePath, profile)));
	//もう使わないリソースを解放
	shaderSource->Release();
	shaderResult->Release();
	//実行用のバイナリを返却
	return shaderBlob;
}
#pragma endregion CompileShader関数

#pragma region CreateBufferResource関数
ID3D12Resource* CreateBufferResource(ID3D12Device* device, size_t sizeInBytes) {
	ID3D12Resource* resource = nullptr;
	//頂点リソース用のヒープの設定
	D3D12_HEAP_PROPERTIES uploadHeapProperties{};
	uploadHeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;//UploadHeapを使う
	//頂点リソースの設定
	D3D12_RESOURCE_DESC resourceDesc{};
	//バッファリソース、テクスチャの場合はまた罰の設定をする
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	resourceDesc.Width = sizeInBytes;//リソースのサイズ。今回はvector4を3頂点文
	//バッファリソースの設定
	resourceDesc.Height = 1;
	resourceDesc.DepthOrArraySize = 1;
	resourceDesc.MipLevels = 1;
	resourceDesc.SampleDesc.Count = 1;
	//バッファの場合はこれにする決まり
	resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	//実際に頂点リソースを作る
	HRESULT hr = device->CreateCommittedResource(
		&uploadHeapProperties,
		D3D12_HEAP_FLAG_NONE,
		&resourceDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&resource));
	assert(SUCCEEDED(hr));

	return resource;
}

#pragma endregion CreateBufferResource関数

#pragma region DescriptorHeap関数
ID3D12DescriptorHeap* CreateDescriptorHeap(
	ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE heapType, UINT numDescriptors, bool shaderVisible) {
	ID3D12DescriptorHeap* descriptorHeap = nullptr;
	D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc{};
	descriptorHeapDesc.Type = heapType;
	descriptorHeapDesc.NumDescriptors = numDescriptors;
	descriptorHeapDesc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	HRESULT hr = device->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(&descriptorHeap));
	assert(SUCCEEDED(hr));
	return descriptorHeap;
}
#pragma endregion DescriptorHeap関数

#pragma region LoadTexture関数
DirectX::ScratchImage LoadTexture(const std::string& filePath) {
	// テクスチャを読み込んでプログラムで扱えるようにする
	DirectX::ScratchImage image{};
	std::wstring filePathW = ConvertString(filePath);
	HRESULT hr = DirectX::LoadFromWICFile(filePathW.c_str(), DirectX::WIC_FLAGS_FORCE_SRGB, nullptr, image);
	assert(SUCCEEDED(hr));

	// ミップマップの作成
	DirectX::ScratchImage mipImages{};
	hr = DirectX::GenerateMipMaps(image.GetImages(), image.GetImageCount(), image.GetMetadata(), DirectX::TEX_FILTER_SRGB, 0, mipImages);
	assert(SUCCEEDED(hr));

	// ミップマップ付きのデータを表す
	return mipImages;
}
#pragma endregion LoadTexture関数

#pragma region CreateTextureResourec関数
ID3D12Resource* CreateTextureResourec(ID3D12Device* device, const DirectX::TexMetadata& metadata) {
	// metadataを基にResourceの設定
	D3D12_RESOURCE_DESC resourceDesc{};
	resourceDesc.Width = UINT(metadata.width);// Textureの幅
	resourceDesc.Height = UINT(metadata.height);// Textureの高さ
	resourceDesc.MipLevels = UINT16(metadata.mipLevels);// mipmapの数
	resourceDesc.DepthOrArraySize = UINT16(metadata.arraySize);// 奥行き or 配列textureの配列数
	resourceDesc.Format = metadata.format;// TextureのFormat
	resourceDesc.SampleDesc.Count = 1;// サンプリングカウント。1固定
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION(metadata.dimension);// Textureの次元数。普段使っているのは2次元
	// 利用するHeapの設定。非常に特殊な運用02_04exで一般的なケースがある
	D3D12_HEAP_PROPERTIES heapProperties{};
	heapProperties.Type = D3D12_HEAP_TYPE_CUSTOM;// 細かい設定を行う
	heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;// WriteBackポリシーでCPUアクセス可能
	heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;// プロセッサの近くに配置
	// Resourceの生成
	ID3D12Resource* resource = nullptr;
	HRESULT hr = device->CreateCommittedResource(
		&heapProperties,// Heapの設定
		D3D12_HEAP_FLAG_NONE,// Heapの特殊な設定。特になし
		&resourceDesc,// Resourceの設定
		D3D12_RESOURCE_STATE_GENERIC_READ,// 初回のResourceState。Textureは基本読むだけ
		nullptr,// Clear最適値。使わないのでnullptr
		IID_PPV_ARGS(&resource)// 作成するResourceポインタへのポインタ
	);
	assert(SUCCEEDED(hr));
	return resource;
}
#pragma endregion CreateTextureResourec関数

#pragma region UploadTextureData関数
void UploadTextureData(ID3D12Resource* texture, const DirectX::ScratchImage& mipImages) {
	// Meta情報を取得
	const DirectX::TexMetadata& metadata = mipImages.GetMetadata();
	// 全MipMapについて
	for (size_t mipLevel = 0; mipLevel < metadata.mipLevels; ++mipLevel) {
		// MipMapLevelsを指定して書くImageを取得
		const DirectX::Image* img = mipImages.GetImage(mipLevel, 0, 0);
		// Textureに転送
		HRESULT hr = texture->WriteToSubresource(
			UINT(mipLevel),
			nullptr,//全領域へコピー
			img->pixels,//元データアドレス
			UINT(img->rowPitch),//1ラインサイズ
			UINT(img->slicePitch)//1枚サイズ
		);
		assert(SUCCEEDED(hr));
	}
}
#pragma endregion UploadTextureData関数

#pragma region CreateDepthStencilTexture関数
ID3D12Resource* CreateDepthStencilTextureResource(ID3D12Device* device, int32_t width, int32_t height) {
	D3D12_RESOURCE_DESC resourceDesc{};
	resourceDesc.Width = width;// Textureの幅
	resourceDesc.Height = height;// Textureの高さ
	resourceDesc.MipLevels = 1;// mipmapの数
	resourceDesc.DepthOrArraySize = 1;// 奥行き or 配列Textureの配列数
	resourceDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;// DepthStencilとして利用可能なフォーマット
	resourceDesc.SampleDesc.Count = 1;// サンプリングカウント。1固定
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;// 2次元
	resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;// DepthStencilとして使う通知

	// 利用するHeapの設定
	D3D12_HEAP_PROPERTIES heapProperties{};
	heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;// VRAM上に作る

	// 深度値のクリア設定
	D3D12_CLEAR_VALUE depthClearValue{};
	depthClearValue.DepthStencil.Depth = 1.0f;// 1.0f(最大値)でクリア
	depthClearValue.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; // フォーマット。Resourceと合わせる
	ID3D12Resource* resource = nullptr;
	HRESULT hr = device->CreateCommittedResource(
		&heapProperties, // Heapの設定
		D3D12_HEAP_FLAG_NONE, // Heapの特殊な設定
		&resourceDesc, // Resourceの設定
		D3D12_RESOURCE_STATE_DEPTH_WRITE, // 深度値を書き込む状態にしておく
		&depthClearValue, // Clear最適値
		IID_PPV_ARGS(&resource));

	assert(SUCCEEDED(hr));

	return resource;
}
#pragma endregion CreateDepthStencilTexture関数

#pragma region GetCPUDescriptorHandle関数
D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptorHandle(ID3D12DescriptorHeap* descriptorHeap, uint32_t descriptorSize, uint32_t index) {
	D3D12_CPU_DESCRIPTOR_HANDLE handleCPU = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
	handleCPU.ptr += (descriptorSize * index);
	return handleCPU;
}
#pragma endregion GetCPUDescriptorHandle関数

#pragma region GetGPUDescriptorHandle関数
D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptorHandle(ID3D12DescriptorHeap* descriptorHeap, uint32_t descriptorSize, uint32_t index) {
	D3D12_GPU_DESCRIPTOR_HANDLE handleGPU = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
	handleGPU.ptr += (descriptorSize * index);
	return handleGPU;
}
#pragma endregion GetGPUDescriptorHandle関数

#pragma region mtlファイル
MaterialData LoadMaterialTemplateFile(const std::string& directoryPath, const std::string& filemane) {
	// 1. 中で必要となる変数の宣言
	MaterialData materialData; //!< 構築するMaterialData
	std::string line; //!< ファイルから読み込んだ1行を格納するもの
	// 2. ファイルを開く
	std::ifstream file(directoryPath + "/" + filemane);
	assert(file.is_open());
	// 3. 実際にファイルを読み、MaterialDataを構築していく
	while (std::getline(file, line)) {
		std::string identifier;
		std::istringstream s(line);
		s >> identifier;

		// identifierに応じた処置
		if (identifier == "map_Kd") {
			std::string textureFilename;
			s >> textureFilename;
			// 連結してファイルパスにする
			materialData.textureFilePath = directoryPath + "/" + textureFilename;
		}
	}
	// 4. MateriarDataを返す
	return materialData;
}
#pragma endregion mtlファイル

#pragma region Objファイル読み込み
ModelData LoadObjFile(const std::string& directoryPath, const std::string& filename) {
	ModelData modelData; //!< 構築するModelData
	std::vector<Vector4> positions; //!< 位置
	std::vector<Vector3> normals; //!< 法線
	std::vector<Vector2> texcoords; //!< テクスチャ座標
	std::string line; //!< ファイルから読み込んだ1行を格納するもの

	std::ifstream file(directoryPath + "/" + filename); //!< ファイルを開く
	assert(file.is_open()); //!< とりあえず開けなかったら止める

	while (std::getline(file, line)) {
		std::string indentifier;
		std::istringstream s(line);
		s >> indentifier; //!< 先頭の識別子を読む

		// identifierに応じた処理
		if (indentifier == "v") {
			Vector4 position;
			s >> position.x >> position.y >> position.z;
			position.z *= -1.0f;
			position.w = 1.0f;
			positions.push_back(position);
		}
		else if (indentifier == "vt") {
			Vector2 texcood;
			s >> texcood.u >> texcood.v;
			texcoords.push_back(texcood);
		}
		else if (indentifier == "vn") {
			Vector3 normal;
			s >> normal.x >> normal.y >> normal.z;
			normal.z *= -1.0f;
			normals.push_back(normal);
		}
		else if (indentifier == "f") {
			VertexData triangle[3];
			// 面は三角形限定。その他は未対応
			for (int32_t faceVertex = 0; faceVertex < 3; ++faceVertex) {
				std::string vertexDefinion;
				s >> vertexDefinion;
				// 頂点の要素へのIndexは「位置 / UV / 法線」で格納されているので、
				// 分解してIndexを取得する
				std::istringstream v(vertexDefinion);
				uint32_t elementIdices[3];
				for (int32_t element = 0; element < 3; ++element) {
					std::string index;
					std::getline(v, index, '/'); //!< /区切りでインデックスを読んでいく
					elementIdices[element] = std::stoi(index);
				}
				// 要素へのIndexから、実際の要素の値を取得して、頂点を構築する
				Vector4 position = positions[elementIdices[0] - 1];
				Vector2 texcoord = texcoords[elementIdices[1] - 1];
				Vector3 normal = normals[elementIdices[2] - 1];
				VertexData vertex = { position,texcoord,normal };
				//modelData.vertices.push_back(vertex);

				triangle[faceVertex] = { position,texcoord,normal };
			}
			// 頂点を逆順することで、回り順を逆にする
			modelData.vertices.push_back(triangle[2]);
			modelData.vertices.push_back(triangle[1]);
			modelData.vertices.push_back(triangle[0]);
		}
		else if (indentifier == "mtllib") {
			// materialTemplateLibraryファイルの名前を取得する
			std::string materialFilename;
			s >> materialFilename;
			// 基本的にobjファイルと同一階層にmtlは存在させているので、ディレクトリ名と、ファイル名を渡す
			modelData.material = LoadMaterialTemplateFile(directoryPath,materialFilename);
		}
	}
	return modelData;
}
#pragma endregion Objファイル読み込み

//Windowsアプリでのエントリーポイント(main関数)
int WINAPI WinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int) {
#pragma region Windows初期化処理
	//出力ウィンドウへの文字出力
	OutputDebugStringA("Hello,DirectX\n");
	//変数から型を推論してくれる
	Log(std::format("enemyHP:{},texturePath:{}\n", 50, 70));

	WNDCLASS wc{};
	//ウィンドウプロシージャ
	wc.lpfnWndProc = WindowProc;
	//ウィンドウクラス名
	wc.lpszClassName = L"CG2WindowClass";
	//インスタンスハンドル
	wc.hInstance = GetModuleHandle(nullptr);
	//カーソル
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

	//ウィンドウクラスを登録する
	RegisterClass(&wc);

	//クライアント領域のサイズ
	const int32_t kClientWidth = 1280;
	const int32_t kClientHeight = 720;

	//ウィンドウサイズを表す構造体にクライアント領域を入れる
	RECT wrc = { 0,0,kClientWidth,kClientHeight };

	//クライアント領域を元に実際のサイズにwrcを変更してもらう
	AdjustWindowRect(&wrc, WS_OVERLAPPEDWINDOW, false);

	CoInitializeEx(nullptr, COINIT_MULTITHREADED);

	//ウィンドウ生成
	HWND hwnd = CreateWindow(
		wc.lpszClassName,		//利用するクラス名
		L"CG2",					//タイトルバーの文字(なんでもよい)
		WS_OVERLAPPEDWINDOW,	//よく見るウィンドウスタイル
		CW_USEDEFAULT,			//表示X座標(Windowsに任せる)
		CW_USEDEFAULT,			//表示Y座標(WindowsOSに任せる)
		wrc.right - wrc.left,	//ウィンドウ横幅
		wrc.bottom - wrc.top,	//ウィンドウ縦幅
		nullptr,				//親ウィンドウハンドル
		nullptr,				//メニューハンドル
		wc.hInstance,			//インスタンスハンドル
		nullptr);				//オプション

	//ウィンドウを表示する
	ShowWindow(hwnd, SW_SHOW);

	//GXGIファクトリー
	IDXGIFactory7* dxgiFactory = nullptr;

	//HRESULTはWindows系のエラーコードであり
	//関数が成功したかどうかSUCCEEDEDマクロで判定する
	HRESULT hr = CreateDXGIFactory(IID_PPV_ARGS(&dxgiFactory));
	//初期化の根本的な部分でエラーが出た場合はプログラムが間違っているか、どうにもならない場合が多いのでassertにしておく
	assert(SUCCEEDED(hr));
	//使用するアダプタ用の変数、最初にnullptrを入れておく
	IDXGIAdapter4* useAdapter = nullptr;
	//よい順にアダプタを頼む
	for (UINT i = 0;
		dxgiFactory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&useAdapter)) != DXGI_ERROR_NOT_FOUND;
		++i) {
		//アダプターの情報を取得する
		DXGI_ADAPTER_DESC3 adapterDesc{};
		hr = useAdapter->GetDesc3(&adapterDesc);
		assert(SUCCEEDED(hr));
		//ソフトウェアアダプタで無ければ採用!
		if (!(adapterDesc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE)) {
			//採用したアダプタの情報をログに出力。wstringの方なので注意
			Log(std::format(L"Use Adapter:{}\n", adapterDesc.Description));
			break;
		}
		useAdapter = nullptr;//ソフトウェアアダプタの場合は見なかったことにする
	}
	//適切なアダプタが見つからなかったので起動できない
	assert(useAdapter != nullptr);

	ID3D12Device* device = nullptr;
	//機能レベルとログ出力用の文字列
	D3D_FEATURE_LEVEL featureLevels[] = {
		D3D_FEATURE_LEVEL_12_2,D3D_FEATURE_LEVEL_12_1,D3D_FEATURE_LEVEL_12_0
	};
	const char* featureLevelString[] = { "12.2","12.1","12.0" };
	//高い順生成できるか試していく
	for (size_t i = 0; i < _countof(featureLevels); ++i) {
		//採用したアダプターでデバイスを生成
		hr = D3D12CreateDevice(useAdapter, featureLevels[i], IID_PPV_ARGS(&device));
		if (SUCCEEDED(hr)) {
			//生成出来たのでログ出力を行ってループを抜ける
			Log(std::format("FeatureLevel :	{}\n", featureLevelString[i]));
			break;
		}
	}
	//デバイスの生成がうまくいかなかったので起動できない
	assert(device != nullptr);
	Log("Complete create D3D12Device!!\n");//初期化完了のログをだす

#ifdef _DEBUG
	ID3D12InfoQueue* infoQueue = nullptr;
	if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
		//やばいエラー時に止まる
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
		//エラー時に止まる
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
		//警告時に止まる
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);
		//抑制するメッセージのID
		D3D12_MESSAGE_ID denyIds[] = {
			//Windows11でのDXGIデバックレイヤーの相互作用バグによるエラーメッセージ
			D3D12_MESSAGE_ID_RESOURCE_BARRIER_MISMATCHING_COMMAND_LIST_TYPE
		};
		//抑制するレベル
		D3D12_MESSAGE_SEVERITY severities[] = { D3D12_MESSAGE_SEVERITY_INFO };
		D3D12_INFO_QUEUE_FILTER filter{};
		filter.DenyList.NumIDs = _countof(denyIds);
		filter.DenyList.pIDList = denyIds;
		filter.DenyList.NumSeverities = _countof(severities);
		filter.DenyList.pSeverityList = severities;
		//指定したメッセージの表示を抑制する
		infoQueue->PushStorageFilter(&filter);
		//解放
		infoQueue->Release();
	}
#endif // _DEBUG


#pragma endregion Windows初期化処理
#pragma region DirectX初期化
	// コマンドキューを生成する
	ID3D12CommandQueue* commandQueue = nullptr;
	D3D12_COMMAND_QUEUE_DESC commandQueueDesc{};
	hr = device->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(&commandQueue));
	// コマンドキューの生成がうまくいかなかったので起動できない
	assert(SUCCEEDED(hr));

	// コマンドアローケータを生成する
	ID3D12CommandAllocator* commandAllocator = nullptr;
	hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator));
	// コマンドアローケータの生成がうまくいかなかったので起動できない
	assert(SUCCEEDED(hr));

	// コマンドリストを生成する
	ID3D12GraphicsCommandList* commandList = nullptr;
	hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator, nullptr, IID_PPV_ARGS(&commandList));
	// コマンドリストの生成がうまくいかなかったので起動できない
	assert(SUCCEEDED(hr));

	// 初期値0でFenceを作る
	ID3D12Fence* fence = nullptr;
	uint64_t fenceValue = 0;
	hr = device->CreateFence(fenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
	assert(SUCCEEDED(hr));

	//FenceのSignalを持つためのイベントを生成する
	HANDLE fenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	assert(SUCCEEDED(hr));

	//スワップチェーンを生成する
	IDXGISwapChain4* swapChain = nullptr;
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
	swapChainDesc.Width = kClientWidth;//画面の幅。ウィンドウのクライアント領域を同じものにしておく
	swapChainDesc.Height = kClientHeight;//画面の高さ。ウィンドウのクライアント領域を同じものにしておく
	swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;//色の形式
	swapChainDesc.SampleDesc.Count = 1;//マルチサンプルしない
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;//描画のターゲットとして利用する
	swapChainDesc.BufferCount = 2;//ダブルバッファ
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;//モニタに写したら、中身を破棄
	//コマンドキュー、ウィンドウハンドル、設定を渡して生成する
	hr = dxgiFactory->CreateSwapChainForHwnd(commandQueue, hwnd, &swapChainDesc, nullptr, nullptr, reinterpret_cast<IDXGISwapChain1**>(&swapChain));
	assert(SUCCEEDED(hr));

	//RTV用のヒープでディスクリプタの数は2。RTVはShaderないで触るような者ではないのでShaderVisibleはfalse
	ID3D12DescriptorHeap* rtvDescriptorHeap = CreateDescriptorHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, false);

	// SRV用のヒープでディスクリプタの数は128。SRVはshaderないで触るものなので、ShaderVisibleはtrue
	ID3D12DescriptorHeap* srvDescriptorHeap = CreateDescriptorHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 128, true);

	//SwapChainからResourceを引っ張ってくる
	ID3D12Resource* swapChainResource[2] = { nullptr };
	hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&swapChainResource[0]));
	//うまく取得できなければ起動できない
	assert(SUCCEEDED(hr));
	hr = swapChain->GetBuffer(1, IID_PPV_ARGS(&swapChainResource[1]));
	assert(SUCCEEDED(hr));

	//RTVの設定
	D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
	rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;//出力結果をSRGBに変換して書き込む
	rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;//2Dテクスチャとして書き込む
	//ディスクリプタの先頭を取得する
	D3D12_CPU_DESCRIPTOR_HANDLE rtvStartHandle = rtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
	//RTVを2つ作るのでディスクリプタを2つ用意
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[2];
	//まず一つ目を作る。一つ目は最初のところを作る。作る場所はこちらで指定してあげる必要がある
	rtvHandles[0] = rtvStartHandle;
	device->CreateRenderTargetView(swapChainResource[0], &rtvDesc, rtvHandles[0]);
	//2つ目のディスクリプタハンドルを得る（自力で）
	rtvHandles[1].ptr = rtvHandles[0].ptr + device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	//2つ目を作る
	device->CreateRenderTargetView(swapChainResource[1], &rtvDesc, rtvHandles[1]);
#pragma endregion DirectX初期化処理
#pragma region dxcCompilerを初期化
	//dxcCompilerを初期化
	IDxcUtils* dxcUtils = nullptr;
	IDxcCompiler3* dxcCompiler = nullptr;
	hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&dxcUtils));
	assert(SUCCEEDED(hr));
	hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxcCompiler));
	assert(SUCCEEDED(hr));

	//現時点でincludeはしないが、includeに対応するための設定を行っている
	IDxcIncludeHandler* includeHandler = nullptr;
	hr = dxcUtils->CreateDefaultIncludeHandler(&includeHandler);
	assert(SUCCEEDED(hr));
#pragma endregion dxcCompilerを初期化
#pragma region 描画初期化処理
	// DescriptorSizeを取得しておく
	const uint32_t descriptorSizeSRV = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const uint32_t descriptorSizeRTV = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	const uint32_t descriptorSizeDSV = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

	//Rootsignature生成	
	D3D12_ROOT_SIGNATURE_DESC descriptionRootSignature{};
	descriptionRootSignature.Flags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	D3D12_DESCRIPTOR_RANGE descriptorRange[1] = {};
	descriptorRange[0].BaseShaderRegister = 0;// 0から始める
	descriptorRange[0].NumDescriptors = 1;// 数は1つ
	descriptorRange[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;// SRVを使う
	descriptorRange[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;// Offsetを自動計算

	D3D12_STATIC_SAMPLER_DESC staticSampler[1] = {};
	staticSampler[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;// バイリニアフィルタ
	staticSampler[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	staticSampler[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	staticSampler[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	staticSampler[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	staticSampler[0].MaxLOD = D3D12_FLOAT32_MAX;
	staticSampler[0].ShaderRegister = 0;// レジスタ番号0を使う
	staticSampler[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;// PixelShaderで使う
	descriptionRootSignature.pStaticSamplers = staticSampler;
	descriptionRootSignature.NumStaticSamplers = _countof(staticSampler);


	//RootParameter生成。
	D3D12_ROOT_PARAMETER rootParamers[4] = {};
	rootParamers[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;//CBVを使う
	rootParamers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;//PixelShaderで使う
	rootParamers[0].Descriptor.ShaderRegister = 0;//レジスタ番号0とバインド

	rootParamers[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;//CBVを使う
	rootParamers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;//PixelShaderで使う
	rootParamers[1].Descriptor.ShaderRegister = 0;//レジスタ番号0とバインド

	rootParamers[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;// DescriptorTableを使う
	rootParamers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;// PixelShaderで使う
	rootParamers[2].DescriptorTable.pDescriptorRanges = descriptorRange;// Tableの中身の配列を指定
	rootParamers[2].DescriptorTable.NumDescriptorRanges = _countof(descriptorRange);// tableで利用する数

	rootParamers[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;	// CBVを使う
	rootParamers[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;	// PixelShaderを使う
	rootParamers[3].Descriptor.ShaderRegister = 1;	// レジスタ番号1を使う

	descriptionRootSignature.pParameters = rootParamers;//ルートパラメータ配列はのポインタ
	descriptionRootSignature.NumParameters = _countof(rootParamers);//配列の長さ

	//シリアライズしてバイナリする
	ID3DBlob* signatureBlob = nullptr;
	ID3DBlob* errorBlob = nullptr;
	hr = D3D12SerializeRootSignature(&descriptionRootSignature,
		D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &errorBlob);
	if (FAILED(hr)) {
		Log(reinterpret_cast<char*>(errorBlob->GetBufferPointer()));
		assert(false);
	}
	//バイナリを元に生成
	ID3D12RootSignature* rootSignature = nullptr;
	hr = device->CreateRootSignature(0, signatureBlob->GetBufferPointer(),
		signatureBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignature));
	assert(SUCCEEDED(hr));

	//InputLayout
	D3D12_INPUT_ELEMENT_DESC inputElementDescs[3] = {};
	inputElementDescs[0].SemanticName = "POSITION";
	inputElementDescs[0].SemanticIndex = 0;
	inputElementDescs[0].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	inputElementDescs[0].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
	inputElementDescs[1].SemanticName = "TEXCOORD";
	inputElementDescs[1].Format = DXGI_FORMAT_R32G32_FLOAT;
	inputElementDescs[1].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
	inputElementDescs[2].SemanticName = "NORMAL";
	inputElementDescs[2].SemanticIndex = 0;
	inputElementDescs[2].Format = DXGI_FORMAT_R32G32B32_FLOAT;
	inputElementDescs[2].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
	
	D3D12_INPUT_LAYOUT_DESC inputLayoutDesc{};
	inputLayoutDesc.pInputElementDescs = inputElementDescs;
	inputLayoutDesc.NumElements = _countof(inputElementDescs);

	//BlendStateの設定
	D3D12_BLEND_DESC blendDesc{};
	//全ての色要素を書き込む
	blendDesc.RenderTarget[0].RenderTargetWriteMask =
		D3D12_COLOR_WRITE_ENABLE_ALL;

	//RasiterzerStateの設定
	D3D12_RASTERIZER_DESC rasterizerDesc{};
	//裏面（時計回り）を表示しない
	rasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
	//三角形の中を塗りつぶす
	rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;

	//Shaderをコンパイルする
	IDxcBlob* vertexShaderBlob = CompileShader(L"Object3d.VS.hlsl",
		L"vs_6_0", dxcUtils, dxcCompiler, includeHandler);
	assert(vertexShaderBlob != nullptr);

	IDxcBlob* pixelShaderBlob = CompileShader(L"Object3d.PS.hlsl",
		L"ps_6_0", dxcUtils, dxcCompiler, includeHandler);
	assert(pixelShaderBlob != nullptr);

	//PSO生成
	D3D12_GRAPHICS_PIPELINE_STATE_DESC graphicPipelineStateDesc{};
	graphicPipelineStateDesc.pRootSignature = rootSignature;//RootSignature
	graphicPipelineStateDesc.InputLayout = inputLayoutDesc;//InputLayout
	graphicPipelineStateDesc.VS = {
		vertexShaderBlob->GetBufferPointer(),
		vertexShaderBlob->GetBufferSize()
	};//vertexShader
	graphicPipelineStateDesc.PS = {
		pixelShaderBlob->GetBufferPointer(),
		pixelShaderBlob->GetBufferSize()
	};//pixelShader
	graphicPipelineStateDesc.BlendState = blendDesc;//BlendState
	graphicPipelineStateDesc.RasterizerState = rasterizerDesc;//Rasterizer
	//書き込むRTVの情報
	graphicPipelineStateDesc.NumRenderTargets = 1;
	graphicPipelineStateDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	//利用するとポロジ（形状）のタイプ、三角形
	graphicPipelineStateDesc.PrimitiveTopologyType =
		D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	//どのように画面に色を打ち込むのか設定（気にしなくてもよい）
	graphicPipelineStateDesc.SampleDesc.Count = 1;
	graphicPipelineStateDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;

	// DepthStencilStateの設定
	D3D12_DEPTH_STENCIL_DESC depthStencilDesc{};
	// Depthの機能を有効化する
	depthStencilDesc.DepthEnable = true;
	// 書き込みをする
	depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	// 比較関数はLessEqual。つまり、近ければ描画される
	depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	// DepthStencilの設定
	graphicPipelineStateDesc.DepthStencilState = depthStencilDesc;
	graphicPipelineStateDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

	//実際に生成
	ID3D12PipelineState* graphicPipelineState = nullptr;
	hr = device->CreateGraphicsPipelineState(&graphicPipelineStateDesc,
		IID_PPV_ARGS(&graphicPipelineState));
	assert(SUCCEEDED(hr));

	// 分割数 球
	const uint32_t kSubdivision = 16;

	//頂点リソースを作成
	ID3D12Resource* vertexResource = CreateBufferResource(device, sizeof(VertexData) * kSubdivision * kSubdivision * 6);
	//頂点バッファビューを作成する
	D3D12_VERTEX_BUFFER_VIEW vertexBufferView{};
	//リソースの先頭のアドレスから使う
	vertexBufferView.BufferLocation = vertexResource->GetGPUVirtualAddress();
	
	//使用するリソースのサイズは頂点3つ分のサイズ
	vertexBufferView.SizeInBytes = sizeof(VertexData) * kSubdivision * kSubdivision * 6;
	//1頂点当たりのサイズ
	vertexBufferView.StrideInBytes = sizeof(VertexData);

	//頂点リソースにデータを書き込む
	VertexData* vertexData = nullptr;
	//書き込むためのアドレスを取得
	vertexResource->Map(0, nullptr, reinterpret_cast<void**>(&vertexData));
	
	// 経度分割1つ分の角度
	const float kLonEvery = static_cast<float>(M_PI) * 2.0f / kSubdivision;
	// 緯度分割1つ分の角度
	const float kLatEvery = static_cast<float>(M_PI) / kSubdivision;

	// 緯度の方向に分割 -M_PI/2 ~ M_PI/2
	for (uint32_t latIndex = 0; latIndex < kSubdivision; ++latIndex) {
		//現在の緯度
		float lat = static_cast<float>(-M_PI) / 2.0f + kLatEvery * latIndex;
		// 経度の方向に分割 0 ~ 2*M_PI
		for (uint32_t lonIndex = 0; lonIndex < kSubdivision; ++lonIndex) {
			uint32_t strat = (latIndex * kSubdivision + lonIndex) * 6;
			// 現在の経度
			float lon = lonIndex * kLonEvery;
			// 頂点データを入力する。基準点a
			// a
			vertexData[strat].position.x = std::cos(lat) * std::cos(lon);
			vertexData[strat].position.y = std::sin(lat);
			vertexData[strat].position.z = std::cos(lat) * std::sin(lon);
			vertexData[strat].position.w = 1.0f;
			vertexData[strat].texcoord.u = float(lonIndex) / float(kSubdivision);
			vertexData[strat].texcoord.v = 1.0f - float(latIndex) / float(kSubdivision);
			vertexData[strat].normal.x = vertexData[strat].position.x;
			vertexData[strat].normal.y = vertexData[strat].position.y;
			vertexData[strat].normal.z = vertexData[strat].position.z;

			// b
			vertexData[strat + 1].position.x = std::cos(lat + kLatEvery) * std::cos(lon);
			vertexData[strat + 1].position.y = std::sin(lat + kLatEvery);
			vertexData[strat + 1].position.z = std::cos(lat + kLatEvery) * std::sin(lon);
			vertexData[strat + 1].position.w = 1.0f;
			vertexData[strat + 1].texcoord.u = float(lonIndex) / float(kSubdivision);
			vertexData[strat + 1].texcoord.v = 1.0f - float(latIndex + 1) / float(kSubdivision);
			vertexData[strat + 1].normal.x = vertexData[strat + 1].position.x;
			vertexData[strat + 1].normal.y = vertexData[strat + 1].position.y;
			vertexData[strat + 1].normal.z = vertexData[strat + 1].position.z;

			// c
			vertexData[strat + 2].position.x = std::cos(lat) * std::cos(lon + kLonEvery);
			vertexData[strat + 2].position.y = std::sin(lat);
			vertexData[strat + 2].position.z = std::cos(lat) * std::sin(lon + kLonEvery);
			vertexData[strat + 2].position.w = 1.0f;
			vertexData[strat + 2].texcoord.u = float(lonIndex + 1) / float(kSubdivision);
			vertexData[strat + 2].texcoord.v = 1.0f - float(latIndex) / float(kSubdivision);
			vertexData[strat + 2].normal.x = vertexData[strat + 2].position.x;
			vertexData[strat + 2].normal.y = vertexData[strat + 2].position.y;
			vertexData[strat + 2].normal.z = vertexData[strat + 2].position.z;

			// c
			vertexData[strat + 3].position.x = std::cos(lat) * std::cos(lon + kLonEvery);
			vertexData[strat + 3].position.y = std::sin(lat);
			vertexData[strat + 3].position.z = std::cos(lat) * std::sin(lon + kLonEvery);
			vertexData[strat + 3].position.w = 1.0f;
			vertexData[strat + 3].texcoord.u = float(lonIndex + 1) / float(kSubdivision);
			vertexData[strat + 3].texcoord.v = 1.0f - float(latIndex) / float(kSubdivision);
			vertexData[strat + 3].normal.x = vertexData[strat + 3].position.x;
			vertexData[strat + 3].normal.y = vertexData[strat + 3].position.y;
			vertexData[strat + 3].normal.z = vertexData[strat + 3].position.z;

			// b
			vertexData[strat + 4].position.x = std::cos(lat + kLatEvery) * std::cos(lon);
			vertexData[strat + 4].position.y = std::sin(lat + kLatEvery);
			vertexData[strat + 4].position.z = std::cos(lat + kLatEvery) * std::sin(lon);
			vertexData[strat + 4].position.w = 1.0f;
			vertexData[strat + 4].texcoord.u = float(lonIndex) / float(kSubdivision);
			vertexData[strat + 4].texcoord.v = 1.0f - float(latIndex + 1) / float(kSubdivision);
			vertexData[strat + 4].normal.x = vertexData[strat + 4].position.x;
			vertexData[strat + 4].normal.y = vertexData[strat + 4].position.y;
			vertexData[strat + 4].normal.z = vertexData[strat + 4].position.z;


			// d
			vertexData[strat + 5].position.x = std::cos(lat + kLatEvery) * std::cos(lon + kLonEvery);
			vertexData[strat + 5].position.y = std::sin(lat + kLatEvery);
			vertexData[strat + 5].position.z = std::cos(lat + kLatEvery) * std::sin(lon + kLonEvery);
			vertexData[strat + 5].position.w = 1.0f;
			vertexData[strat + 5].texcoord.u = float(lonIndex + 1) / float(kSubdivision);
			vertexData[strat + 5].texcoord.v = 1.0f - float(latIndex + 1) / float(kSubdivision);
			vertexData[strat + 5].normal.x = vertexData[strat + 5].position.x;
			vertexData[strat + 5].normal.y = vertexData[strat + 5].position.y;
			vertexData[strat + 5].normal.z = vertexData[strat + 5].position.z;

		}
	}

	//マテリアル用のリソースを作る。今回はcolor1つ分のサイズを用意する
	ID3D12Resource* materialResource = CreateBufferResource(device, sizeof(Material));
	//マテリアルにデータを書き込む
	Material* materialData{};
	//書き込むためのアドレスを取得
	materialResource->Map(0, nullptr, reinterpret_cast<void**>(&materialData));
	// Lightingを有効化する
	materialData->enableLighting = true;
	//今回は赤を書き込んでみる
	materialData->color=Vector4(1.0f, 1.0f, 1.0f, 1.0f);
	// matrixの初期化
	materialData->uvTransform = MakeIdentity4x4();

	//WVP用のリソースを作る。Matrix4x4 1つ分のサイズを用意する
	ID3D12Resource* wvpResource = CreateBufferResource(device, sizeof(TransformationMatrix));
	//データを書き込む
	TransformationMatrix* wvpData = nullptr;
	//書き込むためのアドレスを取得
	wvpResource->Map(0, nullptr, reinterpret_cast<void**>(&wvpData));
	//単位行列を書き込んでおく
	wvpData->world = MakeIdentity4x4();
	wvpData->WVP = MakeIdentity4x4();

	// DirectionalLight用のリソースを作る
	ID3D12Resource* directionalLightResource = CreateBufferResource(device, sizeof(DirectionalLight));
	//データを書き込む
	DirectionalLight* directionalLightData = nullptr;
	//書き込むためのアドレスを取得
	directionalLightResource->Map(0, nullptr, reinterpret_cast<void**>(&directionalLightData));
	// 初期値代入
	directionalLightData->color = { 1.0f,1.0f,1.0f,1.0f };
	directionalLightData->direction = { 0.0f,-1.0f,0.0f };
	directionalLightData->intensiy = 1.0f;

	//Textureを読み込んで転送する
	DirectX::ScratchImage mipImages = LoadTexture("resources/uvChecker.png");
	const DirectX::TexMetadata& metadata = mipImages.GetMetadata();
	ID3D12Resource* textureResourec = CreateTextureResourec(device, metadata);
	UploadTextureData(textureResourec, mipImages);

	// 2枚目のTextureを読み込んで転送する
	DirectX::ScratchImage mipImages2 = LoadTexture("resources/monsterBall.png");
	const DirectX::TexMetadata& matedata2 = mipImages2.GetMetadata();
	ID3D12Resource* textureResource2 = CreateTextureResourec(device, matedata2);
	UploadTextureData(textureResource2, mipImages2);

	// metaDataを基にSRVの設定
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = metadata.format;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; // 2Dテクスチャ
	srvDesc.Texture2D.MipLevels = UINT(metadata.mipLevels);

	// metaDataを基にSRVの設定(二枚目)
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc2{};
	srvDesc2.Format = metadata.format;
	srvDesc2.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc2.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; // 2Dテクスチャ
	srvDesc2.Texture2D.MipLevels = UINT(matedata2.mipLevels);

	// SRVを作成するDescriptorHeapの場所を決める
	D3D12_CPU_DESCRIPTOR_HANDLE textureSrvHandleCPU = GetCPUDescriptorHandle(srvDescriptorHeap, descriptorSizeSRV, 0);
	D3D12_GPU_DESCRIPTOR_HANDLE textureSrvHandleGPU = GetGPUDescriptorHandle(srvDescriptorHeap, descriptorSizeSRV, 0);
	// 先頭はImGuiが使っているのでその次を使う
	textureSrvHandleCPU.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	textureSrvHandleGPU.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	// SRVの生成
	device->CreateShaderResourceView(textureResourec, &srvDesc, textureSrvHandleCPU);

	// SRVを作成するDescriptorHeapの場所を決める（二枚目）
	D3D12_CPU_DESCRIPTOR_HANDLE textureSrvHandleCPU2 = GetCPUDescriptorHandle(srvDescriptorHeap, descriptorSizeSRV, 2);
	D3D12_GPU_DESCRIPTOR_HANDLE textureSrvHandleGPU2 = GetGPUDescriptorHandle(srvDescriptorHeap, descriptorSizeSRV, 2);
	device->CreateShaderResourceView(textureResource2, &srvDesc2, textureSrvHandleCPU2);
	
	// DepthStemcilTextureをウィンドウのサイズで作成
	ID3D12Resource* depthStencilResource = CreateDepthStencilTextureResource(device,kClientWidth,kClientHeight);
	
	// DSV用のヒープでディスクリプタの数は1。DSVはShader内で触るものではないにで、ShaderVisibleはfalse
	ID3D12DescriptorHeap* dsvDescriptorHeap = CreateDescriptorHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);

	// DSVの設定
	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
	dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;// Format。基本的にはResourceに合わせる
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D; // 2DTexture
	
	// DSVHeapの先頭にDSVを作る
	device->CreateDepthStencilView(depthStencilResource, &dsvDesc, dsvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

	// Sprite用の頂点リソースを作る
	ID3D12Resource* vertexResourceSprite = CreateBufferResource(device, sizeof(VertexData) * 6);

	// 頂点バッファビューを作成する
	D3D12_VERTEX_BUFFER_VIEW vertexBufferViewSprite{};
	// リソースの先頭アドレスから使う
	vertexBufferViewSprite.BufferLocation = vertexResourceSprite->GetGPUVirtualAddress();
	// 使用するリソースのサイズは頂点6つ分のサイズ
	vertexBufferViewSprite.SizeInBytes = sizeof(VertexData) * 6;
	// 頂点当たりのサイズ
	vertexBufferViewSprite.StrideInBytes = sizeof(VertexData);

	VertexData* vertexDataSprite = nullptr;
	vertexResourceSprite->Map(0, nullptr, reinterpret_cast<void**>(&vertexDataSprite));

	// 一枚目の三角形
	vertexDataSprite[0].position = { 0.0f,360.0f,0.0f,1.0f };// 左下
	vertexDataSprite[0].texcoord = { 0.0f,1.0f };
	vertexDataSprite[0].normal = { 0.0f,0.0f,-1.0f };
	vertexDataSprite[1].position = { 0.0f,0.0f,0.0f,1.0f };// 左上
	vertexDataSprite[1].texcoord = { 0.0f,0.0f };
	vertexDataSprite[1].normal = { 0.0f,0.0f,-1.0f };
	vertexDataSprite[2].position = { 640.0f,360.0f,0.0f,1.0f };// 右下
	vertexDataSprite[2].texcoord = { 1.0f,1.0f };
	vertexDataSprite[2].normal = { 0.0f,0.0f,-1.0f };
	vertexDataSprite[3].position = { 640.0f,0.0f,0.0f,1.0f };// 右上
	vertexDataSprite[3].texcoord = { 1.0f,0.0f };
	vertexDataSprite[3].normal = { 0.0f,0.0f,-1.0f };

	// Index
	ID3D12Resource* indexResourceSprite = CreateBufferResource(device, sizeof(uint32_t) * 6);
	
	D3D12_INDEX_BUFFER_VIEW indexBufferViewSprite{};
	// リソースの三頭アドレスから使う
	indexBufferViewSprite.BufferLocation = indexResourceSprite->GetGPUVirtualAddress();
	// 使用するリソースのサイズはインデックス6つ分のサイズ
	indexBufferViewSprite.SizeInBytes = sizeof(uint32_t) * 6;
	// インデックスはuint32_tとする
	indexBufferViewSprite.Format = DXGI_FORMAT_R32_UINT;
	// インデックスリソースにデータを書き込む
	uint32_t* indexDataSprite = nullptr;
	indexResourceSprite->Map(0, nullptr, reinterpret_cast<void**>(&indexDataSprite));
	indexDataSprite[0] = 0;
	indexDataSprite[1] = 1;
	indexDataSprite[2] = 2;
	indexDataSprite[3] = 1;
	indexDataSprite[4] = 3;
	indexDataSprite[5] = 2;
	// Sprite用のマテリアルリソースを作る
	ID3D12Resource* materialResourceSprite = CreateBufferResource(device, sizeof(Material));

	//マテリアルにデータを書き込む
	Material* materialDataSprite{};
	//書き込むためのアドレスを取得
	materialResourceSprite->Map(0, nullptr, reinterpret_cast<void**>(&materialDataSprite));
	// Lightingを有効化する
	materialDataSprite->enableLighting = false;
	//今回は赤を書き込んでみる
	materialDataSprite->color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
	// matrixの初期化
	materialData->uvTransform = MakeIdentity4x4();

	// Sprite用のTransformationMarix用のリソースを作る。Matrix4x4 一つ分のサイズを用意する
	ID3D12Resource* transformMatrixResourceSprite = CreateBufferResource(device, sizeof(mat4x4));
	// データを書き込む
	mat4x4* transformationMatrixDataSprite = nullptr;
	// 書き込むためのアドレスを取得
	transformMatrixResourceSprite->Map(0, nullptr, reinterpret_cast<void**>(&transformationMatrixDataSprite));
	// 単位行列を書き込んでおく
	*transformationMatrixDataSprite = MakeIdentity4x4();

	WorldTransform transforSprite{ {1.0f,1.0f,1.0f},{0.0f,0.0f,0.0f},{0.0f,0.0f,0.0f} };

	// モデルの読み込み
	ModelData modelData = LoadObjFile("resources","axis.obj");
	// 頂点リソースを作る
	ID3D12Resource* vertexResourceModel = CreateBufferResource(
		device,
		sizeof(VertexData) * modelData.vertices.size()
	);
	// 頂点バッファビューを作成する
	D3D12_VERTEX_BUFFER_VIEW vertexBufferViewModel{};
	vertexBufferViewModel.BufferLocation = vertexResourceModel->GetGPUVirtualAddress(); //!< リソースの先頭のアドレスから使う
	vertexBufferViewModel.SizeInBytes = UINT(sizeof(VertexData) * modelData.vertices.size()); //!< 使用するリソースのサイズは頂点サイズ
	vertexBufferViewModel.StrideInBytes = sizeof(VertexData); //!< 1頂点辺りのサイズ

	// 頂点リソースにデータを書き込む
	VertexData* vertexDataModel = nullptr;
	vertexResourceModel->Map(0, nullptr, reinterpret_cast<void**>(&vertexDataModel)); //!< 書き込むためのアドレスを取得
	std::memcpy(vertexDataModel, modelData.vertices.data(), sizeof(VertexData)* modelData.vertices.size()); //!< 頂点データをリソースにコピー

	// texcood反転
	for (size_t index = 0; index < modelData.vertices.size(); index++) {
		vertexDataModel[index].texcoord.v = 1.0f - vertexDataModel[index].texcoord.v;
	}

	//WVP用のリソースを作る。Matrix4x4 1つ分のサイズを用意する
	ID3D12Resource* wvpResourceModel = CreateBufferResource(device, sizeof(TransformationMatrix));
	//データを書き込む
	TransformationMatrix* wvpDataModel = nullptr;
	//書き込むためのアドレスを取得
	wvpResourceModel->Map(0, nullptr, reinterpret_cast<void**>(&wvpDataModel));
	//単位行列を書き込んでおく
	wvpDataModel->world = MakeIdentity4x4();
	wvpDataModel->WVP = MakeIdentity4x4();

	// 2枚目のTextureを読み込んで転送する
	DirectX::ScratchImage mipImagesModel = LoadTexture(modelData.material.textureFilePath);
	const DirectX::TexMetadata& matedataModel = mipImagesModel.GetMetadata();
	ID3D12Resource* textureResourceModel = CreateTextureResourec(device, matedataModel);
	UploadTextureData(textureResourceModel, mipImagesModel);

	// metaDataを基にSRVの設定(二枚目)
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDescModel{};
	srvDescModel.Format = metadata.format;
	srvDescModel.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDescModel.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; // 2Dテクスチャ
	srvDescModel.Texture2D.MipLevels = UINT(matedataModel.mipLevels);

	// SRVを作成するDescriptorHeapの場所を決める（二枚目）
	D3D12_CPU_DESCRIPTOR_HANDLE textureSrvHandleCPUModel = GetCPUDescriptorHandle(srvDescriptorHeap, descriptorSizeSRV, 3);
	D3D12_GPU_DESCRIPTOR_HANDLE textureSrvHandleGPUModel = GetGPUDescriptorHandle(srvDescriptorHeap, descriptorSizeSRV, 3);
	device->CreateShaderResourceView(textureResourceModel, &srvDescModel, textureSrvHandleCPUModel);

	//ビューポート
	D3D12_VIEWPORT viewport{};
	//クライアント領域のサイズと一緒にして画面全体を表示
	viewport.Width = kClientWidth;
	viewport.Height = kClientHeight;
	viewport.TopLeftX = 0;
	viewport.TopLeftY = 0;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;

	//シザー矩形
	D3D12_RECT scissorRect{};
	//基本的にビューポートと同じ矩形が構成されるようにする
	scissorRect.left = 0;
	scissorRect.right = kClientWidth;
	scissorRect.top = 0;
	scissorRect.bottom = kClientHeight;


#pragma endregion 描画初期化処理

	
	MSG msg{};

	//Transform変数を作る
	WorldTransform transform{ {0.5f,0.5f,0.5f},{0.0f,0.0f,0.0f},{0.0f,0.0f,0.0f} };

	//camera
	WorldTransform cameraTransform{ {1.0f,1.0f,1.0f},{0.0f,0.0f,0.0f},{0.0f,0.0f,-5.0f} };
	//透視投影
	mat4x4 worldMatrix = MakeAffineMatrix(transform.scale, transform.rotate, transform.translate);
	mat4x4 cameraMatrix = MakeAffineMatrix(cameraTransform.scale, cameraTransform.rotate, cameraTransform.translate);
	mat4x4 viewMatrix = Inverse(cameraMatrix);
	mat4x4 projectionMatrix = MakePerspectiveFovMatrix(0.45f, float(kClientWidth) / float(kClientHeight), 0.1f, 100.0f);
	mat4x4 worldViewProjectionMatrix = Mul(worldMatrix, Mul(viewMatrix, projectionMatrix));
	wvpData->WVP = worldViewProjectionMatrix;

	// Sprite用のWorldViewProjectionMatrixを作る
	mat4x4 worldMatrixSprite = MakeAffineMatrix(transforSprite.scale, transforSprite.rotate, transforSprite.translate);
	mat4x4 viewMatrixSprite = MakeIdentity4x4();
	mat4x4 projectionMatrixSprite = MakeOrthographicMatrix(0.0f, 0.0f, float(kClientWidth), float(kClientHeight), 0.0f, 1000.0f);
	mat4x4 worldViewProjectionMatrixSprite= Mul(worldMatrixSprite, Mul(viewMatrixSprite, projectionMatrixSprite));
	*transformationMatrixDataSprite = worldViewProjectionMatrixSprite;
	
	// UVTransform用
	WorldTransform uvTransformSprite{
		{1.0f,1.0f,1.0f},
		{0.0f,0.0f,0.0f},
		{0.0f,0.0f,0.0f},
	};

	mat4x4 uvMatWorld = MakeAffineMatrix(uvTransformSprite.scale, uvTransformSprite.rotate, uvTransformSprite.translate);
	materialDataSprite->uvTransform = uvMatWorld;
	bool useMonsterBall = true;

	//Transform変数を作る
	WorldTransform transformModel{ {0.5f,0.5f,0.5f},{0.0f,0.0f,0.0f},{0.0f,0.0f,0.0f} };
	mat4x4 worldMatrixModel = MakeAffineMatrix(transformModel.scale, transformModel.rotate, transformModel.translate);
	mat4x4 worldViewProjectionMatrixModel = Mul(worldMatrixModel, Mul(viewMatrix, projectionMatrix));
	wvpDataModel->WVP = worldViewProjectionMatrixModel;

#pragma region ImGuiの初期化
	//ImGuiの初期化。詳細は重要ではないので解説は省略する
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();
	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX12_Init(
		device,
		swapChainDesc.BufferCount,
		rtvDesc.Format,
		srvDescriptorHeap,
		srvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
		srvDescriptorHeap->GetGPUDescriptorHandleForHeapStart()
	);
#pragma endregion ImGuiの初期化
	//ウィンドウのxボタンが押されるまでループ
	while (msg.message != WM_QUIT) {
#pragma region ウィンドウメッセージ処理
		//Windowにメッセージが来ていたら最優先で処理させる
		if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
#pragma endregion ウィンドウメッセージ処理
		else {
#pragma region ImGuiにフレームが始まることを知らせる
			ImGui_ImplDX12_NewFrame();
			ImGui_ImplWin32_NewFrame();
			ImGui::NewFrame();
#pragma endregion ImGuiにフレームが始まることを知らせる
#pragma region DirectX毎フレームの処理
			//ゲームの処理
			//これから書き込むバックバッファのインデックスを取得
			UINT backBufferIndex = swapChain->GetCurrentBackBufferIndex();

			//TransitionBarrierの設定
			D3D12_RESOURCE_BARRIER barrier{};
			//今回のバリアはTransition
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			//Noneにしておく
			barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
			//バリアを張る対象のリソース。現在のバックバッファに対して行う
			barrier.Transition.pResource = swapChainResource[backBufferIndex];
			//遷移前(現在)のResourceState
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
			//遷移後のResourceSteta
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			//TransitionBarrierを張る
			commandList->ResourceBarrier(1, &barrier);

			transform.rotate.y += 0.03f;
			mat4x4 worldMatrix = MakeAffineMatrix(transform.scale, transform.rotate, transform.translate);
			wvpData->world = worldMatrix;

			Material material = *materialData;
			// 開発用UIの処理
			ImGui::Begin("material");
			ImGui::DragFloat3("materialData", &material.color.x, 0.01f);
			ImGui::DragFloat3("materialDataSprite", &transforSprite.translate.x, 1.0f);
			ImGui::End();
			
			*materialData = material;
			worldMatrixSprite = MakeAffineMatrix(transforSprite.scale, transforSprite.rotate, transforSprite.translate);
			worldViewProjectionMatrixSprite = Mul(worldMatrixSprite, Mul(viewMatrixSprite, projectionMatrixSprite));
			*transformationMatrixDataSprite = worldViewProjectionMatrixSprite;

			ImGui::Begin("camera");
			ImGui::DragFloat3("cameraRotate", &cameraTransform.rotate.x, 0.01f);
			ImGui::DragFloat3("cameraTransform", &cameraTransform.translate.x, 0.01f);
			ImGui::End();
			cameraMatrix = MakeAffineMatrix(cameraTransform.scale, cameraTransform.rotate, cameraTransform.translate);
			viewMatrix = Inverse(cameraMatrix);
			projectionMatrix = MakePerspectiveFovMatrix(0.45f, float(kClientWidth) / float(kClientHeight), 0.1f, 100.0f);
			worldViewProjectionMatrix = Mul(worldMatrix, Mul(viewMatrix, projectionMatrix));
			wvpData->WVP = worldViewProjectionMatrix;

			ImGui::Begin("flag");
			ImGui::Checkbox("useMonsterBall", &useMonsterBall);
			ImGui::End();

			ImGui::Begin("directionalLight");
			ImGui::DragFloat3("color", &directionalLightData->color.x, 0.01f);
			ImGui::DragFloat3("direction", &directionalLightData->direction.x, 0.01f);
			ImGui::DragFloat("direction", &directionalLightData->intensiy, 0.01f);
			ImGui::End();

			ImGui::Begin("UV");
			ImGui::DragFloat2("UVTarnslate", &uvTransformSprite.translate.x, 0.01f,-10.0f,10.0f);
			ImGui::DragFloat2("UVScale", &uvTransformSprite.scale.x, 0.01f, -10.0f, 10.0f);
			ImGui::DragFloat("UVRotate", &uvTransformSprite.rotate.z, 0.01f, -10.0f, 10.0f);
			ImGui::End();
			mat4x4 uvMatWorld = MakeAffineMatrix(uvTransformSprite.scale, uvTransformSprite.rotate, uvTransformSprite.translate);
			materialDataSprite->uvTransform = uvMatWorld;

			ImGui::Begin("model");
			ImGui::DragFloat3("Tarnslate", &transformModel.translate.x, 0.01f, -10.0f, 10.0f);
			ImGui::DragFloat3("Scale", &transformModel.scale.x, 0.01f, -10.0f, 10.0f);
			ImGui::DragFloat3("Rotate", &transformModel.rotate.x, 0.01f, -10.0f, 10.0f);
			ImGui::End();
			worldMatrixModel = MakeAffineMatrix(transformModel.scale, transformModel.rotate, transformModel.translate);
			worldViewProjectionMatrixModel = Mul(worldMatrixModel, Mul(viewMatrix, projectionMatrix));
			wvpDataModel->WVP = worldViewProjectionMatrixModel;

			//描画先のRTVとDSVを設定する
			D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = GetCPUDescriptorHandle(dsvDescriptorHeap, descriptorSizeDSV, 0);
			commandList->OMSetRenderTargets(1, &rtvHandles[backBufferIndex], false, &dsvHandle);
			// 指定した深度で画面全体をクリアする
			commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
			//指定した色で画面全体をクリアする
			float clearColor[] = { 0.1f,0.25f,0.5f,1.0f };//青っぽい色(背景)
			commandList->ClearRenderTargetView(rtvHandles[backBufferIndex], clearColor, 0, nullptr);

			// 描画用のDescriptorHeapの設定
			ID3D12DescriptorHeap* descriptorHeaps[] = { srvDescriptorHeap };
			commandList->SetDescriptorHeaps(1, descriptorHeaps);

			commandList->RSSetViewports(1, &viewport);//Viewportを設定
			commandList->RSSetScissorRects(1, &scissorRect);//Scissorを設定

			//RootSignatureを設定。PSOに設定しているけど別途設定が必要
			commandList->SetGraphicsRootSignature(rootSignature);
			commandList->SetPipelineState(graphicPipelineState);//PSOを設定
			commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			
			// マテリアルCBufferの場所を設定
			commandList->SetGraphicsRootConstantBufferView(0, materialResource->GetGPUVirtualAddress());
			// wvp用のCBufferの場所を設定
			commandList->SetGraphicsRootConstantBufferView(1, wvpResource->GetGPUVirtualAddress());
			// DirectionalLight用のCBufferの場所を設定
			commandList->SetGraphicsRootConstantBufferView(3, directionalLightResource->GetGPUVirtualAddress());
			// SRVのDescriptorTabkeの先頭を設定。2はrootParam[2]である
			commandList->SetGraphicsRootDescriptorTable(2, useMonsterBall ? textureSrvHandleGPU2 : textureSrvHandleGPU);
			commandList->IASetVertexBuffers(0, 1, &vertexBufferView);//VBVを設定
			//描画!(DrawCall/ドローコール)。3頂点で1つのインスタンス。インスタンスについては今後
			//commandList->DrawInstanced(kSubdivision * kSubdivision * 6, 1, 0, 0);
			
			// モデル用
			// wvp用のCBufferの場所を設定
			commandList->SetGraphicsRootConstantBufferView(1, wvpResourceModel->GetGPUVirtualAddress());
			commandList->IASetVertexBuffers(0, 1, &vertexBufferViewModel);//VBVを設定
			commandList->SetGraphicsRootDescriptorTable(2,textureSrvHandleGPUModel);
			//描画!(DrawCall/ドローコール)。3頂点で1つのインスタンス。インスタンスについては今後
			commandList->DrawInstanced(UINT(modelData.vertices.size()), 1, 0, 0);

			// Spriteの描画。変更が必要なものだけに変更する
			commandList->SetGraphicsRootDescriptorTable(2, textureSrvHandleGPU);
			commandList->IASetVertexBuffers(0, 1, &vertexBufferViewSprite);
			commandList->IASetIndexBuffer(&indexBufferViewSprite);
			//マテリアルCBufferの場所を設定
			commandList->SetGraphicsRootConstantBufferView(0, materialResourceSprite->GetGPUVirtualAddress());
			// TransformmationMatrixBufferの場所を設定
			commandList->SetGraphicsRootConstantBufferView(1, transformMatrixResourceSprite->GetGPUVirtualAddress());
			//描画!(DrawCall/ドローコール)。3頂点で1つのインスタンス。インスタンスについては今後
			//commandList->DrawIndexedInstanced(6, 1, 0, 0, 0);

			// ImGuiの内部コマンドを生成する
			ImGui::Render();
			// 実際のcommandListのImGuiの描画コマンドを積む
			ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);

			//画面に描く処理は全て終わり、画面に映すので、状態を遷移
			//今回はRebderTargetからPresentにする
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

			//TransitionBarrierを張る
			commandList->ResourceBarrier(1, &barrier);

			//コマンドリストの内容を確定させる。全てのコマンドを積んでからCloseすること
			hr = commandList->Close();
			assert(SUCCEEDED(hr));
			//GPUにコマンドリストの実行を行わせる
			ID3D12CommandList* commandLists[] = { commandList };
			commandQueue->ExecuteCommandLists(1, commandLists);
			//GPUとOSに画面の交換を行うよう通知する
			swapChain->Present(1, 0);

			//Fenceの値を更新
			fenceValue++;
			//GPUがここまでたどり着いた時に、Fenceの値に代入するようにSignalを送る
			commandQueue->Signal(fence, fenceValue);

			//Fenceの値が指定したSignal値にたどりついているか確認する
			//GetCompletedValueの初期値はFence制作時に渡した初期値
			if (fence->GetCompletedValue() < fenceValue) {
				//指定したSignalにたどり着いていないので、たどり着くまで待つようにイベントを指定する
				fence->SetEventOnCompletion(fenceValue, fenceEvent);
				//イベント待つ
				WaitForSingleObject(fenceEvent, INFINITE);
			}
			//次のフレーム用のコマンドリストを準備
			hr = commandAllocator->Reset();
			assert(SUCCEEDED(hr));
			hr = commandList->Reset(commandAllocator, nullptr);
			assert(SUCCEEDED(hr));
		}
#pragma endregion DirectX毎フレームの処理
	}
#pragma region 解放処理
	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	textureResourceModel->Release();
	transformMatrixResourceSprite->Release();
	materialResourceSprite->Release();
	indexResourceSprite->Release();
	vertexResourceModel->Release();
	vertexResourceSprite->Release();
	depthStencilResource->Release();
	textureResource2->Release();
	textureResourec->Release();
	wvpResource->Release();
	materialResource->Release();
	vertexResource->Release();
	graphicPipelineState->Release();
	if (errorBlob) {
		errorBlob->Release();
	}
	rootSignature->Release();
	pixelShaderBlob->Release();
	vertexShaderBlob->Release();
	CloseHandle(fenceEvent);
	fence->Release();
	dsvDescriptorHeap->Release();
	rtvDescriptorHeap->Release();
	srvDescriptorHeap->Release();
	swapChainResource[0]->Release();
	swapChainResource[1]->Release();
	swapChain->Release();
	commandList->Release();
	commandAllocator->Release();
	commandQueue->Release();
	device->Release();
	useAdapter->Release();
	dxgiFactory->Release();
	CoUninitialize();
#ifdef _DEBUG
	//debugController->Release();
#endif // _DEBUG
	CloseWindow(hwnd);
	//リリースリークチェック
	IDXGIDebug1* debug;
	if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&debug)))) {
		debug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
		debug->ReportLiveObjects(DXGI_DEBUG_APP, DXGI_DEBUG_RLO_ALL);
		debug->ReportLiveObjects(DXGI_DEBUG_D3D12, DXGI_DEBUG_RLO_ALL);
		debug->Release();
	}

	EnableShaderBasedValidation();
#pragma endregion 解放処理
	return 0;
}