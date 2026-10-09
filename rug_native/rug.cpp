#define RUG_EXPORTS
#include "rug.h"

#include "cloth.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {

constexpr float kLiftHeight = 60.0f;
constexpr float kClickLift = 100.0f;
constexpr float kLiftRamp = 0.3f;
constexpr int   kColumns = 44;
constexpr bool  kDesktopShadow = false;

struct Vtx { float pos[3]; float nrm[3]; float uv[2]; };
struct Cb {
    float lightDir[4];
    float viewSize[2];
    float rugSize[2];
    float lightK[2];
    float camD;
    float shadowStrength;
    float floorZ;
    float pad[3];
};

const char* kShader = R"HLSL(
cbuffer Cb : register(b0)
{
    float4 lightDir;
    float2 viewSize;
    float2 rugSize;
    float2 lightK;
    float  camD;
    float  shadowStrength;
    float  floorZ;
    float3 _pad;
};

Texture2D    rugTex    : register(t0);
Texture2D    shadowMap : register(t1);
SamplerState rugSamp   : register(s0);
SamplerState pointSamp : register(s1);

static const float2 poisson[16] = {
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725),
    float2(-0.09418410, -0.92938870), float2(0.34495938, 0.29387760),
    float2(-0.91588581, 0.45771432),  float2(-0.81544232, -0.87912464),
    float2(-0.38277543, 0.27676845),  float2(0.97484398, 0.75648379),
    float2(0.44323325, -0.97511554),  float2(0.53742981, -0.47373420),
    float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507),  float2(-0.81409955, 0.91437590),
    float2(0.19984126, 0.78641367),   float2(0.14383161, -0.14100790)
};

float depthFromZ(float z) { return saturate(0.5 - z / 4000.0); }

float2 toTC(float2 f) { return float2(f.x / viewSize.x, 1.0 - f.y / viewSize.y); }

float ign(float2 px) { return frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715)))); }

float shadowAt(float3 p, float2 gp, float bias, float2 px)
{
    float2 f = p.xy - lightK * p.z;
    float a = ign(px) * 6.2831853;
    float2x2 R = float2x2(cos(a), -sin(a), sin(a), cos(a));

    float sumD = 0.0, nB = 0.0;
    [unroll] for (int i = 0; i < 16; i++)
    {
        float2 o = mul(R, poisson[i]) * 34.0;
        float zr = p.z + dot(gp, o);
        float occ = shadowMap.SampleLevel(pointSamp, toTC(f + o), 0).r;
        if (occ > zr + bias) { sumD += occ - zr; nB += 1.0; }
    }
    if (nB < 0.5) return 0.0;

    float r = clamp(2.5 + (sumD / nB) * 0.2, 2.5, 30.0);
    float sh = 0.0;
    [unroll] for (int j = 0; j < 16; j++)
    {
        float2 o = mul(R, poisson[j]) * r;
        float zr = p.z + dot(gp, o);
        float occ = shadowMap.SampleLevel(pointSamp, toTC(f + o), 0).r;
        sh += (occ > zr + bias) ? 1.0 : 0.0;
    }
    return sh / 16.0;
}

struct SIn  { float3 pos : POSITION; };
struct SOut { float4 sv : SV_POSITION; float z : TEXCOORD0; };

SOut ShadowVS(SIn i)
{
    float2 f = i.pos.xy - lightK * i.pos.z;
    float2 c = viewSize * 0.5;
    SOut o;
    o.sv = float4((f - c) / c, depthFromZ(i.pos.z), 1.0);
    o.z = i.pos.z;
    return o;
}

float ShadowPS(SOut i) : SV_TARGET { return i.z; }

float4 FloorVS(uint id : SV_VertexID) : SV_POSITION
{
    float x = (id == 1) ? 3.0 : -1.0;
    float y = (id == 2) ? 3.0 : -1.0;
    return float4(x, y, 0.0, 1.0);
}

