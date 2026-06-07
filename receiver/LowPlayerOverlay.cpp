#define _CRT_SECURE_NO_WARNINGS
#include "LowPlayerOverlay.h"
#include "minhook/MinHook.h"

#include "../../../VCHD/include/dx8/d3d8.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr int kResetVtableIndex = 14;
constexpr int kEndSceneVtableIndex = 35;
constexpr int kTextureReleaseIndex = 2;
constexpr int kTextureLockRectIndex = 16;
constexpr int kTextureUnlockRectIndex = 17;

constexpr DWORD D3DPT_TRIANGLESTRIP_COMPAT = 5;
constexpr DWORD D3DPOOL_MANAGED_COMPAT = 1;
constexpr DWORD D3DFVF_OVERLAY = 0x00000004 | 0x00000040 | 0x00000100; // XYZRHW | DIFFUSE | TEX1

constexpr DWORD D3DRS_ZENABLE_COMPAT = 7;
constexpr DWORD D3DRS_ZWRITEENABLE_COMPAT = 14;
constexpr DWORD D3DRS_SRCBLEND_COMPAT = 19;
constexpr DWORD D3DRS_DESTBLEND_COMPAT = 20;
constexpr DWORD D3DRS_CULLMODE_COMPAT = 22;
constexpr DWORD D3DRS_ALPHABLENDENABLE_COMPAT = 27;
constexpr DWORD D3DRS_LIGHTING_COMPAT = 137;

constexpr DWORD D3DBLEND_SRCALPHA_COMPAT = 5;
constexpr DWORD D3DBLEND_INVSRCALPHA_COMPAT = 6;
constexpr DWORD D3DCULL_NONE_COMPAT = 1;

constexpr DWORD D3DTSS_COLOROP_COMPAT = 1;
constexpr DWORD D3DTSS_COLORARG1_COMPAT = 2;
constexpr DWORD D3DTSS_COLORARG2_COMPAT = 3;
constexpr DWORD D3DTSS_ALPHAOP_COMPAT = 4;
constexpr DWORD D3DTSS_ALPHAARG1_COMPAT = 5;
constexpr DWORD D3DTSS_ALPHAARG2_COMPAT = 6;
constexpr DWORD D3DTSS_MINFILTER_COMPAT = 16;
constexpr DWORD D3DTSS_MAGFILTER_COMPAT = 17;
constexpr DWORD D3DTSS_MIPFILTER_COMPAT = 18;

constexpr DWORD D3DTOP_DISABLE_COMPAT = 1;
constexpr DWORD D3DTOP_SELECTARG1_COMPAT = 2;
constexpr DWORD D3DTOP_MODULATE_COMPAT = 4;
constexpr DWORD D3DTA_DIFFUSE_COMPAT = 0x00000000;
constexpr DWORD D3DTA_TEXTURE_COMPAT = 0x00000002;
constexpr DWORD D3DTEXF_NONE_COMPAT = 0;
constexpr DWORD D3DTEXF_LINEAR_COMPAT = 2;

struct OverlayVertex {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};

struct BitmapPixels {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> bgra;
};

using ResetFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice8*, D3DPRESENT_PARAMETERS*);
using EndSceneFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice8*);
using Direct3DCreate8Fn = IDirect3D8* (WINAPI*)(UINT);
using TextureLockRectFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DTexture8*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
using TextureUnlockRectFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DTexture8*, UINT);
using UnknownReleaseFn = ULONG (STDMETHODCALLTYPE*)(void*);

std::atomic<int> g_activePlayerCount{ 0 };
std::atomic<int> g_threshold{ 2 };
std::atomic<bool> g_hookReady{ false };
std::atomic<bool> g_hookThreadStarted{ false };

std::mutex g_renderMutex;
std::string g_imagePath = "stream_waiting.bmp";
float g_fadeSeconds = 1.5f;
float g_alpha = 1.0f;
DWORD g_lastFrameTick = 0;
IDirect3DTexture8* g_texture = nullptr;
int g_textureWidth = 0;
int g_textureHeight = 0;

ResetFn g_originalReset = nullptr;
EndSceneFn g_originalEndScene = nullptr;

std::string Trim(std::string value) {
    const char* ws = " \t\r\n";
    size_t first = value.find_first_not_of(ws);
    if (first == std::string::npos) return std::string();
    size_t last = value.find_last_not_of(ws);
    return value.substr(first, last - first + 1);
}