float4 FloorPS(float4 sp : SV_POSITION) : SV_TARGET
{
    float2 xy = float2(sp.x, viewSize.y - sp.y);
    float s = shadowAt(float3(xy, floorZ), float2(0.0, 0.0), 0.5, sp.xy);
    float a = s * shadowStrength;
    return float4(float3(0.035, 0.02, 0.01) * a, a);
}

struct VIn  { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
struct VOut { float4 sv : SV_POSITION; float3 world : TEXCOORD1; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };

VOut VSMain(VIn i)
{
    float2 c = viewSize * 0.5;
    float w = (camD - i.pos.z) / camD;
    VOut o;
    o.sv    = float4((i.pos.xy - c) / c, depthFromZ(i.pos.z) * w, w);
    o.world = i.pos;
    o.nrm   = i.nrm;
    o.uv    = i.uv;
    return o;
}

float4 PSMain(VOut i, bool front : SV_IsFrontFace, float4 sp : SV_POSITION) : SV_TARGET
{
    float3 Ng = normalize(i.nrm);
    float3 N  = front ? Ng : -Ng;
    float3 col = rugTex.Sample(rugSamp, i.uv).rgb;

    float h = dot(col, float3(0.299, 0.587, 0.114));
    float2 g = float2(ddx(h), -ddy(h)) * (front ? 1.6 : 0.6);
    float3 Nd = normalize(N - float3(g, 0.0));

    if (!front)
    {
        float gray = dot(col, float3(0.299, 0.587, 0.114));
        col = lerp(col, float3(gray, gray, gray), 0.4) * 0.8 + float3(0.05, 0.04, 0.03);
        float2 k = frac(i.uv * float2(260.0, 175.0));
        float knot = smoothstep(0.0, 0.18, min(min(k.x, 1.0 - k.x), min(k.y, 1.0 - k.y)));
        col *= lerp(0.86, 1.0, knot);
    }

    float2 e2 = min(i.uv, 1.0 - i.uv) * rugSize;
    float e = min(e2.x, e2.y);
    col *= lerp(0.62, 1.0, smoothstep(0.4, 3.0, e));

    float3 L = normalize(lightDir.xyz);
    float amb = 0.28, kd = 0.95;
    float shade = (amb + kd * max(dot(Nd, L), 0.0)) / (amb + kd * L.z);

    float2 gp = float2(0.0, 0.0);
    float bias = 1.5;
    if (abs(Ng.z) > 0.2)
    {
        float2 gg = -Ng.xy / Ng.z;
        float den = 1.0 - dot(gg, lightK);
        if (abs(den) > 0.25) gp = clamp(gg / den, -3.0, 3.0);
        else bias = 5.0;
    }
    else
    {
        bias = 5.0;
    }
    float sh = shadowAt(i.world, gp, bias, sp.xy);
    shade *= 1.0 - 0.55 * sh;

    return float4(min(col * min(shade, 1.3), 1.0), 1.0);
}
)HLSL";

struct State {
    int w = 0, h = 0;
    bool held = false;
    Camera camera;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11Texture2D* rt = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11Texture2D* depth = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11Texture2D* staging = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* vb = nullptr;
    ID3D11Buffer* ib = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11RasterizerState* rs = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11SamplerState* pointSampler = nullptr;
    ID3D11VertexShader* shadowVs = nullptr;
    ID3D11PixelShader* shadowPs = nullptr;
    ID3D11VertexShader* floorVs = nullptr;
    ID3D11PixelShader* floorPs = nullptr;
    ID3D11Texture2D* shadowTex = nullptr;
    ID3D11RenderTargetView* shadowRtv = nullptr;
    ID3D11Texture2D* shadowDepth = nullptr;
    ID3D11DepthStencilView* shadowDsv = nullptr;
    ID3D11ShaderResourceView* shadowSrv = nullptr;
    ID3D11BlendState* blendPremul = nullptr;
    ID3D11DepthStencilState* depthOn = nullptr;
    ID3D11DepthStencilState* depthOff = nullptr;
    ID3D11Texture2D* rugTex = nullptr;
    ID3D11ShaderResourceView* rugSrv = nullptr;
    int texW = 1, texH = 1;
    int ibCount = 0;

    std::unique_ptr<HeightField> field;
    std::unique_ptr<Cloth> cloth;
    std::vector<Vtx> verts;
    bool dragging = false;
    V3 dragFrom{}, dragTo{};
    V2 downScreen{};
    V2 grabBase{};
    float liftFrom = 0.0f, liftTo = 0.0f;
    float rampT = 0.0f;
    float accum = 0.0f;
};

State S;

template <class T>
void Release(T*& ptr) {
    if (ptr) { ptr->Release(); ptr = nullptr; }
}

V2 ToV2(float x, float y) { return { x, y }; }

}

extern "C" void rug_shutdown(void) {
    S.cloth.reset();
    S.field.reset();
    Release(S.rugSrv);
    Release(S.rugTex);
    Release(S.sampler);
    Release(S.pointSampler);
    Release(S.shadowVs);
    Release(S.shadowPs);
    Release(S.floorVs);
    Release(S.floorPs);
    Release(S.shadowSrv);
    Release(S.shadowDsv);
    Release(S.shadowDepth);
    Release(S.shadowRtv);
    Release(S.shadowTex);
    Release(S.blendPremul);
    Release(S.depthOn);
    Release(S.depthOff);
    Release(S.rs);
    Release(S.ib);
    Release(S.vb);
    Release(S.cb);
    Release(S.layout);
    Release(S.vs);
    Release(S.ps);
    Release(S.staging);
    Release(S.dsv);
    Release(S.depth);
    Release(S.rtv);
    Release(S.rt);
    Release(S.ctx);
    Release(S.dev);
    S.verts.clear();
    S.ibCount = 0;
    S.dragging = false;
}