float Clamp01(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

std::string GetDllDirectoryPath() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&InitLowPlayerOverlay),
                            &self)) {
        return std::string();
    }

    char path[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(self, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return std::string();

    std::string full(path, path + len);
    size_t slash = full.find_last_of("\\/");
    if (slash == std::string::npos) return std::string();
    return full.substr(0, slash + 1);
}

std::string ResolvePath(const std::string& configuredPath) {
    if (configuredPath.size() >= 3 &&
        ((configuredPath[0] >= 'A' && configuredPath[0] <= 'Z') ||
         (configuredPath[0] >= 'a' && configuredPath[0] <= 'z')) &&
        configuredPath[1] == ':' &&
        (configuredPath[2] == '\\' || configuredPath[2] == '/')) {
        return configuredPath;
    }

    return GetDllDirectoryPath() + configuredPath;
}

void LoadOverlayConfig() {
    const std::string iniPath = GetDllDirectoryPath() + "vcstreamer.ini";
    std::ifstream ini(iniPath);
    if (!ini.is_open()) return;

    int threshold = 2;
    std::string imagePath = "stream_waiting.bmp";
    float fadeSeconds = 1.5f;

    std::string line;
    while (std::getline(ini, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        size_t equals = line.find('=');
        if (equals == std::string::npos) continue;

        std::string key = Trim(line.substr(0, equals));
        std::string value = Trim(line.substr(equals + 1));

        try {
            if (key == "low_player_overlay_threshold") {
                threshold = std::stoi(value);
            } else if (key == "low_player_overlay_image") {
                if (!value.empty()) imagePath = value;
            } else if (key == "low_player_overlay_fade_seconds") {
                fadeSeconds = std::stof(value);
            }
        } catch (...) {
        }
    }

    if (threshold < 0) threshold = 0;
    if (fadeSeconds < 0.05f) fadeSeconds = 0.05f;

    g_threshold.store(threshold);
    g_imagePath = imagePath;
    g_fadeSeconds = fadeSeconds;
}

void ReleaseUnknown(void* ptr) {
    if (!ptr) return;
    void** vtbl = *(void***)ptr;
    auto releaseFn = reinterpret_cast<UnknownReleaseFn>(vtbl[kTextureReleaseIndex]);
    releaseFn(ptr);
}

void ReleaseTexture() {
    if (g_texture) {
        ReleaseUnknown(g_texture);
        g_texture = nullptr;
    }
    g_textureWidth = 0;
    g_textureHeight = 0;

}

bool LoadBmpPixels(const std::string& path, BitmapPixels& out) {
    HBITMAP bitmap = (HBITMAP)LoadImageA(nullptr,
                                         path.c_str(),
                                         IMAGE_BITMAP,
                                         0,
                                         0,
                                         LR_LOADFROMFILE | LR_CREATEDIBSECTION);
    if (!bitmap) return false;

    BITMAP bm = {};
    if (GetObject(bitmap, sizeof(bm), &bm) == 0 || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        DeleteObject(bitmap);
        return false;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bm.bmWidth;
    bmi.bmiHeader.biHeight = -bm.bmHeight; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    out.width = bm.bmWidth;
    out.height = bm.bmHeight;
    out.bgra.resize((size_t)out.width * (size_t)out.height * 4u);

    HDC dc = GetDC(nullptr);
    int got = GetDIBits(dc, bitmap, 0, (UINT)out.height, out.bgra.data(), &bmi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    DeleteObject(bitmap);

    if (got == 0) return false;

    for (size_t i = 0; i + 3 < out.bgra.size(); i += 4) {
        out.bgra[i + 3] = 255;
    }
    return true;
}

bool EnsureTexture(IDirect3DDevice8* device) {
    if (g_texture) return true;

    BitmapPixels pixels;
    const std::string resolved = ResolvePath(g_imagePath);
    if (!LoadBmpPixels(resolved, pixels)) {
        return false;
    }

    IDirect3DTexture8* texture = nullptr;
    HRESULT hr = device->CreateTexture((UINT)pixels.width,
                                       (UINT)pixels.height,
                                       1,
                                       0,
                                       D3DFMT_A8R8G8B8,
                                       D3DPOOL_MANAGED_COMPAT,
                                       &texture);
    if (FAILED(hr) || !texture) {
        return false;
    }

    void** vtbl = *(void***)texture;
    auto lockRect = reinterpret_cast<TextureLockRectFn>(vtbl[kTextureLockRectIndex]);
    auto unlockRect = reinterpret_cast<TextureUnlockRectFn>(vtbl[kTextureUnlockRectIndex]);

    D3DLOCKED_RECT locked = {};
    hr = lockRect(texture, 0, &locked, nullptr, 0);
    if (FAILED(hr) || !locked.pBits) {
        ReleaseUnknown(texture);
        return false;
    }

    const unsigned char* src = pixels.bgra.data();
    unsigned char* dst = static_cast<unsigned char*>(locked.pBits);
    const size_t rowBytes = (size_t)pixels.width * 4u;
    for (int y = 0; y < pixels.height; ++y) {
        memcpy(dst + (size_t)y * (size_t)locked.Pitch,
               src + (size_t)y * rowBytes,
               rowBytes);
    }

    unlockRect(texture, 0);

    g_texture = texture;
    g_textureWidth = pixels.width;
    g_textureHeight = pixels.height;
    return true;
}


void DrawQuad(IDirect3DDevice8* device,
              float x, float y, float w, float h,
              DWORD color,
              float u0, float v0, float u1, float v1) {
    OverlayVertex vertices[4] = {
        { x - 0.5f,     y - 0.5f,     0.0f, 1.0f, color, u0, v0 },
        { x + w - 0.5f, y - 0.5f,     0.0f, 1.0f, color, u1, v0 },
        { x - 0.5f,     y + h - 0.5f, 0.0f, 1.0f, color, u0, v1 },
        { x + w - 0.5f, y + h - 0.5f, 0.0f, 1.0f, color, u1, v1 },
    };
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP_COMPAT, 2, vertices, sizeof(OverlayVertex));
}

void DrawOverlay(IDirect3DDevice8* device, float alpha) {
    if (alpha <= 0.001f) return;

    D3DVIEWPORT8 vp = {};
    if (FAILED(device->GetViewport(&vp)) || vp.Width == 0 || vp.Height == 0) {
        return;
    }

    DWORD oldZ = 0, oldZWrite = 0, oldAlpha = 0, oldSrc = 0, oldDst = 0, oldCull = 0, oldLighting = 0;
    DWORD oldVertexShader = 0, oldPixelShader = 0;
    DWORD oldColorOp = 0, oldColorArg1 = 0, oldColorArg2 = 0;
    DWORD oldAlphaOp = 0, oldAlphaArg1 = 0, oldAlphaArg2 = 0;
    DWORD oldMinFilter = 0, oldMagFilter = 0, oldMipFilter = 0;
    void* oldTexture = nullptr;

    device->GetRenderState(D3DRS_ZENABLE_COMPAT, &oldZ);
    device->GetRenderState(D3DRS_ZWRITEENABLE_COMPAT, &oldZWrite);
    device->GetRenderState(D3DRS_ALPHABLENDENABLE_COMPAT, &oldAlpha);
    device->GetRenderState(D3DRS_SRCBLEND_COMPAT, &oldSrc);
    device->GetRenderState(D3DRS_DESTBLEND_COMPAT, &oldDst);
    device->GetRenderState(D3DRS_CULLMODE_COMPAT, &oldCull);
    device->GetRenderState(D3DRS_LIGHTING_COMPAT, &oldLighting);
    device->GetVertexShader(&oldVertexShader);
    device->GetPixelShader(&oldPixelShader);
    device->GetTexture(0, &oldTexture);
    device->GetTextureStageState(0, D3DTSS_COLOROP_COMPAT, &oldColorOp);
    device->GetTextureStageState(0, D3DTSS_COLORARG1_COMPAT, &oldColorArg1);
    device->GetTextureStageState(0, D3DTSS_COLORARG2_COMPAT, &oldColorArg2);
    device->GetTextureStageState(0, D3DTSS_ALPHAOP_COMPAT, &oldAlphaOp);
    device->GetTextureStageState(0, D3DTSS_ALPHAARG1_COMPAT, &oldAlphaArg1);
    device->GetTextureStageState(0, D3DTSS_ALPHAARG2_COMPAT, &oldAlphaArg2);
    device->GetTextureStageState(0, D3DTSS_MINFILTER_COMPAT, &oldMinFilter);
    device->GetTextureStageState(0, D3DTSS_MAGFILTER_COMPAT, &oldMagFilter);
    device->GetTextureStageState(0, D3DTSS_MIPFILTER_COMPAT, &oldMipFilter);

    device->SetRenderState(D3DRS_ZENABLE_COMPAT, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE_COMPAT, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE_COMPAT, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND_COMPAT, D3DBLEND_SRCALPHA_COMPAT);
    device->SetRenderState(D3DRS_DESTBLEND_COMPAT, D3DBLEND_INVSRCALPHA_COMPAT);
    device->SetRenderState(D3DRS_CULLMODE_COMPAT, D3DCULL_NONE_COMPAT);
    device->SetRenderState(D3DRS_LIGHTING_COMPAT, FALSE);
    device->SetVertexShader(D3DFVF_OVERLAY);
    device->SetPixelShader(0);
    device->SetTextureStageState(0, D3DTSS_MINFILTER_COMPAT, D3DTEXF_LINEAR_COMPAT);
    device->SetTextureStageState(0, D3DTSS_MAGFILTER_COMPAT, D3DTEXF_LINEAR_COMPAT);
    device->SetTextureStageState(0, D3DTSS_MIPFILTER_COMPAT, D3DTEXF_NONE_COMPAT);

    DWORD a = (DWORD)(Clamp01(alpha) * 255.0f + 0.5f);

    device->SetTexture(0, nullptr);
    device->SetTextureStageState(0, D3DTSS_COLOROP_COMPAT, D3DTOP_SELECTARG1_COMPAT);
    device->SetTextureStageState(0, D3DTSS_COLORARG1_COMPAT, D3DTA_DIFFUSE_COMPAT);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP_COMPAT, D3DTOP_SELECTARG1_COMPAT);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1_COMPAT, D3DTA_DIFFUSE_COMPAT);
    DrawQuad(device, (float)vp.X, (float)vp.Y, (float)vp.Width, (float)vp.Height, a << 24, 0.0f, 0.0f, 1.0f, 1.0f);

    if (EnsureTexture(device)) {
        float viewW = (float)vp.Width;
        float viewH = (float)vp.Height;
        float imageAspect = (float)g_textureWidth / (float)g_textureHeight;
        float viewAspect = viewW / viewH;
        float drawW = viewW;
        float drawH = viewH;
        if (viewAspect > imageAspect) {
            drawH = viewH;
            drawW = drawH * imageAspect;
        } else {
            drawW = viewW;
            drawH = drawW / imageAspect;
        }
        float drawX = (float)vp.X + (viewW - drawW) * 0.5f;
        float drawY = (float)vp.Y + (viewH - drawH) * 0.5f;

        device->SetTexture(0, g_texture);
        device->SetTextureStageState(0, D3DTSS_COLOROP_COMPAT, D3DTOP_MODULATE_COMPAT);
        device->SetTextureStageState(0, D3DTSS_COLORARG1_COMPAT, D3DTA_TEXTURE_COMPAT);
        device->SetTextureStageState(0, D3DTSS_COLORARG2_COMPAT, D3DTA_DIFFUSE_COMPAT);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP_COMPAT, D3DTOP_MODULATE_COMPAT);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1_COMPAT, D3DTA_TEXTURE_COMPAT);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2_COMPAT, D3DTA_DIFFUSE_COMPAT);
        DrawQuad(device, drawX, drawY, drawW, drawH, (a << 24) | 0x00FFFFFF, 0.0f, 0.0f, 1.0f, 1.0f);
    }

    device->SetTexture(0, oldTexture);
    device->SetTextureStageState(0, D3DTSS_COLOROP_COMPAT, oldColorOp);
    device->SetTextureStageState(0, D3DTSS_COLORARG1_COMPAT, oldColorArg1);
    device->SetTextureStageState(0, D3DTSS_COLORARG2_COMPAT, oldColorArg2);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP_COMPAT, oldAlphaOp);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1_COMPAT, oldAlphaArg1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2_COMPAT, oldAlphaArg2);
    device->SetTextureStageState(0, D3DTSS_MINFILTER_COMPAT, oldMinFilter);
    device->SetTextureStageState(0, D3DTSS_MAGFILTER_COMPAT, oldMagFilter);
    device->SetTextureStageState(0, D3DTSS_MIPFILTER_COMPAT, oldMipFilter);
    device->SetRenderState(D3DRS_ZENABLE_COMPAT, oldZ);
    device->SetRenderState(D3DRS_ZWRITEENABLE_COMPAT, oldZWrite);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE_COMPAT, oldAlpha);
    device->SetRenderState(D3DRS_SRCBLEND_COMPAT, oldSrc);
    device->SetRenderState(D3DRS_DESTBLEND_COMPAT, oldDst);
    device->SetRenderState(D3DRS_CULLMODE_COMPAT, oldCull);
    device->SetRenderState(D3DRS_LIGHTING_COMPAT, oldLighting);
    device->SetVertexShader(oldVertexShader);
    device->SetPixelShader(oldPixelShader);

    if (oldTexture) {
        ReleaseUnknown(oldTexture);
    }
}

HRESULT STDMETHODCALLTYPE HookedReset(IDirect3DDevice8* device, D3DPRESENT_PARAMETERS* params) {
    {
        std::lock_guard<std::mutex> lock(g_renderMutex);
        ReleaseTexture();
    }
    return g_originalReset(device, params);
}

HRESULT STDMETHODCALLTYPE HookedEndScene(IDirect3DDevice8* device) {
    {
        std::lock_guard<std::mutex> lock(g_renderMutex);

        DWORD now = GetTickCount();
        if (g_lastFrameTick == 0) g_lastFrameTick = now;
        float dt = (now - g_lastFrameTick) / 1000.0f;
        if (dt < 0.0f || dt > 1.0f) dt = 0.0f;
        g_lastFrameTick = now;

        int threshold = g_threshold.load();
        float targetAlpha = (threshold > 0 && g_activePlayerCount.load() < threshold) ? 1.0f : 0.0f;
        float step = dt / g_fadeSeconds;
        if (g_alpha < targetAlpha) {
            g_alpha += step;
            if (g_alpha > targetAlpha) g_alpha = targetAlpha;
        } else if (g_alpha > targetAlpha) {
            g_alpha -= step;
            if (g_alpha < targetAlpha) g_alpha = targetAlpha;
        }

        DrawOverlay(device, g_alpha);
    }

    return g_originalEndScene(device);
}