extern "C" int rug_init(int w, int h) {
    rug_shutdown();
    S.w = w;
    S.h = h;
    S.camera.size = { float(w), float(h) };
    S.field = std::make_unique<HeightField>(S.camera.size);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    D3D_FEATURE_LEVEL level;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &S.dev, &level, &S.ctx)))
        return 0;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(S.dev->CreateTexture2D(&td, nullptr, &S.rt))) return 0;
    if (FAILED(S.dev->CreateRenderTargetView(S.rt, nullptr, &S.rtv))) return 0;

    D3D11_TEXTURE2D_DESC dd = td;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(S.dev->CreateTexture2D(&dd, nullptr, &S.depth))) return 0;
    if (FAILED(S.dev->CreateDepthStencilView(S.depth, nullptr, &S.dsv))) return 0;

    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(S.dev->CreateTexture2D(&sd, nullptr, &S.staging))) return 0;

    auto compile = [](const char* entry, const char* target) -> ID3DBlob* {
        ID3DBlob* blob = nullptr;
        ID3DBlob* err = nullptr;
        HRESULT hr = D3DCompile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr,
                                entry, target, 0, 0, &blob, &err);
        if (FAILED(hr) && err) std::fprintf(stderr, "shader %s: %s\n", entry, static_cast<const char*>(err->GetBufferPointer()));
        if (err) err->Release();
        return SUCCEEDED(hr) ? blob : nullptr;
    };
    ID3DBlob* vsBlob = compile("VSMain", "vs_5_0");
    ID3DBlob* psBlob = compile("PSMain", "ps_5_0");
    ID3DBlob* shVsBlob = compile("ShadowVS", "vs_5_0");
    ID3DBlob* shPsBlob = compile("ShadowPS", "ps_5_0");
    ID3DBlob* flVsBlob = compile("FloorVS", "vs_5_0");
    ID3DBlob* flPsBlob = compile("FloorPS", "ps_5_0");
    if (!vsBlob || !psBlob || !shVsBlob || !shPsBlob || !flVsBlob || !flPsBlob) return 0;

    S.dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &S.vs);
    S.dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &S.ps);
    S.dev->CreateVertexShader(shVsBlob->GetBufferPointer(), shVsBlob->GetBufferSize(), nullptr, &S.shadowVs);
    S.dev->CreatePixelShader(shPsBlob->GetBufferPointer(), shPsBlob->GetBufferSize(), nullptr, &S.shadowPs);
    S.dev->CreateVertexShader(flVsBlob->GetBufferPointer(), flVsBlob->GetBufferSize(), nullptr, &S.floorVs);
    S.dev->CreatePixelShader(flPsBlob->GetBufferPointer(), flPsBlob->GetBufferSize(), nullptr, &S.floorPs);

    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    S.dev->CreateInputLayout(layoutDesc, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &S.layout);
    vsBlob->Release();
    psBlob->Release();
    shVsBlob->Release();
    shPsBlob->Release();
    flVsBlob->Release();
    flPsBlob->Release();

    D3D11_TEXTURE2D_DESC shd{};
    shd.Width = w;
    shd.Height = h;
    shd.MipLevels = 1;
    shd.ArraySize = 1;
    shd.Format = DXGI_FORMAT_R32_FLOAT;
    shd.SampleDesc.Count = 1;
    shd.Usage = D3D11_USAGE_DEFAULT;
    shd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(S.dev->CreateTexture2D(&shd, nullptr, &S.shadowTex))) return 0;
    if (FAILED(S.dev->CreateRenderTargetView(S.shadowTex, nullptr, &S.shadowRtv))) return 0;
    if (FAILED(S.dev->CreateShaderResourceView(S.shadowTex, nullptr, &S.shadowSrv))) return 0;
    D3D11_TEXTURE2D_DESC sdd = shd;
    sdd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    sdd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(S.dev->CreateTexture2D(&sdd, nullptr, &S.shadowDepth))) return 0;
    if (FAILED(S.dev->CreateDepthStencilView(S.shadowDepth, nullptr, &S.shadowDsv))) return 0;

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(S.dev->CreateBlendState(&bd, &S.blendPremul))) return 0;

    D3D11_DEPTH_STENCIL_DESC dsOn{};
    dsOn.DepthEnable = TRUE;
    dsOn.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsOn.DepthFunc = D3D11_COMPARISON_LESS;
    if (FAILED(S.dev->CreateDepthStencilState(&dsOn, &S.depthOn))) return 0;
    D3D11_DEPTH_STENCIL_DESC dsOff{};
    dsOff.DepthEnable = FALSE;
    dsOff.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsOff.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(S.dev->CreateDepthStencilState(&dsOff, &S.depthOff))) return 0;

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(Cb);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(S.dev->CreateBuffer(&cbd, nullptr, &S.cb))) return 0;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.FrontCounterClockwise = TRUE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    if (FAILED(S.dev->CreateRasterizerState(&rd, &S.rs))) return 0;

    D3D11_SAMPLER_DESC samp{};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samp.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(S.dev->CreateSamplerState(&samp, &S.sampler))) return 0;
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(S.dev->CreateSamplerState(&samp, &S.pointSampler))) return 0;

    return 1;
}

extern "C" int rug_load_texture(const wchar_t* path) {
    if (!S.dev || !path) return 0;

    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    int ok = 0;
    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory))))
            break;
        if (FAILED(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, &decoder)))
            break;
        if (FAILED(decoder->GetFrame(0, &frame))) break;
        if (FAILED(factory->CreateFormatConverter(&converter))) break;
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                         nullptr, 0, WICBitmapPaletteTypeCustom)))
            break;

        UINT w = 0, h = 0;
        converter->GetSize(&w, &h);
        std::vector<unsigned char> px(size_t(w) * h * 4);
        if (FAILED(converter->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data()))) break;

        Release(S.rugSrv);
        Release(S.rugTex);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = px.data();
        init.SysMemPitch = w * 4;
        if (FAILED(S.dev->CreateTexture2D(&td, &init, &S.rugTex))) break;
        if (FAILED(S.dev->CreateShaderResourceView(S.rugTex, nullptr, &S.rugSrv))) break;
        S.texW = int(w);
        S.texH = int(h);
        ok = 1;
    } while (false);

    Release(converter);
    Release(frame);
    Release(decoder);
    Release(factory);
    return ok;
}

static float unitScale() { return float(S.w) / 2560.0f; }
static float liftH() { return kLiftHeight * unitScale(); }

extern "C" void rug_rebuild(float scale, float drop) {
    if (!S.dev) return;
    float width = S.camera.size.x * scale;
    float aspect = float(S.texH) / float(S.texW);
    S.cloth = std::make_unique<Cloth>(width, width * aspect, kColumns);
    S.cloth->ground = S.field.get();
    S.cloth->unitScale = unitScale();
    S.cloth->layout(S.camera.center(), 0.0f, drop * unitScale());
    S.cloth->updateDerived(S.camera);
    S.dragging = false;

    Release(S.vb);
    Release(S.ib);
    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth = UINT(S.cloth->count * sizeof(Vtx));
    vbd.Usage = D3D11_USAGE_DYNAMIC;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    S.dev->CreateBuffer(&vbd, nullptr, &S.vb);

    D3D11_BUFFER_DESC ibd{};
    ibd.ByteWidth = UINT(S.cloth->indices.size() * sizeof(uint32_t));
    ibd.Usage = D3D11_USAGE_DEFAULT;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ibData{};
    ibData.pSysMem = S.cloth->indices.data();
    S.dev->CreateBuffer(&ibd, &ibData, &S.ib);
    S.ibCount = int(S.cloth->indices.size());
    S.verts.assign(S.cloth->count, Vtx{});
}

extern "C" void rug_set_bumps(const float* data, int count) {
    std::vector<Bump> bumps;
    bumps.reserve(count);
    for (int i = 0; i < count; ++i) {
        const float* b = data + i * 5;
        bumps.push_back({ { b[0], b[1] }, { b[2], b[3] }, b[4] * unitScale() });
    }
    if (S.field) S.field->setBumps(bumps);
}

extern "C" void rug_step(float dt) {
    if (!S.cloth) return;

    const float fixed = 1.0f / 60.0f;
    S.accum += std::clamp(dt, 0.0f, 0.25f);
    int due = 0;
    for (float a = S.accum; a >= fixed && due < 4; a -= fixed) ++due;
    int steps = 0;
    while (S.accum >= fixed && steps < 4) {
        if (S.dragging) {

            float t = float(steps + 1) / float(std::max(due, 1));
            V3 g = S.dragFrom + (S.dragTo - S.dragFrom) * t;

            S.rampT = std::min(kLiftRamp, S.rampT + fixed);
            g.z = S.liftFrom + (S.liftTo - S.liftFrom) * (S.rampT / kLiftRamp);
            S.cloth->grabTarget = g;
        }
        if (S.field) S.field->animate(fixed);
        S.cloth->step(fixed);
        S.accum -= fixed;
        ++steps;
    }
    if (steps == 4) S.accum = 0.0f;
    if (steps > 0 && S.dragging) S.dragFrom = S.dragTo;

    if (S.held && !S.cloth->pos.empty() && std::fabs(S.cloth->pos[0].z - liftH()) > 0.5f)
        S.cloth->setHeld(true, liftH());
    if (steps > 0) S.cloth->updateDerived(S.camera);
}

static void activeRect(int out[4]) {
    int left = 0, top = 0, right = S.w, bottom = S.h;
    if (S.cloth) {
        const float pad = 96.0f;
        const V2 lo = S.cloth->bboxMin, hi = S.cloth->bboxMax;
        left   = std::clamp(int(std::floor(lo.x - pad)), 0, S.w);
        right  = std::clamp(int(std::ceil(hi.x + pad)), 0, S.w);
        top    = std::clamp(int(std::floor(S.h - hi.y - pad)), 0, S.h);
        bottom = std::clamp(int(std::ceil(S.h - lo.y + pad)), 0, S.h);
    }
    out[0] = left; out[1] = top; out[2] = std::max(0, right - left); out[3] = std::max(0, bottom - top);
}