LRESULT CALLBACK DummyWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcA(hwnd, msg, wp, lp);
}

HWND CreateDummyWindow() {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DummyWndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "VCStreamerOverlayD3D8Dummy";
    RegisterClassA(&wc);
    return CreateWindowA(wc.lpszClassName,
                         wc.lpszClassName,
                         WS_OVERLAPPEDWINDOW,
                         0, 0, 16, 16,
                         nullptr,
                         nullptr,
                         wc.hInstance,
                         nullptr);
}

bool InstallD3D8Hooks() {
    HMODULE d3d8 = LoadLibraryA("d3d8.dll");
    if (!d3d8) return false;

    auto createD3D = reinterpret_cast<Direct3DCreate8Fn>(GetProcAddress(d3d8, "Direct3DCreate8"));
    if (!createD3D) return false;

    IDirect3D8* d3d = createD3D(D3D_SDK_VERSION);
    if (!d3d) return false;

    HWND hwnd = CreateDummyWindow();
    if (!hwnd) {
        d3d->Release();
        return false;
    }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = hwnd;

    IDirect3DDevice8* device = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT,
                                   D3DDEVTYPE_HAL,
                                   hwnd,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                   &pp,
                                   &device);
    if (FAILED(hr) || !device) {
        DestroyWindow(hwnd);
        d3d->Release();
        return false;
    }

    void** vtbl = *(void***)device;

    MH_STATUS mh = MH_Initialize();
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED) {
        device->Release();
        DestroyWindow(hwnd);
        d3d->Release();
        return false;
    }

    bool ok = true;
    mh = MH_CreateHook(vtbl[kResetVtableIndex],
                       reinterpret_cast<void*>(&HookedReset),
                       reinterpret_cast<void**>(&g_originalReset));
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_CREATED) ok = false;

    mh = MH_CreateHook(vtbl[kEndSceneVtableIndex],
                       reinterpret_cast<void*>(&HookedEndScene),
                       reinterpret_cast<void**>(&g_originalEndScene));
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_CREATED) ok = false;

    if (ok) {
        MH_EnableHook(vtbl[kResetVtableIndex]);
        MH_EnableHook(vtbl[kEndSceneVtableIndex]);
        g_hookReady.store(true);
    }

    device->Release();
    DestroyWindow(hwnd);
    d3d->Release();
    return ok;
}

DWORD WINAPI HookThreadProc(void*) {
    for (int i = 0; i < 300 && !g_hookReady.load(); ++i) {
        if (InstallD3D8Hooks()) break;
        Sleep(100);
    }
    return 0;
}

} // namespace

void InitLowPlayerOverlay() {
    LoadOverlayConfig();
    if (g_hookThreadStarted.exchange(true)) return;

    HANDLE thread = CreateThread(nullptr, 0, HookThreadProc, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
}

void LowPlayerOverlay_SetActivePlayerCount(int count) {
    if (count < 0) count = 0;
    g_activePlayerCount.store(count);
}