extern "C" void rug_render(unsigned char* dst, int stride, int* rect) {
    if (!S.ctx || !dst) return;

    static int prevRc[4] = { 0, 0, 0, 0 };
    int rc[4] = { 0, 0, 0, 0 };
    activeRect(rc);
    int drawRc[4] = { rc[0], rc[1], rc[2], rc[3] };
    if (prevRc[2] > 0 && prevRc[3] > 0) {
        const int l = std::min(rc[0], prevRc[0]);
        const int t = std::min(rc[1], prevRc[1]);
        const int r = std::max(rc[0] + rc[2], prevRc[0] + prevRc[2]);
        const int b = std::max(rc[1] + rc[3], prevRc[1] + prevRc[3]);
        drawRc[0] = l; drawRc[1] = t; drawRc[2] = r - l; drawRc[3] = b - t;
    }
    std::memcpy(prevRc, rc, sizeof(rc));
    if (rect) std::memcpy(rect, drawRc, sizeof(drawRc));

    if (S.cloth && S.vb) {
        const Cloth& c = *S.cloth;
        for (int i = 0; i < c.count; ++i) {
            Vtx& v = S.verts[i];
            v.pos[0] = c.pos[i].x; v.pos[1] = c.pos[i].y; v.pos[2] = c.pos[i].z;
            v.nrm[0] = c.nrm[i].x; v.nrm[1] = c.nrm[i].y; v.nrm[2] = c.nrm[i].z;
            v.uv[0] = c.uvs[i].x;  v.uv[1] = c.uvs[i].y;
        }
        D3D11_MAPPED_SUBRESOURCE m{};
        S.ctx->Map(S.vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        std::memcpy(m.pData, S.verts.data(), S.verts.size() * sizeof(Vtx));
        S.ctx->Unmap(S.vb, 0);
    }

    Cb cb{};
    {
        float lx = -0.5f, ly = 0.58f, lz = 0.64f;
        float l = std::sqrt(lx * lx + ly * ly + lz * lz);
        cb.lightDir[0] = lx / l; cb.lightDir[1] = ly / l; cb.lightDir[2] = lz / l; cb.lightDir[3] = 0.0f;
        float sx = -0.3f, sy = 0.38f, sz = 0.87f;
        float s = std::sqrt(sx * sx + sy * sy + sz * sz);
        cb.lightK[0] = (sx / s) / (sz / s);
        cb.lightK[1] = (sy / s) / (sz / s);
    }
    cb.viewSize[0] = S.camera.size.x; cb.viewSize[1] = S.camera.size.y;
    cb.camD = S.camera.D();
    cb.shadowStrength = 0.0f;
    cb.floorZ = -4.0f;
    if (S.cloth) { cb.rugSize[0] = S.cloth->width; cb.rugSize[1] = S.cloth->height; }
    D3D11_MAPPED_SUBRESOURCE mc{};
    S.ctx->Map(S.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mc);
    std::memcpy(mc.pData, &cb, sizeof(cb));
    S.ctx->Unmap(S.cb, 0);

    D3D11_VIEWPORT vp{ 0.0f, 0.0f, float(S.w), float(S.h), 0.0f, 1.0f };
    S.ctx->RSSetViewports(1, &vp);
    S.ctx->RSSetState(S.rs);

    RECT scissor{ rc[0], rc[1], rc[0] + rc[2], rc[1] + rc[3] };
    S.ctx->RSSetScissorRects(1, &scissor);
    S.ctx->VSSetConstantBuffers(0, 1, &S.cb);
    S.ctx->PSSetConstantBuffers(0, 1, &S.cb);
    S.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const bool hasCloth = S.cloth && S.vb && S.ib && S.rugSrv;
    UINT vstride = sizeof(Vtx), offset = 0;

    ID3D11ShaderResourceView* nullSrv[2] = { nullptr, nullptr };
    S.ctx->PSSetShaderResources(0, 2, nullSrv);
    const float zero[4] = { 0, 0, 0, 0 };
    S.ctx->ClearRenderTargetView(S.shadowRtv, zero);
    S.ctx->ClearDepthStencilView(S.shadowDsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    S.ctx->OMSetRenderTargets(1, &S.shadowRtv, S.shadowDsv);
    if (hasCloth) {
        S.ctx->IASetInputLayout(S.layout);
        S.ctx->IASetVertexBuffers(0, 1, &S.vb, &vstride, &offset);
        S.ctx->IASetIndexBuffer(S.ib, DXGI_FORMAT_R32_UINT, 0);
        S.ctx->VSSetShader(S.shadowVs, nullptr, 0);
        S.ctx->PSSetShader(S.shadowPs, nullptr, 0);
        S.ctx->DrawIndexed(UINT(S.ibCount), 0, 0);
    }

    const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    S.ctx->ClearRenderTargetView(S.rtv, clear);
    S.ctx->ClearDepthStencilView(S.dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    S.ctx->OMSetRenderTargets(1, &S.rtv, S.dsv);

    float blendFactor[4] = { 0, 0, 0, 0 };
    S.ctx->OMSetBlendState(S.blendPremul, blendFactor, 0xFFFFFFFF);
    S.ctx->OMSetDepthStencilState(S.depthOff, 0);
    S.ctx->IASetInputLayout(nullptr);
    S.ctx->VSSetShader(S.floorVs, nullptr, 0);
    S.ctx->PSSetShader(S.floorPs, nullptr, 0);
    if (kDesktopShadow) S.ctx->Draw(3, 0);

    S.ctx->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);
    S.ctx->OMSetDepthStencilState(S.depthOn, 0);
    if (hasCloth) {
        ID3D11ShaderResourceView* srvs[2] = { S.rugSrv, S.shadowSrv };
        ID3D11SamplerState* samplers[2] = { S.sampler, S.pointSampler };
        S.ctx->IASetInputLayout(S.layout);
        S.ctx->IASetVertexBuffers(0, 1, &S.vb, &vstride, &offset);
        S.ctx->IASetIndexBuffer(S.ib, DXGI_FORMAT_R32_UINT, 0);
        S.ctx->VSSetShader(S.vs, nullptr, 0);
        S.ctx->PSSetShader(S.ps, nullptr, 0);
        S.ctx->PSSetShaderResources(0, 2, srvs);
        S.ctx->PSSetSamplers(0, 2, samplers);
        S.ctx->DrawIndexed(UINT(S.ibCount), 0, 0);
    }
    S.ctx->PSSetShaderResources(0, 2, nullSrv);

    if (drawRc[2] > 0 && drawRc[3] > 0) {
        const D3D11_BOX box{ UINT(drawRc[0]), UINT(drawRc[1]), 0u, UINT(drawRc[0] + drawRc[2]), UINT(drawRc[1] + drawRc[3]), 1u };
        S.ctx->CopySubresourceRegion(S.staging, 0, UINT(drawRc[0]), UINT(drawRc[1]), 0, S.rt, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(S.ctx->Map(S.staging, 0, D3D11_MAP_READ, 0, &m))) {
            const unsigned char* src = static_cast<const unsigned char*>(m.pData);
            for (int y = drawRc[1]; y < drawRc[1] + drawRc[3]; ++y)
                std::memcpy(dst + size_t(y) * stride + size_t(drawRc[0]) * 4,
                            src + size_t(y) * m.RowPitch + size_t(drawRc[0]) * 4,
                            size_t(drawRc[2]) * 4);
            S.ctx->Unmap(S.staging, 0);
        }
    }
}

extern "C" int rug_mouse_down(float x, float y) {
    if (!S.cloth) return 0;
    V2 s = ToV2(x, y);
    auto hit = S.cloth->pick(s, true);
    if (!hit) return 0;
    auto [vertex, z] = *hit;
    S.dragging = true;

    V3 p = S.cloth->pos[vertex];
    V3 target{ p.x, p.y, p.z };
    S.downScreen = s;
    S.grabBase = V2{ target.x, target.y };
    S.liftFrom = p.z;
    S.liftTo = std::max(z, 0.0f) + kClickLift * unitScale();
    S.rampT = 0.0f;
    S.cloth->beginGrab(vertex, target);
    S.dragFrom = S.dragTo = target;
    return 1;
}

extern "C" void rug_mouse_drag(float x, float y) {
    if (!S.cloth || !S.dragging) return;

    V3 from = S.camera.unproject(S.downScreen, S.dragTo.z);
    V3 now = S.camera.unproject(ToV2(x, y), S.dragTo.z);
    S.dragTo = { S.grabBase.x + (now.x - from.x), S.grabBase.y + (now.y - from.y), S.dragTo.z };
}

extern "C" void rug_mouse_up(void) {
    if (!S.cloth || !S.dragging) return;
    S.dragging = false;
    S.cloth->endGrab();
}

extern "C" int rug_hit_test(float x, float y) {
    if (!S.cloth) return 0;
    return S.cloth->pick(ToV2(x, y), false).has_value() ? 1 : 0;
}

extern "C" void rug_get_bounds(float* out) {
    if (!S.cloth || !out) return;
    out[0] = S.cloth->bboxMin.x; out[1] = S.cloth->bboxMin.y;
    out[2] = S.cloth->bboxMax.x; out[3] = S.cloth->bboxMax.y;
}

extern "C" void rug_rotate(float angle) {
    if (!S.cloth) return;
    S.cloth->rotate(angle);
    S.cloth->updateDerived(S.camera);
}

extern "C" void rug_get_pose(float* out) {
    if (!S.cloth || !out) return;
    V2 c{};
    float a = 0.0f;
    S.cloth->pose(c, a);
    out[0] = c.x; out[1] = c.y; out[2] = a;
}

extern "C" void rug_get_corners(float* out) {
    if (!S.cloth || !out) return;
    const Cloth& c = *S.cloth;
    const int idxs[4] = { 0, c.nx - 1, c.count - c.nx, c.count - 1 };
    for (int i = 0; i < 4; ++i) {
        V2 p = c.proj[idxs[i]];
        out[i * 2] = p.x;
        out[i * 2 + 1] = p.y;
    }
}

extern "C" void rug_resize(float scale) {
    if (!S.cloth) return;
    V2 pc{};
    float pa = 0.0f;
    S.cloth->pose(pc, pa);
    bool wasHeld = S.held;
    rug_rebuild(scale, 0.0f);
    if (!S.cloth) return;
    S.cloth->layout(pc, pa, 0.0f);
    if (wasHeld) S.cloth->setHeld(true, liftH());
    S.cloth->updateDerived(S.camera);
}

extern "C" void rug_hold_begin(void) {
    if (!S.cloth) return;
    S.dragging = false;
    S.cloth->layout(S.camera.center(), 0.0f, 60.0f * unitScale());
    V2 pc{};
    float pa = 0.0f;
    S.cloth->pose(pc, pa);
    S.cloth->layout(S.camera.center(), pa, 60.0f * unitScale());
    S.held = true;
    S.cloth->setHeld(true, liftH());
    S.cloth->updateDerived(S.camera);
}

extern "C" void rug_hold_end(void) {
    if (!S.cloth) return;
    S.held = false;
    S.cloth->setHeld(false, 0.0f);
}

extern "C" void rug_layout(float cx, float cy, float angle, float drop) {
    if (!S.cloth) return;
    S.dragging = false;
    S.cloth->layout(V2{ cx, cy }, angle, drop * unitScale());
    S.cloth->updateDerived(S.camera);
}
