#include "fluid.h"
#include "shaders.h"
#include "acid_slots.h"
#include <d3dcompiler.h>
#include <pdh.h>          // VramCardUsage: the "GPU Adapter Memory" counter (Windows SDK)
#include <pdhmsg.h>       //   (PDH_MORE_DATA)
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <thread>
#include <atomic>         // the shader cache's worker count
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

extern void Fail(const char* what, HRESULT hr);   // main.cpp
extern void WpLog(const char* fmt, ...);          // main.cpp: rolling log file
extern void ForegroundDesc(char* out, size_t cap);
// Every failed HRESULT goes through HrFailed(): Fail() as always, EXCEPT a GPU device
// loss (driver update / crash / TDR, Win+Ctrl+Shift+B) in a renderer told to survive
// one -- that only raises m_deviceLost and the caller rebuilds (review 2026-10-04 R3).
// Members only: every HR() site is inside a FluidRenderer method.
#define HR(expr) do { HRESULT _hr = (expr); if (FAILED(_hr)) HrFailed(#expr, _hr); } while (0)

// Thrown by HrFailed() only while TryInit/TryReattach run (m_softInit) and caught
// there: a device that dies half-way through CreateDevice leaves null objects behind
// that the next lines would use, and unwinding is the one way out that cannot miss
// one. Nothing else in the renderer throws (review 2026-10-04 R3).
struct DeviceLostInInit { HRESULT hr; };

bool FluidRenderer::IsDeviceLossHr(HRESULT hr) const {
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
        hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR)
        return true;
    return m_device && FAILED(m_device->GetDeviceRemovedReason());
}

// Logged once per loss; everything after it in the frame path returns early.
void FluidRenderer::NoteDeviceLost(const char* what, HRESULT hr) {
    if (m_deviceLost) return;
    m_deviceLost = true;
    m_deviceLostHr = hr;
    m_recoveringDevice = true;
    const HRESULT why = m_device ? m_device->GetDeviceRemovedReason() : S_OK;
    WpLog("GPU device lost at %s: hr=0x%08lX, removed reason 0x%08lX -- tearing the renderer "
          "down and rebuilding it", what, (unsigned long)hr, (unsigned long)why);
    printf("GPU device lost at %s (hr=0x%08lX, reason 0x%08lX)\n",
           what, (unsigned long)hr, (unsigned long)why);
}

void FluidRenderer::HrFailed(const char* what, HRESULT hr) {
    const bool lost = IsDeviceLossHr(hr);
    // (Re)creation under TryInit: a loss -- or, while recovering from one, ANY failure
    // (the driver can still be restarting) -- makes TryInit return false and the
    // caller's backoff try again, instead of a fatal box.
    if (m_surviveDeviceLoss && m_softInit && (lost || m_recoveringDevice)) {
        if (lost) NoteDeviceLost(what, hr);
        else WpLog("device rebuild: %s failed hr=0x%08lX (will retry)", what, (unsigned long)hr);
        throw DeviceLostInInit{ hr };
    }
    if (!m_surviveDeviceLoss || !lost) Fail(what, hr);   // unchanged for everything else
    NoteDeviceLost(what, hr);
}

// Must match cbuffer CB in shaders.h (24 DWORDs, see the static_assert below).
struct SimCB {
    float texelW, texelH;
    float dt;
    float dissipation;
    float dissipationFast;
    float decayThreshold;
    float satRestore;
    float curlStrength;
    float value;
    float aspect;
    float pointX, pointY;
    float colorR, colorG, colorB;
    float radius;
    float cap;
    int   dimsW, dimsH;
    float baroclinic;
    float gravity;       // dye-weighted gravity (0 = off; the branch is skipped)
    float gravityPow;
    float gravityBlur;
    float dyeRise;       // uv/s upward, read by CSAdvectDye only ([drops] ink_rise; 0 = off)
};
static_assert(sizeof(SimCB) == 24 * 4, "SimCB must match the HLSL cbuffer layout (24 DWORDs)");

static float RandF() { return (float)rand() / (float)RAND_MAX; }
static float HalfToFloat(uint16_t h);   // defined below

struct RGB { float r, g, b; };
// (CSS filter matrices now live in the display shader; see kDisplaySrc.)

static RGB HSVtoRGB(float h, float s, float v) {
    int i = (int)floorf(h * 6.0f);
    float f = h * 6.0f - i;
    float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    switch (((i % 6) + 6) % 6) {
    case 0: return { v, t, p };
    case 1: return { q, v, p };
    case 2: return { p, v, t };
    case 3: return { p, q, v };
    case 4: return { t, p, v };
    default: return { v, p, q };
    }
}

// reference cycledColor(): slow wheel position + offset, dimmed to 0.15
static RGB CycledColorAt(float hue) {
    RGB c = HSVtoRGB(fmodf(hue + 1.0f, 1.0f), 1.0f, 1.0f);
    c.r *= 0.15f; c.g *= 0.15f; c.b *= 0.15f;
    return c;
}

// ---- SHADER CACHE (user 2026-10-30, a black left screen: "this is just a stress test, it should still work") ----
// D3DCompile of kDisplaySrc takes 10-60 s per variant on one core, and the live wallpaper built ~6 of them before its
// first frame (black for ~1.5 min after every start) and another on each new lamp / water variant (the picture froze).
// Every blob is now kept in memory and on disk (%LOCALAPPDATA%\FluidWallpaper\shadercache), keyed by a hash of all
// D3DCompile sees (source, entry, target, defines): a restart loads them in milliseconds, and the bytes are exactly
// what D3DCompile returns, so shots stay bit-identical. CompileAsync builds a missing blob on a worker thread instead.
namespace {
std::mutex g_scMutex;
std::unordered_map<std::string, ComPtr<ID3DBlob>> g_scMem;   // key hex -> blob (survives renderer rebuilds)
std::unordered_set<std::string> g_scPending, g_scFailed;      // CompileAsync jobs running / given up
std::atomic<int> g_scWorkers{ 0 };
constexpr int kScMaxWorkers = 6;                               // a few cores; the CPU has 24 threads

std::string ShaderKey(const char* src, const char* entry, const char* target, const D3D_SHADER_MACRO* defines) {
    unsigned long long a = 1469598103934665603ull, b = 0x84222325cbf29ce4ull;
    auto mix = [&](const char* s) {
        for (; *s; s++) {
            const unsigned char c = (unsigned char)*s;
            a = (a ^ c) * 1099511628211ull;
            b = (b ^ c) * 0x9e3779b97f4a7c15ull; b ^= b >> 29;
        }
        a = (a ^ 0x1f) * 1099511628211ull; b = (b ^ 0x1f) * 0x9e3779b97f4a7c15ull;
    };
    mix("fw-shadercache 1 | d3dcompiler_47 | flags 0 0");   // bump when the compile call itself changes
    mix(src); mix(entry); mix(target);
    for (const D3D_SHADER_MACRO* d = defines; d && d->Name; d++) { mix(d->Name); mix(d->Definition ? d->Definition : ""); }
    char hex[40];
    snprintf(hex, sizeof(hex), "%016llx%016llx", a, b);
    return hex;
}

std::wstring ShaderCachePath(const std::string& key) {
    static std::wstring dir;
    static std::once_flag once;
    std::call_once(once, [] {
        wchar_t base[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        if (!n || n >= MAX_PATH) return;
        dir = std::wstring(base) + L"\\FluidWallpaper";
        CreateDirectoryW(dir.c_str(), nullptr);
        dir += L"\\shadercache";
        CreateDirectoryW(dir.c_str(), nullptr);
    });
    if (dir.empty()) return std::wstring();
    return dir + L"\\" + std::wstring(key.begin(), key.end()) + L".dxbc";
}

// a cached blob: memory first, then disk (a DXBC container whose own size field matches the file)
ComPtr<ID3DBlob> ShaderCacheGet(const std::string& key) {
    {
        std::lock_guard<std::mutex> lk(g_scMutex);
        auto it = g_scMem.find(key);
        if (it != g_scMem.end()) return it->second;
    }
    const std::wstring path = ShaderCachePath(key);
    if (path.empty()) return nullptr;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    ComPtr<ID3DBlob> blob;
    LARGE_INTEGER sz = {};
    DWORD got = 0;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart >= 32 && sz.QuadPart < (64ll << 20) &&
        SUCCEEDED(D3DCreateBlob((SIZE_T)sz.QuadPart, &blob)) &&
        ReadFile(f, blob->GetBufferPointer(), (DWORD)sz.QuadPart, &got, nullptr) && got == (DWORD)sz.QuadPart) {
        const unsigned char* p = (const unsigned char*)blob->GetBufferPointer();
        uint32_t total = 0;
        memcpy(&total, p + 24, 4);
        if (memcmp(p, "DXBC", 4) != 0 || total != got) blob.Reset();
    } else {
        blob.Reset();
    }
    CloseHandle(f);
    if (!blob) { DeleteFileW(path.c_str()); return nullptr; }
    std::lock_guard<std::mutex> lk(g_scMutex);
    g_scMem[key] = blob;
    return blob;
}

// keep a fresh blob: memory, and disk through a temp file + rename (a live wallpaper and a shot may race; both
// write the same bytes, and a reader never sees half a file)
void ShaderCachePut(const std::string& key, const ComPtr<ID3DBlob>& blob) {
    {
        std::lock_guard<std::mutex> lk(g_scMutex);
        g_scMem[key] = blob;
    }
    const std::wstring path = ShaderCachePath(key);
    if (path.empty()) return;
    wchar_t tmp[64];
    swprintf_s(tmp, L".%lu.%lu.tmp", GetCurrentProcessId(), GetCurrentThreadId());
    const std::wstring tmpPath = path + tmp;
    HANDLE f = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    const BOOL ok = WriteFile(f, blob->GetBufferPointer(), (DWORD)blob->GetBufferSize(), &put, nullptr);
    CloseHandle(f);
    if (!ok || put != blob->GetBufferSize() || !MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        DeleteFileW(tmpPath.c_str());
}
}  // namespace

// DROP_LIFE (droplet_shadow / droplet_wobble, user 2026-11-02 "make the black oil dots look more alive"):
// while either key is on, every acid display PSO (kDisplaySrc compiled with LIQUID_ACID) is compiled with
// DROP_LIFE as well -- one variant of the whole family decided here, at compile time, instead of one more
// column in every variant table. With both keys 0 every PSO is built from exactly today's defines, so it is
// today's bytecode to the bit. FluidRenderer::SyncDropLife flips the flag and drops the acid PSOs so they
// rebuild. Read by the async workers' callers only on the main thread (the defines are copied at enqueue).
static std::atomic<bool> g_dropLife{ false };
// post-night T4: DROP_RIGLAMP (1, droplet_shadow_lamp 1) / DROP_HAZERIM (2, droplet_haze_rim > 0), the same
// whole-family way, only with DROP_LIFE; 0 = exactly the DROP_LIFE defines of before
static std::atomic<int> g_dropLifeX{ 0 };
static std::vector<D3D_SHADER_MACRO> DropLifeDefs(const char* src, const D3D_SHADER_MACRO* defines) {
    std::vector<D3D_SHADER_MACRO> v;
    if (!g_dropLife.load() || src != kDisplaySrc || !defines) return v;
    bool acid = false;
    for (const D3D_SHADER_MACRO* d = defines; d->Name; d++) if (strcmp(d->Name, "LIQUID_ACID") == 0) acid = true;
    if (!acid) return v;
    v.push_back({ "DROP_LIFE", "1" });
    const int x = g_dropLifeX.load();
    if (x & 1) v.push_back({ "DROP_RIGLAMP", "1" });
    if (x & 2) v.push_back({ "DROP_HAZERIM", "1" });
    for (const D3D_SHADER_MACRO* d = defines; d->Name; d++) v.push_back(*d);
    v.push_back({ nullptr, nullptr });
    return v;
}

// post-night T5: mass_black_soft, the same whole-family way for the post pass: while it is on, every kPostSrc
// compile with POST_BLACK also gets POST_BLACK_SOFT (FluidRenderer::SyncPostBlackSoft drops the PSOs). Off =
// exactly the defines of before.
static std::atomic<bool> g_postBlackSoft{ false };
// T7: [post] bloom_ambient the same way on every kPostSrc compile with BN_OPTICS (where bloom_warmth lives):
// POST_BLOOM_AMB. Off = exactly the defines of before.
static std::atomic<bool> g_postBloomAmb{ false };
static std::vector<D3D_SHADER_MACRO> PostSoftDefs(const char* src, const D3D_SHADER_MACRO* defines) {
    std::vector<D3D_SHADER_MACRO> v;
    if ((!g_postBlackSoft.load() && !g_postBloomAmb.load()) || src != kPostSrc || !defines) return v;
    bool pb = false, bn = false;
    for (const D3D_SHADER_MACRO* d = defines; d->Name; d++) {
        if (strcmp(d->Name, "POST_BLACK") == 0) pb = true;
        if (strcmp(d->Name, "BN_OPTICS") == 0) bn = true;
    }
    pb = pb && g_postBlackSoft.load();
    bn = bn && g_postBloomAmb.load();
    if (!pb && !bn) return v;
    if (pb) v.push_back({ "POST_BLACK_SOFT", "1" });
    if (bn) v.push_back({ "POST_BLOOM_AMB", "1" });
    for (const D3D_SHADER_MACRO* d = defines; d->Name; d++) v.push_back(*d);
    v.push_back({ nullptr, nullptr });
    return v;
}

// T7: oil_spec_tint, the same whole-family way on every acid display PSO (any look, not only DROP_LIFE): while it
// is 1 or 2, SPEC_TINT=<value> is added (FluidRenderer::SyncDropLife drops the acid PSOs on a change). 0 = exactly
// the defines of before.
static std::atomic<int> g_specTint{ 0 };
static const char* const kSpecTintVal[3] = { "0", "1", "2" };
// T8b: film_hue2_seam_stretch rides the same way, in quarter steps (SEAM_STRETCH=<1..12>; 0 = no define = the
// shader source of before, so the default is the same blob).
static std::atomic<int> g_seamStretch{ 0 };
// film_hue2_leak the same way: amount | reach << 8 | core << 16 (SEAM_LEAK / SEAM_LEAK_R in twentieths,
// SEAM_LEAK_C in quarters, each 1..24); 0 = no define.
static std::atomic<int> g_seamLeak{ 0 };
static const char* const kSeamStretchVal[25] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12",
    "13", "14", "15", "16", "17", "18", "19", "20", "21", "22", "23", "24" };
static std::vector<D3D_SHADER_MACRO> SpecTintDefs(const char* src, const D3D_SHADER_MACRO* defines) {
    std::vector<D3D_SHADER_MACRO> v;
    const int t = g_specTint.load(), q = g_seamStretch.load();
    const bool tOn = t >= 1 && t <= 2, qOn = q >= 1 && q <= 24;
    const int lk = g_seamLeak.load(), lA = lk & 255, lR = (lk >> 8) & 255, lC = (lk >> 16) & 255, lI = (lk >> 24) & 31;
    const bool lOn = lA >= 1 && lA <= 24 && lR >= 1 && lR <= 24 && lC >= 1 && lC <= 24;
    if ((!tOn && !qOn && !lOn) || src != kDisplaySrc || !defines) return v;
    bool acid = false;
    for (const D3D_SHADER_MACRO* d = defines; d->Name; d++) if (strcmp(d->Name, "LIQUID_ACID") == 0) acid = true;
    if (!acid) return v;
    if (tOn) v.push_back({ "SPEC_TINT", kSpecTintVal[t] });
    if (qOn) v.push_back({ "SEAM_STRETCH", kSeamStretchVal[q] });
    if (lOn) {
        v.push_back({ "SEAM_LEAK", kSeamStretchVal[lA] });
        v.push_back({ "SEAM_LEAK_R", kSeamStretchVal[lR] });
        v.push_back({ "SEAM_LEAK_C", kSeamStretchVal[lC] });
        if (lI >= 1 && lI <= 24) v.push_back({ "SEAM_LEAK_I", kSeamStretchVal[lI] });
    }
    for (const D3D_SHADER_MACRO* d = defines; d->Name; d++) v.push_back(*d);
    v.push_back({ nullptr, nullptr });
    return v;
}

static ComPtr<ID3DBlob> Compile(const char* src, const char* entry, const char* target,
                                const D3D_SHADER_MACRO* defines = nullptr) {
    const std::vector<D3D_SHADER_MACRO> ps = PostSoftDefs(src, defines);   // POST_BLACK_SOFT: + the define
    if (!ps.empty()) defines = ps.data();
    const std::vector<D3D_SHADER_MACRO> st = SpecTintDefs(src, defines);   // SPEC_TINT: + the define
    if (!st.empty()) defines = st.data();
    const std::vector<D3D_SHADER_MACRO> dl = DropLifeDefs(src, defines);   // DROP_LIFE: + the define
    if (!dl.empty()) defines = dl.data();
    const std::string key = ShaderKey(src, entry, target, defines);
    if (ComPtr<ID3DBlob> hit = ShaderCacheGet(key)) return hit;
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3DCompile(src, strlen(src), entry, defines, nullptr, entry, target, 0, 0, &blob, &err))) {
        fprintf(stderr, "shader '%s' compile error:\n%s\n", entry,
                err ? (char*)err->GetBufferPointer() : "?");
        // post-night T2: every way out of the process leaves an "exit:" line in FluidWallpaper.log
        WpLog("exit: shader '%s' did not compile (synchronous build): %.400s", entry,
              err ? (char*)err->GetBufferPointer() : "?");
        ExitProcess(1);
    }
    ShaderCachePut(key, blob);
    return blob;
}

// The blob if it is cached; otherwise a worker thread starts building it (below-normal priority: never at the cost
// of a game or the sim) and this returns null until it is done. `src` must outlive the job (the shader sources are
// static strings); the defines are copied. A blob that fails to compile is logged once and never retried.
static ComPtr<ID3DBlob> CompileAsync(const char* src, const char* entry, const char* target,
                                     const D3D_SHADER_MACRO* defines) {
    const std::vector<D3D_SHADER_MACRO> ps = PostSoftDefs(src, defines);   // POST_BLACK_SOFT: + the define
    if (!ps.empty()) defines = ps.data();
    const std::vector<D3D_SHADER_MACRO> st = SpecTintDefs(src, defines);   // SPEC_TINT: + the define
    if (!st.empty()) defines = st.data();
    const std::vector<D3D_SHADER_MACRO> dl = DropLifeDefs(src, defines);   // DROP_LIFE: + the define
    if (!dl.empty()) defines = dl.data();
    const std::string key = ShaderKey(src, entry, target, defines);
    if (ComPtr<ID3DBlob> hit = ShaderCacheGet(key)) return hit;
    {
        std::lock_guard<std::mutex> lk(g_scMutex);
        if (g_scPending.count(key) || g_scFailed.count(key)) return nullptr;
        if (g_scWorkers.load() >= kScMaxWorkers) return nullptr;   // asked again next frame
        g_scPending.insert(key);
        g_scWorkers++;
    }
    std::vector<std::pair<std::string, std::string>> defs;
    for (const D3D_SHADER_MACRO* d = defines; d && d->Name; d++) defs.emplace_back(d->Name, d->Definition ? d->Definition : "");
    std::thread([src, key, e = std::string(entry), t = std::string(target), defs]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        std::vector<D3D_SHADER_MACRO> m;
        for (const auto& d : defs) m.push_back({ d.first.c_str(), d.second.c_str() });
        m.push_back({ nullptr, nullptr });
        const ULONGLONG t0 = GetTickCount64();
        ComPtr<ID3DBlob> blob, err;
        const bool ok = SUCCEEDED(D3DCompile(src, strlen(src), e.c_str(), m.data(), nullptr, e.c_str(), t.c_str(),
                                             0, 0, &blob, &err));
        if (ok) ShaderCachePut(key, blob);
        else fprintf(stderr, "shader '%s' compile error (async, kept on the fallback):\n%s\n", e.c_str(),
                     err ? (char*)err->GetBufferPointer() : "?");
        printf("shader cache: %s %s built on a worker in %llu ms%s\n", e.c_str(), key.c_str(), GetTickCount64() - t0,
               ok ? "" : " (FAILED)");
        std::lock_guard<std::mutex> lk(g_scMutex);
        g_scPending.erase(key);
        if (!ok) g_scFailed.insert(key);
        g_scWorkers--;
    }).detach();
    return nullptr;
}

// post-night T3: the water pre-warm. Blobs only, into the same cache (memory + disk) CompileAsync fills, on ONE
// worker at below-normal priority, in turn: each define set is compiled VS + PS exactly as MakeGraphicsPso would
// ask for it (the sets come with DROP_LIFE already applied), so the first frame that wants the twin finds it
// cached. A key already cached, pending or failed is skipped.
static void ShaderPrewarmAsync(const char* src, std::vector<std::vector<std::pair<std::string, std::string>>> sets) {
    std::thread([src, sets = std::move(sets)]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        static const char* const kEt[2][2] = { { "VSMain", "vs_5_0" }, { "PSMain", "ps_5_0" } };
        int built = 0, skipped = 0, failed = 0;
        const ULONGLONG t0 = GetTickCount64();
        for (const auto& s : sets) {
            std::vector<D3D_SHADER_MACRO> m;
            for (const auto& d : s) m.push_back({ d.first.c_str(), d.second.c_str() });
            m.push_back({ nullptr, nullptr });
            for (const auto& et : kEt) {
                const std::string key = ShaderKey(src, et[0], et[1], m.data());
                if (ShaderCacheGet(key)) { skipped++; continue; }
                {
                    std::lock_guard<std::mutex> lk(g_scMutex);
                    if (g_scPending.count(key) || g_scFailed.count(key)) { skipped++; continue; }
                    g_scPending.insert(key);
                }
                ComPtr<ID3DBlob> blob, err;
                const bool ok = SUCCEEDED(D3DCompile(src, strlen(src), et[0], m.data(), nullptr, et[0], et[1],
                                                     0, 0, &blob, &err));
                if (ok) { ShaderCachePut(key, blob); built++; }
                else {
                    failed++;
                    fprintf(stderr, "shader '%s' compile error (water prewarm):\n%s\n", et[0],
                            err ? (char*)err->GetBufferPointer() : "?");
                }
                std::lock_guard<std::mutex> lk(g_scMutex);
                g_scPending.erase(key);
                if (!ok) g_scFailed.insert(key);
            }
        }
        WpLog("water prewarm: done in %llu s: %d blobs built, %d already cached (or building elsewhere), %d failed",
              (GetTickCount64() - t0) / 1000ull, built, skipped, failed);
    }).detach();
}

// brief BE: the Liquid Acid PSO's defines. LIQUID_ACID switches the look on
// in kDisplaySrc; every row of ACID_SLOTS (src\acid_slots.h) adds
// "#define LA_<NAME> laP<vec>.<c>", so the acid literals read each packed
// scalar by the same name UploadAcidConstants writes it under. Macros only:
// the preprocessed HLSL is token-for-token what the literal said before.
#define ACID_SLOT_MACRO(name, vec, comp, doc) { "LA_" #name, "laP" #vec "." #comp },
static const D3D_SHADER_MACRO kAcidSlotMacros[] = {
    { "LIQUID_ACID", "1" },
    ACID_SLOTS(ACID_SLOT_MACRO)
    { nullptr, nullptr }
};
// Oil3D Stage A: the same list plus OIL_HEIGHT -- the acid PSO variant that
// computes the wax's height field. kAcidSlotMacros itself is untouched, so the
// shipped acid PSO compiles from the identical preprocessed source.
static const D3D_SHADER_MACRO kAcidSlotMacrosH[] = {
    { "LIQUID_ACID", "1" },
    { "OIL_HEIGHT", "1" },
    ACID_SLOTS(ACID_SLOT_MACRO)
    { nullptr, nullptr }
};
// Oil3D C2: OIL_HEIGHT + OIL_COLUMN -- the variant with the lit water column
// (shafts + haze), used only while water_scatter > 0 with a tank. Keeping it a
// separate PSO leaves the OIL_HEIGHT PSO's bytecode exactly e615b23's (an
// inline uniform branch reshuffled its code and moved 42 pixels by one code).
static const D3D_SHADER_MACRO kAcidSlotMacrosC[] = {
    { "LIQUID_ACID", "1" },
    { "OIL_HEIGHT", "1" },
    { "OIL_COLUMN", "1" },
    ACID_SLOTS(ACID_SLOT_MACRO)
    { nullptr, nullptr }
};
// Oil3D L1: each of the three acid variants again with OIL_LAMP (lamp 1's
// spectral window over the film, laLamp). Separate PSOs, built only while the
// window / film dyes differ from today, so the three above keep their bytecode.
static const D3D_SHADER_MACRO kAcidSlotMacrosL[] = {
    { "LIQUID_ACID", "1" },
    { "OIL_LAMP", "1" },
    ACID_SLOTS(ACID_SLOT_MACRO)
    { nullptr, nullptr }
};
static const D3D_SHADER_MACRO kAcidSlotMacrosHL[] = {
    { "LIQUID_ACID", "1" },
    { "OIL_HEIGHT", "1" },
    { "OIL_LAMP", "1" },
    ACID_SLOTS(ACID_SLOT_MACRO)
    { nullptr, nullptr }
};
static const D3D_SHADER_MACRO kAcidSlotMacrosCL[] = {
    { "LIQUID_ACID", "1" },
    { "OIL_HEIGHT", "1" },
    { "OIL_COLUMN", "1" },
    { "OIL_LAMP", "1" },
    ACID_SLOTS(ACID_SLOT_MACRO)
    { nullptr, nullptr }
};
// Oil3D L2: each of the three OIL_LAMP variants again with OIL_LAMP2 (lamp 2:
// laLamp2, its own contact shadow + under-layer, the two-lamp sum). Built only
// while lamp2_level > 0, so the six PSOs above keep their bytecode.
static const D3D_SHADER_MACRO kAcidSlotMacrosL2[] = {
    { "LIQUID_ACID", "1" },
    { "OIL_LAMP", "1" },
    { "OIL_LAMP2", "1" },
    ACID_SLOTS(ACID_SLOT_MACRO)
    { nullptr, nullptr }
};
static const D3D_SHADER_MACRO kAcidSlotMacrosHL2[] = {
    { "LIQUID_ACID", "1" },
    { "OIL_HEIGHT", "1" },
    { "OIL_LAMP", "1" },
    { "OIL_LAMP2", "1" },
    ACID_SLOTS(ACID_SLOT_MACRO)
    { nullptr, nullptr }
};
static const D3D_SHADER_MACRO kAcidSlotMacrosCL2[] = {
    { "LIQUID_ACID", "1" },
    { "OIL_HEIGHT", "1" },
    { "OIL_COLUMN", "1" },
    { "OIL_LAMP", "1" },
    { "OIL_LAMP2", "1" },
    ACID_SLOTS(ACID_SLOT_MACRO)
    { nullptr, nullptr }
};
#undef ACID_SLOT_MACRO

// Aspect-corrected resolution, same as reference getResolution().
static void GetResolution(int base, int screenW, int screenH, int* outW, int* outH) {
    float aspect = (float)screenW / (float)screenH;
    if (aspect < 1.0f) aspect = 1.0f / aspect;
    int mx = (int)lroundf(base * aspect);
    int mn = base;
    if (screenW > screenH) { *outW = mx; *outH = mn; }
    else                   { *outW = mn; *outH = mx; }
}

// --shot determinism: when non-zero, every rand()-driven behavior (startup
// splat burst, wanderer placement, darts, idle bursts, mood jitter) replays
// from this seed instead of the wall clock.
static unsigned g_randSeed = 0;
void FluidRenderer::SetRandomSeed(unsigned seed) { g_randSeed = seed; }

// [liquid_acid] depth_amount: t units of the 2026-10-30 depth sheet's A->C step on top of each key, clamped to the
// key's slider range (never below a value the key already holds past it). t = 0 returns the key untouched.
static float DepthT(const FluidConfig& c) { return fminf(fmaxf(c.acid.depthAmount, 0.0f), 2.0f); }
float FluidRenderer::EffDropletDepth() const {
    const float t = DepthT(m_cfg), v = m_cfg.acid.dropletDepth;
    return t > 0.0f ? fminf(v + 0.45f * t, fmaxf(v, 1.0f)) : v;
}
float FluidRenderer::EffDepthRise() const {
    const float t = DepthT(m_cfg), v = m_cfg.acid.depthRise;
    return t > 0.0f ? fminf(v + 0.35f * t, fmaxf(v, 1.0f)) : v;
}
float FluidRenderer::EffDofMaxPx() const {
    const float t = DepthT(m_cfg), v = m_cfg.post.dofMaxPx;
    return t > 0.0f ? fminf(v + 7.0f * t, fmaxf(v, 16.0f)) : v;
}
float FluidRenderer::EffFocusBandPx() const {
    const float t = DepthT(m_cfg), v = m_cfg.post.focusBandPx;
    return t > 0.0f ? fmaxf(v - 60.0f * t, fminf(v, 40.0f)) : v;
}

void FluidRenderer::Init(HWND hwnd, int width, int height, const FluidConfig& cfg) {
    m_headless = false;
    InitCommon(hwnd, width, height, cfg);
}

// Headless: same setup, no window and no swap chain. CreateDevice branches on
// m_headless and builds an offscreen FP16 render target instead.
void FluidRenderer::InitOffscreen(int width, int height, const FluidConfig& cfg) {
    m_headless = true;
    InitCommon(nullptr, width, height, cfg);
}

void FluidRenderer::InitCommon(HWND hwnd, int width, int height, const FluidConfig& cfg) {
    m_cfg = cfg;
    m_width = width;
    m_height = height;
    if (g_randSeed) {
        srand(g_randSeed);
        m_globalHue = RandF();
    } else {
        m_globalHue = RandF();
        srand(GetTickCount());
    }

    CreateDevice(hwnd, width, height);
    if (FAILED(m_initHr)) return;   // swap chain refused; TryInit() cleans up
    // palette_start_hue (brief BW); -1 = no-op. Once per renderer: a resume
    // (Shutdown + TryInit on the same object) keeps the running clock.
    if (!m_paletteStarted) { m_paletteStarted = true; ApplyPaletteStart("app start"); }
    if (!m_cfg.gradientMode) CreateSimResources();
    printf("Renderer ready (%s), sim %dx%d, dye %dx%d\n",
           m_cfg.gradientMode ? "gradient mode" : "fluid",
           m_simW, m_simH, m_dyeW, m_dyeH);
}

// The one call that is allowed to fail without taking the process with it.
// DXGI refuses a flip-model swap chain with E_ACCESSDENIED while another app
// holds the output exclusively -- a fullscreen game, or DWM composition still
// being torn down -- and the resume out of a fullscreen pause fires at exactly
// that moment. Every attempt is logged with its HRESULT and with whatever owns
// the foreground, because that is the diagnosis.
bool FluidRenderer::CreateSwapChainSoft(HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& sd,
                                        Microsoft::WRL::ComPtr<IDXGISwapChain1>& out) {
    HRESULT hr = m_factory->CreateSwapChainForHwnd(m_queue.Get(), hwnd, &sd,
                                                   nullptr, nullptr, &out);
    if (SUCCEEDED(hr)) return true;
    char fg[256];
    ForegroundDesc(fg, sizeof(fg));
    WpLog("CreateSwapChainForHwnd hwnd=%p %dx%d FAILED hr=0x%08lX  %s",
          (void*)hwnd, (int)sd.Width, (int)sd.Height, (unsigned long)hr, fg);
    printf("CreateSwapChainForHwnd failed (hr=0x%08lX) - %s\n", (unsigned long)hr, fg);
    if (m_softInit) { m_initHr = hr; return false; }
    Fail("m_factory->CreateSwapChainForHwnd(m_queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1)", hr);
    return false;
}

// Init(), but a refused swap chain unwinds to a clean torn-down renderer and
// returns false instead of putting a fatal dialog over a black desktop.
bool FluidRenderer::TryInit(HWND hwnd, int width, int height, const FluidConfig& cfg) {
    m_softInit = true;
    m_initHr = S_OK;
    try {
        InitCommon(hwnd, width, height, cfg);
    } catch (const DeviceLostInInit& e) {   // review 2026-10-04 R3: see HrFailed()
        m_initHr = e.hr;
    }
    m_softInit = false;
    if (FAILED(m_initHr)) {
        Shutdown();       // releases whatever CreateDevice managed to build
        return false;
    }
    m_recoveringDevice = false;   // a device stands again: failures are fatal as before
    return true;
}

// The headless twin of TryInit, for the --test-device-lost self-test: the same
// recovery the live loop runs (Shutdown, then a soft init with the same config).
bool FluidRenderer::TryInitOffscreen(int width, int height, const FluidConfig& cfg) {
    m_headless = true;
    return TryInit(nullptr, width, height, cfg);
}

bool FluidRenderer::TryReattach(HWND hwnd) {
    m_softInit = true;
    m_initHr = S_OK;
    try {
        Reattach(hwnd);
    } catch (const DeviceLostInInit& e) {   // review 2026-10-04 R3: see HrFailed()
        m_initHr = e.hr;
    }
    m_softInit = false;
    // a lost device cannot be reattached: the caller's DeviceLost() check rebuilds it
    return SUCCEEDED(m_initHr) && !m_deviceLost;
}

// --test-device-lost: the D3D12 way to fake a removal (driver crash / update) on our
// own device only, so the recovery can be exercised headless (review 2026-10-04 R3).
bool FluidRenderer::SimulateDeviceRemoved() {
    ComPtr<ID3D12Device5> d5;
    if (!m_device || FAILED(m_device.As(&d5))) return false;
    d5->RemoveDevice();
    return true;
}

void FluidRenderer::CreateDevice(HWND hwnd, int width, int height) {
    UINT factoryFlags = 0;
#ifdef _DEBUG
    {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
            dbg->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif
    HR(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory)));

    ComPtr<IDXGIAdapter1> adapter;
    HR(m_factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                             IID_PPV_ARGS(&adapter)));
    DXGI_ADAPTER_DESC1 adesc = {};
    adapter->GetDesc1(&adesc);
    printf("GPU: %ls\n", adesc.Description);
    // Rebuilding after a device loss while the driver is being replaced, the only
    // adapter left can be the Microsoft Basic Render Driver: a 4K sim on the CPU.
    // Not ready yet -- the caller's backoff tries again (review 2026-10-04 R3).
    if (m_softInit && m_recoveringDevice && (adesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
        WpLog("device rebuild: only the software adapter (%ls) is there, retrying later",
              adesc.Description);
        m_initHr = DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
        return;
    }

    HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)));
    adapter.As(&m_adapter3);   // optional: no IDXGIAdapter3 = VramOverBudget() says false
    m_adapterLuid = adesc.AdapterLuid;
    m_vramTotal = adesc.DedicatedVideoMemory;

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HR(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue)));

    if (!m_headless) {
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = width;
        sd.Height = height;
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kFrames;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        ComPtr<IDXGISwapChain1> sc1;
        if (!CreateSwapChainSoft(hwnd, sd, sc1)) return;
        HR(sc1.As(&m_swapChain));
        m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

        // Colour space, NOT fatal. An output that is being handed back by a
        // fullscreen app can refuse these for a moment, and a wallpaper that
        // is merely in the wrong colour space is still a wallpaper -- dying
        // over it is not a trade anyone would make. ReassertColorSpace() runs
        // on every resume and every reattach anyway, so a refusal here is
        // corrected within a second.
        const DXGI_COLOR_SPACE_TYPE scRGB = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        UINT support = 0;
        HRESULT cshr = m_swapChain->CheckColorSpaceSupport(scRGB, &support);
        if (FAILED(cshr)) {
            WpLog("CheckColorSpaceSupport failed hr=0x%08lX (continuing in the default space)",
                  (unsigned long)cshr);
            printf("WARNING: CheckColorSpaceSupport failed (hr=0x%08lX)\n", (unsigned long)cshr);
        } else if (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) {
            HRESULT sshr = m_swapChain->SetColorSpace1(scRGB);
            if (FAILED(sshr))
                WpLog("SetColorSpace1 failed hr=0x%08lX", (unsigned long)sshr);
            else
                printf("Swap chain: R16G16B16A16_FLOAT, scRGB color space set (1.0 = 80 nits)\n");
        } else {
            printf("WARNING: scRGB color space not supported on this output!\n");
        }
    } else {
        printf("Headless: no swap chain, offscreen R16G16B16A16_FLOAT target "
               "%dx%d (linear scRGB, 1.0 = 80 nits)\n", width, height);
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kFrames * 2 + 5;   // backbuffers + analyzer + mirror buffers + post + Oil3D surface x2 + C2 mass coverage
    HR(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_rtvHeap)));
    m_rtvStride = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrames; i++) {
        if (!m_headless) {   // headless: no back buffers; RTV slot 0 = shot target
            HR(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])));
            m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, rtv);
        }
        rtv.ptr += m_rtvStride;
        HR(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(&m_allocators[i])));
    }
    HR(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                   m_allocators[0].Get(), nullptr, IID_PPV_ARGS(&m_cmd)));
    HR(m_cmd->Close());

    HR(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_frameIndex = m_headless ? 0 : m_swapChain->GetCurrentBackBufferIndex();

    // Shader-visible heap: 2 slots (SRV, UAV) per texture, 16 textures max.
    D3D12_DESCRIPTOR_HEAP_DESC sh = {};
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.NumDescriptors = 34;   // + heap 32 = the Oil3D W1 murk volume (t8, the display table's 7th slot)
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HR(m_device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&m_srvHeap)));
    m_srvStride = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // --- root signatures ---
    // Compute: b0 root constants, t0, t1, t2 (baroclinic dye), u0..u2.
    {
        D3D12_DESCRIPTOR_RANGE rSrv0 = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_DESCRIPTOR_RANGE rSrv1 = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0 };
        D3D12_DESCRIPTOR_RANGE rSrv2 = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 0, 0 };
        D3D12_DESCRIPTOR_RANGE rUav0 = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
        D3D12_DESCRIPTOR_RANGE rUav1 = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0, 0 };
        D3D12_DESCRIPTOR_RANGE rUav2 = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 2, 0, 0 };

        D3D12_ROOT_PARAMETER params[8] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = { 0, 0, sizeof(SimCB) / 4 };
        auto table = [](D3D12_ROOT_PARAMETER& p, D3D12_DESCRIPTOR_RANGE* r) {
            p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p.DescriptorTable = { 1, r };
        };
        table(params[1], &rSrv0);
        table(params[2], &rSrv1);
        table(params[3], &rUav0);
        table(params[4], &rUav1);
        table(params[5], &rUav2);
        table(params[6], &rSrv2);
        // Param 7 is the Liquid Acid blob buffer (t3) as a ROOT descriptor,
        // read only by the oil-mask pass (oil_drag / oil_dye_block). Every
        // other compute shader compiled from kComputeSrc ignores it, so it
        // costs the fluid sim two DWORDs of root signature and nothing else.
        params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[7].Descriptor = { 3, 0 };            // t3

        D3D12_STATIC_SAMPLER_DESC samp = {};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 8;
        rsd.pParameters = params;
        rsd.NumStaticSamplers = 1;
        rsd.pStaticSamplers = &samp;

        ComPtr<ID3DBlob> sig, err;
        HR(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        HR(m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                         IID_PPV_ARGS(&m_computeRS)));
    }
    // Graphics: b0 constants, t0 table, s0 static sampler.
    // Params 2/3 are the Liquid Acid look's blob buffer (t1) and parameter
    // block (b1) as ROOT descriptors â€” no descriptor-heap slots needed. The
    // fluid display shader references neither, so they cost it nothing.
    // Param 4 is the shared ink-in-water parameter block (b2), read by the INK
    // PSO and by LIQUID_ACID when ink_mode=water. Same story: the fluid
    // display shader does not reference it.
    // Param 6 is the [mirror] fold block (b3) â€” 12 ROOT CONSTANTS, because
    // unlike the blocks above this one IS read by all three display PSOs
    // (the fold is in the shared part of the shader), so it must be bound on
    // every display draw and an upload-buffer ring would buy nothing.
    {
        D3D12_DESCRIPTOR_RANGE rSrv0 = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        // Param 5's table: t3 (low-res velocity) and, appended in the same
        // table, t5/t6 = the Oil3D surface pre-pass targets and t7 = the C2
        // padded mass coverage. The root signature is at 64 DWORDs, so the
        // new SRVs ride this table instead of a new parameter (a table is one
        // DWORD whatever its length); shaders that read only t3 see no
        // difference.
        // Oil3D W1: t8 = the murk volume, at table offset 6 (heap 32: heap
        // 30/31 belong to the post target), read by the OIL_MURK variants only.
        D3D12_DESCRIPTOR_RANGE rSrv3[3] = {
            { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3, 0, 0 },
            { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 5, 0, 1 },
            { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 8, 0, 6 } };
        D3D12_ROOT_PARAMETER params[9] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = { 0, 0, 32 };
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = { 1, &rSrv0 };
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[2].Descriptor = { 1, 0 };            // b1
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[3].Descriptor = { 1, 0 };            // t1
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[4].Descriptor = { 2, 0 };            // b2 (InkCB)
        params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[5].DescriptorTable = { 3, rSrv3 };   // t3 (low-res velocity) + t5/t6 (Oil3D surface) + t7 (C2 mass coverage) + t8 (W1 murk)
        params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[6].Constants = { 3, 0, 20 };         // b3 (MirrorCB + [post])
        params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        // Params 7/8 are the droplet particle sim's two structured buffers â€”
        // the particles (t2) and the uniform-grid cell table (t4) â€” as ROOT
        // descriptors, same deal as the blob buffer above: no heap slots, and
        // nothing at all for the shaders that never reference them.
        params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[7].Descriptor = { 2, 0 };            // t2 (AcidDrops)
        params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[8].Descriptor = { 4, 0 };            // t4 (DropCells)
        params[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // s0 linear clamp; s1 linear WRAP (Oil3D W1: the tiling murk volume,
        // read by the OIL_MURK variants only; static samplers cost no DWORDs)
        D3D12_STATIC_SAMPLER_DESC samp[2] = {};
        samp[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp[0].AddressU = samp[0].AddressV = samp[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        samp[1] = samp[0];
        samp[1].AddressU = samp[1].AddressV = samp[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samp[1].ShaderRegister = 1;

        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 9;
        rsd.pParameters = params;
        rsd.NumStaticSamplers = 2;
        rsd.pStaticSamplers = samp;

        ComPtr<ID3DBlob> sig, err;
        HR(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        HR(m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                         IID_PPV_ARGS(&m_graphicsRS)));
    }
    // Oil3D surface pre-pass: its OWN root signature (60 DWORDs): b0 display
    // constants, b1 acid block, t1 blobs, t2 droplets, t4 cells, b3 mirror.
    {
        D3D12_ROOT_PARAMETER params[6] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = { 0, 0, 32 };
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor = { 1, 0 };            // b1
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[2].Descriptor = { 1, 0 };            // t1 blobs
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[3].Descriptor = { 2, 0 };            // t2 droplets
        params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[4].Descriptor = { 4, 0 };            // t4 cells
        params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[5].Constants = { 3, 0, 20 };         // b3 mirror
        for (auto& p : params) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC samp = {};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 6;
        rsd.pParameters = params;
        rsd.NumStaticSamplers = 1;
        rsd.pStaticSamplers = &samp;
        ComPtr<ID3DBlob> sig, err;
        HR(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        HR(m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                         IID_PPV_ARGS(&m_surfRS)));
    }

    // --- compute PSOs ---
    auto makeCS = [&](const char* entry, ComPtr<ID3D12PipelineState>& pso) {
        ComPtr<ID3DBlob> cs = Compile(kComputeSrc, entry, "cs_5_0");
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = m_computeRS.Get();
        cd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        HR(m_device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&pso)));
    };
    makeCS("CSClearV", m_psoClearV);
    makeCS("CSClear4", m_psoClear4);
    makeCS("CSClear1", m_psoClear1);
    makeCS("CSCurl", m_psoCurl);
    makeCS("CSVorticity", m_psoVorticity);
    makeCS("CSDivergence", m_psoDivergence);
    makeCS("CSClearPressure", m_psoClearPressure);
    makeCS("CSPressure", m_psoPressure);
    makeCS("CSGradientSubtract", m_psoGradSub);
    makeCS("CSAdvectVelocity", m_psoAdvectVel);
    makeCS("CSAdvectDye", m_psoAdvectDye);
    makeCS("CSSplatVelocity", m_psoSplatVel);
    makeCS("CSSplatDye", m_psoSplatDye);
    {   // finite-support variant for ink drops (see DROP_COMPACT in shaders.h)
        const D3D_SHADER_MACRO defs[] = { { "DROP_COMPACT", "1" }, { nullptr, nullptr } };
        ComPtr<ID3DBlob> cs = Compile(kComputeSrc, "CSSplatDye", "cs_5_0", defs);
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = m_computeRS.Get();
        cd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        HR(m_device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&m_psoSplatDyeCompact)));
    }
    makeCS("CSDownsample", m_psoDownsample);
    makeCS("CSDiffuseDye", m_psoDiffuseDye);

    // --- graphics PSOs (display + gradient) ---
    auto makeGfx = [&](const char* src, ComPtr<ID3D12PipelineState>& pso,
                       const D3D_SHADER_MACRO* defines = nullptr) {
        MakeGraphicsPso(src, pso, defines);
    };
    makeGfx(kDisplaySrc, m_psoDisplay);
    makeGfx(kGradientSrc, m_psoGradient);
    makeGfx(kPostSrc, m_psoPost);          // [post] image-space camera pass
    // Liquid Acid: the SAME display source compiled with LIQUID_ACID defined.
    // Built only when the look is on, so the normal path pays no compile cost
    // and, more importantly, its own shader has none of this code in it.
    if (m_cfg.acid.enabled) {
        makeGfx(kDisplaySrc, m_psoLiquidAcid, kAcidSlotMacros);
    }
    // "Ink in water": the same display source with INK defined. Same rule â€”
    // built only when the look is on, so style=fluid keeps its exact shader.
    if (m_cfg.ink.enabled) {
        const D3D_SHADER_MACRO defs[] = { { "INK", "1" }, { nullptr, nullptr } };
        makeGfx(kDisplaySrc, m_psoInk, defs);
    }
    CreateAcidBuffers();

    if (m_headless) CreateOffscreenTarget();
}

// One display/gradient graphics PSO. Identical to the lambda CreateDevice used
// to inline, so a PSO compiled later by EnsureLookResources() is built exactly
// the same way as one compiled at device creation.
bool FluidRenderer::MakeGraphicsPso(const char* src,
                                    Microsoft::WRL::ComPtr<ID3D12PipelineState>& pso,
                                    const D3D_SHADER_MACRO* defines, bool async) {
    const bool bg = async && !m_headless;
    ComPtr<ID3DBlob> vs = bg ? CompileAsync(src, "VSMain", "vs_5_0", defines) : Compile(src, "VSMain", "vs_5_0", defines);
    ComPtr<ID3DBlob> ps = bg ? CompileAsync(src, "PSMain", "ps_5_0", defines) : Compile(src, "PSMain", "ps_5_0", defines);
    if (!vs || !ps) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_graphicsRS.Get();
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pd.SampleDesc.Count = 1;
    HR(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));
    if (bg) WpLog("shader cache: an on-demand display variant is ready (%d defines)",
                  [defines] { int n = 0; for (const D3D_SHADER_MACRO* d = defines; d && d->Name; d++) n++; return n; }());
    return true;
}

// Runtime look switching. The upload rings (acid blobs, acid params, InkCB)
// and the 64x36 velocity readback are already created unconditionally by
// CreateAcidBuffers() / CreateSimResources(), and the acid blob population is
// (re)seeded lazily by Frame(); the ONLY thing missing when a look is turned
// on after startup is its display PSO. Compiling one takes ~0.3 s (D3DCompile
// of kDisplaySrc), so do it off the GPU timeline: idle-wait first, exactly as
// SetResolutions does, then compile. Both branches are no-ops once built, so
// this is safe to call on every preset apply and every checkbox click.
void FluidRenderer::EnsureLookResources() {
    if (!m_device || m_deviceLost) return;   // lost: the rebuild compiles it (R3)
    const bool needAcid = m_cfg.acid.enabled && !m_psoLiquidAcid;
    const bool needInk  = m_cfg.ink.enabled  && !m_psoInk;
    m_lastLookCompileMs = 0.0;
    if (!needAcid && !needInk) return;
    LARGE_INTEGER qf, q0, q1;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&q0);
    WaitForGpuIdle();
    if (m_deviceLost) return;
    if (needAcid) {
        MakeGraphicsPso(kDisplaySrc, m_psoLiquidAcid, kAcidSlotMacros);
        m_acidSeeded = false;          // Frame() reseeds under the current keys
        m_acidTurnAcc = 0.0f;          // brief BZ: turnover restarts with the population
        printf("look: liquid_acid PSO compiled on demand\n");
    }
    if (needInk) {
        const D3D_SHADER_MACRO defs[] = { { "INK", "1" }, { nullptr, nullptr } };
        MakeGraphicsPso(kDisplaySrc, m_psoInk, defs);
        printf("look: ink PSO compiled on demand\n");
    }
    QueryPerformanceCounter(&q1);
    m_lastLookCompileMs = 1000.0 * (double)(q1.QuadPart - q0.QuadPart) / (double)qf.QuadPart;
    printf("look: PSO compile took %.0f ms\n", m_lastLookCompileMs);
}

// Oil3D Stage A: the OIL_HEIGHT acid PSO, compiled the first time oil_height
// (or --shot-debug) is on with the acid look, and kept -- the BN/BX post
// variants' pattern. PSO creation is off the command list; ~0.3 s once.
void FluidRenderer::EnsureOilHeightPso() {
    if (!m_device || m_deviceLost || m_psoLiquidAcidH) return;
    if (!m_cfg.acid.enabled || !OilHeightWanted()) return;
    MakeGraphicsPso(kDisplaySrc, m_psoLiquidAcidH, kAcidSlotMacrosH);
    printf("look: liquid_acid OIL_HEIGHT PSO compiled on demand\n");
    // the surface pre-pass: the same source with OIL_SURFACE, two FP16 targets
    {
        std::vector<D3D_SHADER_MACRO> defs(kAcidSlotMacrosH, kAcidSlotMacrosH
                                           + sizeof(kAcidSlotMacrosH) / sizeof(kAcidSlotMacrosH[0]));
        defs.insert(defs.begin(), D3D_SHADER_MACRO{ "OIL_SURFACE", "1" });
        ComPtr<ID3DBlob> vs = Compile(kDisplaySrc, "VSMain", "vs_5_0", defs.data());
        ComPtr<ID3DBlob> ps = Compile(kDisplaySrc, "PSMain", "ps_5_0", defs.data());
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = m_surfRS.Get();
        pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;   // P1: (wax, h_oil)
        pd.SampleDesc.Count = 1;
        HR(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_psoSurface)));
        printf("look: Oil3D surface pre-pass PSO compiled\n");
    }
}

// heap 26 = a second SRV of m_velLow, 27 = the surface target (28 = a null
// SRV since P1: the second target is gone), 29 = the C2 mass coverage, 32 =
// the W1 murk volume (null SRVs until they exist): the display root
// signature's param-5 table starts here.
void FluidRenderer::WriteVelTable() {
    const UINT base = 26;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE c = cpu; c.ptr += (SIZE_T)base * m_srvStride;
    m_device->CreateShaderResourceView(m_velLow.res.Get(), &sv, c);
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC ss = sv;
        ss.Format = DXGI_FORMAT_R16G16_FLOAT;
        D3D12_CPU_DESCRIPTOR_HANDLE ck = cpu; ck.ptr += (SIZE_T)(base + 1) * m_srvStride;
        m_device->CreateShaderResourceView(m_surfTex.Get(), &ss, ck);   // null resource = null SRV
        ck.ptr += m_srvStride;
        m_device->CreateShaderResourceView(nullptr, &sv, ck);           // t6: unused (null)
    }
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sc = sv;
        sc.Format = DXGI_FORMAT_R16_FLOAT;
        D3D12_CPU_DESCRIPTOR_HANDLE ck = cpu; ck.ptr += (SIZE_T)(base + 3) * m_srvStride;
        m_device->CreateShaderResourceView(m_massCovTex.Get(), &sc, ck);   // null until built
    }
    WriteMurkSrv();   // heap 32 = base + 6: t8, the Oil3D W1 murk volume (null until baked)
    m_velTable = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    m_velTable.ptr += (SIZE_T)base * m_srvStride;
}

// P1: ONE R16G16_FLOAT target (wax coverage, h_oil): the only two channels a
// reader samples. It was two RGBA16F targets (16 bytes/px written, 4 read).
void FluidRenderer::EnsureSurfaceTargets() {
    if (m_surfTex && m_surfW == m_width && m_surfH == m_height) return;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT64)m_width;
    rd.Height = (UINT)m_height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R16G16_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    m_surfTex.Reset();
    HR(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                         nullptr, IID_PPV_ARGS(&m_surfTex)));
    if (!m_surfTex) return;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)(kFrames * 2 + 2) * m_rtvStride;
    m_device->CreateRenderTargetView(m_surfTex.Get(), nullptr, rtv);
    m_surfState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_surfW = m_width; m_surfH = m_height;
    WriteVelTable();
    printf("Oil3D: surface pre-pass target %dx%d R16G16_FLOAT\n", m_width, m_height);
}

// The surface PRE-PASS: the masses' coverage and h_oil to one R16G16F target
// (P1), for the non-local Oil3D terms (the lamp and contact shadows today).
// Runs only while oil_height (or a --shot-debug) is on with the acid look;
// otherwise it records nothing at all (marks 1 and 2 collapse to 0 ms).
// P1 timing experiments: FW_P1_EXP bits, read once, honoured only in a headless
// run with --timings (a live or plain --shot run never sees them):
//   1 skip the pre-pass draw   2 skip its barriers   32 skip the mass coverage
// (4/8/16, the per-target write masks of the two-target layout, went with it)
int FluidRenderer::P1Exp() const {
    static int v = -1;
    if (v < 0) {
        char buf[32] = {};
        size_t n = 0;
        v = (getenv_s(&n, buf, sizeof(buf), "FW_P1_EXP") == 0 && n > 0) ? atoi(buf) : 0;
        if (v) printf("P1: FW_P1_EXP=%d (timing experiment)\n", v);
    }
    return (m_headless && m_timingsOn) ? v : 0;
}

void FluidRenderer::RenderSurfacePass() {
    TimestampMark(1);
    bool marked = false;
    if (m_cfg.acid.enabled && SurfacePassWanted() && !m_photoMode && m_psoSurface && m_surfRS) {
        EnsureSurfaceTargets();
        if (m_surfTex) {
            const int exp = P1Exp();
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = m_surfTex.Get();
            b.Transition.StateBefore = m_surfState;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            if (m_surfState != D3D12_RESOURCE_STATE_RENDER_TARGET && !(exp & 2))
                m_cmd->ResourceBarrier(1, &b);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += (SIZE_T)(kFrames * 2 + 2) * m_rtvStride;
            m_cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            D3D12_VIEWPORT vp = { 0, 0, (float)m_width, (float)m_height, 0, 1 };
            D3D12_RECT sc = { 0, 0, m_width, m_height };
            m_cmd->RSSetViewports(1, &vp);
            m_cmd->RSSetScissorRects(1, &sc);
            m_cmd->SetGraphicsRootSignature(m_surfRS.Get());
            m_cmd->SetPipelineState(m_psoSurface.Get());
            float consts[32];
            BuildDisplayConstants(consts);
            m_cmd->SetGraphicsRoot32BitConstants(0, 32, consts, 0);
            const UINT fi = m_frameIndex;
            if (m_acidParamUpload[fi])
                m_cmd->SetGraphicsRootConstantBufferView(1, m_acidParamUpload[fi]->GetGPUVirtualAddress());
            if (m_acidBlobUpload[fi])
                m_cmd->SetGraphicsRootShaderResourceView(2, m_acidBlobUpload[fi]->GetGPUVirtualAddress());
            if (m_dropletUpload[fi])
                m_cmd->SetGraphicsRootShaderResourceView(3, m_dropletUpload[fi]->GetGPUVirtualAddress());
            if (m_dropletCellUpload[fi])
                m_cmd->SetGraphicsRootShaderResourceView(4, m_dropletCellUpload[fi]->GetGPUVirtualAddress());
            float mc[20];
            BuildMirrorConstants(mc, m_width, m_height);
            m_cmd->SetGraphicsRoot32BitConstants(5, 20, mc, 0);
            m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            TimestampMark(10);
            if (!(exp & 1)) m_cmd->DrawInstanced(3, 1, 0, 0);
            TimestampMark(11);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            if (!(exp & 2)) m_cmd->ResourceBarrier(1, &b);
            m_surfState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            TimestampMark(12);
            marked = true;
            if (!(exp & 32)) RenderMassCovPass();   // Oil3D C2: records nothing unless water_scatter > 0
        }
    }
    if (!marked) { TimestampMark(10); TimestampMark(11); TimestampMark(12); }
    TimestampMark(2);
}

// ---- Oil3D L1: the OIL_LAMP display variants --------------------------------
// The variant this frame WANTS (not the one already built): the lamp variant
// of it is compiled the first time lamp 1's window or a film dye leaves
// today's state, and kept. ~0.3 s once, off the command list.
void FluidRenderer::EnsureLampPso() {
    if (!m_device || m_deviceLost || !m_cfg.acid.enabled) return;
    const bool l1 = m_cfg.acid.LampWanted(), l2 = m_cfg.acid.Lamp2Wanted();
    if (!m_cfg.acid.LampPathWanted()) return;
    const int v = MassCovWanted() ? 2 : (OilHeightWanted() ? 1 : 0);
    // Oil3D L3: a key with an L3 path = L1's or L2's macro list + the path's
    // defines (inserted before the slot macros), its own PSO; keys 0 / 1 are
    // the L1 / L2 PSOs below, unchanged.
    const int key = LampKey();
    if (key > 1) {
        if (!m_psoLampX[v][key]) {
            static const D3D_SHADER_MACRO* const kBase[2][3] = {
                { kAcidSlotMacrosL,  kAcidSlotMacrosHL,  kAcidSlotMacrosCL },
                { kAcidSlotMacrosL2, kAcidSlotMacrosHL2, kAcidSlotMacrosCL2 } };
            // L3b: key bit 16 = the plain acid variant (no OIL_LAMP) + OIL_MASSDYE
            static const D3D_SHADER_MACRO* const kPlain[3] = { kAcidSlotMacros, kAcidSlotMacrosH, kAcidSlotMacrosC };
            std::vector<D3D_SHADER_MACRO> defs;
            const D3D_SHADER_MACRO* b = (key & 16) ? kPlain[v] : kBase[(key & 1) ? 1 : 0][v];
            for (; b->Name && strncmp(b->Name, "LA_", 3) != 0; b++) defs.push_back(*b);
            if (key & 2) defs.push_back({ "OIL_L2FILL", "1" });
            if (key & 4) defs.push_back({ "OIL_FLUIDB", "1" });
            if (key & 8) defs.push_back({ "OIL_MASSDYE", "1" });
            if (key & 32) defs.push_back({ "OIL_DYEWHITE", "1" });
            for (; b->Name; b++) defs.push_back(*b);
            defs.push_back({ nullptr, nullptr });
            // live: built on a worker (the director moves the lamps; each new key froze the picture for its compile)
            if (MakeGraphicsPso(kDisplaySrc, m_psoLampX[v][key], defs.data(), true))
            printf("look: liquid_acid lamp PSO (variant %d, key %d:%s%s%s%s%s%s) compiled on demand\n", v, key,
                   (key & 1) ? " OIL_LAMP2" : "", (key & 2) ? " OIL_L2FILL" : "", (key & 4) ? " OIL_FLUIDB" : "",
                   (key & 8) ? " OIL_MASSDYE" : "", (key & 16) ? " (no OIL_LAMP: white lamps)" : "",
                   (key & 32) ? " OIL_DYEWHITE" : "");
        }
        return;
    }
    if (l1 && !m_psoLiquidAcidL[v]) {
        static const D3D_SHADER_MACRO* const kDefs[3] = { kAcidSlotMacrosL, kAcidSlotMacrosHL,
                                                          kAcidSlotMacrosCL };
        if (MakeGraphicsPso(kDisplaySrc, m_psoLiquidAcidL[v], kDefs[v], true))
            printf("look: liquid_acid OIL_LAMP PSO (variant %d) compiled on demand\n", v);
    }
    // Oil3D L2: lamp 2 on = the OIL_LAMP2 twin of the same variant (it carries
    // lamp 1's window, the white track and the cap too, so it is all a frame
    // with lamp 2 needs, whatever lamp 1 does)
    if (l2 && !m_psoLiquidAcidL2[v]) {
        static const D3D_SHADER_MACRO* const kDefs2[3] = { kAcidSlotMacrosL2, kAcidSlotMacrosHL2,
                                                           kAcidSlotMacrosCL2 };
        if (MakeGraphicsPso(kDisplaySrc, m_psoLiquidAcidL2[v], kDefs2[v], true))
            printf("look: liquid_acid OIL_LAMP2 PSO (variant %d) compiled on demand\n", v);
    }
}

// ---- DROP_LIFE: droplet_shadow / droplet_wobble (2026-11-02) ------------------
// The flag the compiler reads (g_dropLife, DropLifeDefs) follows the keys. A change idles the GPU (the old
// PSOs may still be in flight) and drops the acid display PSOs: the shipped one is rebuilt here at once
// (DisplayPso() would otherwise fall back to the fluid look), the Oil3D ones by this frame's own
// EnsureOilHeightPso / EnsureMassCovPso / EnsureLampPso / EnsureMurk calls, which run right after this.
// The coverage PSO (PSCoverage) has no droplet in it and is kept.
void FluidRenderer::SyncDropLife() {
    if (!m_device || m_deviceLost) return;
    const bool want = m_cfg.acid.enabled && m_cfg.acid.DropLifeWanted();
    const int  wantX = want ? m_cfg.acid.DropLifeExtra() : 0;   // post-night T4
    const int  ot = m_cfg.acid.oilSpecTint;                      // T7: oil_spec_tint, any look
    const int  wantT = (m_cfg.acid.enabled && ot >= 1 && ot <= 2) ? ot : 0;
    // T8b: film_hue2_seam_stretch in quarter steps (0..6 -> 0..24), any look
    const int  wantQ = m_cfg.acid.enabled ? (int)fminf(fmaxf(m_cfg.acid.filmHue2SeamStretch * 4.0f + 0.5f, 0.0f), 24.0f) : 0;
    // film_hue2_leak: amount and reach in twentieths, the core in quarters, packed; any look
    int wantL = 0;
    if (m_cfg.acid.enabled && m_cfg.acid.filmHue2Leak > 0.024f) {
        const int la = (int)fminf(fmaxf(m_cfg.acid.filmHue2Leak * 20.0f + 0.5f, 1.0f), 24.0f);
        const int lr = (int)fminf(fmaxf(m_cfg.acid.filmHue2LeakReach * 20.0f + 0.5f, 1.0f), 24.0f);
        const int lc = (int)fminf(fmaxf(m_cfg.acid.filmHue2LeakCore * 4.0f + 0.5f, 1.0f), 24.0f);
        const int li = (int)fminf(fmaxf(m_cfg.acid.filmHue2LeakIn * 20.0f + 0.5f, 0.0f), 24.0f);
        wantL = la | (lr << 8) | (lc << 16) | (li << 24);
    }
    if (want == g_dropLife.load() && wantX == g_dropLifeX.load() && wantT == g_specTint.load()
        && wantQ == g_seamStretch.load() && wantL == g_seamLeak.load()) return;
    WaitForGpuIdle();
    if (m_deviceLost) return;
    g_dropLife.store(want);
    g_dropLifeX.store(wantX);
    g_specTint.store(wantT);
    if (wantQ != g_seamStretch.load()) printf("look: SEAM_STRETCH %d/4 (film_hue2_seam_stretch %.2f)\n", wantQ, m_cfg.acid.filmHue2SeamStretch);
    g_seamStretch.store(wantQ);
    if (wantL != g_seamLeak.load()) printf("look: SEAM_LEAK %d/20 reach %d/20 core %d/4 in %d/20\n", wantL & 255, (wantL >> 8) & 255, (wantL >> 16) & 255, (wantL >> 24) & 31);
    g_seamLeak.store(wantL);
    m_psoLiquidAcidH.Reset();
    m_psoSurface.Reset();
    m_psoLiquidAcidC.Reset();
    for (auto& pl : m_psoLiquidAcidL) pl.Reset();
    for (auto& pl : m_psoLiquidAcidL2) pl.Reset();
    for (auto& row : m_psoLampX) for (auto& pl : row) pl.Reset();
    for (auto& row : m_psoMurk) for (auto& pl : row) pl.Reset();
    m_psoTap.clear();
    m_psoLiquidAcid.Reset();
    if (m_cfg.acid.enabled) MakeGraphicsPso(kDisplaySrc, m_psoLiquidAcid, kAcidSlotMacros);
    printf("look: DROP_LIFE %s%s%s%s (droplet_shadow %.2f, droplet_wobble %.2f, droplet_core %.2f): acid PSOs rebuilt\n",
           want ? "on" : "off", (wantX & 1) ? " +DROP_RIGLAMP" : "", (wantX & 2) ? " +DROP_HAZERIM" : "",
           wantT == 1 ? " +SPEC_TINT=1" : (wantT == 2 ? " +SPEC_TINT=2" : ""),
           m_cfg.acid.dropletShadow, m_cfg.acid.dropletWobble, m_cfg.acid.dropletCore);
}

void FluidRenderer::SyncPostBlackSoft() {
    if (!m_device || m_deviceLost) return;
    const bool want = m_cfg.post.massBlackSoft && m_cfg.post.massBlack > 0.0005f;
    const bool wantA = m_cfg.post.bloomAmbient;                  // T7: bloom_ambient (BN_OPTICS family)
    if (want == g_postBlackSoft.load() && wantA == g_postBloomAmb.load()) return;
    WaitForGpuIdle();
    if (m_deviceLost) return;
    const bool ambChanged = wantA != g_postBloomAmb.load();
    g_postBlackSoft.store(want);
    g_postBloomAmb.store(wantA);
    m_psoPostBlack.Reset();
    m_psoPostL[3].Reset();      // the POST_BLACK + LAMP_GLOW twin
    if (ambChanged) {           // every BN_OPTICS post PSO
        m_psoPostBN.Reset(); m_psoPostHeart.Reset();
        for (auto& pl : m_psoPostL) pl.Reset();
    }
    m_psoTap.clear();
    printf("post: POST_BLACK_SOFT %s, POST_BLOOM_AMB %s: the post PSOs rebuild\n", want ? "on" : "off",
           wantA ? "on" : "off");
}

// ---- Oil3D C2: the PADDED MASS COVERAGE ------------------------------------
// One small PSO: kDisplaySrc's PSCoverage (the blob loop alone: field below the
// isoline = a mass) on the surface pass's root signature, one R16F target.
void FluidRenderer::EnsureMassCovPso() {
    if (!m_device || m_deviceLost) return;
    if (!m_psoLiquidAcidC) {
        MakeGraphicsPso(kDisplaySrc, m_psoLiquidAcidC, kAcidSlotMacrosC);
        printf("look: liquid_acid OIL_HEIGHT + OIL_COLUMN PSO compiled on demand\n");
    }
    if (m_psoMassCov || !m_surfRS) return;
    std::vector<D3D_SHADER_MACRO> defs(kAcidSlotMacrosH, kAcidSlotMacrosH
                                       + sizeof(kAcidSlotMacrosH) / sizeof(kAcidSlotMacrosH[0]));
    defs.insert(defs.begin(), D3D_SHADER_MACRO{ "OIL_SURFACE", "1" });
    ComPtr<ID3DBlob> vs = Compile(kDisplaySrc, "VSMain", "vs_5_0", defs.data());
    ComPtr<ID3DBlob> ps = Compile(kDisplaySrc, "PSCoverage", "ps_5_0", defs.data());
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_surfRS.Get();
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R16_FLOAT;
    pd.SampleDesc.Count = 1;
    HR(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_psoMassCov)));
    printf("look: Oil3D C2 mass-coverage PSO compiled\n");
}

// ---- Oil3D W1: MURK, the baked density volume ------------------------------
// User 2026-10-29: "give the fluid noise and have the light interact with
// that", "different algorithms ... very spikey vs very gentle", "figure out
// how to combine it". The density is ONE tileable volume (N^2 x N/4 texels,
// R16F, MurkDims: 128 x 128 x 32 up to murk_scale 0.5) over the unit cube,
// four periodic noise LAYERS, each shaped and normalised to mean 1,
// multiplied (independent mean-1 fields keep mean 1), then divided by the
// product's own mean: d = 1 + water_murk x murk_cloud_share (D - 1). The
// average density is today's at every setting; spiky vs gentle moves the
// TEXTURE only. Baked on the CPU (threads over z slices) whenever a baked key
// changes, uploaded on the next frame's list; the shader reads one tap per
// column depth (OilMurkAt, OIL_MURK only).
static float MurkHash(int x, int y, int z, int s) {
    uint32_t n = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)z * 1442695041u
               + (uint32_t)s * 2654435761u;
    n = (n ^ (n >> 13)) * 1274126177u;
    n ^= n >> 16;
    return (float)(n & 0xFFFFFFu) / 16777215.0f;
}
// periodic value noise: (x, y, z) in cells, period P cells on every axis
static float MurkValue(float x, float y, float z, int P, int s) {
    const int xi = (int)floorf(x), yi = (int)floorf(y), zi = (int)floorf(z);
    const float fx = x - xi, fy = y - yi, fz = z - zi;
    const float u = fx * fx * (3 - 2 * fx), v = fy * fy * (3 - 2 * fy), w = fz * fz * (3 - 2 * fz);
    auto H = [&](int i, int j, int k) {
        return MurkHash(((xi + i) % P + P) % P, ((yi + j) % P + P) % P, ((zi + k) % P + P) % P, s); };
    const float x00 = H(0, 0, 0) + (H(1, 0, 0) - H(0, 0, 0)) * u, x10 = H(0, 1, 0) + (H(1, 1, 0) - H(0, 1, 0)) * u;
    const float x01 = H(0, 0, 1) + (H(1, 0, 1) - H(0, 0, 1)) * u, x11 = H(0, 1, 1) + (H(1, 1, 1) - H(0, 1, 1)) * u;
    const float y0 = x00 + (x10 - x00) * v, y1 = x01 + (x11 - x01) * v;
    return y0 + (y1 - y0) * w;
}
// fBm over the unit cube: octave o has period P0 << o
static float MurkFbm(float x, float y, float z, int P0, int oct, int s) {
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    for (int o = 0; o < oct; o++) {
        const int P = P0 << o;
        sum += amp * MurkValue(x * P, y * P, z * P, P, s + 17 * o);
        norm += amp; amp *= 0.5f;
    }
    return sum / norm;
}
// periodic "marine snow": one fleck per cell, random radius 0.2..0.65 cell and weight 0.35..1, smoothstep
// falloff, the max over the neighbours. The clumps layer since v3: v2's Worley F1 (equal round cells)
// read as a polka-dot grid, and as fish scales once warped; these vary in size and brightness like flocs.
static float MurkFlecks(float x, float y, float z, int P, int s) {
    x *= P; y *= P; z *= P;
    const int xi = (int)floorf(x), yi = (int)floorf(y), zi = (int)floorf(z);
    float best = 0.0f;
    for (int k = -1; k <= 1; k++) for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) {
        const int cx = ((xi + i) % P + P) % P, cy = ((yi + j) % P + P) % P, cz = ((zi + k) % P + P) % P;
        const float px = xi + i + MurkHash(cx, cy, cz, s), py = yi + j + MurkHash(cx, cy, cz, s + 1),
                    pz = zi + k + MurkHash(cx, cy, cz, s + 2);
        const float r = 0.2f + 0.45f * MurkHash(cx, cy, cz, s + 3), wgt = 0.35f + 0.65f * MurkHash(cx, cy, cz, s + 4);
        const float d = sqrtf((px - x) * (px - x) + (py - y) * (py - y) + (pz - z) * (pz - z));
        const float t = fmaxf(0.0f, 1.0f - d / r);
        best = fmaxf(best, wgt * t * t * (3.0f - 2.0f * t));
    }
    return best;
}

// post-night T1: one worker bake, shared by the renderer (m_murkJob) and its thread; whoever lets go
// last frees it, so a teardown, a device loss or the exit never leaves the thread a dangling pointer
struct FluidRenderer::MurkJob {
    MurkBakeKeys keys = {};
    float key[9] = {};                 // EnsureMurk's key array at the start (compared when it lands)
    std::vector<uint16_t> texels;
    float dMin = 0.0f, dMax = 0.0f;
    unsigned long long ms = 0;
    std::atomic<bool> done{ false };
};

// a non-negative float -> IEEE half, round to nearest (the density is 0..~20)
static uint16_t MurkHalf(float f) {
    if (!(f > 0.0f)) return 0;
    uint32_t x; memcpy(&x, &f, sizeof(x));
    const int32_t e = (int32_t)((x >> 23) & 0xFFu) - 127 + 15;
    if (e >= 31) return 0x7BFFu;                       // clamp to the largest finite half
    if (e <= 0) return 0;                              // below the normal range: 0
    uint32_t h = ((uint32_t)e << 10) | ((x & 0x7FFFFFu) >> 13);
    if ((x >> 12) & 1u) h++;
    return (uint16_t)(h > 0x7BFFu ? 0x7BFFu : h);
}

// Oil3D W3 (user 2026-11-02: "the murk render resolution probably needs to be
// turned wayyyy up"): one tile spans murk_scale frame heights, so at the fixed
// 128 texels a live scale of 1.2 drew the clouds with 13.5-px texels (1440p)
// and trilinear's kinks between them. The volume now keeps the texels per
// frame height of scale 0.5 (5.6 px): N = 128 S / 0.5, in steps of 16, 128 up
// to scale 0.5 (today's volume, to the bit) and capped at kMurkNMax (384 from
// scale 1.5: the bake's transient memory, NT x (live layers + 1) floats, and
// its time grow with N^3); NZ = N / 4 like today's 128 / 32.
void FluidRenderer::MurkDims(int& n, int& nz) const {
    // only while the W3 fine silt / depth haze is on (GrainWanted): with both 0 the
    // volume stays today's 128 x 128 x 32 at every murk_scale (the live frame, to the bit)
    if (!m_cfg.acid.GrainWanted()) { n = kMurkN; nz = kMurkNZ; return; }
    const float S = fminf(fmaxf(m_cfg.acid.murkScale, 0.05f), 8.0f);
    int v = (int)(128.0f * S / 0.5f + 0.5f);
    v = (v + 8) / 16 * 16;
    n  = v < kMurkN ? kMurkN : (v > kMurkNMax ? kMurkNMax : v);
    nz = n / 4;
}

// post-night T1: static, from the keys alone (MurkBakeKeys, copied on the main thread), so the live
// wallpaper runs it on a worker; `worker` = that path: half the cores, below normal priority (the
// thread count never changes a texel: each slice is its own, the sums below run in one order)
void FluidRenderer::BakeMurk(const MurkBakeKeys& a, std::vector<uint16_t>& out, float& dMin, float& dMax,
                             bool worker) {
    const int N = a.n, NZ = a.nz;
    const int NT = N * N * NZ;
    // a layer amount above 1 clips that layer's thin parts to 0 (clear pockets between the silt); the
    // product is renormalised below, so the mean stays 1 at any amount (v3 sheet: the silt at 1 was too faint)
    const float amt[4] = { fminf(fmaxf(a.clouds, 0.0f), 2.0f), fminf(fmaxf(a.waves, 0.0f), 2.0f),
                           fminf(fmaxf(a.strands, 0.0f), 2.0f), fminf(fmaxf(a.clumps, 0.0f), 2.0f) };
    const float shape = fminf(fmaxf(a.shape, 0.1f), 12.0f);
    const float warp = fminf(fmaxf(a.warp, 0.0f), 1.0f);
    // W3: raw holds only the LIVE layers (slot j of nA per texel; the live layout's
    // two = 8 bytes a texel), and later the product D in each texel's slot 0: the
    // same values summed in the same order as the old 4-slot array, so a bake of
    // the old size is today's to the bit
    int lay[4] = {}, nA = 0;
    for (int l = 0; l < 4; l++) if (amt[l] > 0.0005f) lay[nA++] = l;
    const int nS = nA > 0 ? nA : 1;
    std::vector<float> raw((size_t)NT * nS, 0.0f);
    // layer l at (x, y, z): the texel centre's cube coordinate after the warp; the clumps read
    // (hx, hy, hz), half the warp, so a fleck stays a fleck instead of smearing into an ellipse
    auto layer = [](int l, float x, float y, float z, float hx, float hy, float hz) -> float {
        const float TAU = 6.2831853f;
        switch (l) {
        case 0: return MurkFbm(x, y, z, 4, 4, 101);                         // clouds
        case 1: {                                                          // waves: integer wave vectors = periodic
            const float s = sinf(TAU * (2 * x + z)) + sinf(TAU * (x + 3 * y) + 1.1f)
                          + sinf(TAU * (-3 * x + y + z) + 2.3f);
            return (s / 3.0f + 1.0f) * 0.5f; }
        case 2: return 1.0f - fabsf(2.0f * MurkFbm(x, y, z, 3, 3, 211) - 1.0f);   // strands (ridged)
        default: return MurkFlecks(hx, hy, hz, 12, 307);                    // clumps (marine snow)
        }
    };
    unsigned nth = std::thread::hardware_concurrency();
    if (worker) nth /= 2;
    nth = nth < 1 ? 1 : (nth > 16 ? 16 : nth);
    std::vector<std::thread> th;
    for (unsigned w = 0; w < nth; w++) {
        th.emplace_back([&, w]() {
            if (worker) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            for (int z = (int)w; z < NZ; z += (int)nth)
                for (int y = 0; y < N; y++)
                    for (int x = 0; x < N; x++) {
                        const size_t i = ((size_t)z * N + y) * N + x;
                        float fx = (x + 0.5f) / N, fy = (y + 0.5f) / N, fz = (z + 0.5f) / NZ;
                        float hx = fx, hy = fy, hz = fz;
                        // murk_warp: ONE shared, periodic offset field (fBm, periods 2 and 4) moves the
                        // lookup of every layer the same way, so clouds go wispy, strands become streaks
                        // and the clumps irregular flecks, and the layers stay one structure. v2 without
                        // it read as round soft disks (out-of-focus lights), not silt.
                        if (warp > 0.0005f) {
                            const float k = 0.7f * warp;
                            const float ox = MurkFbm(fx, fy, fz, 2, 2, 401) - 0.5f;
                            const float oy = MurkFbm(fx, fy, fz, 2, 2, 419) - 0.5f;
                            const float oz = MurkFbm(fx, fy, fz, 2, 2, 433) - 0.5f;
                            hx += 0.5f * k * ox; hy += 0.5f * k * oy; hz += 0.5f * k * oz;
                            fx += k * ox; fy += k * oy; fz += k * oz;
                        }
                        for (int j = 0; j < nA; j++)
                            raw[i * nS + j] = powf(fminf(fmaxf(layer(lay[j], fx, fy, fz, hx, hy, hz), 0.0f), 1.0f),
                                                   shape);
                    }
        });
    }
    for (auto& t : th) t.join();
    // each layer: 1 + amount (n / mean - 1); the product; its mean -> 1
    double mean[4] = {};
    for (int i = 0; i < NT; i++) for (int j = 0; j < nA; j++) mean[j] += raw[(size_t)i * nS + j];
    for (int j = 0; j < 4; j++) mean[j] = mean[j] / NT > 1e-6 ? mean[j] / NT : 1.0;
    double dSum = 0.0;
    for (int i = 0; i < NT; i++) {
        float v = 1.0f;
        for (int j = 0; j < nA; j++)
            v *= fmaxf(0.0f, 1.0f + amt[lay[j]] * (float)(raw[(size_t)i * nS + j] / mean[j] - 1.0));
        raw[(size_t)i * nS] = v; dSum += v;   // D, in the texel's slot 0 (its layers are read above)
    }
    const float inv = dSum > 1e-6 ? (float)(NT / dSum) : 1.0f;
    // murk_cloud_share (W3): the clouds' contrast; x 1.0 = today's bake exactly
    const float m = fminf(fmaxf(a.waterMurk, 0.0f), 1.0f) * fminf(fmaxf(a.cloudShare, 0.0f), 1.0f);
    out.resize((size_t)NT);
    dMin = 1e9f; dMax = 0.0f;
    for (int i = 0; i < NT; i++) {
        const float d = fmaxf(0.0f, 1.0f + m * (raw[(size_t)i * nS] * inv - 1.0f));
        dMin = fminf(dMin, d); dMax = fmaxf(dMax, d);
        out[i] = MurkHalf(d);
    }
}

// heap 32 (the display table's offset 6): t8 = the murk volume, a null
// Texture3D SRV until the first bake
void FluidRenderer::WriteMurkSrv() {
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R16_FLOAT;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture3D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE c = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    c.ptr += (SIZE_T)32 * m_srvStride;
    m_device->CreateShaderResourceView(m_murkTex.Get(), &sv, c);
}

// The murk PSO the frame wants: the same choice DisplayPso() makes among the
// column variants (C / L / L2 / an L3 or L3b key 2..63), plus OIL_MURK;
// index kMurkNoLamp = C.
int FluidRenderer::MurkPsoIdxFor(const LiquidAcidConfig& a) const {
    if (a.LampPathWanted()) {
        const int key = LampKeyFor(a);
        if (key > 1) return key;
        if (a.Lamp2Wanted()) return 1;
        if (a.LampWanted()) return 0;
    }
    return kMurkNoLamp;
}

// The water twin's macros: EnsureLampPso's list for the column variant (key bit 16 = the plain C list: the
// dark-dye oil under white lamps has no OIL_LAMP), + the water defines, null-terminated
void FluidRenderer::MurkPsoDefs(int wf, int mi, std::vector<D3D_SHADER_MACRO>& defs) const {
    const D3D_SHADER_MACRO* b = kAcidSlotMacrosC;
    if (mi != kMurkNoLamp && !(mi > 1 && (mi & 16))) b = (mi & 1) ? kAcidSlotMacrosCL2 : kAcidSlotMacrosCL;
    defs.clear();
    for (; b->Name && strncmp(b->Name, "LA_", 3) != 0; b++) defs.push_back(*b);
    if (mi > 1 && mi != kMurkNoLamp) {
        if (mi & 2) defs.push_back({ "OIL_L2FILL", "1" });
        if (mi & 4) defs.push_back({ "OIL_FLUIDB", "1" });
        if (mi & 8) defs.push_back({ "OIL_MASSDYE", "1" });
        if (mi & 32) defs.push_back({ "OIL_DYEWHITE", "1" });
    }
    if (wf & 1) defs.push_back({ "OIL_MURK", "1" });
    if (wf & 2) defs.push_back({ "OIL_MOTES", "1" });
    if (wf & 4) defs.push_back({ "OIL_WTINT", "1" });
    if (wf & 8) defs.push_back({ "OIL_GRAIN", "1" });
    if (wf & 16) defs.push_back({ "OIL_MURKK", "1" });   // post-night T3 (director_water)
    for (; b->Name; b++) defs.push_back(*b);
    defs.push_back({ nullptr, nullptr });
}

// post-night T3: queue the blobs of every water twin the colour director can reach from here. The lamp indices:
// the config as it is, and while the director runs its sixteen kinds of state (lamp 1 white / a window + the
// second range; dye A2 off / on; dye B1 off / on; the scheme dip's film_hue2_amt 0 or not), through the same
// MurkPsoIdxFor the frame uses. The flags: today's, with and without the motes, and with director_water the
// OIL_MURKK twin (its motes always on). Nothing here touches a PSO or a pixel.
void FluidRenderer::PrewarmWaterNow() {
    const LiquidAcidConfig& a = m_cfg.acid;
    std::vector<int> idx;
    auto addIdx = [&](int i) { if (std::find(idx.begin(), idx.end(), i) == idx.end()) idx.push_back(i); };
    addIdx(MurkPsoIdxFor(a));
    if (a.colourDirector) {
        const float k = a.filmDyeK[0] > 0.0f ? a.filmDyeK[0] : 1.0f;
        for (int s = 0; s < 16; s++) {
            LiquidAcidConfig c = a;
            if (s & 1) { c.lamp1Lo = -25.0f; c.lamp1Hi = 25.0f; c.lamp1Lo2 = 60.0f; c.lamp1Hi2 = 115.0f; c.lamp1Level = 1.3f; }
            else       { c.lamp1Lo = 0.0f;   c.lamp1Hi = 360.0f; c.lamp1Lo2 = 0.0f; c.lamp1Hi2 = 0.0f; c.lamp1Level = 1.0f; }
            c.dyeA2Amount = (s & 2) ? 0.25f : 0.0f; c.filmDyeK[1] = k;
            c.dyeB1Amount = (s & 4) ? 1.0f : 0.0f;  c.filmDyeK[2] = k;
            if (s & 8) c.filmHue2Amt = 0.0f;
            addIdx(MurkPsoIdxFor(c));
        }
    }
    const int f0 = (MurkWanted() ? 1 : 0) | (WaterTintWanted() ? 4 : 0) | (GrainWanted() ? 8 : 0);
    std::vector<int> flags;
    if (f0) flags.push_back(f0);
    flags.push_back(f0 | 2);
    if (a.colourDirector && a.directorWater && MurkWanted()) flags.push_back(f0 | 2 | 16);
    std::vector<std::vector<std::pair<std::string, std::string>>> sets;
    std::vector<D3D_SHADER_MACRO> defs;
    for (int wf : flags)
        for (int mi : idx) {
            if (m_psoMurk[wf][mi]) continue;                   // built: its blobs are cached
            MurkPsoDefs(wf, mi, defs);
            const std::vector<D3D_SHADER_MACRO> st = SpecTintDefs(kDisplaySrc, defs.data());   // as CompileAsync
            const D3D_SHADER_MACRO* d0 = st.empty() ? defs.data() : st.data();
            const std::vector<D3D_SHADER_MACRO> dl = DropLifeDefs(kDisplaySrc, d0);
            std::vector<std::pair<std::string, std::string>> s;
            for (const D3D_SHADER_MACRO* d = dl.empty() ? d0 : dl.data(); d->Name; d++)
                s.emplace_back(d->Name, d->Definition ? d->Definition : "");
            sets.push_back(std::move(s));
        }
    WpLog("water prewarm: %d water twins queued (%d flag sets x %d lamp indices, built ones skipped)",
          (int)sets.size(), (int)flags.size(), (int)idx.size());
    if (!sets.empty()) ShaderPrewarmAsync(kDisplaySrc, std::move(sets));
}

// Bake (when a baked key changed), the texture, the SRV and the PSO. The
// upload itself rides the next frame's list (RecordMurkUpload). A re-bake
// waits for the GPU first: the upload buffer may still be read.
void FluidRenderer::EnsureMurk() {
    if (!m_device || m_deviceLost) return;
    const LiquidAcidConfig& a = m_cfg.acid;
    const int mi = MurkPsoIdx();
    // the water PSO this frame wants: OIL_MURK (its bake below runs in this same
    // frame, before the draw) and / or OIL_MOTES (Oil3D C3) and / or OIL_WTINT (W2)
    // and / or OIL_GRAIN (W3: the fine silt + depth haze, only with OIL_MURK)
    const int wf = (MurkWanted() ? 1 : 0) | (MotesWanted() ? 2 : 0) | (WaterTintWanted() ? 4 : 0)
                 | (GrainWanted() ? 8 : 0) | (MurkKWanted() ? 16 : 0);
    if (wf && !m_psoMurk[wf][mi]) {
        std::vector<D3D_SHADER_MACRO> defs;
        MurkPsoDefs(wf, mi, defs);
        // live: built on a worker; until then DisplayPso() draws the column variant without the water defines
        if (MakeGraphicsPso(kDisplaySrc, m_psoMurk[wf][mi], defs.data(), true))
        printf("look: liquid_acid water PSO (%s%s%s%s%s, column variant, lamp index %d) compiled on demand\n",
               (wf & 1) ? " OIL_MURK" : "", (wf & 2) ? " OIL_MOTES" : "", (wf & 4) ? " OIL_WTINT" : "",
               (wf & 8) ? " OIL_GRAIN" : "", (wf & 16) ? " OIL_MURKK" : "", mi);
    }
    // post-night T3: the pre-warm of the water twins, again whenever what it depends on changes (a look
    // start, a water key, the director's Engage via RequestWaterPrewarm); live only
    if (!m_headless) {
        const unsigned long long sig = 1ull | ((unsigned long long)(wf & ~2) << 1)
            | ((unsigned long long)(a.colourDirector ? 1 : 0) << 8) | ((unsigned long long)(a.directorWater ? 1 : 0) << 9)
            | ((unsigned long long)(g_dropLife.load() ? 1 : 0) << 10) | ((unsigned long long)(m_lamp2Capped ? 1 : 0) << 11)
            | ((unsigned long long)(unsigned)g_dropLifeX.load() << 14)
            | ((unsigned long long)(unsigned)g_specTint.load() << 16)
            | ((unsigned long long)(unsigned)g_seamStretch.load() << 20)
            | ((unsigned long long)(unsigned)g_seamLeak.load() << 32)
            | ((unsigned long long)(a.Lamp2Wanted() ? 1 : 0) << 12) | ((unsigned long long)(a.DarkDyeWanted() ? 1 : 0) << 13);
        if (sig != m_waterPrewarmSig) {
            m_waterPrewarmSig = sig;
            PrewarmWaterNow();
        }
    }
    if (!MurkWanted()) return;   // motes alone bake nothing
    int nN = kMurkN, nNZ = kMurkNZ;
    MurkDims(nN, nNZ);
    const float key[9] = { a.waterMurk, a.murkClouds, a.murkWaves, a.murkStrands, a.murkClumps, a.murkShape,
                           a.murkWarp, a.murkCloudShare, (float)nN };
    // post-night T1: a worker's bake that has finished lands here, on the main thread. If the keys
    // moved on while it ran, it is dropped and the keys as they are now bake once more (below).
    if (m_murkJob && m_murkJob->done.load(std::memory_order_acquire)) {
        std::shared_ptr<MurkJob> j = std::move(m_murkJob);
        if (memcmp(j->key, key, sizeof(key)) == 0) {
            MurkInstall(j->keys.n, j->keys.nz, j->texels, j->key, j->keys, j->ms, j->dMin, j->dMax);
            return;
        }
        printf("murk: a worker's bake (%llu ms) was overtaken by a key change; baking the new keys\n", j->ms);
    }
    if (m_murkTex && memcmp(key, m_murkKey, sizeof(key)) == 0) return;
    if (m_murkJob) return;       // a bake is running: compared with the keys when it lands (coalesced)
    // A Settings slider drag changes a baked key every frame, and a re-bake costs ~50 ms plus a GPU
    // wait: re-bake once the keys have held still for 150 ms (the old volume shows meanwhile). The
    // first bake is immediate; headless runs never wait (a wall-clock delay would make a shot's
    // frame depend on render speed).
    if (m_murkTex && !m_headless) {
        const ULONGLONG now = GetTickCount64();
        if (memcmp(key, m_murkSeen, sizeof(key)) != 0) {
            memcpy(m_murkSeen, key, sizeof(key));
            m_murkSeenAt = now;
            return;
        }
        if (now - m_murkSeenAt < 150) return;
    }
    const MurkBakeKeys bk = { a.waterMurk, a.murkClouds, a.murkWaves, a.murkStrands, a.murkClumps, a.murkShape,
                              a.murkWarp, a.murkCloudShare, nN, nNZ };
    // post-night T1 (a ~0.4 s main-thread stall at the live 304 x 304 x 76): the live wallpaper bakes on a
    // worker and keeps drawing the volume that is up (none at the first bake: WaterFlags' not-ready path)
    if (!m_headless || m_testMurkAsync) {
        auto j = std::make_shared<MurkJob>();
        j->keys = bk;
        memcpy(j->key, key, sizeof(key));
        m_murkJob = j;
        std::thread([j]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            const ULONGLONG t0 = GetTickCount64();
            BakeMurk(j->keys, j->texels, j->dMin, j->dMax, true);
            j->ms = GetTickCount64() - t0;
            j->done.store(true, std::memory_order_release);
        }).detach();
        return;
    }
    const ULONGLONG t0 = GetTickCount64();
    std::vector<uint16_t> texels;
    float dMin = 0.0f, dMax = 0.0f;
    BakeMurk(bk, texels, dMin, dMax, false);
    MurkInstall(nN, nNZ, texels, key, bk, GetTickCount64() - t0, dMin, dMax);
}

// The texture, its upload buffer and the CPU copy from a finished bake (main thread). A re-bake
// waits for the GPU first: the upload buffer may still be read.
void FluidRenderer::MurkInstall(int nN, int nNZ, std::vector<uint16_t>& texels, const float key[9],
                                const MurkBakeKeys& a, unsigned long long ms, float dMin, float dMax) {
    if (m_murkTex) WaitForGpuIdle();
    if (m_deviceLost) return;
    // W3: murk_scale moved the volume's size: a new texture + upload buffer (the GPU is idle)
    if (m_murkTex && (nN != m_murkN || nNZ != m_murkNZ)) {
        m_murkTex.Reset();
        m_murkUpload.Reset();
        m_murkReady = false;   // until this frame's upload is recorded
    }
    if (!m_murkTex) {
        m_murkN = nN; m_murkNZ = nNZ;
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        rd.Width = m_murkN; rd.Height = m_murkN; rd.DepthOrArraySize = m_murkNZ;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R16_FLOAT;
        rd.SampleDesc.Count = 1;
        m_murkState = D3D12_RESOURCE_STATE_COPY_DEST;
        HR(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, m_murkState, nullptr,
                                             IID_PPV_ARGS(&m_murkTex)));
        if (!m_murkTex) return;
        UINT64 total = 0;
        m_device->GetCopyableFootprints(&rd, 0, 1, 0, &m_murkFp, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES up = {};
        up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HR(m_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                             nullptr, IID_PPV_ARGS(&m_murkUpload)));
        if (!m_murkUpload) { m_murkTex.Reset(); return; }
        WriteMurkSrv();
    }
    uint8_t* dst = nullptr;
    D3D12_RANGE none = { 0, 0 };
    if (FAILED(m_murkUpload->Map(0, &none, (void**)&dst)) || !dst) return;
    const UINT rowPitch = m_murkFp.Footprint.RowPitch;
    for (int z = 0; z < m_murkNZ; z++)
        for (int y = 0; y < m_murkN; y++)
            memcpy(dst + m_murkFp.Offset + ((size_t)z * m_murkN + y) * rowPitch,
                   &texels[((size_t)z * m_murkN + y) * m_murkN], (size_t)m_murkN * sizeof(uint16_t));
    m_murkUpload->Unmap(0, nullptr);
    memcpy(m_murkKey, key, sizeof(m_murkKey));
    m_murkUploadPending = true;
    m_murkTexels.swap(texels);   // kept for MurkDensityCpu (the motes' spawn)
    printf("murk: baked %dx%dx%d in %llu ms%s (water_murk %.2f, clouds %.2f waves %.2f strands %.2f clumps %.2f, "
           "shape %.2f warp %.2f, cloud share %.2f): density %.3f..%.3f, mean 1\n", m_murkN, m_murkN, m_murkNZ,
           ms, (!m_headless || m_testMurkAsync) ? " on a worker" : "", a.waterMurk, a.clouds, a.waves, a.strands,
           a.clumps, a.shape, a.warp, a.cloudShare, dMin, dMax);
}

void FluidRenderer::RecordMurkUpload() {
    m_murkUploadPending = false;
    if (!m_murkTex || !m_murkUpload) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = m_murkTex.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (m_murkState != D3D12_RESOURCE_STATE_COPY_DEST) {
        b.Transition.StateBefore = m_murkState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        m_cmd->ResourceBarrier(1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = m_murkTex.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    src.pResource = m_murkUpload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = m_murkFp;
    m_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_cmd->ResourceBarrier(1, &b);
    m_murkState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_murkReady = true;
}

// The murk density at screen uv (x, y) and depth t, read from the CPU copy of
// the bake with the mapping the shader used last frame (m_murkMap = laMurk[0]
// xyzw, laMurk[1] xy), nearest texel, wrapped. 1 (uniform) while murk is off.
static float MurkHalfToFloat(uint16_t h) {
    const uint32_t e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    if (e == 0) return 0.0f;                       // the bake writes no subnormals
    const uint32_t x = ((e - 15 + 127) << 23) | (m << 13);
    float f; memcpy(&f, &x, sizeof(f));
    return f;
}
float FluidRenderer::MurkDensityCpu(float x, float y, float t) const {
    const int N = m_murkN, NZ = m_murkNZ;
    if (!MurkWanted() || m_murkTexels.size() != (size_t)N * N * NZ) return 1.0f;
    auto wrap = [](float v) { return v - floorf(v); };
    const float qx = wrap(x * m_murkMap[0] + m_murkMap[2]);
    const float qy = wrap(y * m_murkMap[1] + m_murkMap[3]);
    const float qz = wrap(t * m_murkMap[4] + m_murkMap[5]);
    const int ix = (int)(qx * N) % N, iy = (int)(qy * N) % N, iz = (int)(qz * NZ) % NZ;
    const float d = MurkHalfToFloat(m_murkTexels[((size_t)iz * N + iy) * N + ix]);
    return m_dirWaterOn ? 1.0f + m_dirMurkK * (d - 1.0f) : d;   // director_water: as OIL_MURKK reads it
}

// ---- Oil3D C3: MOTES -----------------------------------------------------
// The resolved particles of the column. Each lives at screen uv (x, y) and
// depth t in [0, 1] (z = t tank_depth), rides the murk's drift (the same
// rise and sideways wander, so the specks travel with the silt they sit in)
// plus a slow Brownian jitter, leaves over the top edge and comes back in
// below the bottom one (never popping in or out on screen), and is born
// where the murk is dense: a candidate spot is kept with probability
// min(1, d / 2.5), so clear pockets stay nearly empty and silt carries the
// specks. Without murk they spread evenly. A share mote_glint flashes now and
// then (a brief sharp sin^24 peak, its own rate). Deterministic: one xorshift
// stream, stepped with the frame's dt (shots reproduce).
float FluidRenderer::MoteRand() {
    uint32_t s = m_moteRng;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    m_moteRng = s;
    return (float)(s >> 8) * (1.0f / 16777216.0f);
}

void FluidRenderer::SpawnMote(Mote& m, bool inView) {
    const LiquidAcidConfig& a = m_cfg.acid;
    const float bias = fminf(fmaxf(a.moteDepthBias, -1.0f), 1.0f);
    const float ex = exp2f(-2.0f * bias);           // t = u^ex: +1 -> u^0.25 (deep), -1 -> u^4 (near)
    for (int tries = 0; tries < 24; tries++) {
        m.x = -0.02f + 1.04f * MoteRand();
        m.y = inView ? MoteRand() : 1.0f + 0.03f * MoteRand();
        m.t = powf(MoteRand(), ex);
        if (MoteRand() * 2.5f <= MurkDensityCpu(m.x, m.y, m.t)) break;
    }
    m.r = 0.6f + 0.8f * MoteRand();                 // x mote_px at upload
    m.vx = m.vy = 0.0f;
    m.phase = 6.2831853f * MoteRand();
    m.rate = 0.35f + 0.9f * MoteRand();
    m.glint = MoteRand() < fminf(fmaxf(a.moteGlint, 0.0f), 1.0f);
    m.fade = inView ? 0.0f : 1.0f;                  // born on screen: fades in over 1.5 s
}

void FluidRenderer::StepMotes(float dt) {
    const LiquidAcidConfig& a = m_cfg.acid;
    // director_water (post-night T3): the director's fractional count; mote i shows clamp(count - i, 0, 1)
    // (the upload), so the ones past ceil(count) are already invisible when they go: no pop either way
    const int want = m_dirWaterOn ? (MotesWanted() ? std::min((int)ceilf(m_dirMotes), kMoteMax) : 0)
                                  : (MotesWanted() ? std::min(a.motes, kMoteMax) : 0);
    if (want == 0) { m_motes.clear(); return; }
    while ((int)m_motes.size() < want) { Mote m; SpawnMote(m, true); m_motes.push_back(m); }
    if ((int)m_motes.size() > want) m_motes.resize(want);
    if (dt <= 0.0f) return;
    // the murk's drift (UploadAcidConstants' laMurk), in screen uv per second: the
    // field moves -0.0021 drift tiles in x and -0.0090 drift frame heights in y
    const float aspect = (float)m_width / fmaxf((float)m_height, 1.0f);
    const float S = fminf(fmaxf(a.murkScale, 0.05f), 8.0f);
    const float drift = fminf(fmaxf(a.murkDrift, 0.0f), 10.0f);
    const float dxdt = -0.0021f * drift * S / aspect, dydt = -0.0090f * drift;
    const float jit = 2.5f / 1440.0f;               // Brownian: ~2.5 px/s at 1440p
    const float damp = expf(-dt / 1.5f);
    const float sq = sqrtf(dt);
    for (Mote& m : m_motes) {
        m.vx = m.vx * damp + (MoteRand() - 0.5f) * 3.4f * jit * sq / aspect;
        m.vy = m.vy * damp + (MoteRand() - 0.5f) * 3.4f * jit * sq;
        m.x += (dxdt + m.vx) * dt;
        m.y += (dydt + m.vy) * dt;
        if (m.x < -0.02f) m.x += 1.04f; else if (m.x > 1.02f) m.x -= 1.04f;
        m.fade = fminf(1.0f, m.fade + dt / 1.5f);
        if (m.y < -0.03f) SpawnMote(m, false);      // over the top: back in below the bottom
    }
}

// (Re)creates the coverage target when the pad the upload asked for or the
// frame size changed. It waits for the GPU first: the old target and the
// table's descriptor 29 (rewritten below) may still be in flight. The pad is
// bucketed to 16 texels with hysteresis, so this happens on a key change only
// (one short stall), never on the azimuth drift.
bool FluidRenderer::EnsureMassCovTarget() {
    const int P = m_massCovWantP;
    if (P <= 0) return false;
    if (m_massCovTex && m_massCovP == P && m_massCovFrameW == m_width && m_massCovFrameH == m_height)
        return true;
    WaitForGpuIdle();
    m_massCovTex.Reset();
    const int bw = (m_width + 3) / 4, bh = (m_height + 3) / 4;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT64)(bw + 2 * P);
    rd.Height = (UINT)(bh + 2 * P);
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R16_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 nullptr, IID_PPV_ARGS(&m_massCovTex)))) {
        m_massCovTex.Reset();
        WriteVelTable();               // back to a null SRV: the shader reads 0 = all lit
        return false;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)(kFrames * 2 + 4) * m_rtvStride;
    m_device->CreateRenderTargetView(m_massCovTex.Get(), nullptr, rtv);
    m_massCovState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_massCovW = bw + 2 * P; m_massCovH = bh + 2 * P; m_massCovP = P;
    m_massCovFrameW = m_width; m_massCovFrameH = m_height;
    WriteVelTable();
    printf("Oil3D C2: mass coverage %dx%d R16_FLOAT (quarter res, pad %d texels = %d px)\n",
           m_massCovW, m_massCovH, P, 4 * P);
    return true;
}

// Inside the surface pass's timing marks; records nothing unless water_scatter
// > 0 with tank_depth > 0 (and the surface pre-pass itself ran).
void FluidRenderer::RenderMassCovPass() {
    if (!MassCovWanted() || !m_psoMassCov || !m_surfRS) return;
    if (!EnsureMassCovTarget()) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = m_massCovTex.Get();
    b.Transition.StateBefore = m_massCovState;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (m_massCovState != D3D12_RESOURCE_STATE_RENDER_TARGET) m_cmd->ResourceBarrier(1, &b);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)(kFrames * 2 + 4) * m_rtvStride;
    m_cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0, 0, (float)m_massCovW, (float)m_massCovH, 0, 1 };
    D3D12_RECT sc = { m_massCovSc[0] > 0 ? m_massCovSc[0] : 0,
                      m_massCovSc[1] > 0 ? m_massCovSc[1] : 0,
                      m_massCovSc[2] < m_massCovW ? m_massCovSc[2] : m_massCovW,
                      m_massCovSc[3] < m_massCovH ? m_massCovSc[3] : m_massCovH };
    m_cmd->RSSetViewports(1, &vp);
    m_cmd->RSSetScissorRects(1, &sc);
    // same bindings as the surface pre-pass just recorded (root sig m_surfRS:
    // b0, b1 acid block, t1 blobs, t2/t4 droplets, b3 mirror) are still set
    m_cmd->SetPipelineState(m_psoMassCov.Get());
    m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmd->DrawInstanced(3, 1, 0, 0);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_cmd->ResourceBarrier(1, &b);
    m_massCovState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

// --timings (shot mode only). Slot = frame index; kTsMarks marks per slot.
void FluidRenderer::TimestampCollect(UINT slot) {
    if (!m_tsReadback || !m_tsPending[slot]) return;
    m_tsPending[slot] = false;
    UINT64* t = nullptr;
    D3D12_RANGE r = { slot * kTsMarks * sizeof(UINT64), (slot * kTsMarks + kTsMarks) * sizeof(UINT64) };
    if (FAILED(m_tsReadback->Map(0, &r, (void**)&t))) return;
    // marks: 0 frame start, 1/2 surface pre-pass, 3/4 display draw, 5 post end
    // (+ the P1 detail marks 6-12, see fluid.h)
    const UINT64* q = t + slot * kTsMarks;
    bool ok = q[0] != 0;
    for (int m = 1; m < 6; m++) ok = ok && q[m] >= q[m - 1];
    if (ok && m_tsFreq) {
        const double k = 1000.0 / (double)m_tsFreq;
        m_tsSum[0] += (double)(q[1] - q[0]) * k;   // sim
        m_tsSum[1] += (double)(q[4] - q[3]) * k;   // display
        m_tsSum[2] += (double)(q[5] - q[4]) * k;   // post
        m_tsSum[3] += (double)(q[5] - q[0]) * k;   // frame
        m_tsSum[4] += (double)(q[2] - q[1]) * k;   // surface pre-pass
        m_tsMaxDisp = fmax(m_tsMaxDisp, (double)(q[4] - q[3]) * k);
        m_tsCount++;
        // P1 detail: the timeline 0 6 7 8 9 1 10 11 12 2 3 4 5 in GPU order
        static const int kOrder[13] = { 0, 6, 7, 8, 9, 1, 10, 11, 12, 2, 3, 4, 5 };
        bool dok = true;
        for (int m = 1; m < 13; m++) dok = dok && q[kOrder[m]] >= q[kOrder[m - 1]];
        if (dok) {
            float d[kTsDet];
            for (int m = 0; m < 12; m++) d[m] = (float)((double)(q[kOrder[m + 1]] - q[kOrder[m]]) * k);
            d[12] = (float)((double)(q[5] - q[0]) * k);
            // gap since the previous frame's post end / period since its start
            // (slots are collected in frame order; a capture break reads < 0 or
            // huge and is dropped)
            d[13] = -1.0f; d[14] = -1.0f;
            if (m_tsPrevEnd && q[0] >= m_tsPrevEnd) {
                const double g = (double)(q[0] - m_tsPrevEnd) * k;
                const double p = (double)(q[0] - m_tsPrevStart) * k;
                if (g < 100.0) { d[13] = (float)g; d[14] = (float)p; }
            }
            m_tsDet.insert(m_tsDet.end(), d, d + kTsDet);
        }
        m_tsPrevStart = q[0]; m_tsPrevEnd = q[5];
    }
    D3D12_RANGE none = { 0, 0 };
    m_tsReadback->Unmap(0, &none);
}

void FluidRenderer::TimestampBegin() {
    if (!m_timingsOn || !m_headless) return;
    if (!m_tsHeap) {
        D3D12_QUERY_HEAP_DESC qh = {};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = kFrames * kTsMarks;
        if (FAILED(m_device->CreateQueryHeap(&qh, IID_PPV_ARGS(&m_tsHeap)))) { m_timingsOn = false; return; }
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = kFrames * kTsMarks * sizeof(UINT64);
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_tsReadback)))) {
            m_tsHeap.Reset(); m_timingsOn = false; return;
        }
        m_queue->GetTimestampFrequency(&m_tsFreq);
    }
    TimestampCollect(m_frameIndex);   // BeginFrame waited on this slot's fence
    m_tsOpen = true;
    TimestampMark(0);
}

void FluidRenderer::TimestampMark(UINT k) {
    if (!m_timingsOn || !m_tsHeap || !m_tsOpen) return;
    m_cmd->EndQuery(m_tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, m_frameIndex * kTsMarks + k);
}

void FluidRenderer::TimestampResolve() {
    if (!m_timingsOn || !m_tsHeap || !m_tsOpen) return;
    m_tsOpen = false;
    m_cmd->ResolveQueryData(m_tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, m_frameIndex * kTsMarks,
                            kTsMarks, m_tsReadback.Get(), m_frameIndex * kTsMarks * sizeof(UINT64));
    m_tsPending[m_frameIndex] = true;
}

static size_t AcidDebugSlotOffset();   // defined after AcidParamsGPU

bool FluidRenderer::CaptureDebugFrame(int mode, std::vector<float>& out) {
    if (!m_headless || !m_cfg.acid.enabled || m_acidLastFi < 0 || !m_acidParamData[m_acidLastFi])
        return false;
    WaitForGpuIdle();
    if (m_deviceLost) return false;
    const int  keepMode = m_oilHeightDebug;
    const bool keepTs   = m_timingsOn;
    m_oilHeightDebug = mode;
    m_timingsOn = false;
    EnsureOilHeightPso();
    bool ok = m_psoLiquidAcidH != nullptr;
    float* dbg = (float*)((uint8_t*)m_acidParamData[m_acidLastFi]
                          + AcidDebugSlotOffset());   // LA_OIL_H_DEBUG
    const float keepV = *dbg;
    if (ok) {
        *dbg = (float)mode;
        for (UINT i = 0; i < kFrames; i++) m_fenceValues[i] = 0;   // GPU idle
        m_frameIndex = (UINT)m_acidLastFi;
        ok = BeginFrame();
        if (ok) {
            RenderDisplayOffscreen();
            EndFrameAndPresent();
            ok = CaptureOffscreen(out);   // resets the ring to index 0, as after a shot
        }
        *dbg = keepV;
    }
    m_oilHeightDebug = keepMode;
    m_timingsOn = keepTs;
    return ok;
}

// ---- Oil3D L3b: --shot-stage-tap (shot-only debug) ---------------------------
// The defines of the display PSO DisplayPso() runs this frame (the lamp path's
// key, or the plain acid variant), with `extra` inserted before the LA_ slot
// macros; null-terminated.
void FluidRenderer::DisplayDefsNow(std::vector<D3D_SHADER_MACRO>& defs, const D3D_SHADER_MACRO& extra) const {
    const int v = AcidVariant();
    static const D3D_SHADER_MACRO* const kPlain[3] = { kAcidSlotMacros, kAcidSlotMacrosH, kAcidSlotMacrosC };
    static const D3D_SHADER_MACRO* const kBase[2][3] = {
        { kAcidSlotMacrosL,  kAcidSlotMacrosHL,  kAcidSlotMacrosCL },
        { kAcidSlotMacrosL2, kAcidSlotMacrosHL2, kAcidSlotMacrosCL2 } };
    const bool lamp = m_cfg.acid.LampPathWanted();
    const int  key  = lamp ? LampKey() : 0;
    const D3D_SHADER_MACRO* b = (lamp && !(key & 16)) ? kBase[(key & 1) ? 1 : 0][v] : kPlain[v];
    for (; b->Name && strncmp(b->Name, "LA_", 3) != 0; b++) defs.push_back(*b);
    if (lamp && (key & 2)) defs.push_back({ "OIL_L2FILL", "1" });
    if (lamp && (key & 4)) defs.push_back({ "OIL_FLUIDB", "1" });
    if (lamp && (key & 8)) defs.push_back({ "OIL_MASSDYE", "1" });
    if (lamp && (key & 32)) defs.push_back({ "OIL_DYEWHITE", "1" });
    if (v == 2 && MurkWanted() && m_murkReady) defs.push_back({ "OIL_MURK", "1" });   // W1: as DisplayPso
    if (v == 2 && MotesWanted()) defs.push_back({ "OIL_MOTES", "1" });                 // C3: as DisplayPso
    if (v == 2 && WaterTintWanted()) defs.push_back({ "OIL_WTINT", "1" });             // W2: as DisplayPso
    if (v == 2 && GrainWanted() && m_murkReady) defs.push_back({ "OIL_GRAIN", "1" });  // W3: as DisplayPso
    if (v == 2 && MurkKWanted() && m_murkReady) defs.push_back({ "OIL_MURKK", "1" });  // post-night T3: as DisplayPso
    defs.push_back(extra);
    for (; b->Name; b++) defs.push_back(*b);
    defs.push_back({ nullptr, nullptr });
}

ID3D12PipelineState* FluidRenderer::StageTapPso(bool post, long long key, const D3D_SHADER_MACRO* defs) {
    for (auto& e : m_psoTap) if (e.first == key) return e.second.Get();
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    MakeGraphicsPso(post ? kPostSrc : kDisplaySrc, pso, defs);
    if (!pso) return nullptr;
    m_psoTap.push_back({ key, pso });
    return pso.Get();
}

bool FluidRenderer::CaptureStageTap(int tap, std::vector<float>& out) {
    if (!m_headless || !m_cfg.acid.enabled || m_acidLastFi < 0 || !m_acidParamData[m_acidLastFi])
        return false;
    WaitForGpuIdle();
    if (m_deviceLost) return false;
    const int v = AcidVariant();
    if (v < 0) return false;
    if (tap > 0 && tap < 200) {
        char tv[16];
        snprintf(tv, sizeof tv, "%d", tap);
        std::vector<D3D_SHADER_MACRO> defs;
        DisplayDefsNow(defs, { "OIL_STAGETAP", tv });
        const int key = m_cfg.acid.LampPathWanted() ? LampKey() : -1;
        m_stageTapPso = StageTapPso(false, (long long)tap * 10000 + v * 100 + key + 1, defs.data());
        if (!m_stageTapPso) return false;
    } else if (tap <= 200 || tap > 299) {
        return false;
    }
    const bool keepTs = m_timingsOn;
    m_timingsOn = false;
    m_stageTap = tap;
    for (UINT i = 0; i < kFrames; i++) m_fenceValues[i] = 0;   // GPU idle
    m_frameIndex = (UINT)m_acidLastFi;
    bool ok = BeginFrame();
    if (ok) {
        RenderDisplayOffscreen();
        EndFrameAndPresent();
        ok = CaptureOffscreen(out);   // resets the ring to index 0, as after a shot
    }
    m_stageTap = 0;
    m_stageTapPso = nullptr;
    m_timingsOn = keepTs;
    return ok;
}

void FluidRenderer::DumpDroplets(std::vector<float>& xyr) const {
    xyr.clear();
    const float aspect = (float)m_width / fmaxf((float)m_height, 1.0f);
    for (const AcidDrop& d : m_acidDrops) {
        if (d.kind != 0 || d.gate < 0.01f || d.r <= 0.0f) continue;   // black (water) droplets only
        xyr.push_back(d.x);
        xyr.push_back(d.y);
        xyr.push_back(d.r / aspect);   // uv-y radius -> fraction of the frame width
    }
}

bool FluidRenderer::TakeTimings(double& simMs, double& dispMs, double& postMs,
                                double& totalMs, double& maxDispMs, int& frames) {
    frames = m_tsCount;
    if (m_tsCount <= 0) return false;
    simMs = m_tsSum[0] / m_tsCount; dispMs = m_tsSum[1] / m_tsCount;
    postMs = m_tsSum[2] / m_tsCount; totalMs = m_tsSum[3] / m_tsCount;
    m_tsLastSurf = m_tsSum[4] / m_tsCount;
    maxDispMs = m_tsMaxDisp;
    {   // P1 detail: mean / p10 / median / min per interval over the window
        static const char* kName[kTsDet] = { "pre", "simsteps", "mid", "oildrag", "tail",
                                             "surf_setup", "surf_draw", "surf_barrier", "masscov",
                                             "disp_setup", "display", "post", "frame", "gap", "period" };
        const size_t n = m_tsDet.size() / kTsDet;
        std::string s;
        char line[160];
        _snprintf_s(line, _TRUNCATE, "[timings-detail] %zu frames: interval  mean  p10  p50  min  max (GPU ms)\n", n);
        s += line;
        std::vector<float> v;
        for (int c = 0; c < kTsDet && n > 0; c++) {
            v.clear();
            for (size_t f = 0; f < n; f++) {
                const float x = m_tsDet[f * kTsDet + c];
                if (x >= 0.0f) v.push_back(x);
            }
            if (v.empty()) continue;
            std::sort(v.begin(), v.end());
            double sum = 0.0;
            for (float x : v) sum += x;
            _snprintf_s(line, _TRUNCATE, "[timings-detail]   %-13s %7.3f %7.3f %7.3f %7.3f %7.3f\n",
                        kName[c], sum / v.size(), v[v.size() / 10], v[v.size() / 2], v.front(), v.back());
            s += line;
        }
        m_tsDetailText = s;
        m_tsDet.clear();
    }
    m_tsSum[0] = m_tsSum[1] = m_tsSum[2] = m_tsSum[3] = m_tsSum[4] = 0.0;
    m_tsMaxDisp = 0.0;
    m_tsCount = 0;
    return true;
}

// Headless render target: one FP16 texture the size of the requested shot,
// plus a readback buffer. RTV slot 0 (the first back-buffer slot, unused when
// there is no swap chain) â€” the analyzer keeps slot kFrames, the mirror
// kFrames+1.., so nothing collides.
void FluidRenderer::CreateOffscreenTarget() {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT64)m_width;
    rd.Height = (UINT)m_height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    m_shotState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    HR(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, m_shotState,
                                         nullptr, IID_PPV_ARGS(&m_shotTex)));

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateRenderTargetView(m_shotTex.Get(), nullptr, rtv);

    m_shotPitch = ((UINT)m_width * 8 + 255) & ~255u;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (UINT64)m_shotPitch * m_height;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES rb = {};
    rb.Type = D3D12_HEAP_TYPE_READBACK;
    HR(m_device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd,
                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                         IID_PPV_ARGS(&m_shotReadback)));
}

FluidRenderer::Tex FluidRenderer::CreateTex(int w, int h, DXGI_FORMAT fmt, int heapSlot) {
    Tex t;
    t.w = w; t.h = h;

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    t.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    HR(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, t.state,
                                         nullptr, IID_PPV_ARGS(&t.res)));

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    UINT srvSlot = heapSlot * 2, uavSlot = heapSlot * 2 + 1;

    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE sc = cpu; sc.ptr += (SIZE_T)srvSlot * m_srvStride;
    m_device->CreateShaderResourceView(t.res.Get(), &sv, sc);
    t.srv = gpu; t.srv.ptr += (SIZE_T)srvSlot * m_srvStride;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.Format = fmt;
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE uc = cpu; uc.ptr += (SIZE_T)uavSlot * m_srvStride;
    m_device->CreateUnorderedAccessView(t.res.Get(), nullptr, &uv, uc);
    t.uav = gpu; t.uav.ptr += (SIZE_T)uavSlot * m_srvStride;
    return t;
}

void FluidRenderer::CreateSimResources() {
    GetResolution(m_cfg.simRes, m_width, m_height, &m_simW, &m_simH);
    GetResolution(m_cfg.dyeRes, m_width, m_height, &m_dyeW, &m_dyeH);

    int slot = 0;
    m_velocity.a = CreateTex(m_simW, m_simH, DXGI_FORMAT_R16G16_FLOAT, slot++);
    m_velocity.b = CreateTex(m_simW, m_simH, DXGI_FORMAT_R16G16_FLOAT, slot++);
    m_velocity.read = &m_velocity.a; m_velocity.write = &m_velocity.b;
    m_dye.a = CreateTex(m_dyeW, m_dyeH, DXGI_FORMAT_R16G16B16A16_FLOAT, slot++);
    m_dye.b = CreateTex(m_dyeW, m_dyeH, DXGI_FORMAT_R16G16B16A16_FLOAT, slot++);
    m_dye.read = &m_dye.a; m_dye.write = &m_dye.b;
    m_pressure.a = CreateTex(m_simW, m_simH, DXGI_FORMAT_R16_FLOAT, slot++);
    m_pressure.b = CreateTex(m_simW, m_simH, DXGI_FORMAT_R16_FLOAT, slot++);
    m_pressure.read = &m_pressure.a; m_pressure.write = &m_pressure.b;
    m_divergence = CreateTex(m_simW, m_simH, DXGI_FORMAT_R16_FLOAT, slot++);
    m_curl = CreateTex(m_simW, m_simH, DXGI_FORMAT_R16_FLOAT, slot++);
    m_coverage = CreateTex(kCovW, kCovH, DXGI_FORMAT_R16G16B16A16_FLOAT, slot++);
    // Liquid Acid: 64x36 velocity downsample for CPU blob advection. Same
    // async-readback pattern as the coverage governor; one frame of latency.
    m_velLow = CreateTex(kVelW, kVelH, DXGI_FORMAT_R16G16B16A16_FLOAT, slot++);
    m_surfW = m_surfH = 0;            // the surface targets follow the frame size
    WriteVelTable();                  // heap 26..28: velLow + the Oil3D surface (null until built)
    // Liquid Acid oil_drag / oil_dye_block: sim-res oil coverage mask. 72 KB
    // and one heap slot; nothing ever writes or reads it unless one of those
    // two keys is non-zero (see StepOilDrag), so the fluid look is untouched.
    m_oilMask = CreateTex(m_simW, m_simH, DXGI_FORMAT_R16_FLOAT, slot++);
    m_velPitch = (kVelW * 8 + 255) & ~255u;
    m_velCpu.assign((size_t)kVelW * kVelH * 2, 0.0f);
    m_velPending = false;
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (UINT64)m_velPitch * kVelH;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HR(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&m_velReadback)));
    }

    // coverage governor readback (48x27 RGBA16F, 256-aligned pitch)
    m_covPitch = (kCovW * 8 + 255) & ~255u;
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (UINT64)m_covPitch * kCovH;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HR(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&m_covReadback)));
    }
    InitWanderers();

    // stats readback buffer (dye-sized RGBA16F rows, 256-aligned pitch)
    if (m_cfg.stats) {
        m_readbackPitch = (m_dyeW * 8 + 255) & ~255u;
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (UINT64)m_readbackPitch * m_dyeH;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HR(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&m_readback)));
    }
}

void FluidRenderer::Transition(Tex& t, D3D12_RESOURCE_STATES to) {
    if (t.state == to) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t.res.Get();
    b.Transition.StateBefore = t.state;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmd->ResourceBarrier(1, &b);
    t.state = to;
}

void FluidRenderer::UavBarrier(ID3D12Resource* res) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = res;
    m_cmd->ResourceBarrier(1, &b);
}

// false = the device is lost (review 2026-10-04 R3): the caller records nothing and
// returns; the shell sees DeviceLost() and rebuilds the renderer.
bool FluidRenderer::BeginFrame() {
    if (m_deviceLost) return false;
    // Not every call on a removed device fails: after RemoveDevice() the allocator /
    // list Reset, Close and Signal all came back S_OK and only Map() refused, so the
    // removed reason is asked once per frame. Only by a renderer that survives the loss
    // (live, --test-device-lost): a plain --shot never calls it (review 2026-10-04 R3).
    if (m_surviveDeviceLoss) {
        const HRESULT why = m_device->GetDeviceRemovedReason();
        if (FAILED(why)) {
            NoteDeviceLost("GetDeviceRemovedReason", why);
            return false;
        }
    }
    const UINT i = m_frameIndex;
    if (m_fence->GetCompletedValue() < m_fenceValues[i]) {
        HR(m_fence->SetEventOnCompletion(m_fenceValues[i], m_fenceEvent));
        if (!m_deviceLost) WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    HR(m_allocators[i]->Reset());
    HR(m_cmd->Reset(m_allocators[i].Get(), nullptr));
    if (m_deviceLost) return false;
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    m_cmd->SetDescriptorHeaps(1, heaps);
    return true;
}

void FluidRenderer::EndFrameAndPresent() {
    if (m_deviceLost) return;   // lost while recording: nothing to submit (R3)
    HR(m_cmd->Close());
    if (m_deviceLost) return;
    ID3D12CommandList* lists[] = { m_cmd.Get() };
    m_queue->ExecuteCommandLists(1, lists);
    if (m_headless) {
        // no swap chain: just fence the frame and rotate the allocator ring
        m_fenceValues[m_frameIndex] = m_nextFence;
        HR(m_queue->Signal(m_fence.Get(), m_nextFence++));
        m_frameIndex = (m_frameIndex + 1) % kFrames;
        return;
    }
    // Present fails (not fatally) when Explorer tears our window down
    // mid-frame â€” flag it so the shell can rebuild instead of dying.
    HRESULT phr = m_swapChain->Present(1, 0);
    // A removed / reset device is not a lost window: no reattach onto the dead device
    // and no Signal on its queue -- the shell rebuilds the device instead (R3).
    if (FAILED(phr) && m_surviveDeviceLoss && IsDeviceLossHr(phr)) {
        NoteDeviceLost("m_swapChain->Present", phr);
        return;
    }
    if (FAILED(phr)) {
        if (!m_presentBroken)
            printf("Present failed (0x%08lX) - wallpaper window lost, awaiting reattach\n",
                   (unsigned long)phr);
        m_presentBroken = true;
    }
    if (m_mirrorChain) {
        // interval 0: the main chain already provides vsync pacing
        HRESULT mhr = m_mirrorChain->Present(0, 0);
        if (FAILED(mhr) && m_surviveDeviceLoss && IsDeviceLossHr(mhr)) {
            NoteDeviceLost("m_mirrorChain->Present", mhr);
            return;
        }
        if (FAILED(mhr)) m_mirrorBroken = true;
    }
    m_fenceValues[m_frameIndex] = m_nextFence;
    HR(m_queue->Signal(m_fence.Get(), m_nextFence++));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

// SimOnlyStep on a swap chain: execute and fence exactly like a frame, but no
// Present -- so the back-buffer index does not advance and m_frameIndex stays
// put (the next BeginFrame waits on this very fence before reusing the
// allocator, which serialises the warm-up steps; that is intended). The fence
// value comes from the same m_nextFence counter, so the coverage/velocity
// readbacks that stamped m_nextFence this frame see it signalled as usual.
void FluidRenderer::EndFrameNoPresent() {
    if (m_deviceLost) return;   // R3, as in EndFrameAndPresent
    HR(m_cmd->Close());
    if (m_deviceLost) return;
    ID3D12CommandList* lists[] = { m_cmd.Get() };
    m_queue->ExecuteCommandLists(1, lists);
    m_fenceValues[m_frameIndex] = m_nextFence;
    HR(m_queue->Signal(m_fence.Get(), m_nextFence++));
}

// Dispatch one compute pass. srv0/srv1 read, one of the u-slots written
// (chosen by target format type inside the shader entry point).
static UINT Groups(int n) { return (UINT)((n + 7) / 8); }

void FluidRenderer::SimStep(float dt) {
    SimCB cb = {};
    cb.texelW = 1.0f / m_simW;
    cb.texelH = 1.0f / m_simH;
    cb.dt = dt;
    cb.curlStrength = m_cfg.curl;
    cb.baroclinic = m_cfg.baroclinic;
    cb.gravity = m_cfg.gravity;
    cb.gravityPow = m_cfg.gravityPow;
    cb.gravityBlur = m_cfg.gravityBlur;
    cb.aspect = (float)m_width / (float)m_height;

    // Frame-rate independence: the reference applied its multiplicative decays
    // once per step at ~120 steps/s (60 fps, two substeps). Raise each factor
    // to dt/(1/120) so a 144 fps loop doesn't damp harder than a 60 fps one.
    const float stepsRef = dt * 120.0f;

    auto bind = [&](ID3D12PipelineState* pso, Tex* s0, Tex* s1, Tex* dst, UINT dstParam,
                    Tex* s2 = nullptr) {
        if (s0) Transition(*s0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (s1) Transition(*s1, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (s2) Transition(*s2, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(*dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_cmd->SetPipelineState(pso);
        cb.dimsW = dst->w; cb.dimsH = dst->h;
        m_cmd->SetComputeRoot32BitConstants(0, sizeof(SimCB) / 4, &cb, 0);
        m_cmd->SetComputeRootDescriptorTable(1, s0 ? s0->srv : dst->srv);
        m_cmd->SetComputeRootDescriptorTable(2, s1 ? s1->srv : dst->srv);
        // set all three UAV params; only the one the entry point uses matters
        m_cmd->SetComputeRootDescriptorTable(3, dst->uav);
        m_cmd->SetComputeRootDescriptorTable(4, dst->uav);
        m_cmd->SetComputeRootDescriptorTable(5, dst->uav);
        m_cmd->SetComputeRootDescriptorTable(6, s2 ? s2->srv : dst->srv);
        m_cmd->Dispatch(Groups(dst->w), Groups(dst->h), 1);
    };

    m_cmd->SetComputeRootSignature(m_computeRS.Get());

    // 1. curl
    bind(m_psoCurl.Get(), m_velocity.read, nullptr, &m_curl, 5);
    // 2. vorticity confinement (+ baroclinic dye-front torque)
    bind(m_psoVorticity.Get(), m_velocity.read, &m_curl, m_velocity.write, 3, m_dye.read);
    m_velocity.Swap();
    // 3. divergence
    bind(m_psoDivergence.Get(), m_velocity.read, nullptr, &m_divergence, 5);
    // 4. pressure decay
    cb.value = powf(m_cfg.pressureDissipation, stepsRef);
    bind(m_psoClearPressure.Get(), m_pressure.read, nullptr, m_pressure.write, 5);
    m_pressure.Swap();
    // 5. Jacobi
    for (int i = 0; i < m_cfg.pressureIterations; i++) {
        bind(m_psoPressure.Get(), m_pressure.read, &m_divergence, m_pressure.write, 5);
        m_pressure.Swap();
    }
    // 6. gradient subtract
    bind(m_psoGradSub.Get(), m_pressure.read, m_velocity.read, m_velocity.write, 3);
    m_velocity.Swap();
    // 7. advect velocity
    cb.dissipation = powf(m_cfg.velocityDissipation, stepsRef);
    bind(m_psoAdvectVel.Get(), m_velocity.read, nullptr, m_velocity.write, 3);
    m_velocity.Swap();
    // 8. advect dye (velocity displacement still uses sim texels, like reference)
    cb.dissipation = powf(m_cfg.densityDissipation, stepsRef);
    cb.dissipationFast = powf(m_cfg.decayFast, stepsRef);
    cb.decayThreshold = m_cfg.decayThreshold;
    cb.satRestore = 1.0f - powf(1.0f - fminf(m_cfg.satRestore, 0.9999f), dt);
    // [drops] ink_rise: the dye drifts up with the oil picture (x rise_speed,
    // liquid_acid only). 0 = the field stays 0 and the shader's branch is skipped.
    if (m_cfg.drops.inkRise > 0.0f && m_cfg.acid.enabled)
        cb.dyeRise = m_cfg.drops.inkRise * m_cfg.acid.riseSpeed;
    // [drops] ink_swirl (round 3): the dye's own slow noise current. SimCB is full (24 DWORDs), so for THIS
    // pass only two fields CSAdvectDye never reads carry it: gravityPow = -amplitude (negative = the flag,
    // its real value is always > 0) and value = the clock. Both are put back right after. Off = untouched.
    const float gpKeep = cb.gravityPow, valKeep = cb.value;
    if (m_cfg.drops.inkSwirl > 0.0f && m_cfg.acid.enabled) {
        cb.gravityPow = -m_cfg.drops.inkSwirl;
        cb.value = m_time;
    }
    bind(m_psoAdvectDye.Get(), m_velocity.read, m_dye.read, m_dye.write, 4);
    cb.gravityPow = gpKeep; cb.value = valKeep;
    cb.dyeRise = 0.0f;
    m_dye.Swap();
    // 9. dye diffusion (optional): the Dâˆ‡Â²c smoke-spread term
    if (m_cfg.dyeDiffusion > 0.0001f) {
        cb.value = fminf(m_cfg.dyeDiffusion * stepsRef, 0.9f);
        bind(m_psoDiffuseDye.Get(), m_dye.read, nullptr, m_dye.write, 4);
        m_dye.Swap();
    }
}

// Shared body of the three splat entry points. `which` selects the passes:
// 1 = velocity only, 2 = dye only, 3 = both (velocity first, exactly the order
// the original Splat() used - the fluid look's seeded sequence depends on it).
void FluidRenderer::SplatImpl(int which, float x, float y, float dx, float dy,
                              float r, float g, float b, float radiusPct, float cap) {
    SimCB cb = {};
    cb.aspect = (float)m_width / (float)m_height;
    cb.pointX = x / m_width;
    cb.pointY = y / m_height;
    cb.radius = (radiusPct > 0.0f ? radiusPct : m_cfg.splatRadius) / 100.0f;

    m_cmd->SetComputeRootSignature(m_computeRS.Get());

    auto run = [&](ID3D12PipelineState* pso, Tex* src, Tex* dst) {
        Transition(*src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(*dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_cmd->SetPipelineState(pso);
        cb.dimsW = dst->w; cb.dimsH = dst->h;
        m_cmd->SetComputeRoot32BitConstants(0, sizeof(SimCB) / 4, &cb, 0);
        m_cmd->SetComputeRootDescriptorTable(1, src->srv);
        m_cmd->SetComputeRootDescriptorTable(2, src->srv);
        m_cmd->SetComputeRootDescriptorTable(3, dst->uav);
        m_cmd->SetComputeRootDescriptorTable(4, dst->uav);
        m_cmd->SetComputeRootDescriptorTable(5, dst->uav);
        m_cmd->Dispatch(Groups(dst->w), Groups(dst->h), 1);
    };

    if (which & 1) {   // velocity: add impulse, never capped
        cb.colorR = dx * m_cfg.flowSpeed; cb.colorG = dy * m_cfg.flowSpeed; cb.colorB = 0.0f;
        cb.cap = 1000000.0f;
        run(m_psoSplatVel.Get(), m_velocity.read, m_velocity.write);
        m_velocity.Swap();
    }
    if (which & 2) {   // dye: proportional brightness cap
        cb.colorR = r; cb.colorG = g; cb.colorB = b;
        cb.cap = (cap > 0.0f ? cap : m_cfg.maxBrightness);
        ID3D12PipelineState* pso = ((which & 4) && m_psoSplatDyeCompact)
                                 ? m_psoSplatDyeCompact.Get() : m_psoSplatDye.Get();
        run(pso, m_dye.read, m_dye.write);
        m_dye.Swap();
    }
}

void FluidRenderer::Splat(float x, float y, float dx, float dy, float r, float g, float b,
                          float radiusPct, float cap) {
    SplatImpl(3, x, y, dx, dy, r, g, b, radiusPct, cap);
}
void FluidRenderer::SplatVelocity(float x, float y, float dx, float dy, float radiusPct) {
    SplatImpl(1, x, y, dx, dy, 0.0f, 0.0f, 0.0f, radiusPct, -1.0f);
}
void FluidRenderer::SplatDye(float x, float y, float r, float g, float b,
                             float radiusPct, float cap, bool compact) {
    SplatImpl(compact ? 6 : 2, x, y, 0.0f, 0.0f, r, g, b, radiusPct, cap);
}

void FluidRenderer::MultipleSplats(int amount) {
    for (int i = 0; i < amount; i++) {
        // generateColor(): hue near the global wheel (or palette); intensity
        // is cfg.idleBrightness (wanderers paint at 0.15 â€” bursts were 1.5)
        RGB c;
        if (m_cfg.colorful) {
            float h = fmodf(WheelHue((RandF() - 0.5f) * 0.12f) + 1.0f, 1.0f);
            c = HSVtoRGB(h, 1.0f, 1.0f);
        } else {
            int n = m_cfg.moreColors ? 5 : 1;
            const float* p = m_cfg.splatColors + 3 * (rand() % n);
            c = { p[0], p[1], p[2] };
        }
        float x = m_width * RandF();
        float y = m_height * RandF();
        float dx = 1000.0f * (RandF() - 0.5f);
        float dy = 1000.0f * (RandF() - 0.5f);
        Splat(x, y, dx, dy, c.r * m_cfg.idleBrightness,
              c.g * m_cfg.idleBrightness, c.b * m_cfg.idleBrightness);
    }
}

void FluidRenderer::RenderDisplay() {
    const UINT i = m_frameIndex;
    Transition(*m_dye.read, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    RenderSurfacePass();   // Oil3D: records nothing unless oil_height is on

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = m_backBuffers[i].Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmd->ResourceBarrier(1, &b);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)i * m_rtvStride;
    // [post]: the display pass goes to m_postTex first and the camera pass
    // brings it to the back buffer; without it, straight to the back buffer.
    // A photo (brief BY) never runs the post pass.
    const bool post = !m_photoMode && PostActive();
    D3D12_CPU_DESCRIPTOR_HANDLE target = post ? BeginPostTarget() : rtv;
    m_cmd->OMSetRenderTargets(1, &target, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0, 0, (float)m_width, (float)m_height, 0, 1 };
    D3D12_RECT sc = { 0, 0, m_width, m_height };
    m_cmd->RSSetViewports(1, &vp);
    m_cmd->RSSetScissorRects(1, &sc);

    if (m_photoMode) {                     // brief BY: the photo stage's PSO
        DrawPhoto(m_width, m_height, m_sdrScale);
    } else {
    m_cmd->SetGraphicsRootSignature(m_graphicsRS.Get());
    m_cmd->SetPipelineState(DisplayPso());
    float consts[32];
    BuildDisplayConstants(consts);
    m_cmd->SetGraphicsRoot32BitConstants(0, 32, consts, 0);
    m_cmd->SetGraphicsRootDescriptorTable(1, m_dye.read->srv);
    BindAcid();
    BindInk();
    m_postGrainDeferred = post;
    BindMirrorFold();
    m_postGrainDeferred = false;
    m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmd->DrawInstanced(3, 1, 0, 0);
    }
    if (post) RunPostPass(rtv);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_cmd->ResourceBarrier(1, &b);
}

// ---- [post] image-space camera pass (kPostSrc) ----------------------------
bool FluidRenderer::PostActive() const {
    const PostConfig& po = m_cfg.post;
    // Any of the image-space effects turns the pass on -- the film overlay
    // and the second noise layer live in it as much as the defocus does, and
    // film_stock is applied there too. All default to 0, so style=fluid still
    // never enters it.
    // dof_max_px counts only on the acid look: the circle of confusion is
    // written by the LIQUID_ACID block of the display pass and nothing else
    // has an isoline or a droplet depth to derive one from, so on any other
    // style the alpha channel is still a plain 1.0 and must not be read as a
    // blur radius.
    // Oil3D Stage A: --shot-debug writes the raw h / n view; no lens on it.
    // (Oil3D L2: != 0, the lamps view is mode -1; every older mode is > 0)
    if (m_oilHeightDebug != 0 && m_cfg.acid.enabled) return false;
    return m_psoPost && (po.postBlurPx > 0.01f || po.postGlow > 0.0005f ||
                         po.filmDust > 0.0005f || po.filmHairs > 0.0005f ||
                         po.filmScratches > 0.0005f || po.filmLeak > 0.0005f ||
                         po.filmNoise > 0.0005f || po.filmStock > 0.0005f ||
                         po.fog > 0.0005f || po.bloom > 0.0005f ||
                         po.psfPx > 0.01f || po.dither > 0.0005f ||
                         po.aberration > 0.0005f ||
                         po.halation > 0.0005f || po.lid > 0.0005f ||
                         po.shimmer > 0.0005f || po.pixelShiftPx > 0.01f ||
                         po.cornerWarp > 0.0005f ||   // brief BN; the other BN keys ride on bloom/halation/lid
                         (m_cfg.acid.enabled && EffDofMaxPx() > 0.01f));
}

// One FP16 frame-sized texture: RTV slot kFrames*2+1 (after the back buffers,
// the analyzer and the mirror buffers), SRV heap slot 15 (the sim textures
// use 0..10). Created on first use; Shutdown() drops it with the device.
void FluidRenderer::EnsurePostTex() {
    if (m_postTex && m_postW == m_width && m_postH == m_height) return;
    if (m_postTex) { WaitForGpuIdle(); m_postTex.Reset(); }
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT64)m_width;
    rd.Height = (UINT)m_height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    m_postState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    HR(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, m_postState,
                                         nullptr, IID_PPV_ARGS(&m_postTex)));
    // a device lost right here (survive mode: HR only flags it) leaves no texture; no views
    // on null and no "done" stamp, the frame's post pass skips, the rebuild makes it again
    if (!m_postTex) return;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)(kFrames * 2 + 1) * m_rtvStride;
    m_device->CreateRenderTargetView(m_postTex.Get(), nullptr, rtv);

    const UINT srvSlot = 15 * 2;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = rd.Format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE sc = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    sc.ptr += (SIZE_T)srvSlot * m_srvStride;
    m_device->CreateShaderResourceView(m_postTex.Get(), &sv, sc);
    m_postSrv = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    m_postSrv.ptr += (SIZE_T)srvSlot * m_srvStride;
    m_postW = m_width; m_postH = m_height;
    printf("post: image-space pass target %dx%d\n", m_width, m_height);
}

D3D12_CPU_DESCRIPTOR_HANDLE FluidRenderer::BeginPostTarget() {
    EnsurePostTex();
    if (m_postTex && m_postState != D3D12_RESOURCE_STATE_RENDER_TARGET) {   // no barrier on null
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_postTex.Get();
        b.Transition.StateBefore = m_postState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_cmd->ResourceBarrier(1, &b);
        m_postState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)(kFrames * 2 + 1) * m_rtvStride;
    return rtv;
}

// The finished frame -> defocus disc -> veiling glare -> film grain -> dst.
// Radii are authored in px at 1440p and scaled with the frame height, so the
// preview and the panel agree; the shader takes them in texels of this frame.
void FluidRenderer::RunPostPass(D3D12_CPU_DESCRIPTOR_HANDLE dst) {
    if (!m_postTex) return;              // lost the device while making it: nothing to run on
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = m_postTex.Get();
    b.Transition.StateBefore = m_postState;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmd->ResourceBarrier(1, &b);
    m_postState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    const PostConfig& po = m_cfg.post;
    const float scale = (float)m_height / 1440.0f;
    float c[32] = {};
    c[0]  = 1.0f / (float)(m_width  > 0 ? m_width  : 1);
    c[1]  = 1.0f / (float)(m_height > 0 ? m_height : 1);
    c[2]  = fmaxf(po.postBlurPx, 0.0f) * scale;
    c[3]  = fminf(fmaxf(po.postGlow, 0.0f), 1.0f);
    c[4]  = fmaxf(po.postGlowPx, 1.0f) * scale;
    c[5]  = fminf(fmaxf(po.filmGrain, 0.0f), 1.0f);
    // grain and noise cell sizes are authored in px at 1440p like every other
    // optical width here, so the same stock reads the same at any output size
    c[6]  = fmaxf(fmaxf(po.filmGrainSize, 0.25f) * scale, 0.25f);
    c[7]  = fmaxf(po.filmGrainSpeed, 0.0f);
    c[8]  = fminf(fmaxf(po.filmGrainColor, 0.0f), 1.0f);
    c[9]  = m_time;
    c[10] = m_sdrScale * m_fade;   // cycle fade (1.0 = today, bit-identical)
    c[11] = fminf(fmaxf(po.postGlowDark, 0.0f), 1.0f);
    // the film overlay: population masters, the clock they change on, the
    // second noise layer, and this frame's 1440p scale (the shader divides
    // SV_Position by it, so every artefact is authored once, at 1440p)
    c[12] = fminf(fmaxf(po.filmDust, 0.0f), 1.0f);
    c[13] = fminf(fmaxf(po.filmHairs, 0.0f), 1.0f);
    c[14] = fminf(fmaxf(po.filmScratches, 0.0f), 1.0f);
    c[15] = fminf(fmaxf(po.filmLeak, 0.0f), 1.0f);
    c[16] = fmaxf(po.filmArtefactRate, 0.25f);
    c[17] = fminf(fmaxf(po.filmNoise, 0.0f), 1.0f);
    c[18] = fmaxf(fmaxf(po.filmNoiseSize, 0.25f) * scale, 0.25f);
    c[19] = fmaxf(scale, 1e-4f);
    c[20] = fminf(fmaxf(po.filmStock, 0.0f), 1.0f);
    // "light in the water": the haze reach is a fraction of the frame (it is
    // a distance in the picture), the bloom radius is in this frame's texels
    c[21] = fminf(fmaxf(po.fog, 0.0f), 1.0f);
    c[22] = fminf(fmaxf(po.bloom, 0.0f), 1.0f);
    // DEPTH OF FIELD. The display pass has already written this pixel's circle
    // of confusion, in px at 1440p, into the source's ALPHA; this is the clamp
    // on it, in THIS frame's texels, and its being non-zero is what tells the
    // pass that alpha is a blur radius at all rather than the plain 1.0 every
    // other path writes. Only the acid look produces a CoC (see PostActive).
    const float dofPx = EffDofMaxPx();
    c[23] = (m_cfg.acid.enabled && dofPx > 0.01f) ? dofPx * scale : 0.0f;
    c[24] = fmaxf(po.fogPx, 1.0f) / 1440.0f;
    c[25] = fmaxf(po.bloomPx, 1.0f) * scale;
    // DIFFRACTION. The floor under the defocus radius, in THIS frame's texels,
    // applied whatever the focus: no optical system resolves a point to a
    // point, so the sharpest thing in the frame is still this wide.
    c[26] = fmaxf(po.psfPx, 0.0f) * scale;
    // OUTPUT DITHER: half an LSB of ordered noise against 10-bit banding.
    c[28] = fminf(fmaxf(po.dither, 0.0f), 8.0f);
    // [post] dither_auto / mass_black run the POST_BLACK variant, which reads
    // this slot PACKED and negative (so the plain dither branch never runs in
    // it): -(1 + dq + 64 mode + 256 mbq), dq = dither x 4 (0..32), mode 0
    // today's dither / 1 dither_auto with HDR off (8-bit sRGB: DWM composes
    // the SDR desktop at 8 bits) / 2 dither_auto with HDR on (10-bit PQ), mbq
    // = mass_black x 63. Both keys off: the slot and the PSO are today's.
    const float qmb = (float)(int)(fminf(fmaxf(po.massBlack, 0.0f), 1.0f) * 63.0f + 0.5f);
    const bool postBlack = po.ditherAuto || qmb >= 1.0f;
    if (postBlack) {
        const float dq = (float)(int)(c[28] * 4.0f + 0.5f);
        const float md = (po.ditherAuto && dq >= 1.0f) ? (m_hdrActive ? 2.0f : 1.0f) : 0.0f;
        c[28] = -(1.0f + dq + 64.0f * md + 256.0f * qmb);
    }
    // HALATION: a tight warm glow taken only from the bright areas.
    c[29] = fminf(fmaxf(po.halation, 0.0f), 1.0f);
    c[30] = fmaxf(po.halationPx, 1.0f) * scale;
    c[31] = fminf(fmaxf(po.halationWarmth, 0.0f), 1.0f);
    // GRAIN FRAME RATE (pp6.w): the grain and film_noise quantise their time
    // to this many patterns per second. light_x / light_y / light_drift used
    // to live in c[27]; the lamp is part of the RIG now (block below).
    c[27] = fmaxf(po.filmGrainFps, 1.0f);

    // ---- THE RIG, as ONE contiguous block ---------------------------------
    // b3 -- the root-constant block the display pass uses for the mirror fold
    // and the post pass has never bound. 20 constants, of which the rig is the
    // first 8 and the rest are reserved for the lid ghosts, the flare and the
    // vignette centre, all of which are motions of this same body and must be
    // read from these numbers rather than re-derived beside them.
    float rig[20] = {};
    bool bnOptics = false;   // brief BN: set in the rg1 pack below
    bool heartPost = false;  // brief BX: set in the rg1 pack below
    rig[0] = m_rig.lampX;   rig[1] = m_rig.lampY;
    rig[2] = m_rig.axisX;   rig[3] = m_rig.axisY;
    // rig[4..5] (rg1.xy) used to carry the tilt, which the post pass never
    // read (the display pass gets it through LA_TILT_AMT). They are now two
    // packs of four 6-bit fields, most significant first, the layout fixed
    // for briefs BO and BN so neither repacks the other; a field whose key
    // does not exist yet is written 0:
    //   rg1.x  artefact_lum_gate : 6 | corner_warp : 6 | corner_warp_r : 6 | bloom_warmth : 6
    //   rg1.y  glass_streaks : 6 | halation_threshold : 6 | spare : 6 | spare : 6
    // artefact_lum_gate 0 quantises to 0 and unpacks to exactly 0.0, which is
    // the shader's off test, so no preset moves by a bit.
    {
        auto q6 = [](float v) -> float {
            float u = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            return (float)(int)(u * 63.0f + 0.5f);
        };
        // brief BN: corner_warp_r (0.4..0.9) is only written while the warp
        // itself quantises to at least 1, so with the warp off rg1.x is what
        // it was before BN existed. glass_streaks needs the lid (its positions
        // are the lid's), so it is written 0 while the lid is off.
        const float qcw  = q6(po.cornerWarp);
        const float qcwr = qcw >= 1.0f ? q6((po.cornerWarpR - 0.4f) / 0.5f) : 0.0f;
        const float qgs  = po.lid > 0.0005f ? q6(po.glassStreaks) : 0.0f;
        const float qbw  = q6(po.bloomWarmth);
        const float qht  = q6(po.halationThreshold);
        // Any BN field live -> the BN_OPTICS build of the post shader. With all
        // of them 0 the pass keeps today's PSO, bytecode for bytecode: the
        // extra code alone, never taken, moved 1-2 identity pixels by a code
        // through the driver's ISA compile.
        bnOptics = qcw >= 1.0f || qgs >= 1.0f || qbw >= 1.0f || qht >= 1.0f;
        rig[4] = q6(po.artefactLumGate) * 262144.0f   // artefact_lum_gate (BO)
               + qcw * 4096.0f                         // corner_warp (BN)
               + qcwr * 64.0f                          // corner_warp_r (BN)
               + qbw;                                  // bloom_warmth (BN)
        rig[5] = qgs * 262144.0f                       // glass_streaks (BN)
               + qht * 4096.0f;                        // halation_threshold (BN)
        // brief BX (fix round): the grey heart's desaturation runs at the END
        // of this pass on the acid look (fields 3 and 4 of rg1.y: amount,
        // size (0.8..2.2 -> 0..1)), so bloom / halation / glow / grain / stock
        // grey with the film and the kept chroma is hue-independent; the
        // display pass then does only the film lift (UploadAcidConstants
        // writes HEART_K negative). Both fields 0 when the heart is off.
        const LiquidAcidConfig& ha = m_cfg.acid;
        const float qhk = ha.enabled ? q6(ha.lampGreyHeart) : 0.0f;
        heartPost = qhk >= 1.0f;
        if (heartPost) rig[5] += qhk * 64.0f + q6((fminf(fmaxf(ha.lampGreyHeartSize, 0.8f), 2.2f) - 0.8f) / 1.4f);
    }
    // rig[6..7] (rg1.zw) are brief BM's lid scratches, packed below.
    // The LENS's own chromatic split (item Z). It lives in the rig block
    // because it is a property of the same lens the rig carries the centre
    // of: the split is radial about that centre, so the two numbers have to
    // travel together or the null point drifts away from the axis.
    rig[8]  = fminf(fmaxf(po.aberration, 0.0f), 1.0f);
    rig[9]  = fmaxf(po.aberrationPx, 0.0f) * scale;
    rig[10] = fminf(fmaxf(po.aberrationField, 0.0f), 2.0f);
    // ---- brief BD, four knobs in rg2.w -----------------------------------
    // b0's 32 constants are full and the root signature is at the 64-DWORD
    // limit, so these ride in the one float rg2 had spare, packed the way the
    // lid below packs its twelve: four 6-bit fields in an exact integer at
    // most 2^24-1, which float32 carries without loss. Six bits is 1/63 of a
    // taste knob read once per pixel, finer than the panel resolves. The
    // DEFAULTS are the point -- 1 / 0 / 0 / 0 quantise to 63 / 0 / 0 / 0 and
    // unpack to exactly 1.0 / 0.0 / 0.0 / 0.0, so no preset that leaves these
    // alone moves by a bit.
    //   grainChroma : 6 | grainDensity : 6 | fogMassGate : 6 | aberrCoc : 6
    {
        auto q6 = [](float v) -> float {
            float u = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            return (float)(int)(u * 63.0f + 0.5f);
        };
        rig[11] = q6(po.filmGrainChroma)  * 262144.0f
                + q6(po.filmGrainDensity) * 4096.0f
                + q6(po.fogMassGate)      * 64.0f
                + q6(po.aberrationCoc);
    }
    // ---- THE LID (task V2) -- ALL OF IT IN rg4 ---------------------------
    // rg2 is the lens's chromatic split (item Z) and rg3 is reserved for the
    // camera executor's V3 motion, so the lid gets ONE float4. Twelve numbers
    // into four is a packing problem, not a reason to grow the root signature
    // (b0's 32 constants plus the rest of this signature already come to
    // exactly the 64-DWORD limit) and not a reason to repack somebody else's
    // slot. Each float carries an exact integer below 2^24, which a float32
    // holds without loss, so the unpack on the other side is two divides and
    // a floor per field and nothing is approximated twice.
    //   rg4.x  lidX : 12 | lidY : 12          the cover's own wander, uv
    //   rg4.y  lidRot : 12 | refractPx : 12   orientation and reflection wobble
    //   rg4.z  ghost : 8 | rings : 8 | sheen : 8
    //   rg4.w  glint : 7 | iris : 7 | sheenPx : 6 | ghostSpread : 4
    // The MASTER is not sent at all: it is folded into every amplitude here,
    // which costs nothing and buys back the twelve bits it would have taken.
    // With the lid off all four floats are written as exact 0, which is what
    // the shader's one branch tests.
    {
        auto qz = [](float v, float lo, float hi, int bits) -> float {
            float u = (v - lo) / (hi - lo);
            u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
            const float m = (float)((1 << bits) - 1);
            return (float)(int)(u * m + 0.5f);
        };
        const float L = fminf(fmaxf(po.lid, 0.0f), 1.0f);
        if (L <= 0.0005f) {
            rig[16] = rig[17] = rig[18] = rig[19] = 0.0f;
        } else {
            rig[16] = qz(m_rig.lidX, -0.5f, 0.5f, 12) * 4096.0f
                    + qz(m_rig.lidY, -0.5f, 0.5f, 12);
            rig[17] = qz(m_rig.lidRot, -4.0f, 4.0f, 12) * 4096.0f
                    + qz(fmaxf(po.lidRefractPx, 0.0f) * scale, 0.0f, 24.0f, 12);
            rig[18] = qz(L * po.lidGhost, 0.0f, 1.0f, 8) * 65536.0f
                    + qz(L * po.lidRings, 0.0f, 1.0f, 8) * 256.0f
                    + qz(L * po.lidSheen, 0.0f, 1.0f, 8);
            rig[19] = qz(L * po.lidGlint, 0.0f, 1.0f, 7) * 131072.0f
                    + qz(L * po.lidIris,  0.0f, 1.0f, 7) * 1024.0f
                    + qz(fmaxf(po.lidSheenPx, 8.0f) * scale, 8.0f, 1400.0f, 6) * 16.0f
                    + qz(po.lidGhostSpread, 0.0f, 2.0f, 4);
        }
    }
    // ---- brief BM: the lid's SCRATCHES, in rg1.zw -------------------------
    // rg1.zw carried the focus depth and the move phase, which this pass has
    // never read (the display pass gets the focus through its acid slot), so
    // they are the free rig floats. Packed exactly like rg4.z: three 8-bit
    // fields in an exact integer below 2^24. Both are exact 0 unless the lid
    // is on AND the scratch amount quantises to at least 1 -- rg1.z > 0.5 is
    // the shader's only test, so every preset without scratches is untouched.
    //   rg1.z  amount : 8 | density : 8 | len : 8
    //   rg1.w  corner : 8 | soft : 8 | tint : 8
    {
        auto q8 = [](float v) -> float {
            float u = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            return (float)(int)(u * 255.0f + 0.5f);
        };
        const float qa = q8(po.lidScratch);
        if (po.lid > 0.0005f && qa >= 1.0f) {
            rig[6] = qa * 65536.0f + q8(po.lidScratchDensity) * 256.0f
                   + q8(po.lidScratchLen);
            rig[7] = q8(po.lidScratchCorner) * 65536.0f
                   + q8(po.lidScratchSoft) * 256.0f + q8(po.lidScratchTint);
        } else {
            rig[6] = rig[7] = 0.0f;
        }
    }
    // rg3 (12..15) -- MOTION (item V3). rg4 (16..19) is the lid, filled just
    // above: the two branches were given a slot each and neither had to
    // repack the other.
    rig[12] = fminf(fmaxf(po.shimmer, 0.0f), 1.0f);
    rig[13] = fmaxf(po.shimmerPx, 0.0f) * scale;
    // the orbit is authored in px at 1440p and handed over in uv
    rig[14] = m_rig.shiftX * scale / (float)(m_width  > 0 ? m_width  : 1);
    rig[15] = m_rig.shiftY * scale / (float)(m_height > 0 ? m_height : 1);

    // brief BN: the BN_OPTICS post PSO is compiled the first time a BN key
    // goes live (~0.3 s, once; PSO creation is off the command list) and kept.
    if (bnOptics && !m_psoPostBN) {
        const D3D_SHADER_MACRO defs[] = { { "BN_OPTICS", "1" }, { nullptr, nullptr } };
        MakeGraphicsPso(kPostSrc, m_psoPostBN, defs);
        printf("post: BN_OPTICS PSO compiled on demand\n");
    }
    m_cmd->OMSetRenderTargets(1, &dst, FALSE, nullptr);
    // brief BX: the grey heart's post PSO (BN_OPTICS + BX_HEART), compiled the
    // first time the heart is on with this pass running, and kept. BN code in
    // it is still gated on its own fields, so one variant covers both.
    if (heartPost && !m_psoPostHeart) {
        const D3D_SHADER_MACRO defs[] = { { "BN_OPTICS", "1" }, { "BX_HEART", "1" }, { nullptr, nullptr } };
        MakeGraphicsPso(kPostSrc, m_psoPostHeart, defs);
        printf("post: BX_HEART PSO compiled on demand\n");
    }
    // Oil3D L1b: while lamp 1's light is not white, the same variant with
    // LAMP_GLOW (the fog and bloom_warmth take the window's colour, read from
    // the acid cbuffer at b1, which this pass binds then). Built once on
    // demand; with the lamp white the three plain PSOs run, bytecode for
    // bytecode.
    const bool lampGlow = m_cfg.acid.enabled && m_cfg.acid.LampGlowWanted()
                          && m_acidParamUpload[m_frameIndex];
    // [post] mass_black / dither_auto: the POST_BLACK variant (BN_OPTICS +
    // BX_HEART + POST_BLACK, each gated on its own fields), on demand, kept.
    if (postBlack && !m_psoPostBlack) {
        const D3D_SHADER_MACRO defs[] = { { "BN_OPTICS", "1" }, { "BX_HEART", "1" }, { "POST_BLACK", "1" },
                                          { nullptr, nullptr } };
        MakeGraphicsPso(kPostSrc, m_psoPostBlack, defs);
        printf("post: POST_BLACK PSO compiled on demand\n");
    }
    const int  postV = postBlack ? 3 : (heartPost ? 2 : (bnOptics ? 1 : 0));
    if (lampGlow && !m_psoPostL[postV]) {
        static const D3D_SHADER_MACRO kGlow0[] = { { "LAMP_GLOW", "1" }, { nullptr, nullptr } };
        static const D3D_SHADER_MACRO kGlow1[] = { { "BN_OPTICS", "1" }, { "LAMP_GLOW", "1" }, { nullptr, nullptr } };
        static const D3D_SHADER_MACRO kGlow2[] = { { "BN_OPTICS", "1" }, { "BX_HEART", "1" },
                                                   { "LAMP_GLOW", "1" }, { nullptr, nullptr } };
        static const D3D_SHADER_MACRO kGlow3[] = { { "BN_OPTICS", "1" }, { "BX_HEART", "1" }, { "POST_BLACK", "1" },
                                                   { "LAMP_GLOW", "1" }, { nullptr, nullptr } };
        static const D3D_SHADER_MACRO* const kGlowDefs[4] = { kGlow0, kGlow1, kGlow2, kGlow3 };
        MakeGraphicsPso(kPostSrc, m_psoPostL[postV], kGlowDefs[postV]);
        printf("post: LAMP_GLOW PSO (variant %d) compiled on demand\n", postV);
    }
    if (m_stageTap > 200 && m_stageTap < 299) {
        // Oil3D L3b: the PSO below again with POST_STAGETAP (CaptureStageTap only)
        char tv[16];
        snprintf(tv, sizeof tv, "%d", m_stageTap - 200);
        const bool glow = lampGlow && m_psoPostL[postV];
        D3D_SHADER_MACRO defs[6];
        int nd = 0;
        if (heartPost || bnOptics || postBlack) defs[nd++] = { "BN_OPTICS", "1" };
        if (heartPost || postBlack)             defs[nd++] = { "BX_HEART", "1" };
        if (postBlack)                          defs[nd++] = { "POST_BLACK", "1" };
        if (glow)                               defs[nd++] = { "LAMP_GLOW", "1" };
        defs[nd++] = { "POST_STAGETAP", tv };
        defs[nd]   = { nullptr, nullptr };
        m_cmd->SetPipelineState(StageTapPso(true, (long long)m_stageTap * 10000 + postV * 10 + (glow ? 1 : 0), defs));
        if (glow) m_cmd->SetGraphicsRootConstantBufferView(2, m_acidParamUpload[m_frameIndex]->GetGPUVirtualAddress());
    } else if (lampGlow && m_psoPostL[postV]) {
        m_cmd->SetPipelineState(m_psoPostL[postV].Get());
        m_cmd->SetGraphicsRootConstantBufferView(2, m_acidParamUpload[m_frameIndex]->GetGPUVirtualAddress());
    } else {
        m_cmd->SetPipelineState(postBlack ? m_psoPostBlack.Get()
                                : heartPost ? m_psoPostHeart.Get()
                                : bnOptics ? m_psoPostBN.Get() : m_psoPost.Get());
    }
    m_cmd->SetGraphicsRoot32BitConstants(0, 32, c, 0);
    m_cmd->SetGraphicsRoot32BitConstants(6, 20, rig, 0);
    // The sim's own low-res velocity, so the thermal shimmer is ADVECTED by
    // the fluid rather than wobbling on its own clock: a burst that moves the
    // oil pushes the heat ahead of it (the user's rule -- every post effect's
    // position is an idle drift plus a term from the fluid). Only bound when
    // the shimmer is actually on; t3 is otherwise untouched by this pass.
    if (po.shimmer > 0.0005f && m_velLow.res)
        m_cmd->SetGraphicsRootDescriptorTable(5, m_velTable);
    m_cmd->SetGraphicsRootDescriptorTable(1, m_postSrv);
    m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmd->DrawInstanced(3, 1, 0, 0);
}

// Same display pass as RenderDisplay(), aimed at the offscreen FP16 target.
// Identical constants, identical shader â€” only the render target differs, so
// the captured pixels are exactly what the swap chain would have received.
void FluidRenderer::RenderDisplayOffscreen() {
    Transition(*m_dye.read, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    if (m_shotState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_shotTex.Get();
        b.Transition.StateBefore = m_shotState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_cmd->ResourceBarrier(1, &b);
        m_shotState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    RenderSurfacePass();   // Oil3D: records nothing unless oil_height / --shot-debug is on
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const bool postReal = !m_photoMode && PostActive();   // a photo never runs the post pass
    // Oil3D L3b: a DISPLAY-stage tap (CaptureStageTap) reads the display pass
    // itself: no [post] pass behind it, its grain still deferred as in the frame
    const bool post = postReal && !(m_stageTap > 0 && m_stageTap < 200);
    D3D12_CPU_DESCRIPTOR_HANDLE target = post ? BeginPostTarget() : rtv;
    m_cmd->OMSetRenderTargets(1, &target, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0, 0, (float)m_width, (float)m_height, 0, 1 };
    D3D12_RECT sc = { 0, 0, m_width, m_height };
    m_cmd->RSSetViewports(1, &vp);
    m_cmd->RSSetScissorRects(1, &sc);

    if (m_photoMode) {                     // brief BY: the photo stage's PSO
        DrawPhoto(m_width, m_height, m_sdrScale);
    } else {
    m_cmd->SetGraphicsRootSignature(m_graphicsRS.Get());
    m_cmd->SetPipelineState(DisplayPso());
    float consts[32];
    BuildDisplayConstants(consts);
    m_cmd->SetGraphicsRoot32BitConstants(0, 32, consts, 0);
    m_cmd->SetGraphicsRootDescriptorTable(1, m_dye.read->srv);
    BindAcid();
    BindInk();
    m_postGrainDeferred = postReal;
    BindMirrorFold();
    m_postGrainDeferred = false;
    m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    TimestampMark(3);
    m_cmd->DrawInstanced(3, 1, 0, 0);
    TimestampMark(4);
    }
    if (post) RunPostPass(rtv);
    if (!m_photoMode) { TimestampMark(5); TimestampResolve(); }
}

// Copy the offscreen target back to the CPU as linear scRGB floats. Runs its
// own one-shot command list after a full GPU flush, so it can be called
// between Frame() calls without disturbing the frame ring.
bool FluidRenderer::CaptureOffscreen(std::vector<float>& out) {
    if (!m_headless || !m_shotTex || !m_shotReadback) return false;
    WaitForGpuIdle();

    HR(m_allocators[0]->Reset());
    HR(m_cmd->Reset(m_allocators[0].Get(), nullptr));
    if (m_deviceLost) return false;   // R3: only reachable under --test-device-lost

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = m_shotTex.Get();
    b.Transition.StateBefore = m_shotState;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (m_shotState != D3D12_RESOURCE_STATE_COPY_SOURCE) m_cmd->ResourceBarrier(1, &b);
    m_shotState = D3D12_RESOURCE_STATE_COPY_SOURCE;

    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
    src.pResource = m_shotTex.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = m_shotReadback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    dst.PlacedFootprint.Footprint.Width = (UINT)m_width;
    dst.PlacedFootprint.Footprint.Height = (UINT)m_height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = m_shotPitch;
    m_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    HR(m_cmd->Close());
    if (m_deviceLost) return false;
    ID3D12CommandList* lists[] = { m_cmd.Get() };
    m_queue->ExecuteCommandLists(1, lists);
    WaitForGpuIdle();
    if (m_deviceLost) return false;

    uint8_t* data = nullptr;
    D3D12_RANGE range = { 0, (SIZE_T)m_shotPitch * m_height };
    if (FAILED(m_shotReadback->Map(0, &range, (void**)&data))) return false;
    out.resize((size_t)m_width * m_height * 4);
    for (int y = 0; y < m_height; y++) {
        const uint16_t* row = (const uint16_t*)(data + (SIZE_T)y * m_shotPitch);
        float* d = out.data() + (size_t)y * m_width * 4;
        for (int i = 0; i < m_width * 4; i++) d[i] = HalfToFloat(row[i]);
    }
    D3D12_RANGE none = { 0, 0 };
    m_shotReadback->Unmap(0, &none);

    // the frame ring's allocator 0 was reset out of band â€” resync so the next
    // BeginFrame() doesn't try to reset an allocator whose work is in flight
    for (UINT i = 0; i < kFrames; i++) m_fenceValues[i] = 0;
    m_frameIndex = 0;
    return true;
}

void FluidRenderer::BuildDisplayConstants(float out[32]) {
    BuildDisplayConstantsEx(out, m_width, m_height, m_sdrScale, m_cfg.hdrPeakNits, m_hdrActive,
                            m_panelPrimValid ? m_panelPrim : nullptr, m_sdrManaged);
}

// RGB -> XYZ of a set of primaries (xy of R, G, B) with a D65 white. false when
// the triangle is degenerate.
static bool PrimariesToXyz(const float p[6], double m[9]) {
    double c[9];
    for (int k = 0; k < 3; k++) {
        const double x = p[2 * k], y = p[2 * k + 1];
        if (!(y > 1e-4)) return false;
        c[k] = x / y; c[3 + k] = 1.0; c[6 + k] = (1.0 - x - y) / y;
    }
    const double wx = 0.3127, wy = 0.3290;
    const double W[3] = { wx / wy, 1.0, (1.0 - wx - wy) / wy };
    const double det = c[0] * (c[4] * c[8] - c[5] * c[7]) - c[1] * (c[3] * c[8] - c[5] * c[6]) +
                       c[2] * (c[3] * c[7] - c[4] * c[6]);
    if (fabs(det) < 1e-9) return false;
    const double inv[9] = {
        (c[4] * c[8] - c[5] * c[7]) / det, (c[2] * c[7] - c[1] * c[8]) / det, (c[1] * c[5] - c[2] * c[4]) / det,
        (c[5] * c[6] - c[3] * c[8]) / det, (c[0] * c[8] - c[2] * c[6]) / det, (c[2] * c[3] - c[0] * c[5]) / det,
        (c[3] * c[7] - c[4] * c[6]) / det, (c[1] * c[6] - c[0] * c[7]) / det, (c[0] * c[4] - c[1] * c[3]) / det };
    double S[3];
    for (int r = 0; r < 3; r++) S[r] = inv[3 * r] * W[0] + inv[3 * r + 1] * W[1] + inv[3 * r + 2] * W[2];
    for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) m[3 * r + k] = c[3 * r + k] * S[k];
    return true;
}

// [hdr] sdr_gamut = 1: the configured gamut (1 P3, 2 BT.2020) expressed in the
// PANEL's primaries. A wide-gamut panel shows an SDR desktop's values in its
// native gamut, so sending these coordinates puts the same chromaticity on the
// glass that HDR does (colours outside the panel clip in DWM, as they clip in
// the monitor under HDR). Rows sum to 1: white stays white.
bool FluidRenderer::SdrMatchMatrix(int gamutMode, const float primXY[6], float out[9]) {
    static const float kP3[6]   = { 0.680f, 0.320f, 0.265f, 0.690f, 0.150f, 0.060f };
    static const float k2020[6] = { 0.708f, 0.292f, 0.170f, 0.797f, 0.131f, 0.046f };
    if (!primXY || gamutMode < 1 || gamutMode > 2) return false;
    // plausibility: a real panel sits between a little under sRGB and BT.2020
    for (int k = 0; k < 6; k++) if (!(primXY[k] > 0.01f && primXY[k] < 0.90f)) return false;
    const double area = 0.5 * fabs(primXY[0] * (primXY[3] - primXY[5]) + primXY[2] * (primXY[5] - primXY[1]) +
                                   primXY[4] * (primXY[1] - primXY[3]));
    if (area < 0.085 || area > 0.225) return false;       // sRGB 0.112, BT.2020 0.212
    double src[9], pan[9];
    if (!PrimariesToXyz(gamutMode == 2 ? k2020 : kP3, src) || !PrimariesToXyz(primXY, pan)) return false;
    const double det = pan[0] * (pan[4] * pan[8] - pan[5] * pan[7]) - pan[1] * (pan[3] * pan[8] - pan[5] * pan[6]) +
                       pan[2] * (pan[3] * pan[7] - pan[4] * pan[6]);
    if (fabs(det) < 1e-9) return false;
    const double inv[9] = {
        (pan[4] * pan[8] - pan[5] * pan[7]) / det, (pan[2] * pan[7] - pan[1] * pan[8]) / det, (pan[1] * pan[5] - pan[2] * pan[4]) / det,
        (pan[5] * pan[6] - pan[3] * pan[8]) / det, (pan[0] * pan[8] - pan[2] * pan[6]) / det, (pan[2] * pan[3] - pan[0] * pan[5]) / det,
        (pan[3] * pan[7] - pan[4] * pan[6]) / det, (pan[1] * pan[6] - pan[0] * pan[7]) / det, (pan[0] * pan[4] - pan[1] * pan[3]) / det };
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++)
            out[3 * r + k] = (float)(inv[3 * r] * src[k] + inv[3 * r + 1] * src[3 + k] + inv[3 * r + 2] * src[6 + k]);
    return true;
}

// hdrActive = the Windows HDR state of the monitor this frame goes to (the
// primary's for the wallpaper, the second monitor's own for the mirror);
// primXY = that monitor's primaries (null = unknown); sdrManaged = Windows
// colour-manages its SDR desktop (see SetSdrGamut).
void FluidRenderer::BuildDisplayConstantsEx(float out[32], int w, int h,
                                            float sdrScale, float peakNits, bool hdrActive,
                                            const float* primXY, bool sdrManaged) {
    // CSS-filter chain parameters. The shader evaluates them primitive by
    // primitive with a clamp after each (Chromium/Skia behaviour); see
    // kDisplaySrc. Canvas filter: saturate -> brightness -> contrast ->
    // hue-rotate (the hue-shift burst). Then WE's right-panel adjust,
    // outermost: saturate -> brightness -> contrast -> hue-rotate.
    float hdrOn = (hdrActive && m_cfg.hdrCompensation) ? 1.0f : 0.0f;
    float hue = fmodf(m_hueAngle, 360.0f);
    if (hue > -0.05f && hue < 0.05f) hue = 0.0f;

    // HDR highlight expansion: gain that carries hot dye from SDR white up to
    // the peak-nits target (resolved by the shell; 0 = parity mode).
    float peakGain = 1.0f;
    if (hdrActive && peakNits > 0.0f) {
        float sdrWhiteNits = 80.0f * sdrScale;
        peakGain = fmaxf(1.0f, peakNits / fmaxf(sdrWhiteNits, 1.0f));
    }

    // HDR on: the gamut stretch (P3 / BT.2020), shown colour-accurately.
    // HDR off: the desktop is sRGB-coded, DWM clips what leaves it, and the
    // panel then shows those values in ITS OWN gamut. [hdr] sdr_gamut says what
    // to send (user 2026-10-03: "it should switch between BT.2020 and SDR when
    // I toggle"; 2026-10-04: plain sRGB alone left SDR visibly less saturated
    // than HDR in the reds / oranges / magentas on the test panel):
    //   1 (default) the gamut converted to the panel's primaries = the colours
    //     HDR shows (gamut 3 + the matrix); primaries unknown -> the P3 stretch
    //     for a BT.2020 config (the test panel, a QD-OLED: mean ~0.003, worst ~0.017 u'v' at the
    //     blue-violets), plain for a P3 config;
    //   0 plain sRGB;   2 the HDR stretch itself, clipped (sRGB-mode monitors).
    // An SDR desktop that Windows colour-manages (sdrManaged) is the HDR case:
    // DWM maps our scRGB to the panel, so the stretch goes out untouched.
    float gamut = (float)m_cfg.gamutMode;
    float gm[9] = {};
    if (!hdrActive && !sdrManaged && m_cfg.gamutMode > 0) {
        if (m_sdrGamutMode == 0) gamut = 0.0f;
        else if (m_sdrGamutMode == 1) {
            if (SdrMatchMatrix(m_cfg.gamutMode, primXY, gm)) gamut = 3.0f;
            else gamut = m_cfg.gamutMode == 2 ? 1.0f : 0.0f;
        }
    }

    // Cycle fade: sdrScale above is the UNFADED scale (so peakGain does not
    // grow as the frame dims); the constant the shader multiplies by carries
    // the fade. m_fade == 1.0f -> sdrScale * 1.0f == sdrScale exactly.
    const float clipOut = (!hdrActive && !sdrManaged) ? 2.0f
                        : ((PostActive() && m_cfg.post.aberration > 0.0005f) ? 1.0f : 0.0f);
    float consts[32] = { 1.0f / w, 1.0f / h,
                         m_cfg.shading ? 1.0f : 0.0f, sdrScale * m_fade,
                         gamut, peakGain, m_cfg.hdrKnee,
                         fmaxf(m_cfg.maxBrightness, m_cfg.hdrKnee + 0.05f),
                         m_cfg.hdrSaturation, m_cfg.hdrBrightness, m_cfg.hdrContrast, hdrOn,
                         hue, m_cfg.postSaturation, m_cfg.postBrightness, m_cfg.postContrast,
                         m_cfg.postHue, gm[0], gm[1], gm[3],
                         // fmOff.w (Oil3D L1b): the wide-gamut negatives are
                         // CLIPPED before the panel -- by DWM (HDR off on an SDR
                         // desktop it does not manage) or by the [post] pass
                         // (its lateral aberration ends in max(d, 0)); read only
                         // by the OIL_LAMP output cap, so no other PSO sees it
                         gm[4], gm[6], gm[7], clipOut,
                         m_cfg.curveEnabled ? 1.0f : 0.0f,
                         m_cfg.curveCenter, m_cfg.curveWidth, m_cfg.curveHeight,
                         m_cfg.shadowFloor, m_cfg.shadowKnee,
                         // shadow.z: [post] sdr_shoulder, HDR OFF only (this
                         // monitor's state; with HDR on it is 0 = today)
                         hdrActive ? 0.0f : fminf(fmaxf(m_cfg.post.sdrShoulder, 0.0f), 1.0f),
                         0.0f };
    memcpy(out, consts, sizeof(consts));
}

// ===========================================================================
// [mirror] â€” screen mirroring / kaleidoscope.
//
// The fold itself is one uv transform at the top of the display pixel shader
// (see MirrorFold in kDisplaySrc); everything here is its plumbing: the 12
// root constants, and a CPU twin of the same fold so a pointer splat lands
// where the user SEES it rather than where the pointer literally is.
//
// Note on names: m_mirrorChain / m_mirrorW below are the unrelated
// SECOND-MONITOR mirror. Everything to do with the fold says "MirrorFold"
// or lives in m_cfg.mirror.
// ===========================================================================

// b3 also carries the [post] block (film grain + aberration vignette): it is
// bound on exactly the same draws as the fold, it is look-agnostic in exactly
// the same way, and it needs the two numbers -- time and aspect -- that mrP0
// already holds. One root-constants slot for both.
void FluidRenderer::BuildMirrorConstants(float out[20], int w, int h) const {
    const MirrorConfig& mr = m_cfg.mirror;
    const PostConfig& po = m_cfg.post;
    const float aspect = (float)(w > 0 ? w : 1) / (float)(h > 0 ? h : 1);
    const float vw = fminf(fmaxf(po.vignetteWander, 0.0f), 1.0f);
    const float vignCx = fminf(fmaxf(0.5f + (m_rig.axisX - 0.5f) * 4.0f * vw, 0.18f), 0.82f);
    const float vignCy = fminf(fmaxf(0.5f + (m_rig.axisY - 0.5f) * 4.0f * vw, 0.18f), 0.82f);
    // mode 5 (the fish-eye camera, fluid.h) has no fold: it reads its own four
    // numbers out of the slots the folds use (segments -> fov, rotate_period
    // -> pitch, drift -> height, soft -> horizon fade), so the root constants
    // keep their 20 floats. Every other mode: the values these always were.
    const bool cam = (mr.mode == 5);
    const float c[20] = {
        (float)mr.mode, cam ? mr.camFov : (float)mr.segments, aspect, m_time,
        mr.centerX, mr.centerY, cam ? mr.camPitch : fmaxf(mr.rotatePeriod, 0.0f),
        cam ? fmaxf(mr.camHeight, 0.01f) : fminf(fmaxf(mr.drift, 0.0f), 1.0f),
        // 10, 11: brief BD's two grain knobs for the DISPLAY pass's own copy
        // of the film grain -- the one that runs when no image-space pass is
        // on. 1 / 0 is the behaviour this had when these two slots were the
        // literal zeros they replace.
        // (mode 5: the `source` slot carries cam_relief, the dome height the camera's relief walk uses)
        cam ? fmaxf(mr.camFade, 0.0f) : fmaxf(mr.soft, 0.0f),
        cam ? fminf(fmaxf(mr.camRelief, 0.0f), 1.5f) : (float)(mr.source & 3),
        fminf(fmaxf(po.filmGrainChroma, 0.0f), 1.0f),
        fminf(fmaxf(po.filmGrainDensity, 0.0f), 1.0f),
        // grain moves to the post pass on the draws it follows (see kPostSrc)
        m_postGrainDeferred ? 0.0f : fminf(fmaxf(po.filmGrain, 0.0f), 1.0f),
        // px at 1440p, scaled with the frame, so the display pass's own grain
        // is the same stock as the post pass's and neither changes size with
        // the output resolution
        fmaxf(fmaxf(po.filmGrainSize, 0.25f) * ((float)(h > 0 ? h : 1) / 1440.0f), 0.25f),
        fmaxf(po.filmGrainSpeed, 0.0f), fminf(fmaxf(po.filmGrainColor, 0.0f), 1.0f),
        // 16,17: the VIGNETTE's centre in uv (item V3). It follows the rig's
        // lens, amplified, so the darkest corner rotates over minutes rather
        // than being burnt into one corner of the panel. wander 0 puts it
        // back at 0.5, 0.5 -- which is the constant this used to be.
        vignCx, vignCy,
        // 18: the grain frame rate (film_grain_fps); was aberration_field
        // until the lateral aberration moved to the post pass (item Z).
        fmaxf(po.filmGrainFps, 1.0f),
        fminf(fmaxf(po.vignette, 0.0f), 1.0f),
    };
    memcpy(out, c, sizeof(c));
}

void FluidRenderer::BindMirrorFold(int w, int h) {
    float c[20];
    BuildMirrorConstants(c, w > 0 ? w : m_width, h > 0 ? h : m_height);
    m_cmd->SetGraphicsRoot32BitConstants(6, 20, c, 0);
}

// Must stay in step with MirrorFold()/MirFoldAxis() in kDisplaySrc. `soft` is
// deliberately ignored: it is a display nicety a few pixels wide, and a splat
// does not care. NOT compensated: the kaleidoscope's rotation makes the
// impulse direction ill-defined, so there the flips stay 1.
void FluidRenderer::MirrorMapPointer(float& px, float& py,
                                     float& flipX, float& flipY) const {
    flipX = flipY = 1.0f;
    const MirrorConfig& mr = m_cfg.mirror;
    if (mr.mode <= 0 || m_width <= 0 || m_height <= 0) return;
    if (mr.mode >= 5) return;      // the fish-eye camera: the pointer is not carried through the lens (test)

    float u = px / (float)m_width, v = py / (float)m_height;
    float cx = mr.centerX, cy = mr.centerY;
    if (mr.drift > 0.0005f) {
        auto wander = [](float t) {
            return 0.55f * sinf(t) + 0.30f * sinf(t * 1.913f + 1.7f)
                 + 0.15f * sinf(t * 3.271f + 4.1f);
        };
        const float t = m_time * 0.05f;
        const float d = 0.18f * fminf(mr.drift, 1.0f);
        cx = fminf(fmaxf(cx + d * wander(t), 0.2f), 0.8f);
        cy = fminf(fmaxf(cy + d * wander(t * 0.83f + 11.0f), 0.2f), 0.8f);
    }

    if (mr.mode >= 4) {
        const float aspect = (float)m_width / (float)m_height;
        const float rot = (mr.rotatePeriod > 0.01f)
                        ? (6.2831853f * m_time / mr.rotatePeriod) : 0.0f;
        const float x = (u - cx) * aspect, y = v - cy;
        // same central-disc scaling as MirrorFold()
        const float r = sqrtf(x * x + y * y)
                      * (0.5f / sqrtf(0.25f * aspect * aspect + 0.25f));
        const float seg = 6.2831853f / fmaxf((float)mr.segments, 2.0f);
        const float a = atan2f(y, x) - rot;
        const float m2 = a - seg * 2.0f * floorf(a / (seg * 2.0f));
        const float ang = fabsf(m2 - seg) + rot;
        u = cx + cosf(ang) * r / aspect;
        v = cy + sinf(ang) * r;
    } else {
        auto axis = [](float uu, float c, float sgn, float& flip) {
            const float d = uu - c;
            const float M = fmaxf(fmaxf(c, 1.0f - c), 1e-4f);
            flip = sgn * ((d >= 0.0f) ? 1.0f : -1.0f) * (0.5f / M);
            return 0.5f + sgn * fminf(fabsf(d) / M, 1.0f) * 0.5f;
        };
        const float sgnX = (mr.source & 1) ? 1.0f : -1.0f;
        const float sgnY = (mr.source & 2) ? 1.0f : -1.0f;
        if (mr.mode == 1 || mr.mode == 3) u = axis(u, cx, sgnX, flipX);
        if (mr.mode == 2 || mr.mode == 3) v = axis(v, cy, sgnY, flipY);
    }
    px = u * (float)m_width;
    py = v * (float)m_height;
}

void FluidRenderer::CreateAnalyzerResources() {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = kAnaW;
    rd.Height = kAnaH;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    m_anaState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    HR(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, m_anaState,
                                         nullptr, IID_PPV_ARGS(&m_anaTex)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)kFrames * m_rtvStride;
    m_device->CreateRenderTargetView(m_anaTex.Get(), nullptr, rtv);

    m_anaPitch = (kAnaW * 8 + 255) & ~255u;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (UINT64)m_anaPitch * kAnaH;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES rb = {};
    rb.Type = D3D12_HEAP_TYPE_READBACK;
    HR(m_device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd,
                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                         IID_PPV_ARGS(&m_anaReadback)));
}

// Re-render the display pass at analyzer resolution and queue a readback.
// Runs at ~10 Hz; the result is the exact final scRGB output (post gamut,
// post peak expansion) â€” what the display receives.
void FluidRenderer::MaybeRenderAnalyzer() {
    if (m_anaPending || m_time - m_lastAnaTime < 0.1f) return;
    if (!m_anaTex) CreateAnalyzerResources();
    m_lastAnaTime = m_time;

    if (m_anaState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_anaTex.Get();
        b.Transition.StateBefore = m_anaState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_cmd->ResourceBarrier(1, &b);
        m_anaState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)kFrames * m_rtvStride;
    m_cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0, 0, (float)kAnaW, (float)kAnaH, 0, 1 };
    D3D12_RECT sc = { 0, 0, kAnaW, kAnaH };
    m_cmd->RSSetViewports(1, &vp);
    m_cmd->RSSetScissorRects(1, &sc);

    m_cmd->SetGraphicsRootSignature(m_graphicsRS.Get());
    m_cmd->SetPipelineState(DisplayPso());
    float consts[32];
    BuildDisplayConstants(consts);
    m_cmd->SetGraphicsRoot32BitConstants(0, 32, consts, 0);
    m_cmd->SetGraphicsRootDescriptorTable(1, m_dye.read->srv);
    BindAcid();
    BindInk();
    BindMirrorFold(kAnaW, kAnaH);
    m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmd->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = m_anaTex.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmd->ResourceBarrier(1, &b);
    m_anaState = D3D12_RESOURCE_STATE_COPY_SOURCE;

    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
    src.pResource = m_anaTex.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = m_anaReadback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    dst.PlacedFootprint.Footprint.Width = kAnaW;
    dst.PlacedFootprint.Footprint.Height = kAnaH;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = m_anaPitch;
    m_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    m_anaFence = m_nextFence;   // signaled by EndFrameAndPresent
    m_anaPending = true;
}

bool FluidRenderer::ReadAnalyzerFrame(std::vector<float>& out) {
    if (!m_anaPending || !m_anaReadback) return false;
    if (m_fence->GetCompletedValue() < m_anaFence) return false;

    uint8_t* data = nullptr;
    D3D12_RANGE range = { 0, (SIZE_T)m_anaPitch * kAnaH };
    if (FAILED(m_anaReadback->Map(0, &range, (void**)&data))) return false;
    out.resize((size_t)kAnaW * kAnaH * 4);
    for (int y = 0; y < kAnaH; y++) {
        const uint16_t* row = (const uint16_t*)(data + (SIZE_T)y * m_anaPitch);
        float* dst = out.data() + (size_t)y * kAnaW * 4;
        for (int i = 0; i < kAnaW * 4; i++)
            dst[i] = HalfToFloat(row[i]);
    }
    D3D12_RANGE none = { 0, 0 };
    m_anaReadback->Unmap(0, &none);
    m_anaPending = false;
    return true;
}

// ---------------------------------------------------------------------------
// ui3 B5: the Settings window's picture of "now" (fluid.h RequestSnapshot)
// ---------------------------------------------------------------------------

// Frame()'s end, only while a request is pending: ONE copy of the finished frame (the back
// buffer after the display and post passes, i.e. exactly what Present shows; headless: the
// offscreen target the --shot PNG reads) into a readback buffer, recorded in the frame's own
// command list. The copy only READS the frame; every state it changes is put back, so the
// presented image is the same with and without it. Read on the frame's fence by TakeSnapshot,
// as ReadAnalyzerFrame does: never a stall.
void FluidRenderer::RecordSnapshot() {
    if (m_snapInFlight) return;   // the last copy is not taken yet: this request waits for a later frame
    ID3D12Resource* src = m_headless ? m_shotTex.Get() : m_backBuffers[m_frameIndex].Get();
    if (!src) return;
    const D3D12_RESOURCE_DESC sd = src->GetDesc();
    if (sd.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || sd.Width != (UINT64)m_width || sd.Height != (UINT)m_height) {
        if (!m_snapFailLogged) WpLog("snapshot: the frame is not the expected FP16 %dx%d target, no picture", m_width, m_height);
        m_snapFailLogged = true;
        m_snapWanted = false;
        return;
    }
    // a resized frame (re-init at another size): the old buffer has no copy in flight, so it can go
    if (m_snapReadback && (m_snapW != m_width || m_snapH != m_height)) m_snapReadback.Reset();
    if (!m_snapReadback) {
        const UINT pitch = ((UINT)m_width * 8 + 255) & ~255u;   // as CreateOffscreenTarget's m_shotPitch
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = (UINT64)pitch * m_height;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES rb = {};
        rb.Type = D3D12_HEAP_TYPE_READBACK;
        // not HR(): a picture for the Settings window is never worth a fatal box; a lost device
        // is found by the frame path's own calls, which go through HR as always
        const HRESULT hr = m_device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&m_snapReadback));
        if (FAILED(hr) || !m_snapReadback) {
            if (!m_snapFailLogged) WpLog("snapshot: readback buffer refused hr=0x%08lX, no picture", (unsigned long)hr);
            m_snapFailLogged = true;
            m_snapReadback.Reset();
            m_snapWanted = false;   // the window asks again in 10 s
            return;
        }
        m_snapPitch = pitch;
        m_snapW = m_width;
        m_snapH = m_height;
    }

    // The state the frame left the target in: the swap chain's back buffer is PRESENT after
    // RenderDisplay's last barrier; the headless target stays in m_shotState (RENDER_TARGET).
    const D3D12_RESOURCE_STATES was = m_headless ? m_shotState : D3D12_RESOURCE_STATE_PRESENT;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = src;
    b.Transition.StateBefore = was;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (was != D3D12_RESOURCE_STATE_COPY_SOURCE) m_cmd->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION s = {}, dst = {};
    s.pResource = src;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = m_snapReadback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    dst.PlacedFootprint.Footprint.Width = (UINT)m_width;
    dst.PlacedFootprint.Footprint.Height = (UINT)m_height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = m_snapPitch;
    m_cmd->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);

    if (was != D3D12_RESOURCE_STATE_COPY_SOURCE) {   // back to exactly the state it was found in
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = was;
        m_cmd->ResourceBarrier(1, &b);
    }

    m_snapFence = m_nextFence;   // signaled by EndFrameAndPresent, right after this
    m_snapSdrScale = m_sdrScale;
    m_snapInFlight = true;
    m_snapWanted = false;
}

static inline float SnapClamp01(float v) { return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f; }   // NaN -> 0
static inline uint8_t SnapSrgb8(float c) {   // main.cpp LinearToSrgb + ToByte, the --shot PNG's encode
    c = SnapClamp01(c);
    const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
    const int v = (int)(s * 255.0f + 0.5f);
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

bool FluidRenderer::TakeSnapshot(std::vector<uint8_t>& rgba, int& w, int& h) {
    if (!m_snapInFlight || !m_snapReadback || !m_fence || m_deviceLost) return false;
    if (m_fence->GetCompletedValue() < m_snapFence) return false;
    LARGE_INTEGER q0, q1, qf;
    QueryPerformanceCounter(&q0);

    const int sw = m_snapW, sh = m_snapH;
    const int mw = kSnapMaxW, mh = kSnapMaxH;   // locals: std::min takes references
    const float k = fminf(1.0f, fminf((float)mw / (float)sw, (float)mh / (float)sh));
    const int dw = std::max(1, std::min(std::min(mw, sw), (int)lroundf((float)sw * k)));
    const int dh = std::max(1, std::min(std::min(mh, sh), (int)lroundf((float)sh * k)));

    uint8_t* data = nullptr;
    D3D12_RANGE range = { 0, (SIZE_T)m_snapPitch * sh };
    if (FAILED(m_snapReadback->Map(0, &range, (void**)&data))) {
        m_snapInFlight = false;   // a lost device: the rebuild starts clean; else the next request
        return false;
    }
    // every half value -> float once (64 K entries), so the 3.7 M px of a 1440p frame cost
    // a table read per channel instead of HalfToFloat's bit work
    static const std::vector<float> lut = [] {
        std::vector<float> t(65536);
        for (uint32_t i = 0; i < 65536; i++) t[i] = HalfToFloat((uint16_t)i);
        return t;
    }();
    // display-referred as the --shot SDR PNG: SDR white -> 1.0 (HDR on: / the SDR white scale;
    // HDR off: the scale is 1), negatives and NaN -> 0, clip at 1; then a box average in linear
    // light over each output pixel's footprint, then the sRGB encode
    const float inv = 1.0f / fmaxf(m_snapSdrScale, 1e-4f);
    std::vector<int> xs((size_t)dw + 1);
    for (int i = 0; i <= dw; i++) xs[i] = (int)((long long)i * sw / dw);
    std::vector<float> acc((size_t)dw * 3);
    rgba.resize((size_t)dw * dh * 4);
    for (int oy = 0; oy < dh; oy++) {
        const int y0 = (int)((long long)oy * sh / dh), y1 = (int)((long long)(oy + 1) * sh / dh);
        std::fill(acc.begin(), acc.end(), 0.0f);
        for (int y = y0; y < y1; y++) {
            const uint16_t* row = (const uint16_t*)(data + (SIZE_T)y * m_snapPitch);
            for (int ox = 0; ox < dw; ox++) {
                float r = 0, g = 0, bl = 0;
                for (int x = xs[ox]; x < xs[ox + 1]; x++) {
                    const uint16_t* p = row + (size_t)x * 4;
                    r  += SnapClamp01(lut[p[0]] * inv);
                    g  += SnapClamp01(lut[p[1]] * inv);
                    bl += SnapClamp01(lut[p[2]] * inv);
                }
                float* a = acc.data() + (size_t)ox * 3;
                a[0] += r; a[1] += g; a[2] += bl;
            }
        }
        uint8_t* o = rgba.data() + (size_t)oy * dw * 4;
        for (int ox = 0; ox < dw; ox++) {
            const float n = (float)((y1 - y0) * (xs[ox + 1] - xs[ox]));
            const float in = n > 0.0f ? 1.0f / n : 0.0f;
            const float* a = acc.data() + (size_t)ox * 3;
            o[ox * 4 + 0] = SnapSrgb8(a[0] * in);
            o[ox * 4 + 1] = SnapSrgb8(a[1] * in);
            o[ox * 4 + 2] = SnapSrgb8(a[2] * in);
            o[ox * 4 + 3] = 255;
        }
    }
    D3D12_RANGE none = { 0, 0 };
    m_snapReadback->Unmap(0, &none);
    m_snapInFlight = false;
    w = dw;
    h = dh;
    QueryPerformanceCounter(&q1);
    QueryPerformanceFrequency(&qf);
    m_snapCpuMs = (float)(1000.0 * (double)(q1.QuadPart - q0.QuadPart) / (double)qf.QuadPart);
    // ui3: the conversion's CPU cost in the app log, the first time and whenever it is over 8 ms (a hitch to find later)
    static bool s_snapCpuLogged = false;
    if (!s_snapCpuLogged || m_snapCpuMs > 8.0f) {
        s_snapCpuLogged = true;
        WpLog("[snapshot] cpu %.1f ms (%dx%d -> %dx%d)", m_snapCpuMs, sw, sh, dw, dh);
    }
    return true;
}

// The Settings window closed: the readback buffer goes (a copy still on the GPU is waited
// for first, at most the frame in flight). The next request makes a new one.
void FluidRenderer::ReleaseSnapshot() {
    m_snapWanted = false;
    if (m_snapInFlight && m_snapReadback && m_fence && !m_deviceLost &&
        m_fence->GetCompletedValue() < m_snapFence)
        WaitForGpuIdle();
    m_snapInFlight = false;
    m_snapReadback.Reset();
    m_snapPitch = 0;
    m_snapW = m_snapH = 0;
}

// ---------------------------------------------------------------------------
// Second-monitor mirror: same dye field, own swapchain + HDR mapping
// ---------------------------------------------------------------------------

void FluidRenderer::EnableMirror(HWND hwnd, int width, int height) {
    DisableMirror();
    if (m_deviceLost) return;   // R3: the rebuilt device brings the mirror back
    m_mirrorW = width;
    m_mirrorH = height;

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = width;
    sd.Height = height;
    sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kFrames;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(m_factory->CreateSwapChainForHwnd(m_queue.Get(), hwnd, &sd,
                                                 nullptr, nullptr, &sc1)) ||
        FAILED(sc1.As(&m_mirrorChain))) {
        printf("mirror: swapchain creation failed\n");
        m_mirrorChain.Reset();
        return;
    }
    const DXGI_COLOR_SPACE_TYPE scRGB = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    UINT support = 0;
    if (SUCCEEDED(m_mirrorChain->CheckColorSpaceSupport(scRGB, &support)) &&
        (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
        m_mirrorChain->SetColorSpace1(scRGB);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)(kFrames + 1) * m_rtvStride;
    for (UINT i = 0; i < kFrames; i++) {
        HR(m_mirrorChain->GetBuffer(i, IID_PPV_ARGS(&m_mirrorBuffers[i])));
        if (m_deviceLost) {   // R3: no half-built mirror on a dead device
            for (UINT k = 0; k < kFrames; k++) m_mirrorBuffers[k].Reset();
            m_mirrorChain.Reset();
            return;
        }
        m_device->CreateRenderTargetView(m_mirrorBuffers[i].Get(), nullptr, rtv);
        rtv.ptr += m_rtvStride;
    }
    m_mirrorBroken = false;
    printf("mirror enabled: %dx%d\n", width, height);
}

void FluidRenderer::DisableMirror() {
    if (!m_mirrorChain) return;
    WaitForGpuIdle();
    for (UINT i = 0; i < kFrames; i++) m_mirrorBuffers[i].Reset();
    m_mirrorChain.Reset();
    m_mirrorBroken = false;
    printf("mirror disabled\n");
}

void FluidRenderer::RenderMirror() {
    UINT bi = m_mirrorChain->GetCurrentBackBufferIndex();

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = m_mirrorBuffers[bi].Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmd->ResourceBarrier(1, &b);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)(kFrames + 1 + bi) * m_rtvStride;
    m_cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0, 0, (float)m_mirrorW, (float)m_mirrorH, 0, 1 };
    D3D12_RECT sc = { 0, 0, m_mirrorW, m_mirrorH };
    m_cmd->RSSetViewports(1, &vp);
    m_cmd->RSSetScissorRects(1, &sc);

    if (m_photoMode) {                     // brief BY: the photo, fitted to this monitor
        DrawPhoto(m_mirrorW, m_mirrorH, m_mirrorSdrScale);
    } else {
    m_cmd->SetGraphicsRootSignature(m_graphicsRS.Get());
    m_cmd->SetPipelineState(DisplayPso());
    float consts[32];
    BuildDisplayConstantsEx(consts, m_mirrorW, m_mirrorH, m_mirrorSdrScale, m_mirrorPeakNits,
                            m_mirrorHdrActive, m_mirrorPrimValid ? m_mirrorPrim : nullptr,
                            m_mirrorSdrManaged);
    m_cmd->SetGraphicsRoot32BitConstants(0, 32, consts, 0);
    m_cmd->SetGraphicsRootDescriptorTable(1, m_dye.read->srv);
    BindAcid();
    BindInk();
    BindMirrorFold(m_mirrorW, m_mirrorH);
    m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmd->DrawInstanced(3, 1, 0, 0);
    }

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_cmd->ResourceBarrier(1, &b);
}

void FluidRenderer::RenderGradient(float timeSec) {
    const UINT i = m_frameIndex;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = m_backBuffers[i].Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmd->ResourceBarrier(1, &b);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)i * m_rtvStride;
    m_cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0, 0, (float)m_width, (float)m_height, 0, 1 };
    D3D12_RECT sc = { 0, 0, m_width, m_height };
    m_cmd->RSSetViewports(1, &vp);
    m_cmd->RSSetScissorRects(1, &sc);

    m_cmd->SetGraphicsRootSignature(m_graphicsRS.Get());
    m_cmd->SetPipelineState(m_psoGradient.Get());
    float consts[8] = { (float)m_width, (float)m_height, timeSec,
                        m_hdrActive ? 1.0f : 0.0f, (float)m_cfg.calibratePage, 0, 0, 0 };
    m_cmd->SetGraphicsRoot32BitConstants(0, 8, consts, 0);
    m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmd->DrawInstanced(3, 1, 0, 0);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_cmd->ResourceBarrier(1, &b);
}

// One black frame, presented and then left on screen. Manual pause used to
// stop calling Frame(), which leaves the LAST rendered frame sitting on the
// panel -- a static image burning on an OLED for as long as the user is away.
// Clearing the back buffer and presenting once costs a single frame and the
// panel then shows black until the pause is lifted. Nothing about the sim is
// touched: m_time, the dye and the blob population all resume untouched.
void FluidRenderer::PresentBlack() {
    if (m_headless || !m_swapChain || m_presentBroken) return;
    if (!BeginFrame()) return;   // device lost (R3)
    const UINT i = m_frameIndex;
    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = m_backBuffers[i].Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmd->ResourceBarrier(1, &b);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)i * m_rtvStride;
    m_cmd->ClearRenderTargetView(rtv, black, 0, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_cmd->ResourceBarrier(1, &b);
    // the second monitor, if it is mirroring us, goes dark with the first
    if (m_mirrorChain) {
        const UINT mi = m_mirrorChain->GetCurrentBackBufferIndex();
        D3D12_RESOURCE_BARRIER mb = b;
        mb.Transition.pResource = m_mirrorBuffers[mi].Get();
        mb.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        mb.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        m_cmd->ResourceBarrier(1, &mb);
        D3D12_CPU_DESCRIPTOR_HANDLE mrtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        mrtv.ptr += (SIZE_T)(kFrames + 1 + mi) * m_rtvStride;
        m_cmd->ClearRenderTargetView(mrtv, black, 0, nullptr);
        mb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        mb.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        m_cmd->ResourceBarrier(1, &mb);
    }
    EndFrameAndPresent();
}

void FluidRenderer::ReassertColorSpace() {
    if (!m_swapChain) return;
    const DXGI_COLOR_SPACE_TYPE scRGB = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    UINT support = 0;
    if (SUCCEEDED(m_swapChain->CheckColorSpaceSupport(scRGB, &support)) &&
        (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
        m_swapChain->SetColorSpace1(scRGB);
}

void FluidRenderer::Frame(float dt, float sdrScale, bool hdrActive, const FrameInput& input) {
    m_hdrActive = hdrActive;
    m_sdrScale = sdrScale;
    AdvanceClock(dt);
    AccumFrozen(dt);   // animators.h freezes; no-op unless something is frozen

    SyncDropLife();                                // droplet_shadow / droplet_wobble (no-op unless they toggle)
    SyncPostBlackSoft();                           // [post] mass_black_soft (no-op unless it toggles)
    if (OilHeightWanted()) EnsureOilHeightPso();   // Oil3D Stage A (no-op once built)
    if (MassCovWanted()) EnsureMassCovPso();       // Oil3D C2 (no-op once built)
    if (m_cfg.acid.enabled && m_cfg.acid.LampPathWanted())
        EnsureLampPso();                                                   // Oil3D L1 / L2 / L3
    if (MurkWanted() || MotesWanted() || WaterTintWanted())
        EnsureMurk();                                      // Oil3D W1 / C3 / W2 (bakes only when a murk key changed)
    if (!BeginFrame()) return;   // device lost: the shell rebuilds the renderer (R3)
    if (m_murkUploadPending) RecordMurkUpload();   // Oil3D W1: before any draw reads t8
    TimestampBegin();            // --timings only; nothing recorded otherwise

    if (m_cfg.gradientMode || m_cfg.calibratePage > 0) {
        RenderGradient(m_time);
        EndFrameAndPresent();
        return;
    }

    // brief BY photo stage: retired photo objects go once their fence is
    // done; in photo mode a decoded photo is uploaded in this (black) frame's
    // list and the sim is NOT stepped (pre-flight 6). All of it is skipped
    // without a photo stage: the recorded commands are today's.
    if (!m_photoRetire.empty()) ReleaseRetiredPhotos();
    if (m_photoMode) {
        PhotoPump();
        if (m_photoHavePending && m_photoAccept) RecordPhotoUpload();
    } else {
        FrameSim(dt, input);
    }

    if (m_blackOut || (m_photoMode && (!m_photoShown || !m_psoPhoto))) {
        ClearTargetBlack();
    } else {
        if (m_headless) RenderDisplayOffscreen();
        else            RenderDisplay();
        if (m_mirrorChain && !m_mirrorBroken) RenderMirror();
        if (m_anaEnabled && !m_photoMode) MaybeRenderAnalyzer();
        if (m_snapWanted) RecordSnapshot();   // ui3 B5: records nothing unless the Settings window asked
    }
    EndFrameAndPresent();

    if (m_readbackPending) {
        WaitForGpuIdle();
        ReportStats();
        m_readbackPending = false;
    }
}

// The sim half of Frame(), recorded into the open command list. Split out
// verbatim (same calls, same order) so the director's warm-up can run it
// without a display pass; Frame() itself records exactly what it did before.
void FluidRenderer::FrameSim(float dt, const FrameInput& input) {
    if (m_firstFrame) {
        // zero all sim textures, then the reference's startup burst
        SimCB cb = {};
        m_cmd->SetComputeRootSignature(m_computeRS.Get());
        auto clear = [&](ID3D12PipelineState* pso, Tex& t) {
            Transition(t, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_cmd->SetPipelineState(pso);
            cb.dimsW = t.w; cb.dimsH = t.h;
            m_cmd->SetComputeRoot32BitConstants(0, sizeof(SimCB) / 4, &cb, 0);
            m_cmd->SetComputeRootDescriptorTable(1, t.srv);
            m_cmd->SetComputeRootDescriptorTable(2, t.srv);
            m_cmd->SetComputeRootDescriptorTable(3, t.uav);
            m_cmd->SetComputeRootDescriptorTable(4, t.uav);
            m_cmd->SetComputeRootDescriptorTable(5, t.uav);
            m_cmd->Dispatch(Groups(t.w), Groups(t.h), 1);
            UavBarrier(t.res.Get());
        };
        clear(m_psoClearV.Get(), m_velocity.a);
        clear(m_psoClearV.Get(), m_velocity.b);
        clear(m_psoClear4.Get(), m_dye.a);
        clear(m_psoClear4.Get(), m_dye.b);
        clear(m_psoClear1.Get(), m_pressure.a);
        clear(m_psoClear1.Get(), m_pressure.b);
        clear(m_psoClear1.Get(), m_divergence);
        clear(m_psoClear1.Get(), m_curl);
        // The reference's startup burst. Gated on idle_splats: a look whose
        // whole premise is CLEAR water (style=ink) cannot open with a
        // screenful of dye that then takes a minute of decay to clear. Looks
        // that keep idle splats on (every parity/WE config) are unchanged,
        // RandF() consumption included.
        if (m_cfg.idleSplats) MultipleSplats((int)(RandF() * 20) + 3);
        m_firstFrame = false;
    }
    // The director's WE entry burst (QueueSplatBurst, brief FINAL-CYCLE B.1):
    // after the clear above, in the last black warm-up frame. 0 = nothing
    // pending, which is every cycle-off frame.
    if (m_entrySplats > 0) {
        const int n = m_entrySplats;
        m_entrySplats = 0;
        MultipleSplats(n);
        m_idleTimer = 0.0f;         // the regular idle burst does not pile on
    }

    // global color wheel
    m_globalHue = fmodf(m_globalHue + dt / fmaxf(1.0f, m_cfg.colorCyclePeriod), 1.0f);

    // continuous emitters (wanderers/dart/mouse) fire once per FRAME; scale
    // their dye by dt so a 144 fps loop doesn't paint 2.4x hotter than 60 fps
    m_emitScale = fminf(fmaxf(dt * 60.0f, 0.25f), 2.0f);

    // reference update() order: coverage, wanderers, dart, hue shift, input
    UpdateCoverage();
    if (m_cfg.wanderers && (m_time - m_lastInteraction > m_cfg.wandererResumeDelay))
        UpdateWanderers(dt);
    UpdateDart(dt);
    if (!m_animFrozen[3]) UpdateHueShift(dt);   // ANIM_HUE_SHIFT freeze holds the angle
    HandleInput(input);

    // ink drops (style-agnostic emitter; inert unless [drops] drops=1 and no
    // tail is still being painted)
    UpdateDrops(dt);

    // idle random splats (project.json: every 9.6 s, 8 splats)
    if (m_cfg.idleSplats) {
        m_idleTimer += dt;
        if (m_idleTimer >= m_cfg.idleInterval) {
            m_idleTimer = 0;
            MultipleSplats(m_cfg.idleAmount);
        }
    }

    // fixed-substep integration like the reference update loop
    float remaining = fminf(dt, 0.05f);
    const float stepSize = 0.016f;
    TimestampMark(6);   // --timings: the SimStep dispatches alone (6..7)
    while (remaining > stepSize) {
        SimStep(stepSize);
        remaining -= stepSize;
    }
    if (remaining > 0.0001f) SimStep(remaining);
    TimestampMark(7);

    if (m_cfg.stats) {
        m_statsTimer += dt;
        if (m_statsTimer >= 5.0f && !m_readbackPending) {
            m_statsTimer = 0;
            Transition(*m_dye.read, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = m_dye.read->res.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = m_readback.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            dst.PlacedFootprint.Footprint.Width = m_dyeW;
            dst.PlacedFootprint.Footprint.Height = m_dyeH;
            dst.PlacedFootprint.Footprint.Depth = 1;
            dst.PlacedFootprint.Footprint.RowPitch = m_readbackPitch;
            m_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            m_readbackPending = true;
        }
    }

    // The camera rig -- lamp, optical axis, focus plane -- moves whether or
    // not the acid look is on: [post] fog and bloom hang off the lamp and any
    // style can name them.
    StepCameraRig(m_animFrozen[2] ? 0.0f : dt);   // ANIM_RIG freeze: no motion at all

    // Liquid Acid oil layer: advect the blobs with the fluid we just stepped
    // (one frame of readback latency), then hand them to the display pass.
    if (m_cfg.acid.enabled) {
        // Re-seed when the population size changes from the settings window;
        // the per-kind fractions and radii are read at seed time, so moving
        // the count slider is also how you apply those live.
        int want = m_cfg.acid.blobCount;
        want = want < 1 ? 1 : (want > kAcidMaxBlobs ? kAcidMaxBlobs : want);
        if (!m_acidSeeded) {
            SeedAcidBlobs();
        } else if (m_cfg.acid.conserveMass > 0.5f) {
            // Walk to the new count instead of replacing the field. Called
            // every frame the count differs from the request: it is idempotent
            // (it counts blobs that are not already retiring) and costs one
            // pass over 96 blobs, so the surplus keeps shrinking and the
            // shortfall keeps growing until the two agree.
            if (m_acidWantBlobs != want) m_acidWantBlobs = want;
            // brief BZ blob_turnover_s: retire ONE blob per period; the walk
            // below then grows ONE through the unchanged grow-in path. The
            // guard is the whole of the off-state: at 0 nothing is drawn,
            // nothing is read and the population is today's bit for bit.
            if (m_cfg.acid.blobTurnoverS > 0.0f) StepAcidTurnover(dt, want);
            int live = 0;
            for (size_t bi = 0; bi < m_acidBlobs.size(); bi++)
                if (m_acidBlobs[bi].rTarget > 0.0f) live++;
            if (live != want) AdjustAcidBlobCount(want);
        } else if ((int)m_acidBlobs.size() != want) {
            SeedAcidBlobs();
        }
        UpdateVelocityReadback();
        StepAcidBlobs(dt);
        StepAcidDroplets(dt);
        StepMotes(dt);         // Oil3D C3 (nothing while motes is 0)
        StepHueField(dt);      // brief AE: the second dye hue's patch field
        UploadAcidConstants();
        // Oil as an obstacle to the ink: needs the blob buffer this frame's
        // display draw will read, so it runs after the upload. Returns at once
        // (and compiles nothing) while oil_drag and oil_dye_block are both 0.
        TimestampMark(8);   // --timings: StepOilDrag alone (8..9)
        StepOilDrag(dt);
        TimestampMark(9);
    } else {
        TimestampMark(8); TimestampMark(9);
    }
    // Shared ink-in-water constants: needed by style=ink and by the acid look
    // when ink_mode=water. 112 bytes; skipped entirely for style=fluid.
    if (m_cfg.ink.enabled || m_cfg.acid.enabled) {
        UploadInkConstants();
        // The ink's motion gate samples the low-res velocity texture. The acid
        // path already refreshes it for the blob advection; style=ink has to
        // ask for it (one 64x36 dispatch).
        if (!m_cfg.acid.enabled && m_cfg.ink.motionHi > m_cfg.ink.motionLo)
            UpdateVelocityReadback();
    }
}

void FluidRenderer::SimOnlyStep(float dt) {
    if (!m_device || m_cfg.gradientMode || m_cfg.calibratePage > 0) return;
    if (m_photoMode) return;                // brief BY: no sim under a photo
    AdvanceClock(dt);
    AccumFrozen(dt);
    if (!BeginFrame()) return;   // device lost (R3)
    FrameSim(dt, FrameInput{});
    if (m_headless) EndFrameAndPresent();   // headless: fence + ring, no Present
    else            EndFrameNoPresent();
    if (m_readbackPending) {
        WaitForGpuIdle();
        ReportStats();
        m_readbackPending = false;
    }
}

// SetBlackOut(true): the frame's target is cleared to black instead of drawn.
// Same barriers as PresentBlack() (swap chain + second-monitor mirror); the
// headless shot texture is cleared in place so a capture reads true zero.
void FluidRenderer::ClearTargetBlack() {
    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (m_headless) {
        if (!m_shotTex) return;
        if (m_shotState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = m_shotTex.Get();
            b.Transition.StateBefore = m_shotState;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            m_cmd->ResourceBarrier(1, &b);
            m_shotState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        m_cmd->ClearRenderTargetView(rtv, black, 0, nullptr);
        return;
    }
    const UINT i = m_frameIndex;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = m_backBuffers[i].Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmd->ResourceBarrier(1, &b);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)i * m_rtvStride;
    m_cmd->ClearRenderTargetView(rtv, black, 0, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_cmd->ResourceBarrier(1, &b);
    if (m_mirrorChain && !m_mirrorBroken) {
        const UINT mi = m_mirrorChain->GetCurrentBackBufferIndex();
        D3D12_RESOURCE_BARRIER mb = b;
        mb.Transition.pResource = m_mirrorBuffers[mi].Get();
        mb.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        mb.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        m_cmd->ResourceBarrier(1, &mb);
        D3D12_CPU_DESCRIPTOR_HANDLE mrtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        mrtv.ptr += (SIZE_T)(kFrames + 1 + mi) * m_rtvStride;
        m_cmd->ClearRenderTargetView(mrtv, black, 0, nullptr);
        mb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        mb.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        m_cmd->ResourceBarrier(1, &mb);
    }
}

void FluidRenderer::ResetLookState() {
    m_firstFrame = true;            // clear every sim texture + startup burst
    m_idleTimer = 0.0f;
    m_dart.active = false;
    m_lastDartTime = m_time;        // no dart the instant the new look opens
    m_lastInteraction = -1000.0f;
    // liquid_acid: population, droplets, hue2 mix field -- all reseed lazily
    // on the next frame under the NEW stage's keys (the same flags Shutdown()
    // resets for a resume)
    m_acidBlobs.clear();
    m_acidSeeded = false;
    m_acidTurnAcc = 0.0f;           // brief BZ: turnover restarts with the population
    m_acidWantBlobs = -1;
    m_acidDrops.clear();
    m_dropletOrder.clear();
    m_dropletSeededFor = -1;
    m_dropletSpawnAcc = 0.0f;
    m_mixSeeded = false;
    m_coverSeeded = false;
    // the 64x36 velocity copy the blobs advect with is of the OLD field
    for (float& v : m_velCpu) v = 0.0f;
    // [drops] emitter: re-prime its first interval, drop any pending tail
    m_dropTimer = 0.0f;
    m_dropPrimed = false;
    m_dropTailLeft = 0.0f;
    m_dropQueued = false;
    InitWanderers();
    printf("[cycle] look state reset (sim cleared, acid/droplets/mix reseed next frame)\n");
}

double FluidRenderer::PrecompilePostPsos() {
    if (!m_device || m_psoPostBN || !PostActive()) return 0.0;
    LARGE_INTEGER qf, q0, q1;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&q0);
    const D3D_SHADER_MACRO defs[] = { { "BN_OPTICS", "1" }, { nullptr, nullptr } };
    MakeGraphicsPso(kPostSrc, m_psoPostBN, defs);
    QueryPerformanceCounter(&q1);
    const double ms = 1000.0 * (double)(q1.QuadPart - q0.QuadPart) / (double)qf.QuadPart;
    printf("post: BN_OPTICS PSO precompiled at the cycle black point (%.0f ms)\n", ms);
    return ms;
}

// ===========================================================================
// brief BY PHOTO-STAGE (phase 1): the fourth display PSO. See fluid.h for the
// call contract and photo.h for the worker; cycle.cpp decides when.
// ===========================================================================
static double PhotoNowMs() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return 1000.0 * (double)t.QuadPart / (double)f.QuadPart;
}

// kPhotoSrc, compiled lazily at the first photo black point, off the GPU
// timeline the same way EnsureLookResources does (pre-flight 9).
void FluidRenderer::EnsurePhotoPso() {
    if (m_psoPhoto || !m_device) return;
    const double t0 = PhotoNowMs();
    WaitForGpuIdle();
    MakeGraphicsPso(kPhotoSrc, m_psoPhoto);
    PhotoLog("[photo] kPhotoSrc PSO compiled on demand (%.0f ms)\n", PhotoNowMs() - t0);
}

void FluidRenderer::RetirePhotoTex() {
    if (!m_photoTex) return;
    // m_nextFence is signalled at the end of the NEXT submitted frame, so it
    // is at or past the last frame that drew this texture
    m_photoRetire.push_back({ m_photoTex, m_nextFence, false, PhotoNowMs() });
    m_photoTex.Reset();
    m_photoShown = false;
}

void FluidRenderer::ReleaseRetiredPhotos() {
    if (!m_fence) return;
    const UINT64 done = m_fence->GetCompletedValue();
    for (size_t k = 0; k < m_photoRetire.size();) {
        if (done >= m_photoRetire[k].fence) {
            if (m_photoRetire[k].upload)
                PhotoLog("[photo] upload done: the GPU copy has completed, upload buffer released "
                         "%.1f ms after the record\n", PhotoNowMs() - m_photoRetire[k].t0);
            m_photoRetire.erase(m_photoRetire.begin() + k);
        } else {
            k++;
        }
    }
}

void FluidRenderer::PhotoRequest(const std::wstring& path, bool fill) {
    if (m_photoJob) { PhotoWorkerCancel(m_photoJob); m_photoJob = 0; }
    m_photoPending = PhotoResult{};            // never on the GPU: just drop it
    m_photoHavePending = false;
    m_photoFailed = false;
    m_photoWhy.clear();
    m_photoReqPath = path;
    m_photoReqFill = fill;
    m_photoAccept = false;                     // the shown photo stays until PhotoEnter
    if (m_device) m_photoJob = PhotoWorkerSubmit(path, m_device.Get(), m_width, m_height, fill);
}

void FluidRenderer::PhotoCancel() {
    if (m_photoJob) { PhotoWorkerCancel(m_photoJob); m_photoJob = 0; }
    m_photoPending = PhotoResult{};
    m_photoHavePending = false;
    m_photoFailed = false;
    m_photoReqPath = m_photoShown ? m_photoShowPath : std::wstring();
}

void FluidRenderer::PhotoEnter() {
    EnsurePhotoPso();
    RetirePhotoTex();                          // the previous photo, if any
    m_photoMode = true;
    m_photoAccept = true;
}

void FluidRenderer::PhotoLeave() {
    if (!m_photoMode && !m_photoJob && !m_photoHavePending && !m_photoTex) return;
    if (m_photoJob) { PhotoWorkerCancel(m_photoJob); m_photoJob = 0; }
    m_photoPending = PhotoResult{};
    m_photoHavePending = false;
    RetirePhotoTex();
    m_photoMode = false;
    m_photoAccept = false;
    m_photoFailed = false;
    m_photoReqPath.clear();
}

// Take a finished job; after a device re-init (fullscreen suspend) re-send
// the request, so the photo comes back without the director noticing.
void FluidRenderer::PhotoPump() {
    if (m_photoJob) {
        PhotoResult r;
        if (PhotoWorkerTake(m_photoJob, r)) {
            m_photoJob = 0;
            if (r.ok) {
                m_photoPending = std::move(r);
                m_photoHavePending = true;
            } else {
                m_photoFailed = true;
                m_photoWhy = r.why;
            }
        }
    } else if (m_photoMode && m_photoAccept && !m_photoShown && !m_photoHavePending && !m_photoFailed &&
               !m_photoReqPath.empty() && m_device) {
        PhotoLog("[photo] re-requesting %ls (renderer re-initialised)\n", PhotoNameOf(m_photoReqPath));
        m_photoJob = PhotoWorkerSubmit(m_photoReqPath, m_device.Get(), m_width, m_height, m_photoReqFill);
    }
}

int FluidRenderer::PhotoStatus() {
    PhotoPump();
    if (m_photoFailed) return PHOTO_FAILED;
    if (m_photoShown) return PHOTO_SHOWN;
    if (m_photoJob || m_photoHavePending) return PHOTO_LOADING;
    return PHOTO_NONE;
}

double FluidRenderer::PhotoJoin() {
    return m_photoJob ? PhotoWorkerWait(m_photoJob) : 0.0;
}

// Pre-flight 20: the worker made the texture (COPY_DEST) and filled the
// upload buffer; here, in the black frame's list on m_cmd/m_queue: the copy,
// the barrier to PIXEL_SHADER_RESOURCE and the SRV into the heap slot NOT
// being drawn (ping-pong 22 / 24). The upload buffer is released once this
// frame's fence (m_nextFence, as the analyzer does) completes.
void FluidRenderer::RecordPhotoUpload() {
    const double t0 = PhotoNowMs();
    RetirePhotoTex();
    PhotoResult& r = m_photoPending;
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = r.tex.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    src.pResource = r.upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = r.fp;
    m_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r.tex.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmd->ResourceBarrier(1, &b);

    m_photoSlot ^= 1;
    const UINT srvSlot = (UINT)(11 + m_photoSlot) * 2;     // heap 22 or 24
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = r.fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE sc = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    sc.ptr += (SIZE_T)srvSlot * m_srvStride;
    m_device->CreateShaderResourceView(r.tex.Get(), &sv, sc);
    m_photoSrv = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    m_photoSrv.ptr += (SIZE_T)srvSlot * m_srvStride;

    m_photoTex = r.tex;
    m_photoTexW = r.texW;
    m_photoTexH = r.texH;
    m_photoFill = r.fill;
    m_photoRetire.push_back({ r.upload, m_nextFence, true, t0 });
    m_photoInfo = r;
    m_photoInfo.tex.Reset();
    m_photoInfo.upload.Reset();
    m_photoShowPath = r.path;
    m_photoReqPath = r.path;
    m_photoShown = true;
    m_photoPending = PhotoResult{};
    m_photoHavePending = false;
    m_photoRecordMs = PhotoNowMs() - t0;
}

// The photo on the bound target (w x h): its fit (or cover) rect, 1:1 on the
// output it was decoded for. b0 = {x0, y0, 1/rw, 1/rh | sdrScale*fade, alpha,
// exact}; t0 = its SRV through param 1; params 2-8 untouched (pre-flight 10).
void FluidRenderer::DrawPhoto(int w, int h, float sdrScale) {
    const int tw = m_photoTexW > 0 ? m_photoTexW : 1, th = m_photoTexH > 0 ? m_photoTexH : 1;
    const double sx = (double)w / tw, sy = (double)h / th;
    const double s = m_photoFill ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
    int rw = (int)llround(tw * s), rh = (int)llround(th * s);
    if (!m_photoFill) { if (rw > w) rw = w; if (rh > h) rh = h; }
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    const int x0 = (int)floor((w - rw) * 0.5), y0 = (int)floor((h - rh) * 0.5);
    float c[32] = {};
    c[0] = (float)x0;
    c[1] = (float)y0;
    c[2] = 1.0f / (float)rw;
    c[3] = 1.0f / (float)rh;
    c[4] = sdrScale * m_fade;                  // the same product as the display pass
    c[5] = m_photoInfo.alpha ? 1.0f : 0.0f;
    c[6] = (rw == tw && rh == th) ? 1.0f : 0.0f;   // 1:1: Load, no filtering
    m_cmd->SetGraphicsRootSignature(m_graphicsRS.Get());
    m_cmd->SetPipelineState(m_psoPhoto.Get());
    m_cmd->SetGraphicsRoot32BitConstants(0, 32, c, 0);
    m_cmd->SetGraphicsRootDescriptorTable(1, m_photoSrv);
    m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmd->DrawInstanced(3, 1, 0, 0);
}

int FluidRenderer::AcidBlobCount() const {
    int live = 0;
    for (const AcidBlob& b : m_acidBlobs) if (b.rTarget > 0.0f) live++;
    return live;
}

// animators.h live-value getters: the same formulas the constant upload uses,
// read-only (they cannot change a frame).
static void RgbToHsv(const float c[3], float& h, float& sv, float& v);   // below
static void BuildAnchorCdf(float w, float cdf[257]);                     // below
float FluidRenderer::PaletteHueDeg() const {
    const LiquidAcidConfig& a = m_cfg.acid;
    if (!(a.hueRotatePeriod > 0.01f)) return 0.0f;
    const float u = fmodf(AnimatorTime(0) / a.hueRotatePeriod, 1.0f);
    // brief BV: the warped clock (exact with the sweep off, which every Scheme
    // preset sets; with the sweep on the upload warps from the swept hue).
    if (a.hueAnchorWeight > 0.0005f) {
        float h0, s0, v0;
        RgbToHsv(&a.oilColors[0], h0, s0, v0);
        return AnchorWarpDeg(u, h0 * 360.0f, a.hueAnchorWeight);
    }
    return 360.0f * u;
}

float FluidRenderer::PaletteSweepPos() const {
    const LiquidAcidConfig& a = m_cfg.acid;
    if (!(a.hueSweepPeriod > 0.01f)) return -1.0f;
    int np = a.sweepCount;
    if (np < 1) np = 1;
    if (np > LiquidAcidConfig::kSweepMax) np = LiquidAcidConfig::kSweepMax;
    return fmodf(AnimatorTime(0) / a.hueSweepPeriod, 1.0f) * np;
}

float FluidRenderer::Hue2Deg() const {
    const LiquidAcidConfig& a = m_cfg.acid;
    float h = a.filmHue2;
    if (fabsf(a.filmHue2Wobble) > 0.001f) {
        const float P = fmaxf(a.filmHue2WobbleP, 5.0f);
        const float TAU = 6.2831853f;
        const float th = AnimatorTime(1);
        h += a.filmHue2Wobble * (0.62f * sinf(TAU * th / P + m_mixPhase)
                               + 0.38f * sinf(TAU * th / (P * 0.61803399f)
                                              + 1.7f + m_mixPhase * 0.7f));
    }
    return h;
}

// ---- AnimatorKick (animators.h, brief FINAL-CYCLE B.2) ----------------------
// The kicked clock reads AnimatorTime(a) = m_time - m_animFrozenAccum[a]; a
// kick LOWERS the accumulator by the smoothstepped share of clockDeltaSec, so
// the clock runs ahead and then keeps the offset (the drift resumes from the
// landing). Only the director calls this; with the cycle off m_kickOn stays
// false and the accumulators stay exactly 0.0f.
void FluidRenderer::AnimatorKick(int a, float clockDeltaSec, float overSec) {
    if (a < 0 || a >= kAnimClocks) return;
    if (m_kickDur[a] > 0.0f)                      // land a running kick first
        m_animFrozenAccum[a] -= m_kickTotal[a] - m_kickDone[a];
    m_kickTotal[a] = clockDeltaSec;
    m_kickDone[a] = 0.0f;
    m_kickT[a] = 0.0f;
    m_kickDur[a] = fmaxf(overSec, 0.05f);
    m_kickOn = true;
}

void FluidRenderer::StepKicks(float dt) {
    bool any = false;
    for (int a = 0; a < kAnimClocks; a++) {
        if (!(m_kickDur[a] > 0.0f)) continue;
        m_kickT[a] += dt;
        float x = fminf(m_kickT[a] / m_kickDur[a], 1.0f);
        x = x * x * (3.0f - 2.0f * x);
        const float want = m_kickTotal[a] * x;
        m_animFrozenAccum[a] -= want - m_kickDone[a];
        m_kickDone[a] = want;
        if (m_kickT[a] >= m_kickDur[a]) m_kickDur[a] = 0.0f;   // landed
        else any = true;
    }
    m_kickOn = any;
}

float FluidRenderer::PaletteBaseHueDeg() const {
    float h0, s0, v0;
    RgbToHsv(&m_cfg.acid.oilColors[0], h0, s0, v0);
    return h0 * 360.0f;
}

float FluidRenderer::PalettePhaseForHue(float hueDeg) const {
    const LiquidAcidConfig& a = m_cfg.acid;
    float h0 = fmodf(PaletteBaseHueDeg(), 360.0f);
    if (h0 < 0.0f) h0 += 360.0f;
    float H = fmodf(hueDeg, 360.0f);
    if (H < 0.0f) H += 360.0f;
    if (!(a.hueAnchorWeight > 0.0005f)) {         // linear clock: deg = 360 u
        float d = H - h0;
        if (d < 0.0f) d += 360.0f;
        return d / 360.0f;
    }
    // AnchorWarpDeg: y = F(h0) + u (mod 1), landing hue = Finv(y) -> u = F(H) - F(h0)
    AnchorWarpDeg(0.0f, h0, a.hueAnchorWeight);   // (re)builds m_anchorCdf for this weight
    auto F = [&](float x0) {                      // the same piecewise-linear F
        const float x = x0 * (256.0f / 360.0f);
        int j = (int)floorf(x);
        if (j < 0) j = 0; if (j > 255) j = 255;
        const float t = x - (float)j;
        return m_anchorCdf[j] + (m_anchorCdf[j + 1] - m_anchorCdf[j]) * t;
    };
    float u = F(H) - F(h0);
    if (u < 0.0f) u += 1.0f;
    if (u >= 1.0f) u -= 1.0f;
    return u;
}

// ---- palette_start_hue (brief BW item 4 + the user's random mode) ---------
// Today the palette clock starts at 0, so every Scheme opens on oil_color_1's
// own hue (~22 deg, orange on monotone-post-0924). This moves the ANIM_PALETTE
// accumulator ONCE, before anything is on screen (renderer init, or a cycle
// stage entry at black), so AnimatorTime(0) lands on the clock phase whose
// palette hue (effOil[0], the upload's own palette) is the wanted one. Sweep
// off (every Scheme): exact, u* = PalettePhaseForHue (the inverse of the
// anchor warp; linear when hue_anchor_weight is 0) and clock = u* P. Sweep on
// (monotone-post-0924: eight tile9 families cross-faded every 630 s under the
// 3600 s rotation): the hue is not a monotone function of the clock, so the
// clock is SEARCHED -- 8192 phases over 8 periods, then a local refine -- for
// the nearest palette hue. The drift then runs on from there. -1 returns
// before touching anything: the accumulator stays exactly 0.0f and every
// constant is bit-identical to today.
float FluidRenderer::PaletteAbsHueAt(float clk) const {
    float eo[12], ei[12], em[3];
    EffectivePalette(clk, eo, ei, em);
    float h, s, v;
    RgbToHsv(&eo[0], h, s, v);
    return h * 360.0f;
}

float FluidRenderer::ApplyPaletteStart(const char* why) {
    const LiquidAcidConfig& a = m_cfg.acid;
    const float want = a.paletteStartHue;
    const bool random = want <= -1.5f;
    if (!a.enabled || (!random && want < 0.0f)) return -1.0f;
    const bool sweep = a.hueSweepPeriod > 0.01f;
    if (!(a.hueRotatePeriod > 0.01f) && !sweep) {
        printf("[palette] start hue %s skipped (%s): the palette clock drives nothing "
               "(hue_rotate_period and hue_sweep_period both off)\n", random ? "random" : "fixed", why);
        return -1.0f;
    }
    float H = fmodf(want, 360.0f);
    unsigned y32 = 0;
    if (random) {
        // Its own generator, never rand(): drawing must not shift any other
        // seeded behaviour. --seed (headless) makes it reproducible; the live
        // app mixes the wall clock and the performance counter.
        static unsigned s_draws = 0;
        s_draws++;
        uint64_t x;
        if (g_randSeed) {
            x = (uint64_t)g_randSeed * 0x9E3779B97F4A7C15ull + (uint64_t)s_draws * 0xD1B54A32D192ED03ull;
        } else {
            LARGE_INTEGER q;
            QueryPerformanceCounter(&q);
            x = (uint64_t)GetTickCount64() * 0x9E3779B97F4A7C15ull ^ (uint64_t)q.QuadPart
              ^ (uint64_t)s_draws * 0xD1B54A32D192ED03ull;
        }
        x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;           // splitmix64 finaliser
        x ^= x >> 27; x *= 0x94D049BB133111EBull;
        x ^= x >> 31;
        y32 = (unsigned)(x >> 40);                           // 24 bits
        const float yv = (float)y32 * (1.0f / 16777216.0f);
        // the anchor density at weight 1 (the Scheme presets' warp), whatever
        // this preset's own hue_anchor_weight: magenta 325, blue 215, red 355,
        // violet 275 hold ~58% of the draws, lime/olive the least per degree
        static float s_cdf[257];
        static bool s_cdfBuilt = false;
        if (!s_cdfBuilt) { BuildAnchorCdf(1.0f, s_cdf); s_cdfBuilt = true; }
        int lo = 0, hi = 256;
        while (hi - lo > 1) { const int mid = (lo + hi) >> 1; if (s_cdf[mid] <= yv) lo = mid; else hi = mid; }
        const float span = s_cdf[lo + 1] - s_cdf[lo];
        const float t = span > 1e-9f ? (yv - s_cdf[lo]) / span : 0.0f;
        H = 360.0f * ((float)lo + t) / 256.0f;
    }
    if (H < 0.0f) H += 360.0f;
    if (H >= 360.0f) H -= 360.0f;
    auto angd = [](float x, float y) {
        float d = fabsf(fmodf(x - y, 360.0f));
        return d > 180.0f ? 360.0f - d : d;
    };
    float clk = 0.0f;
    if (!sweep) {
        clk = PalettePhaseForHue(H) * a.hueRotatePeriod;
    } else {
        const float span = fminf(8.0f * fmaxf(a.hueRotatePeriod, a.hueSweepPeriod), 30000.0f);
        const int   NS = 8192;
        float bestD = 1e9f;
        for (int k = 0; k < NS; k++) {
            const float tk = span * (float)k / (float)NS;
            const float d = angd(PaletteAbsHueAt(tk), H);
            if (d < bestD) { bestD = d; clk = tk; }
        }
        const float step = span / (float)NS;
        const float c0 = clk;
        for (int k = -64; k <= 64; k++) {
            const float tk = fmaxf(c0 + step * (float)k / 64.0f, 0.0f);
            const float d = angd(PaletteAbsHueAt(tk), H);
            if (d < bestD) { bestD = d; clk = tk; }
        }
    }
    // cancel a running kick on this clock, then place it
    m_kickDur[0] = 0.0f; m_kickTotal[0] = 0.0f; m_kickDone[0] = 0.0f; m_kickT[0] = 0.0f;
    m_animFrozenAccum[0] = m_time - clk;
    const float P = sweep ? a.hueSweepPeriod : a.hueRotatePeriod;
    const float u = fmodf(clk / P, 1.0f);
    float got = PaletteAbsHueAt(AnimatorTime(0));
    static const float kAnchor[4] = { 325.0f, 215.0f, 355.0f, 275.0f };
    int nearK = 0;
    float nearD = 1e9f;
    for (int k = 0; k < 4; k++) {
        float d = fabsf(got - kAnchor[k]);
        if (d > 180.0f) d = 360.0f - d;
        if (d < nearD) { nearD = d; nearK = k; }
    }
    printf("[palette] start hue %s %.1f (%s): palette clock %.1f s (u=%.4f of the %.0f s %s), "
           "palette hue now %.2f deg (err %+.2f), nearest anchor %.0f (%.1f deg away)%s\n",
           random ? "RANDOM" : "fixed", H, why, clk, u, P, sweep ? "sweep" : "rotation", got,
           (got - H) - 360.0f * floorf(((got - H) + 180.0f) / 360.0f), kAnchor[nearK], nearD,
           random ? (g_randSeed ? " [seeded by --seed]" : " [seeded by the clock]") : "");
    return H;
}

int FluidRenderer::AcidDropletCount() const {
    int live = 0;
    for (const AcidDrop& d : m_acidDrops) if (d.rt > 0.0f) live++;
    return live;
}

static float HalfToFloat(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t man = h & 0x3FF;
    uint32_t f;
    if (exp == 0) {
        if (!man) f = sign;
        else {
            exp = 112;
            while (!(man & 0x400)) { man <<= 1; exp--; }
            man &= 0x3FF;
            f = sign | (exp << 23) | (man << 13);
        }
    } else if (exp == 31) {
        f = sign | 0x7F800000u | (man << 13);
    } else {
        f = sign | ((exp + 112) << 23) | (man << 13);
    }
    float r;
    memcpy(&r, &f, 4);
    return r;
}

void FluidRenderer::ReportStats() {
    uint8_t* data = nullptr;
    D3D12_RANGE range = { 0, (SIZE_T)m_readbackPitch * m_dyeH };
    if (FAILED(m_readback->Map(0, &range, (void**)&data))) return;

    double sum = 0;
    float mx = 0;
    int nonBlack = 0, samples = 0, nans = 0;
    for (int y = 0; y < m_dyeH; y += 8) {
        const uint16_t* row = (const uint16_t*)(data + (SIZE_T)y * m_readbackPitch);
        for (int x = 0; x < m_dyeW; x += 8) {
            float r = HalfToFloat(row[x * 4 + 0]);
            float g = HalfToFloat(row[x * 4 + 1]);
            float b = HalfToFloat(row[x * 4 + 2]);
            if (r != r || g != g || b != b) { nans++; continue; }
            float m = fmaxf(r, fmaxf(g, b));
            sum += m;
            if (m > mx) mx = m;
            if (m > 0.02f) nonBlack++;
            samples++;
        }
    }
    D3D12_RANGE none = { 0, 0 };
    m_readback->Unmap(0, &none);
    printf("[stats] dye mean=%.4f max=%.3f lit=%.1f%% NaN=%d\n",
           samples ? sum / samples : 0.0, mx,
           samples ? 100.0 * nonBlack / samples : 0.0, nans);
}

void FluidRenderer::WaitForGpuIdle() {
    // A device that never got its fence (the soft-fail TryInit path: the swap chain was
    // refused before CreateFence) has run no command list: nothing to wait for. Without
    // this the unwind in Shutdown() died on the null fence (review 2026-10-04 R2).
    if (!m_queue || !m_fence || !m_fenceEvent) return;
    // A lost device runs nothing any more, so there is nothing to wait for either, and
    // Shutdown() must be able to release it without reaching Fail() (review 2026-10-04 R3).
    if (m_deviceLost) return;
    const UINT64 v = m_nextFence++;
    HR(m_queue->Signal(m_fence.Get(), v));
    if (m_deviceLost) return;
    if (m_fence->GetCompletedValue() < v) {
        HR(m_fence->SetEventOnCompletion(v, m_fenceEvent));
        if (!m_deviceLost) WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

bool FluidRenderer::VramOverBudget(unsigned long long* usage, unsigned long long* budget) const {
    if (!m_adapter3) return false;
    DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
    if (FAILED(m_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) return false;
    if (usage) *usage = info.CurrentUsage;
    if (budget) *budget = info.Budget;
    return info.CurrentUsage > info.Budget;
}

bool FluidRenderer::VramCardUsage(unsigned long long* used, unsigned long long* total) const {
    if (!m_vramTotal || !used) return false;
    PDH_HQUERY q = nullptr;
    PDH_HCOUNTER c = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return false;
    bool ok = false;
    if (PdhAddEnglishCounterW(q, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &c) == ERROR_SUCCESS &&
        PdhCollectQueryData(q) == ERROR_SUCCESS) {
        DWORD bytes = 0, n = 0;
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_LARGE, &bytes, &n, nullptr) == PDH_MORE_DATA && bytes) {
            std::vector<BYTE> buf(bytes);
            auto* it = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
            if (PdhGetFormattedCounterArrayW(c, PDH_FMT_LARGE, &bytes, &n, it) == ERROR_SUCCESS) {
                // instance names: luid_0x<high>_0x<low>_phys_<n>
                wchar_t want[48];
                swprintf_s(want, L"luid_0x%08X_0x%08X", (unsigned)m_adapterLuid.HighPart, m_adapterLuid.LowPart);
                const size_t wl = wcslen(want);
                for (DWORD i = 0; i < n; i++) {
                    if (it[i].szName && _wcsnicmp(it[i].szName, want, wl) == 0) {
                        *used = (unsigned long long)it[i].FmtValue.largeValue;
                        ok = true;
                        break;
                    }
                }
            }
        }
    }
    PdhCloseQuery(q);
    if (ok && total) *total = m_vramTotal;
    return ok;
}

void FluidRenderer::Shutdown() {
    if (!m_device) return;   // never initialized, or already shut down
    WaitForGpuIdle();

    // brief BY photo stage: join the WIC worker BEFORE the device goes
    // (pre-flight 17), then drop every photo GPU object. Photo mode and the
    // requested file survive, so a resume re-requests the photo (PhotoPump).
    PhotoWorkerStop();
    m_photoJob = 0;
    m_photoPending = PhotoResult{};
    m_photoHavePending = false;
    m_photoTex.Reset();
    m_photoRetire.clear();
    m_photoShown = false;
    m_photoFailed = false;
    m_psoPhoto.Reset();

    // Release EVERYTHING Init/CreateDevice/CreateSimResources (and the lazy
    // mirror/analyzer paths) created, so all RAM/VRAM is actually handed back
    // and a later Init() starts from a clean slate. Used both at process exit
    // and by the fullscreen-pause suspend in main.cpp.

    // second-monitor mirror (its RTVs live in m_rtvHeap)
    for (UINT i = 0; i < kFrames; i++) m_mirrorBuffers[i].Reset();
    m_mirrorChain.Reset();
    m_mirrorBroken = false;

    // HDR analyzer
    m_anaTex.Reset();
    m_anaReadback.Reset();
    m_anaPending = false;
    m_anaState = D3D12_RESOURCE_STATE_RENDER_TARGET;

    // ui3 B5 snapshot: the readback goes with the device (lost / suspended / re-init at another
    // size). A copy that was in flight is asked for again, so the open Settings window gets its
    // picture from the rebuilt renderer's first drawn frame.
    if (m_snapInFlight) m_snapWanted = true;
    m_snapInFlight = false;
    m_snapReadback.Reset();
    m_snapPitch = 0;
    m_snapW = m_snapH = 0;

    // headless capture target
    m_shotTex.Reset();
    m_shotReadback.Reset();
    m_shotState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    // [post] pass target (its PSO goes with the graphics PSOs below)
    m_postTex.Reset();
    m_postState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_postW = m_postH = 0;

    // stats + coverage readbacks
    m_readback.Reset();
    m_readbackPending = false;
    m_covReadback.Reset();
    m_covPending = false;

    // Liquid Acid: blob/param upload rings + the velocity readback
    for (UINT i = 0; i < kFrames; i++) {
        if (m_acidBlobUpload[i])  { m_acidBlobUpload[i]->Unmap(0, nullptr);  m_acidBlobUpload[i].Reset(); }
        if (m_acidParamUpload[i]) { m_acidParamUpload[i]->Unmap(0, nullptr); m_acidParamUpload[i].Reset(); }
        if (m_inkParamUpload[i])  { m_inkParamUpload[i]->Unmap(0, nullptr);  m_inkParamUpload[i].Reset(); }
        if (m_dropletUpload[i])     { m_dropletUpload[i]->Unmap(0, nullptr);     m_dropletUpload[i].Reset(); }
        if (m_dropletCellUpload[i]) { m_dropletCellUpload[i]->Unmap(0, nullptr); m_dropletCellUpload[i].Reset(); }
        m_acidBlobData[i] = nullptr;
        m_acidParamData[i] = nullptr;
        m_inkParamData[i] = nullptr;
        m_dropletData[i] = nullptr;
        m_dropletCellData[i] = nullptr;
    }
    // droplet particle sim: the population is CPU state, a resume reseeds it
    m_acidDrops.clear();
    m_dropletOrder.clear();
    m_dropletCellStart.clear();
    m_dropletCellCount.clear();
    m_dropletSeededFor = -1;
    m_dropletSpawnAcc = 0.0f;
    m_velReadback.Reset();
    m_velPending = false;
    m_acidBlobs.clear();
    m_acidSeeded = false;
    m_acidTurnAcc = 0.0f;           // brief BZ: turnover restarts with the population

    // sim textures
    auto freeTex = [](Tex& t) {
        t.res.Reset();
        t.srv = {};
        t.uav = {};
        t.state = D3D12_RESOURCE_STATE_COMMON;
    };
    freeTex(m_velocity.a);  freeTex(m_velocity.b);
    freeTex(m_dye.a);       freeTex(m_dye.b);
    freeTex(m_pressure.a);  freeTex(m_pressure.b);
    freeTex(m_divergence);  freeTex(m_curl);
    freeTex(m_coverage);
    freeTex(m_velLow);
    freeTex(m_oilMask);

    // root signatures + PSOs (shader blobs are recompiled from embedded source
    // in CreateDevice â€” nothing static is cached, so re-init is idempotent)
    m_psoClearV.Reset(); m_psoClear4.Reset(); m_psoClear1.Reset();
    m_psoCurl.Reset(); m_psoVorticity.Reset(); m_psoDivergence.Reset();
    m_psoClearPressure.Reset(); m_psoPressure.Reset(); m_psoGradSub.Reset();
    m_psoAdvectVel.Reset(); m_psoAdvectDye.Reset();
    m_psoSplatVel.Reset(); m_psoSplatDye.Reset(); m_psoSplatDyeCompact.Reset();
    m_psoDownsample.Reset(); m_psoDiffuseDye.Reset();
    m_psoDisplay.Reset(); m_psoGradient.Reset(); m_psoPost.Reset(); m_psoPostBN.Reset(); m_psoPostHeart.Reset();
    m_psoPostBlack.Reset();   // [post] mass_black / dither_auto
    m_psoLiquidAcid.Reset();
    m_psoLiquidAcidH.Reset();
    m_psoSurface.Reset(); m_surfRS.Reset();
    m_surfTex.Reset(); m_surfW = m_surfH = 0;
    m_psoMassCov.Reset(); m_massCovTex.Reset(); m_psoLiquidAcidC.Reset();
    for (auto& pl : m_psoLiquidAcidL) pl.Reset();   // Oil3D L1
    for (auto& pl : m_psoLiquidAcidL2) pl.Reset();  // Oil3D L2
    for (auto& row : m_psoLampX) for (auto& pl : row) pl.Reset();   // Oil3D L3
    m_psoTap.clear(); m_stageTapPso = nullptr;                       // Oil3D L3b (shot taps)
    for (auto& row : m_psoMurk) for (auto& pl : row) pl.Reset();   // Oil3D W1 / C3 (water family)
    m_murkTexels.clear();
    m_murkJob.reset();   // post-night T1: a bake in flight finishes on its own and frees its job
    m_waterPrewarmSig = 0;   // post-night T3: the rebuilt device asks again (the cached blobs make it quick)
    m_murkTex.Reset(); m_murkUpload.Reset();
    m_murkState = D3D12_RESOURCE_STATE_COPY_DEST;
    m_murkUploadPending = m_murkReady = false;
    memset(m_murkKey, 0, sizeof(m_murkKey));
    memset(m_murkSeen, 0, sizeof(m_murkSeen)); m_murkSeenAt = 0;
    for (auto& pl : m_psoPostL) pl.Reset();         // Oil3D L1b (LAMP_GLOW post)
    m_massCovW = m_massCovH = m_massCovP = m_massCovFrameW = m_massCovFrameH = 0;
    m_tsHeap.Reset(); m_tsReadback.Reset();
    m_tsOpen = false; m_tsPrevStart = m_tsPrevEnd = 0;
    for (bool& b : m_tsPending) b = false;
    m_psoOilMask.Reset(); m_psoOilDrag.Reset(); m_psoOilDyeBlock.Reset();
    m_oilMaskMade = false;
    m_psoInk.Reset();
    m_dropTimer = 0.0f;
    m_dropPrimed = false;
    m_dropTailLeft = 0.0f;
    m_dropQueued = false;
    m_computeRS.Reset();
    m_graphicsRS.Reset();

    // descriptor heaps
    m_srvHeap.Reset();
    m_rtvHeap.Reset();

    // swapchain, back buffers, command infrastructure, fence
    for (UINT i = 0; i < kFrames; i++) {
        m_backBuffers[i].Reset();
        m_allocators[i].Reset();
        m_fenceValues[i] = 0;
    }
    m_swapChain.Reset();
    m_cmd.Reset();
    m_queue.Reset();
    if (m_fenceEvent) CloseHandle(m_fenceEvent);
    m_fenceEvent = nullptr;
    m_fence.Reset();
    m_device.Reset();
    m_adapter3.Reset();
    m_factory.Reset();

    // a following Init() must behave exactly like the first one
    m_frameIndex = 0;
    m_nextFence = 1;
    m_firstFrame = true;      // re-clear sim textures + startup splat burst
    m_presentBroken = false;
    m_deviceLost = false;     // R3: everything of the lost device is gone now
}

// ---------------------------------------------------------------------------
// M3: autonomous behaviors, ported from reference/script.js
// ---------------------------------------------------------------------------

// Map the cycling wheel position into the configured hue band. Full range
// (180) = classic wheel; narrower = a triangle sweep across the themed slice,
// with offsets (wanderer/dart) scaled into the band too.
float FluidRenderer::WheelHue(float offset) {
    float rangeFrac = m_cfg.hueRange / 180.0f;
    // coordination: while a hue command (mood transition bridge) rotates the
    // displayed field, counter-rotate emission so fresh dye still lands inside
    // the intended visible band instead of clashing with the rotated field
    float cmd = m_hsCommanded ? m_hueAngle / 360.0f : 0.0f;
    if (rangeFrac >= 0.999f) return m_globalHue + offset - cmd;
    float tri = 1.0f - fabsf(2.0f * m_globalHue - 1.0f);   // 0..1..0 per lap
    // linger: dwell at the band edges so one palette can fill the screen
    // before drifting on (0 = off = constant-speed triangle)
    float L = m_cfg.hueLinger;
    if (L > 0.0f) {
        float s;
        if (tri < L) s = 0.0f;
        else if (tri > 1.0f - L) s = 1.0f;
        else {
            s = (tri - L) / (1.0f - 2.0f * L);
            s = s * s * (3.0f - 2.0f * s);   // smooth glide between rests
        }
        tri = s;
    }
    return m_cfg.hueCenter / 360.0f + (tri - 0.5f) * rangeFrac + offset * rangeFrac - cmd;
}

// Reference color sources: cycledColor() when "colorful" (hue wheel), else a
// random pick from the fixed splat palette. Both dimmed to 0.15.
void FluidRenderer::PickSplatColor(float hueOffset, float out[3]) {
    if (m_cfg.colorful) {
        RGB c = CycledColorAt(WheelHue(hueOffset));
        out[0] = c.r; out[1] = c.g; out[2] = c.b;
    } else {
        int n = m_cfg.moreColors ? 5 : 1;
        const float* p = m_cfg.splatColors + 3 * (rand() % n);
        out[0] = p[0] * 0.15f; out[1] = p[1] * 0.15f; out[2] = p[2] * 0.15f;
    }
}

// Explorer restarted: the old WorkerW (and our window in it) died. The caller
// made a fresh wallpaper window; rebuild just the swapchain onto it â€” device,
// sim textures, and the fluid state all survive.
void FluidRenderer::Reattach(HWND hwnd) {
    if (!m_device) return;   // suspended: the resume does a full Init instead
    if (m_deviceLost) return;   // R3: TryReattach reports it; the shell rebuilds the device
    DisableMirror();   // its window died with the old WorkerW; shell re-enables
    WaitForGpuIdle();
    for (UINT i = 0; i < kFrames; i++) {
        m_backBuffers[i].Reset();
        m_fenceValues[i] = 0;
    }
    m_swapChain.Reset();
    m_factory.Reset();
    HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory)));

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = m_width;
    sd.Height = m_height;
    sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kFrames;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain1> sc1;
    if (!CreateSwapChainSoft(hwnd, sd, sc1)) return;
    HR(sc1.As(&m_swapChain));
    m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    ReassertColorSpace();

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrames; i++) {
        HR(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])));
        m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, rtv);
        rtv.ptr += m_rtvStride;
    }
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    m_presentBroken = false;
    printf("reattached to new wallpaper window\n");
}

void FluidRenderer::SetResolutions(int simRes, int dyeRes) {
    m_cfg.simRes = simRes;
    m_cfg.dyeRes = dyeRes;
    if (!m_device || m_deviceLost) return;   // suspended / lost (R3): takes effect at the resume Init
    WaitForGpuIdle();
    m_covPending = false;
    m_readbackPending = false;
    m_velPending = false;
    CreateSimResources();     // recreates textures/readbacks, reinits wanderers
    m_firstFrame = true;      // clear + startup splat burst next frame
    printf("resolutions changed: sim %dx%d, dye %dx%d\n", m_simW, m_simH, m_dyeW, m_dyeH);
}

void FluidRenderer::InitWanderers() {
    m_wanderers.clear();
    int n = m_cfg.wandererCount < 1 ? 1 : m_cfg.wandererCount;
    for (int i = 0; i < n; i++) {
        Wanderer w = {};
        w.R = fminf((float)m_width, (float)m_height) * m_cfg.wandererScale *
              (0.6f + RandF() * 0.8f) / 2.0f;
        w.x = m_width * (0.2f + RandF() * 0.6f);
        w.y = m_height * (0.2f + RandF() * 0.6f);
        w.heading = RandF() * 6.2831853f;
        w.turn = 0.0f;
        w.cx = m_width * (0.25f + RandF() * 0.5f);
        w.cy = m_height * (0.3f + RandF() * 0.4f);
        w.phase = RandF() * 6.2831853f;
        w.dir = RandF() < 0.5f ? 1 : -1;
        w.hueOffset = i * 0.13f;
        m_wanderers.push_back(w);
    }
}

void FluidRenderer::UpdateWanderers(float dt) {
    const float margin = 40.0f;
    for (size_t i = 0; i < m_wanderers.size(); i++) {
        // wanderer 0 is the survivor: it obeys its own floor, the rest the group floor
        if (i == 0 ? m_survivorTooFull : m_screenTooFull) continue;
        Wanderer& w = m_wanderers[i];
        float nx, ny;
        if (m_cfg.wandererMode == 1) {          // circle
            w.phase += w.dir * (m_cfg.wandererSpeed * dt) / w.R;
            nx = w.cx + cosf(w.phase) * w.R;
            ny = w.cy + sinf(w.phase) * w.R;
        } else if (m_cfg.wandererMode == 2) {   // figure8
            w.phase += w.dir * (m_cfg.wandererSpeed * dt) / w.R;
            nx = w.cx + sinf(w.phase) * w.R * 1.6f;
            ny = w.cy + sinf(w.phase * 2.0f) * w.R * 0.8f;
        } else {                                // random smooth wander
            w.turn += (RandF() - 0.5f) * 12.0f * dt;
            w.turn = fmaxf(-2.5f, fminf(2.5f, w.turn)) * 0.98f;
            w.heading += w.turn * dt * 4.0f;
            nx = w.x + cosf(w.heading) * m_cfg.wandererSpeed * dt;
            ny = w.y + sinf(w.heading) * m_cfg.wandererSpeed * dt;
            if (nx < margin || nx > m_width - margin) {
                w.heading = 3.14159265f - w.heading;
                nx = fmaxf(margin, fminf((float)m_width - margin, nx));
            }
            if (ny < margin || ny > m_height - margin) {
                w.heading = -w.heading;
                ny = fmaxf(margin, fminf((float)m_height - margin, ny));
            }
        }
        float dx = (nx - w.x) * 5.0f;
        float dy = (ny - w.y) * 5.0f;
        w.x = nx;
        w.y = ny;
        float c[3];
        PickSplatColor(w.hueOffset, c);
        float wb = m_cfg.wandererBrightness * m_emitScale;
        Splat(w.x, w.y, dx, dy, c[0] * wb, c[1] * wb, c[2] * wb);
    }
}

void FluidRenderer::UpdateDart(float dt) {
    if (!m_dart.active) {
        if (!m_cfg.dartEnabled || !m_screenTooFull || !m_cfg.wanderers) return;
        if (m_time - m_lastDartTime < m_cfg.dartInterval) return;
        m_lastDartTime = m_time;
        // start on a random edge, aim at a random point on the opposite edge
        float m = 10.0f, W = (float)m_width, H = (float)m_height;
        int side = (int)(RandF() * 4.0f) & 3;
        float sx, sy, ex, ey;
        if (side == 0)      { sx = m;     sy = RandF() * H; ex = W - m;       ey = RandF() * H; }
        else if (side == 1) { sx = W - m; sy = RandF() * H; ex = m;           ey = RandF() * H; }
        else if (side == 2) { sx = RandF() * W; sy = m;     ex = RandF() * W; ey = H - m; }
        else                { sx = RandF() * W; sy = H - m; ex = RandF() * W; ey = m; }
        float ddx = ex - sx, ddy = ey - sy;
        float len = sqrtf(ddx * ddx + ddy * ddy);
        m_dart = { sx, sy, ddx / len, ddy / len, len, true };
        return;
    }
    float step = fminf(m_cfg.dartSpeed * dt, m_dart.left);
    float nx = m_dart.x + m_dart.ux * step;
    float ny = m_dart.y + m_dart.uy * step;
    // complementary hue to the current wheel position, at wanderer brightness
    float c[3];
    PickSplatColor(0.5f, c);
    float wb = m_cfg.wandererBrightness * m_emitScale;
    Splat(nx, ny, (nx - m_dart.x) * 5.0f, (ny - m_dart.y) * 5.0f,
          c[0] * wb, c[1] * wb, c[2] * wb);
    m_dart.x = nx;
    m_dart.y = ny;
    m_dart.left -= step;
    if (m_dart.left <= 0.5f) m_dart.active = false;
}

void FluidRenderer::CommandHueShift(float targetDeg, float durationSec) {
    m_hsCommanded = true;
    m_hsPhase = 1; m_hsTimer = 0;
    m_hsFrom = m_hueAngle; m_hsTo = targetDeg;
    m_hsGlideOverride = fmaxf(0.05f, durationSec);
}

void FluidRenderer::ReleaseHueShift(bool returnHome) {
    if (returnHome) {
        // rotate forward to the next full turn, then release (completion of
        // phase 3 clears the commanded flag inside UpdateHueShift)
        m_hsPhase = 3; m_hsTimer = 0;
        m_hsFrom = m_hueAngle;
        m_hsTo = ceilf((m_hueAngle + 0.001f) / 360.0f) * 360.0f;
        m_hsGlideOverride = 0.0f;   // return at the configured glide speed
    } else {
        // drop the command, but never snap the field's colors: if the angle
        // isn't home yet, glide to the next full turn like returnHome does
        m_hsGlideOverride = 0.0f;
        m_hsStep = 0;
        float rem = fmodf(m_hueAngle, 360.0f);
        if (rem > 0.5f || rem < -0.5f) {
            m_hsCommanded = true;
            m_hsPhase = 3; m_hsTimer = 0;
            m_hsFrom = m_hueAngle;
            m_hsTo = ceilf((m_hueAngle + 0.001f) / 360.0f) * 360.0f;
        } else {
            m_hsCommanded = false;
            m_hsPhase = 0; m_hsTimer = 0; m_hueAngle = 0;
        }
    }
}

void FluidRenderer::UpdateHueShift(float dt) {
    if (m_hsCommanded) {
        // externally driven: phase 1 glides to the target and holds;
        // phase 3 returns home, then hands back to the scheduler
        if (m_hsPhase == 1 || m_hsPhase == 3) {
            m_hsTimer += dt;
            float dur = m_hsGlideOverride > 0.0f ? m_hsGlideOverride
                                                 : fmaxf(0.05f, m_cfg.hsGlide);
            float t = fminf(1.0f, m_hsTimer / dur);
            t = t * t * (3.0f - 2.0f * t);
            m_hueAngle = m_hsFrom + (m_hsTo - m_hsFrom) * t;
            if (t >= 1.0f && m_hsPhase == 3) {
                m_hsCommanded = false;
                m_hsPhase = 0; m_hsTimer = 0; m_hsStep = 0; m_hueAngle = 0;
            }
        }
        return;
    }
    if (!m_cfg.hsEnabled || m_cfg.hueRange < 179.0f) {
        // Narrow hue bands must never be post-rotated: any rotation drags the
        // whole field out of band (the Ember green-leak bug). Color motion in
        // a banded look comes from emission-side drift (color_cycle_period),
        // never from the shift wheel. And never snap the rotation off â€”
        // glide to the next full turn.
        float rem = fmodf(m_hueAngle, 360.0f);
        if (rem > 0.5f || rem < -0.5f) {
            m_hsCommanded = true;
            m_hsPhase = 3; m_hsTimer = 0;
            m_hsFrom = m_hueAngle;
            m_hsTo = ceilf((m_hueAngle + 0.001f) / 360.0f) * 360.0f;
            m_hsGlideOverride = 0.0f;
        } else {
            m_hueAngle = 0;
            m_hsPhase = 0; m_hsTimer = 0; m_hsStep = 0;
        }
        return;
    }
    m_hsTimer += dt;
    if (m_hsPhase == 0) {           // off
        if (m_hsTimer >= m_cfg.hsOffTime) {
            m_hsPhase = 1; m_hsTimer = 0; m_hsStep = 1;
            m_hsFrom = 0; m_hsTo = m_cfg.hsStep;
        }
    } else if (m_hsPhase == 2) {    // linger
        if (m_hsTimer >= m_cfg.hsLinger) {
            m_hsTimer = 0;
            if (m_hsStep >= m_cfg.hsBurstSteps) {
                // rotate forward to the next full turn so the palette lands home
                m_hsPhase = 3; m_hsFrom = m_hueAngle;
                m_hsTo = ceilf((m_hueAngle + 0.001f) / 360.0f) * 360.0f;
            } else {
                m_hsStep++; m_hsPhase = 1;
                m_hsFrom = m_hueAngle; m_hsTo = m_hsStep * m_cfg.hsStep;
            }
        }
    } else {                        // glide (1) or return (3)
        float t = fminf(1.0f, m_hsTimer / fmaxf(0.05f, m_cfg.hsGlide));
        t = t * t * (3.0f - 2.0f * t);
        m_hueAngle = m_hsFrom + (m_hsTo - m_hsFrom) * t;
        if (t >= 1.0f) {
            m_hsTimer = 0;
            if (m_hsPhase == 1) m_hsPhase = 2;
            else { m_hsPhase = 0; m_hueAngle = 0; m_hsStep = 0; }
        }
    }
}

void FluidRenderer::HandleInput(const FrameInput& in) {
    if (in.userInteracted) m_lastInteraction = m_time;
    float c[3];
    PickSplatColor(0.0f, c);
    float es = m_emitScale;
    // The sim still runs full-size under a mirror; only the DISPLAY folds. So
    // map the pointer through the same fold: the pixel the user is pointing at
    // is showing some source pixel, and that is where the dye must go. Land on
    // a mirrored copy and it maps back to the source, which is the same thing.
    // With mirror.mode = 0 this is the identity and costs nothing.
    float mx = in.mouseX, my = in.mouseY, fx = 1.0f, fy = 1.0f;
    MirrorMapPointer(mx, my, fx, fy);
    // [liquid_acid] mouse_oil_mode reads the pointer from here in uv -- and
    // ONLY the pointer. No splat, no dye, no branch above is affected: the
    // oil modes must never touch the ink (the rise presets ship with
    // show_mouse / hold_to_splat / splat_on_click all 0 and stay that way).
    m_ptrMoved = in.mouseMoved;
    if (in.mouseMoved) {
        m_ptrX = mx / fmaxf((float)m_width, 1.0f);
        m_ptrY = my / fmaxf((float)m_height, 1.0f);
    }
    if (m_cfg.holdToSplat && in.mouseDown) {
        float jx = (RandF() - 0.5f) * 200.0f;
        float jy = (RandF() - 0.5f) * 200.0f;
        Splat(mx, my, in.mouseDx * 0.5f * fx + jx, in.mouseDy * 0.5f * fy + jy,
              c[0] * es, c[1] * es, c[2] * es);
    } else if (!m_cfg.holdToSplat && m_cfg.splatOnClick &&
               in.mouseDown && !m_prevMouseDown) {
        MultipleSplats(rand() % 20 + 5);   // reference click burst (one-shot, unscaled)
    }
    m_prevMouseDown = in.mouseDown;
    if (in.mouseMoved && m_cfg.showMouse)
        Splat(mx, my, in.mouseDx * fx, in.mouseDy * fy,
              c[0] * es, c[1] * es, c[2] * es);
}

// ===========================================================================
// "Liquid Acid" look â€” oil metaballs on inked water.
//
// The oil is a CPU blob population (positive blobs = oil, NEGATIVE blobs =
// round holes/water bubbles eaten out of it) advected by the fluid's own
// velocity field, uploaded once per frame as a structured buffer the display
// shader loops over. See LiquidAcidConfig in fluid.h and the LIQUID_ACID
// section of kDisplaySrc.
// ===========================================================================

// GPU mirrors â€” must match struct AcidBlobGPU / cbuffer AcidCB in shaders.h.
// .c = the comb anisotropy (mouse_oil_mode 2): x = stretch amount along the
// drag direction, yz = that direction. All zero unless the comb is running.
// .c.w = the blob's dye identity in (0,1) (brief AG-b; read for holes only).
struct AcidBlobGPU { float a[4]; float b[4]; float c[4]; };
struct AcidParamsGPU {
    float oil[4][4];
    float ink[4][4];
    float p0[4], p1[4], p2[4], p3[4], p4[4], p5[4], p6[4], p7[4], p8[4], p9[4];
    float p10[4], p11[4], p12[4], p13[4], p14[4], p15[4], p16[4], p17[4];
    float p18[4], p19[4], p20[4], p21[4], p22[4], p23[4], men[4];
    float p24[4], p25[4], p26[4], p27[4], p28[4], p29[4];
    // brief AE: hue2 params, then the 20x12 mix field packed four cells per
    // float4. It rides the cbuffer's slack -- no new texture, no descriptor,
    // and nothing added to a root signature already at its 64-DWORD limit.
    float p30[4], p31[4];
    // brief BC: the cast-shadow block. Cbuffer slack again -- the root
    // signature is at its 64-DWORD limit and a shadow needs no texture.
    float p32[4];
    // brief BL: the fluorescent-oil / ambient-dye block (the cbuffer was full).
    float p33[4];
    // brief AG-b: per-mass dye variation (the cbuffer was full again).
    float p34[4];
    // brief BR: dye_core (.x); .y/.z film_hue3_share/film_equal_load (BV), .w film_hue2_seam (BW) -- see acid_slots.h.
    float p35[4];
    // brief BU: lamp grey (p36 + p37.w) and the split tone's add (p37.xyz).
    // laP38: .x film_equal_load_patches (brief BW); .yzw grey heart (brief BX).
    float p36[4], p37[4], p38[4];
    // brief BU-b: p39 = highlight tint + balance.
    float p39[4];
    // Oil3D Stage A: p40 = oil_height (lambda, toe, --shot-debug).
    float p40[4];
    // Oil3D C1: p41 = tank fade, shadow offset xy, penumbra; p42 = under share,
    // shadow_on_film, footprint stretch, Stage B's cross-fade share.
    float p41[4], p42[4];
    // Oil3D C1 fix: p43.x = the contact shadow's reach; C2: p43.yzw + p44.xyz
    // = the haze level, the column's optical depth, the coverage map (.w free).
    float p43[4], p44[4];
    float mix[60][4];
    // Oil3D L1: lamp 1's window LUT over the hue2 share k2 (BuildLampLut),
    // read only by the OIL_LAMP variants; appended after the mix field so
    // every other member keeps its offset.
    float lamp[64][4];
    // Oil3D L1b: lamp 1's window colour (BuildLampLut), read only by the
    // LAMP_GLOW post variants at b1 c178 (packoffset in kPostSrc).
    float glow[4];
    // Oil3D L2: appended at the END, so no member above moves (every older
    // PSO reads the same offsets; glow stays at c178): p45 = lamp 2's shadow
    // geometry (named slots, src\acid_slots.h), lamp2 = lamp 2's window LUT
    // (BuildLampLut(2, ...)), both read only by the OIL_LAMP2 variants.
    float p45[4];
    float lamp2[64][4];
    // Oil3D L3 (3): appended at the END again (no member above moves): the
    // masses under the lamps (BuildMassConsts), read only by the OIL_MASSDYE
    // variants: [0] / [1] the dyed mass's one-sample change under lamp 1 / 2
    // (hue shift, chroma, lum ratio; [1].z x lamp2_level); [2] the dark-dye
    // oil's two-lamp WINDOWED rgb per unit film Y, [3] its WHITE state's (.w
    // = 1 while oil_dye_amount > 0).
    float mass[4][4];
    // Oil3D L3b: appended at the END again (no member above moves): the WHITE
    // state with the film's dyes (BuildLampLut(0): a white window at level 1),
    // read only by the OIL_DYEWHITE variants (the white track = the same film).
    float lampW[64][4];
    // Oil3D W1: appended at the END (no member above moves): the murk tile
    // mapping (uv scale + drift, depth scale + drift, self-shadow k, murk_light),
    // read only by the OIL_MURK variants.
    float murk[2][4];
    // Oil3D C3: appended at the END (no member above moves): the motes' grid
    // and record bases in the droplet buffers' tail, the frame in px, their
    // blur and brightness, read only by the OIL_MOTES variants.
    float mote[3][4];
    // Oil3D W2: appended at the END (no member above moves): the water's colour
    // as a linear rgb gain on the column's share (white = clear water) + the
    // per-pixel normaliser's cap in .w, and the silt's (water x silt), read only
    // by the OIL_WTINT variants.
    float water[2][4];
    // Oil3D W3: appended at the END (no member above moves): the fine silt +
    // depth haze (amount, lattice px, haze optical depth, veil lift; the three
    // slices' lattice offsets, the slice count, the churn clock), read only by
    // the OIL_GRAIN variants.
    float grain[3][4];
    // DROP_LIFE (droplet_shadow / droplet_wobble, 2026-11-02): appended at the END (no member above
    // moves), read only by the DROP_LIFE variants: [0] = droplet_shadow, cot(light_elev),
    // tan(light_size / 2), droplet_wobble; [1] = the unit p-space direction AWAY from the Oil3D lamp
    // (x, y), the glint's turn to that lamp (0 / 1), the old rig-lamp droplet shadow's keep (1 / 0);
    // [2] = droplet_core, its size ramp (droplet radius in uv-y where the lens starts / is fully open),
    // droplet_haze_rim (post-night T4, read by DROP_HAZERIM only).
    float drop[3][4];
};
static_assert(sizeof(AcidParamsGPU) == 5200, "AcidCB layout");
static_assert(offsetof(AcidParamsGPU, glow) == 178 * 16, "kPostSrc reads laLampGlow at packoffset(c178)");
static_assert(offsetof(AcidParamsGPU, lamp2) == 180 * 16, "laLamp2 follows laP45 at c180");
// --shot-dump patches this one float (LA_OIL_H_DEBUG = laP40.z) in place.
static size_t AcidDebugSlotOffset() { return offsetof(AcidParamsGPU, p40) + 2 * sizeof(float); }

// One particle of the droplet sim. Must match StructuredBuffer<float4>
// AcidDrops in shaders.h: xy = centre uv, z = visible radius SIGNED (negative
// = a water droplet trapped in the oil, positive = an oil droplet on the open
// ink), w = reserved.
struct AcidDropGPU { float a[4]; };
// The same integer hash the nucleation code uses, as a free function: the
// ring shapes are re-derived from a droplet's seed every frame and must land
// on exactly the values that seed drew at birth.
static float DropHash(float hx, float hy, uint32_t salt) {
    uint32_t a1, b1;
    memcpy(&a1, &hx, 4); memcpy(&b1, &hy, 4);
    uint32_t h = (a1 * 2654435761u) ^ (b1 * 2246822519u) ^ salt;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    h *= 3266489917u; h ^= h >> 16;
    return (h >> 8) * (1.0f / 16777216.0f);
}
// accent_mode 1: the per-blob draw that decides who carries the accent shade.
// A HASH of the blob's index rather than a rand() call, for two reasons: it
// takes nothing from the seeding stream, so turning the key on leaves every
// blob's size and position exactly where it was; and it is re-derivable every
// frame, so the membership can never drift.
static float AcidAccentHash(uint32_t i, uint32_t salt) {
    uint32_t h = (i * 2654435761u) ^ salt;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    h *= 3266489917u; h ^= h >> 16;
    return (h >> 8) * (1.0f / 16777216.0f);
}
// brief AG-b: the next dye identity, strictly inside (0,1). A hash of a serial
// number rather than a draw from the seeding / respawn RNGs, so turning the
// dye variation on (or adding this at all) moves no blob by a bit.
float FluidRenderer::NextAcidDyeId() {
    const float u = AcidAccentHash(m_acidDyeSerial++, m_acidDyeSalt ^ 0xD7E1D00Du);
    return 0.002f + 0.996f * u;
}
// Must match StructuredBuffer<uint2> DropCells: (first index, count).
struct DropCellGPU { uint32_t first, count; };

// GPU mirror of cbuffer InkCB in shaders.h (the SHARED ink-in-water block).
struct InkParamsGPU {
    float p0[4], p1[4], p2[4], p3[4], p4[4];
    float paper[4], tintThin[4], tintThick[4];
    float p5[4], p6[4];
};
static_assert(sizeof(InkParamsGPU) == 160, "InkCB layout");

// CPU mirror of AcidHueShift in shaders.h: rotate HUE ONLY, holding saturation
// and value, so a swept colour is exactly as vivid at its new hue as it was at
// its old one. (A W3C matrix hue-rotate holds luma instead and turns a bright
// orange into olive on the way to yellow.) The oil colours and the ink stops
// are rotated here rather than in the pixel shader because the shader picks a
// flat fill per pixel out of the blob buffer.
// brief BU: the split tone's on/off gate, 0..1, on its own clock. 50% duty:
// fully on for the first half-period minus the fades, a smoothstep fade of
// `fade` seconds at each edge, off for the other half. Shifted by one fade so
// it starts fully ON at t = 0 (a freshly launched wallpaper shows the tint
// for its first ~14.5 min at the default 2225 s; the edges are then at
// t = 872..992 s, off, and 2105..2225 s back on). period <= 0 = always on.
// tools-side replica: the BU proof's gear log (report) uses this formula.
static float ShadowToneGate(double t, float period, float fade) {
    if (period <= 0.5f) return 1.0f;
    const double P = period;
    const double half = 0.5 * P;
    const double f = fmin(fmax((double)fade, 0.0), 0.5 * half);
    double v = fmod(t + f, P);
    if (v < 0.0) v += P;
    auto ss = [](double e0, double e1, double x) {
        if (e1 - e0 < 1e-6) return x >= e1 ? 1.0 : 0.0;
        double u = (x - e0) / (e1 - e0);
        u = u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u);
        return u * u * (3.0 - 2.0 * u);
    };
    return (float)(ss(0.0, f, v) * (1.0 - ss(half - f, half, v)));
}

// brief BU: OKLab hue (radians) of an sRGB-ENCODED colour, for the split
// tone's complement. The HSV complement (h + 0.5) misses the perceptual
// opposite by up to ~23 deg (worst on the blues and the pinks this look runs),
// so the tint's hue is solved in OKLab instead.
static float BuOkHueEnc(float r, float g, float b) {
    auto lin = [](float c) {
        c = fmaxf(c, 0.0f);
        return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    };
    const float lr = lin(r), lg = lin(g), lb = lin(b);
    const float l = cbrtf(0.4122214708f * lr + 0.5363325363f * lg + 0.0514459929f * lb);
    const float m = cbrtf(0.2119034982f * lr + 0.6806995451f * lg + 0.1073969566f * lb);
    const float s = cbrtf(0.0883024619f * lr + 0.2817188376f * lg + 0.6299787005f * lb);
    const float A = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    const float B = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
    return atan2f(B, A);
}

static void RgbToHsv(const float c[3], float& h, float& sv, float& v) {
    const float mx = fmaxf(c[0], fmaxf(c[1], c[2]));
    const float mn = fminf(c[0], fminf(c[1], c[2]));
    const float d = mx - mn;
    h = 0.0f;
    if (d > 1e-7f) {
        if (mx == c[0])      h = (c[1] - c[2]) / d + (c[1] < c[2] ? 6.0f : 0.0f);
        else if (mx == c[1]) h = (c[2] - c[0]) / d + 2.0f;
        else                 h = (c[0] - c[1]) / d + 4.0f;
        h /= 6.0f;
    }
    sv = (mx > 1e-7f) ? d / mx : 0.0f;
    v = mx;
}

static void HsvHueShiftCpu(float c[3], float deg) {
    float h, sat, val;
    RgbToHsv(c, h, sat, val);
    h = fmodf(h + deg / 360.0f, 1.0f);
    if (h < 0.0f) h += 1.0f;
    RGB out = HSVtoRGB(h, sat, val);
    c[0] = out.r; c[1] = out.g; c[2] = out.b;
}

// Cross-fade two colours through HSV along the SHORT way round the wheel, so
// a magenta -> green transition passes through red/orange rather than sliding
// through desaturated grey the way a linear RGB lerp does.
static void HsvLerp3(const float a3[3], const float b3[3], float t, float out[3]) {
    float ha, sa, va, hb, sb, vb;
    RgbToHsv(a3, ha, sa, va);
    RgbToHsv(b3, hb, sb, vb);
    float dh = hb - ha;
    if (dh > 0.5f) dh -= 1.0f;
    if (dh < -0.5f) dh += 1.0f;
    float h = fmodf(ha + dh * t + 1.0f, 1.0f);
    RGB c = HSVtoRGB(h, sa + (sb - sa) * t, va + (vb - va) * t);
    out[0] = c.r; out[1] = c.g; out[2] = c.b;
}

// Rebuild the whole palette around two vivid anchors, reusing the S/V ratios
// of the hand-tuned palette so a swept pair has the same internal structure:
// four close oil shades, an ink ramp of [ink mid, near-black, oil dark, oil
// bright], and a bright ink-hue meniscus.
static void BuildAcidPalette(const LiquidAcidConfig& a, const float oilA[3],
                             const float inkA[3], float outOil[12],
                             float outInk[12], float outMen[3]) {
    float ho, so, vo, hi, si, vi;
    RgbToHsv(oilA, ho, so, vo);
    RgbToHsv(inkA, hi, si, vi);
    // the authored oil family's spread around its first entry
    static const float sMul[4] = { 1.000f, 1.055f, 1.019f, 1.058f };
    static const float vMul[4] = { 1.000f, 1.057f, 0.952f, 1.087f };
    for (int i = 0; i < 4; i++) {
        RGB c = HSVtoRGB(ho, fminf(so * sMul[i], 1.0f), fminf(vo * vMul[i], 1.0f));
        outOil[i * 3 + 0] = c.r; outOil[i * 3 + 1] = c.g; outOil[i * 3 + 2] = c.b;
    }
    auto put = [&](int i, float h, float sat, float val) {
        RGB c = HSVtoRGB(h, fminf(sat, 1.0f), fminf(val, 1.0f));
        outInk[i * 3 + 0] = c.r; outInk[i * 3 + 1] = c.g; outInk[i * 3 + 2] = c.b;
    };
    // Dark base. In every reference the ink is mostly NEAR-BLACK, with the
    // colour living in bands and filaments (ref 2: deep purple-black, lilac
    // only at the fronts). Stops 0/1 therefore keep the ink hue but stay dark;
    // only stops 2/3 carry the vivid complement, so the open ink reads as a
    // tinted black instead of flooding with mid tone.
    put(0, hi, si,          vi * 0.260f);   // dark base (the open ink)
    put(1, hi, si * 0.850f, vi * 0.090f);   // near-black, ink-tinted
    put(2, hi, si,          vi * 0.720f);   // mid band
    put(3, hi, si * 0.800f, vi * 1.450f);   // vivid front / filament band
    RGB m = HSVtoRGB(hi, fminf(si * 0.745f, 1.0f), fminf(vi * 1.858f, 1.0f));
    outMen[0] = m.r; outMen[1] = m.g; outMen[2] = m.b;
    (void)a;
}

// Saturation-weighted mean HSV hue of the oil palette, in the same
// parameterisation the shader measures the ink ramp with, so the complement
// clamp is consistent on both sides.
static float OilMeanHueDeg(const float cols[12]) {
    float sx = 0.0f, sy = 0.0f;
    for (int i = 0; i < 4; i++) {
        const float c[3] = { cols[i * 3 + 0], cols[i * 3 + 1], cols[i * 3 + 2] };
        float h, sat, val;
        RgbToHsv(c, h, sat, val);
        const float a = h * 6.2831853f, w = sat * val;
        sx += cosf(a) * w;
        sy += sinf(a) * w;
    }
    if (sx * sx + sy * sy < 1e-12f) return 0.0f;
    float d = atan2f(sy, sx) * 180.0f / 3.14159265358979f;
    return d < 0.0f ? d + 360.0f : d;
}

// Private deterministic RNG: the blob population must replay exactly under a
// --shot seed, and must not perturb the fluid's own rand() sequence (which
// would break parity with the normal look).
namespace {
struct AcidRng {
    uint32_t s;
    explicit AcidRng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float f() { return (next() >> 8) * (1.0f / 16777216.0f); }         // [0,1)
    float f(float lo, float hi) { return lo + (hi - lo) * f(); }
};
}

void FluidRenderer::CreateAcidBuffers() {
    // Tiny (4 KB + 256 B per frame): created unconditionally so the display
    // draw can always bind root params 2/3 whichever PSO is selected.
    for (UINT i = 0; i < kFrames; i++) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        rd.Width = sizeof(AcidBlobGPU) * kAcidMaxBlobs;
        HR(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&m_acidBlobUpload[i])));
        rd.Width = (sizeof(AcidParamsGPU) + 255) & ~255u;
        HR(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&m_acidParamUpload[i])));
        // shared ink-in-water parameter block (b2) â€” 112 bytes, same ring
        rd.Width = (sizeof(InkParamsGPU) + 255) & ~255u;
        HR(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&m_inkParamUpload[i])));
        // droplet particle sim: 128 KB of particles + 18 KB of cell table per
        // frame. Created unconditionally, like the blob ring, so the display
        // draw can always bind root params 7/8 whichever PSO is selected â€”
        // and zeroed, so with the sim off every cell reports a count of 0.
        // TWO records per droplet: [k] is the particle itself and
        // [kAcidMaxDrops + k] the RING SHAPE (see UploadAcidConstants). Split
        // that way rather than interleaved so the hot loop, which reads only
        // the first half, keeps four droplets to a cache line; the second
        // fetch happens inside the ring branch alone.
        // Oil3D C3: + a TAIL for the motes (two records each after both halves,
        // and their own 160x90 cell table after the droplets' 64x36): the root
        // signature is full, so they ride these two buffers. Written only while
        // motes > 0 and read only by the OIL_MOTES PSOs; the droplets' part is
        // untouched.
        rd.Width = sizeof(AcidDropGPU) * (kAcidMaxDrops * 2 + kMoteMax * 2);
        HR(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&m_dropletUpload[i])));
        rd.Width = sizeof(DropCellGPU) * (kDropGridW * kDropGridH + kMoteGridW * kMoteGridH);
        HR(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&m_dropletCellUpload[i])));
        D3D12_RANGE none = { 0, 0 };
        HR(m_acidBlobUpload[i]->Map(0, &none, &m_acidBlobData[i]));
        HR(m_acidParamUpload[i]->Map(0, &none, &m_acidParamData[i]));
        HR(m_inkParamUpload[i]->Map(0, &none, &m_inkParamData[i]));
        HR(m_dropletUpload[i]->Map(0, &none, &m_dropletData[i]));
        HR(m_dropletCellUpload[i]->Map(0, &none, &m_dropletCellData[i]));
        memset(m_acidBlobData[i], 0, sizeof(AcidBlobGPU) * kAcidMaxBlobs);
        memset(m_acidParamData[i], 0, sizeof(AcidParamsGPU));
        memset(m_inkParamData[i], 0, sizeof(InkParamsGPU));
        memset(m_dropletData[i], 0, sizeof(AcidDropGPU) * (kAcidMaxDrops * 2 + kMoteMax * 2));
        memset(m_dropletCellData[i], 0, sizeof(DropCellGPU) * (kDropGridW * kDropGridH + kMoteGridW * kMoteGridH));
    }
}

// Seed the oil population. Four kinds, matching the references:
//   disc   â€” big flat saturated discs      (liquid-acid-ref-1)
//   web    â€” medium blobs seeded in CHAINS so they merge into veined webs
//            with circular openings        (liquid-acid-ref-2)
//   bubble â€” small round blobs, power-law sizes, some clustered
//            (liquid-acid-ref-3)
//   hole   â€” NEGATIVE weight, seeded inside a disc/web parent so it eats a
//            round hole out of the oil. Harmless when it drifts into open
//            ink: a negative field just stays below the isoline.
void FluidRenderer::SeedAcidBlobs() {
    const LiquidAcidConfig& a = m_cfg.acid;
    const int n = m_cfg.acid.blobCount < 1 ? 1 :
                  (m_cfg.acid.blobCount > kAcidMaxBlobs ? kAcidMaxBlobs : m_cfg.acid.blobCount);
    AcidRng rng(g_randSeed ? (g_randSeed * 2654435761u) ^ 0xAC1Du : GetTickCount());
    const float TWO_PI = 6.2831853f;

    int nDisc   = (int)(n * a.discFrac   + 0.5f);
    int nWeb    = (int)(n * a.webFrac    + 0.5f);
    int nBubble = (int)(n * a.bubbleFrac + 0.5f);
    if (nDisc + nWeb + nBubble > n - 1) nBubble = n - 1 - nDisc - nWeb;
    if (nBubble < 0) nBubble = 0;
    int nHole = n - nDisc - nWeb - nBubble;

    m_acidBlobs.assign((size_t)n, AcidBlob{});
    // power-law-ish size draw: pow(U, bias) with bias > 1 crowds the small end,
    // which is what gives "bubbles of every size" instead of one modal radius
    // pow(U, bias): bias > 1 crowds the small end (bubbles/holes, "every
    // size"), bias < 1 crowds the LARGE end (discs and webs, which the
    // references show filling most of the frame).
    auto drawR = [&](float lo, float hi, float bias) {
        return lo + (hi - lo) * powf(rng.f(), fmaxf(bias, 0.05f));
    };
    auto setCol = [&](AcidBlob& b, int idx) { b.colIdx = idx & 3; };
    auto place = [&](AcidBlob& b, float x, float y) {
        b.x = x; b.y = y; b.vx = b.vy = 0.0f;
        b.phase = rng.f(0.0f, TWO_PI);
        b.breathRate = rng.f(0.08f, 0.32f);
        b.s1 = rng.f(0.0f, TWO_PI);
        b.s2 = rng.f(0.0f, TWO_PI);
    };

    int idx = 0;
    for (int k = 0; k < nDisc && idx < n; k++, idx++) {
        AcidBlob& b = m_acidBlobs[idx];
        b.kind = 0; b.wgt = 1.0f;
        b.baseR = drawR(a.discMin, a.discMax, a.bigBias);
        setCol(b, (k % 3 == 0) ? 2 : 0);
        place(b, rng.f(0.03f, 0.97f), rng.f(0.03f, 0.97f));
    }
    // webs: a random walk of touching blobs makes connected veins with
    // circular openings between them, not a field of separate dots
    float cx = rng.f(0.15f, 0.85f), cy = rng.f(0.15f, 0.85f), prevR = 0.06f;
    for (int k = 0; k < nWeb && idx < n; k++, idx++) {
        AcidBlob& b = m_acidBlobs[idx];
        b.kind = 1; b.wgt = 1.0f;
        b.baseR = drawR(a.webMin, a.webMax, a.bigBias);
        setCol(b, (k % 4 == 0) ? 0 : 1);
        if (k % 9 == 0) { cx = rng.f(0.10f, 0.90f); cy = rng.f(0.10f, 0.90f); }
        else {
            float ang = rng.f(0.0f, TWO_PI);
            float step = (prevR + b.baseR) * rng.f(0.75f, 1.25f);
            cx += cosf(ang) * step;
            cy += sinf(ang) * step;
            cx = fminf(fmaxf(cx, -0.05f), 1.05f);
            cy = fminf(fmaxf(cy, -0.05f), 1.05f);
        }
        prevR = b.baseR;
        place(b, cx, cy);
    }
    for (int k = 0; k < nBubble && idx < n; k++, idx++) {
        AcidBlob& b = m_acidBlobs[idx];
        b.kind = 2; b.wgt = 1.0f;
        b.baseR = drawR(a.bubbleMin, a.bubbleMax, a.sizeBias);
        setCol(b, (k % 3 == 0) ? 1 : 3);
        if (k > 0 && rng.f() < 0.45f) {          // cluster on an earlier bubble
            // same draw as ever; only an index before the first blob is clamped (with
            // disc + web <= 1 it read heap bytes before the vector: review 2026-10-04 R12)
            int parIdx = idx - 1 - (int)(rng.f() * 3.0f) % 3;
            if (parIdx < 0) parIdx = 0;
            const AcidBlob& par = m_acidBlobs[parIdx];
            float ang = rng.f(0.0f, TWO_PI);
            float d = (par.baseR + b.baseR) * rng.f(1.15f, 4.0f);
            place(b, par.x + cosf(ang) * d, par.y + sinf(ang) * d);
        } else {
            place(b, rng.f(0.02f, 0.98f), rng.f(0.02f, 0.98f));
        }
    }
    const int positives = nDisc + nWeb;
    for (int k = 0; k < nHole && idx < n; k++, idx++) {
        AcidBlob& b = m_acidBlobs[idx];
        b.kind = 3; b.wgt = -fmaxf(a.holeWeight, 0.05f);
        b.baseR = drawR(a.holeMin, a.holeMax, a.sizeBias);
        setCol(b, 0);                            // unused: holes never fill
        if (positives > 0) {
            const AcidBlob& par = m_acidBlobs[(int)(rng.f() * positives) % positives];
            // Sub-scale to the parent, or the hole swallows the blob it sits
            // in and the boundary between them turns to gravel.
            b.baseR = fminf(b.baseR, par.baseR * rng.f(0.20f, 0.55f));
            float ang = rng.f(0.0f, TWO_PI);
            float d = (par.baseR - b.baseR) * rng.f(0.0f, 0.80f);
            place(b, par.x + cosf(ang) * d, par.y + sinf(ang) * d);
        } else {
            place(b, rng.f(0.05f, 0.95f), rng.f(0.05f, 0.95f));
        }
    }
    // The seeding above already spreads every kind over the FULL height, which
    // is exactly the stagger rise_respawn needs: the column is populated from
    // the first frame and never empties, instead of one batch marching up
    // together and leaving a bare screen behind it.
    // brief AG-b: a fresh dye identity per blob (hash of a serial, no RNG draw).
    m_acidDyeSalt   = g_randSeed ? g_randSeed * 2246822519u : GetTickCount();
    m_acidDyeSerial = 0;
    for (size_t bi = 0; bi < m_acidBlobs.size(); bi++) {
        m_acidBlobs[bi].rTarget = m_acidBlobs[bi].baseR;   // nothing pending
        m_acidBlobs[bi].dyeId   = NextAcidDyeId();
    }
    m_acidRespawnRng = rng.next() | 1u;
    // brief BZ blob_turnover_s: the pick's own stream, from the seed alone (no
    // draw from `rng`, so every other stream is untouched), and a fresh period.
    m_acidTurnRng = ((g_randSeed ? g_randSeed * 0x85EBCA6Bu : GetTickCount()) ^ 0x7B1DB00Cu) | 1u;
    m_acidTurnAcc = 0.0f;
    m_acidWantBlobs = n;
    m_acidSeeded = true;
}

// ---------------------------------------------------------------------------
// blob_count, smoothly (brief S residue). Moving the slider used to call
// SeedAcidBlobs(), which replaces the whole population in one frame: every
// mass on screen vanishes and a new field appears. Under conserve_mass the
// population WALKS to the new count instead -- the surplus shrinks away over
// dissolve_s (preferring blobs that are already off-frame, so most of it is
// never seen at all), and a shortfall is grown in from under the bottom edge,
// through the same door rise_respawn brings a blob back through.
// ---------------------------------------------------------------------------
void FluidRenderer::AdjustAcidBlobCount(int want) {
    const LiquidAcidConfig& a = m_cfg.acid;
    auto rf = [&]() {
        m_acidRespawnRng ^= m_acidRespawnRng << 13;
        m_acidRespawnRng ^= m_acidRespawnRng >> 17;
        m_acidRespawnRng ^= m_acidRespawnRng << 5;
        return (m_acidRespawnRng >> 8) * (1.0f / 16777216.0f);
    };
    const float TWO_PI = 6.2831853f;
    int live = 0;
    for (size_t i = 0; i < m_acidBlobs.size(); i++)
        if (m_acidBlobs[i].rTarget > 0.0f) live++;

    // ---- too many: retire the least visible first ----------------------
    while (live > want) {
        int best = -1; float bestScore = -1e9f;
        for (size_t i = 0; i < m_acidBlobs.size(); i++) {
            const AcidBlob& b = m_acidBlobs[i];
            if (b.rTarget <= 0.0f) continue;
            // off-frame beats on-frame, and small beats large: whoever is
            // cheapest to lose without anyone noticing.
            const float off = fmaxf(fmaxf(-b.y, b.y - 1.0f), 0.0f);
            const float score = off * 10.0f - b.baseR;
            if (score > bestScore) { bestScore = score; best = (int)i; }
        }
        if (best < 0) break;
        m_acidBlobs[(size_t)best].rTarget = 0.0f;
        live--;
    }

    // ---- too few: grow one in from under the bottom edge ----------------
    while (live < want && (int)m_acidBlobs.size() < kAcidMaxBlobs) {
        AcidBlob b{};
        // kind by the authored mix, the same fractions SeedAcidBlobs uses
        const float u = rf();
        float lo, hi, bias;
        if      (u < a.discFrac)                 { b.kind = 0; b.wgt = 1.0f; lo = a.discMin;   hi = a.discMax;   bias = a.bigBias;  b.colIdx = 0; }
        else if (u < a.discFrac + a.webFrac)     { b.kind = 1; b.wgt = 1.0f; lo = a.webMin;    hi = a.webMax;    bias = a.bigBias;  b.colIdx = 1; }
        else if (u < a.discFrac + a.webFrac + a.bubbleFrac)
                                                 { b.kind = 2; b.wgt = 1.0f; lo = a.bubbleMin; hi = a.bubbleMax; bias = a.sizeBias; b.colIdx = 3; }
        else                                     { b.kind = 3; b.wgt = -fmaxf(a.holeWeight, 0.05f);
                                                   lo = a.holeMin; hi = a.holeMax; bias = a.sizeBias; b.colIdx = 0; }
        b.rTarget = lo + (hi - lo) * powf(rf(), fmaxf(bias, 0.05f));
        // Born at a size nobody can resolve and swollen over spawn_grow_s on
        // the way in, so even a blob grown into the middle of the frame (no
        // rise, so no bottom edge to come through) never appears at a size.
        b.baseR = b.rTarget * 0.02f;
        b.x = rf();
        b.y = (a.riseSpeed > 1e-6f && a.riseRespawn)
            ? 1.0f + b.rTarget * a.supportScale * (1.0f + a.breathAmt) * 0.86f
            : rf();
        b.phase = rf() * TWO_PI;
        b.breathRate = 0.08f + rf() * 0.24f;
        b.s1 = rf() * TWO_PI;
        b.s2 = rf() * TWO_PI;
        b.dyeId = NextAcidDyeId();                // brief AG-b: a new blob, a new identity
        b.born = m_time;                          // brief BZ: read only by the turnover pick
        m_acidBlobs.push_back(b);
        live++;
    }
}

// ---------------------------------------------------------------------------
// brief BZ BLACK-MASS: blob_turnover_s. The kind keys only shape blobs that are
// CREATED, so a live change of disc/web/bubble_frac etc. never reached a
// standing population. Turnover retires one blob every
// blob_turnover_s / blob_count seconds -- rTarget 0, so it dissolves over
// dissolve_s exactly as a blob_count decrease does -- and the conserve_mass
// walk in Frame() grows one in its place from the CURRENT keys. The pick
// (pre-flight item 4): a candidate is a live blob that has finished growing
// (baseR >= 0.9 rTarget; else the 2%-radius blob just grown in under the edge
// would be retired every time); among the DUE ones (the oldest cohort, or at
// least half a turnover period old -- else a fresh grow-in parked under the
// bottom edge, which is "clear of the frame", would be retired before it is
// ever seen) a blob whose field is clear of the frame wins, then the oldest,
// then an index HASH on the private stream (the seeded population is all
// born 0 in KIND order, and index order would retire every disc first).
// Hazard 5a: kAcidMaxBlobs counts the retirees still dissolving, and grow-in
// stops at 128, so while the vector holds >= 127 the retire waits (the
// accumulator is held at one period, so a wait never turns into a burst).
// ---------------------------------------------------------------------------
void FluidRenderer::StepAcidTurnover(float dt, int want) {
    const LiquidAcidConfig& a = m_cfg.acid;
    if (a.conserveMass <= 0.5f || a.blobTurnoverS <= 0.0f) return;   // hazard 5e
    const float period = a.blobTurnoverS / (float)(want > 0 ? want : 1);
    m_acidTurnAcc += dt;
    if (m_acidTurnAcc < period) return;
    if ((int)m_acidBlobs.size() >= kAcidMaxBlobs - 1) {
        if (!m_acidTurnBlockedLogged) {
            printf("[turnover] blocked t=%.3f: %d blobs incl. retirees (cap %d) -- "
                   "the retire waits for a dissolve to finish\n",
                   m_time, (int)m_acidBlobs.size(), kAcidMaxBlobs);
            m_acidTurnBlockedLogged = true;
        }
        m_acidTurnAcc = period;
        return;
    }
    m_acidTurnRng ^= m_acidTurnRng << 13;
    m_acidTurnRng ^= m_acidTurnRng >> 17;
    m_acidTurnRng ^= m_acidTurnRng << 5;
    const uint32_t salt = m_acidTurnRng;
    auto hashIdx = [salt](uint32_t i) {
        uint32_t h = (i * 0x9E3779B9u) ^ salt;
        h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
        return h;
    };
    const float aspect = (float)m_width / fmaxf((float)m_height, 1.0f);
    float oldest = 3.4e38f;
    for (size_t i = 0; i < m_acidBlobs.size(); i++) {
        const AcidBlob& b = m_acidBlobs[i];
        if (b.rTarget > 0.0f && b.baseR >= 0.9f * b.rTarget && b.born < oldest) oldest = b.born;
    }
    const float dueBy = m_time - 0.5f * a.blobTurnoverS;
    int best = -1; int bestClear = 0; float bestBorn = 0.0f; uint32_t bestHash = 0;
    for (size_t i = 0; i < m_acidBlobs.size(); i++) {
        const AcidBlob& b = m_acidBlobs[i];
        if (b.rTarget <= 0.0f || b.baseR < 0.9f * b.rTarget) continue;
        if (!(b.born <= oldest || b.born <= dueBy)) continue;
        const float supOut = b.baseR * a.supportScale * (1.0f + a.breathAmt) * 0.86f;
        const float supX = supOut / fmaxf(aspect, 1e-4f);
        const int clear = (b.y < -supOut || b.y > 1.0f + supOut ||
                           b.x < -supX   || b.x > 1.0f + supX) ? 1 : 0;
        const uint32_t h = hashIdx((uint32_t)i);
        bool better = best < 0;
        if (!better) {
            if (clear != bestClear)      better = clear > bestClear;
            else if (b.born != bestBorn) better = b.born < bestBorn;
            else                         better = h < bestHash;
        }
        if (better) { best = (int)i; bestClear = clear; bestBorn = b.born; bestHash = h; }
    }
    if (best < 0) { m_acidTurnAcc = period; return; }   // everyone still growing in
    AcidBlob& r = m_acidBlobs[(size_t)best];
    printf("[turnover] retire t=%.3f blob=%d kind=%d r=%.4f born=%.1f clear=%d y=%.3f\n",
           m_time, best, r.kind, r.baseR, r.born, bestClear, r.y);
    r.rTarget = 0.0f;
    m_acidTurnAcc -= period;
    if (m_acidTurnAcc > period) m_acidTurnAcc = period;
}

// 64x36 velocity downsample -> CPU, one frame late (same trick as
// UpdateCoverage). Headless blocks on the fence so a --shot replays exactly.
void FluidRenderer::UpdateVelocityReadback() {
    if (!m_velLow.res || !m_velReadback) return;
    // 30 Hz is plenty for advecting blobs and keeps the headless path (which
    // blocks on the fence for determinism) from serialising every frame.
    const float kVelPeriod = 1.0f / 30.0f;
    if (m_headless && m_velPending && m_time - m_lastVelTime >= kVelPeriod &&
        m_fence->GetCompletedValue() < m_velFence) {
        HR(m_fence->SetEventOnCompletion(m_velFence, m_fenceEvent));
        if (!m_deviceLost) WaitForSingleObject(m_fenceEvent, INFINITE);   // R3
    }
    if (m_velPending && m_fence->GetCompletedValue() >= m_velFence &&
        (!m_headless || m_time - m_lastVelTime >= kVelPeriod)) {
        uint8_t* data = nullptr;
        D3D12_RANGE range = { 0, (SIZE_T)m_velPitch * kVelH };
        if (!m_cfg.drops.pushFilm) {
            // [drops] push_film = 0 (ag-ink): the CPU copy stays at rest, so the
            // blobs, the droplets and the hue field ride still water whatever
            // the ink drops stir up (the GPU copy, VelLow, still drives the
            // ink's own motion gate). NOTE: this freezes the look's own
            // currents too (oil drag edge push), so the film is not today's.
            for (float& v : m_velCpu) v = 0.0f;
        } else if (SUCCEEDED(m_velReadback->Map(0, &range, (void**)&data))) {
            for (int y = 0; y < kVelH; y++) {
                const uint16_t* row = (const uint16_t*)(data + (SIZE_T)y * m_velPitch);
                for (int x = 0; x < kVelW; x++) {
                    m_velCpu[((size_t)y * kVelW + x) * 2 + 0] = HalfToFloat(row[x * 4 + 0]);
                    m_velCpu[((size_t)y * kVelW + x) * 2 + 1] = HalfToFloat(row[x * 4 + 1]);
                }
            }
            D3D12_RANGE none = { 0, 0 };
            m_velReadback->Unmap(0, &none);
        }
        m_velPending = false;
    }
    if (m_velPending || m_time - m_lastVelTime < kVelPeriod) return;
    m_lastVelTime = m_time;

    SimCB cb = {};
    m_cmd->SetComputeRootSignature(m_computeRS.Get());
    Transition(*m_velocity.read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(m_velLow, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_cmd->SetPipelineState(m_psoDownsample.Get());
    cb.dimsW = kVelW; cb.dimsH = kVelH;
    m_cmd->SetComputeRoot32BitConstants(0, sizeof(SimCB) / 4, &cb, 0);
    m_cmd->SetComputeRootDescriptorTable(1, m_velocity.read->srv);
    m_cmd->SetComputeRootDescriptorTable(2, m_velocity.read->srv);
    m_cmd->SetComputeRootDescriptorTable(3, m_velLow.uav);
    m_cmd->SetComputeRootDescriptorTable(4, m_velLow.uav);
    m_cmd->SetComputeRootDescriptorTable(5, m_velLow.uav);
    m_cmd->SetComputeRootDescriptorTable(6, m_velocity.read->srv);
    m_cmd->Dispatch(Groups(kVelW), Groups(kVelH), 1);

    Transition(m_velLow, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
    src.pResource = m_velLow.res.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = m_velReadback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    dst.PlacedFootprint.Footprint.Width = kVelW;
    dst.PlacedFootprint.Footprint.Height = kVelH;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = m_velPitch;
    m_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    m_velFence = m_nextFence;      // EndFrameAndPresent signals this value
    m_velPending = true;
}

// DIAGNOSTIC ONLY -- see the declaration in fluid.h. Read-only.
void FluidRenderer::DumpAcidCsv(const wchar_t* path) const {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;
    fprintf(f, "# t=%.3f w=%d h=%d simW=%d simH=%d\n",
            m_time, m_width, m_height, m_simW, m_simH);
    fprintf(f, "kind,i,x,y,r,wgt,vx,vy,extra\n");
    for (size_t i = 0; i < m_acidBlobs.size(); i++) {
        const AcidBlob& b = m_acidBlobs[i];
        fprintf(f, "blob,%d,%.6f,%.6f,%.6f,%.4f,%.6f,%.6f,%d\n",
                (int)i, b.x, b.y, b.baseR, b.wgt, b.vx, b.vy, b.kind);
    }
    // brief AG-b: the blob records exactly as the shader read them on the
    // captured frame: x, y (uv), r = breathing radius, wgt, vx = rise_stretch
    // anisotropy (b.w), vy = dye identity (c.w), extra = comb active.
    for (size_t i = 0; i + 12 <= m_acidBlobGpuCopy.size(); i += 12) {
        const float* g = &m_acidBlobGpuCopy[i];
        fprintf(f, "gblob,%d,%.6f,%.6f,%.6f,%.4f,%.6f,%.6f,%d\n",
                (int)(i / 12), g[0], g[1], g[2], g[3], g[7], g[11], g[8] > 0.0f ? 1 : 0);
    }
    for (size_t i = 0; i < m_acidDrops.size(); i++) {
        const AcidDrop& d = m_acidDrops[i];
        fprintf(f, "drop,%d,%.6f,%.6f,%.6f,%.4f,%.6f,%.6f,%d\n",
                (int)i, d.x, d.y, d.r, d.gate, d.vx, d.vy,
                d.kind * 100 + d.ring * 10 + d.racer);
    }
    // brief AE-c: the hue2 mix field, visible AND hidden rows, so the
    // upward-provenance check can run on the field itself.
    if (!m_mixField.empty()) {
        const int TH = (int)(m_mixField.size() / kMixW);
        for (int yy = 0; yy < TH; yy++)
            for (int xx = 0; xx < kMixW; xx++)
                fprintf(f, "mix,%d,%d,%d,%.6f,0,0,0,%d\n", yy * kMixW + xx, xx, yy,
                        m_mixField[(size_t)yy * kMixW + xx], (yy < kMixH) ? 1 : 0);
    }
    for (int y = 0; y < kVelH; y++)
        for (int x = 0; x < kVelW; x++)
            fprintf(f, "vel,%d,%d,%d,0,0,%.6f,%.6f,0\n", y * kVelW + x, x, y,
                    m_velCpu[((size_t)y * kVelW + x) * 2 + 0],
                    m_velCpu[((size_t)y * kVelW + x) * 2 + 1]);
    fclose(f);
}

// ===========================================================================
// THE HUE2 MIX FIELD (brief AE)
//
// One scalar per cell, 0..1, saying how far the film at that point has gone
// toward the second dye hue. Three things happen to it every frame:
//
//   1. ADVECTION. Semi-Lagrangian off the same 64x36 velocity readback the
//      blobs ride: each cell looks BACK along the flow and takes what was
//      there. That is what makes a patch stretch and fold with the sim
//      instead of sliding over it as an overlay would, which is the whole
//      reason the brief asks for t3 rather than a scrolling texture.
//   2. INJECTION. Slow large-scale value noise at the patch scale, added
//      toward its own target, so new patches keep arriving.
//   3. DECAY toward a base of 0.5. Without it, advection plus injection
//      drifts the whole field to one value over minutes and the second hue
//      stops being patches and becomes a flat tint over everything.
//
// The noise PHASE jumps on the rig's readjustment (the focus spring), so the
// patches re-lay at the same moment the lamp, the lid and the focus move --
// nothing in this look is allowed to sit still on the panel.
// ===========================================================================
bool FluidRenderer::Hue2FieldRange(float& mn, float& mx) const {
    const size_t NV = (size_t)kMixW * kMixH;          // the visible rows come first
    if (!m_mixSeeded || m_mixField.size() < NV) return false;
    mn = 1.0f; mx = 0.0f;
    for (size_t i = 0; i < NV; i++) { mn = fminf(mn, m_mixField[i]); mx = fmaxf(mx, m_mixField[i]); }
    return true;
}

// The share of the VISIBLE cells at or above thr (the chapters wait on how much of the new dye has risen in).
bool FluidRenderer::Hue2FieldShare(float thr, float& share) const {
    const size_t NV = (size_t)kMixW * kMixH;
    if (!m_mixSeeded || m_mixField.size() < NV) return false;
    size_t n = 0;
    for (size_t i = 0; i < NV; i++) n += (m_mixField[i] >= thr) ? 1 : 0;
    share = (float)n / (float)NV;
    return true;
}

void FluidRenderer::StepHueField(float dt) {
    const LiquidAcidConfig& a = m_cfg.acid;
    // ---- OFF-SCREEN GENERATION (user's standing rule) --------------------
    // "colours still appear out of thin air. Everything needs to be generated
    // off screen, and within the screen needs to just move up."
    // So the grid is TALLER than the screen: rows 0..kMixH-1 are the visible
    // frame and the rows under them are below the bottom edge. New patch
    // material is made ONLY in those hidden rows. A visible cell is pure
    // advection -- it takes what was below it and nothing else -- so no patch
    // can form, brighten or re-lay inside the frame. The readjustment does
    // not re-lay either: it changes the phase and scale of what is being made
    // below the edge, so the composition still turns over minutes and every
    // bit of it walks in from underneath.
    const int SR = (a.filmHue2SeedRows <= 0.5f) ? 0
                 : (int)fminf(fmaxf(a.filmHue2SeedRows, 1.0f), (float)kMixMaxSeed);
    const int TH = kMixH + SR;               // total rows, visible + hidden
    const int N  = kMixW * TH;
    if ((int)m_mixField.size() != N) { m_mixField.assign((size_t)N, 0.5f); m_mixSeeded = false; }
    // Oil3D L3 (2): fluid B's dyes live on this field too, hue2 on or off
    if (a.filmHue2Amt <= 0.0005f && a.filmHue3Amt <= 0.0005f && !a.FluidBWanted()) return;
    if (dt <= 0.0f) return;

    auto vhash = [](int xi, int yi, int zi) {
        uint32_t h = (uint32_t)(xi * 374761393) ^ (uint32_t)(yi * 668265263)
                   ^ (uint32_t)(zi * 2246822519u);
        h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
        return (h >> 8) * (1.0f / 16777216.0f);
    };
    auto vnoise = [&](float x, float y, int z) {
        const int xi = (int)floorf(x), yi = (int)floorf(y);
        const float fx = x - xi, fy = y - yi;
        const float ux = fx * fx * (3.0f - 2.0f * fx), uy = fy * fy * (3.0f - 2.0f * fy);
        const float a00 = vhash(xi, yi, z),     a10 = vhash(xi + 1, yi, z);
        const float a01 = vhash(xi, yi + 1, z), a11 = vhash(xi + 1, yi + 1, z);
        const float b0 = a00 + (a10 - a00) * ux;
        const float b1 = a01 + (a11 - a01) * ux;
        return b0 + (b1 - b0) * uy;
    };

    m_mixPhase += (m_mixPhaseTarget - m_mixPhase) * (1.0f - expf(-dt / 1.6f));

    const float scale = fminf(fmaxf(a.filmHue2Scale, 0.08f), 0.90f);
    const float drift = fmaxf(a.filmHue2Drift, 0.0f);
    const float decay = fmaxf(a.filmHue2Decay, 0.0f);
    // The readjustment also nudges the SCALE of the next material, so a new
    // spell of patches is a different size as well as a different layout.
    const float freq  = (1.0f / fmaxf(scale, 1e-3f))
                      * (1.0f + 0.18f * sinf(m_mixPhase * 0.7f));
    const float t     = AnimatorTime(1) * 0.03f * drift + m_mixPhase;
    // extra rise, authored in screen heights per minute
    const float riseX = fmaxf(a.filmHue2Rise, 0.0f) / 60.0f;

    const float aspect = (float)m_width / fmaxf((float)m_height, 1.0f);
    // One step of one field. The live field and the coverage logger's shadow
    // fields (--cover-sweep) run the SAME body; only the bias differs.
    // brief BW: fq (the noise frequency, i.e. the patch size) and zo (the
    // noise lattice seed) are parameters so the logger's shadow fields can run
    // other scales and independent layouts; the live field passes freq and 0,
    // the same values as before.
    // film_hue2_refresh: the REFRESH POUR's clock (the live field only; held while the director drives the cover,
    // whose pour steps wait on this field's range)
    bool pourOn = false;
    if (a.filmHue2Refresh > 0.5f && SR > 0 && m_mixSeeded && !m_refreshHold) {
        auto rnd = [this] { m_refreshRng = m_refreshRng * 1664525u + 1013904223u; return (m_refreshRng >> 8) * (1.0f / 16777216.0f); };
        if (m_refreshNext < 0.0f) m_refreshNext = a.filmHue2Refresh * (0.5f + rnd());
        if (m_refreshLeft > 0.0f) {
            m_refreshLeft -= dt;
            pourOn = m_refreshLeft > 0.0f;
        } else if ((m_refreshNext -= dt) <= 0.0f) {
            const int w = 2 + (int)(rnd() * 4.999f);                 // 2..6 of the kMixW columns
            m_refreshX0 = (int)(rnd() * (float)(kMixW - w + 1) * 0.999f);
            m_refreshX1 = m_refreshX0 + w - 1;
            m_refreshVal = rnd() < 0.7f ? 1.0f : 0.0f;               // the patch dye, or a block of the film's own
            m_refreshLeft = 8.0f + 12.0f * rnd();
            m_refreshNext = a.filmHue2Refresh * (0.5f + rnd());
            pourOn = true;
        }
    } else {
        m_refreshLeft = 0.0f;
    }
    // film_hue2_lobes (T8): the live field's pour through a few widening lobes (0 = today: nothing here runs)
    const float lobes = fminf(fmaxf(a.filmHue2Lobes, 0.0f), 1.0f);
    float lobeGrow = -1.0f;                      // < 0 = no lobes this step
    if (lobes > 0.0f && SR > 0 && m_mixSeeded) {
        if (a.filmHue2Cover > 0.3f) {
            if (m_lobeAge < 0.0f) {                  // a pour starts: a fresh layout from the lobes' own stream
                m_lobeRng = m_lobeRng * 1664525u + 1013904223u;
                m_lobeOff = (m_lobeRng >> 8) * (1.0f / 16777216.0f) * 97.0f;
                m_lobeAge = 0.0f;
            }
            m_lobeAge += dt;
            lobeGrow = fminf(m_lobeAge / 45.0f, 1.0f);
        } else {
            m_lobeAge = -1.0f;
        }
    }
    // T8 connections: SetHue2Shapes (0 = off: nothing below reads it)
    const bool shaped = (m_shapeLo > 0.0f || m_shapeHi > 0.0f || m_shapeHold > 0.0f) && SR > 0 && m_mixSeeded;
    if (shaped) m_shapeClock += dt;
    auto stepField = [&](std::vector<float>& field, bool seeding, float bias, float fq, int zo, bool pour) {
        std::vector<float> next((size_t)N, 0.0f);
        const bool lobed = lobeGrow >= 0.0f && &field == &m_mixField;
        for (int y = 0; y < TH; y++) {
            for (int x = 0; x < kMixW; x++) {
                const float u = ((float)x + 0.5f) / (float)kMixW;
                const float v = ((float)y + 0.5f) / (float)kMixH;   // >1 = below the edge
                float vu = 0.0f, vv = 0.0f;
                {
                    float fx = u * kVelW - 0.5f, fy = fminf(v, 1.0f) * kVelH - 0.5f;
                    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
                    float tx = fx - x0, ty = fy - y0;
                    auto at = [&](int xi, int yi, int c) {
                        xi = xi < 0 ? 0 : (xi >= kVelW ? kVelW - 1 : xi);
                        yi = yi < 0 ? 0 : (yi >= kVelH ? kVelH - 1 : yi);
                        return m_velCpu[((size_t)yi * kVelW + xi) * 2 + c];
                    };
                    vu = (at(x0, y0, 0) * (1 - tx) + at(x0 + 1, y0, 0) * tx) * (1 - ty)
                       + (at(x0, y0 + 1, 0) * (1 - tx) + at(x0 + 1, y0 + 1, 0) * tx) * ty;
                    vv = (at(x0, y0, 1) * (1 - tx) + at(x0 + 1, y0, 1) * tx) * (1 - ty)
                       + (at(x0, y0 + 1, 1) * (1 - tx) + at(x0 + 1, y0 + 1, 1) * tx) * ty;
                    vu /= fmaxf((float)m_simW, 1.0f);
                    vv /= fmaxf((float)m_simH, 1.0f);
                }
                // ONLY UP. uv y is down, so the vertical velocity is clamped at or
                // below zero: a patch can be carried up fast or slow, sideways,
                // stretched -- but it can never travel down the screen, which is
                // the other half of the user's rule.
                float velV = vv - a.riseSpeed * 0.35f - riseX;
                if (lobed) {                 // T8: the lobes also rise faster (up to 1.5x lobes more), so the
                                             // front comes up as rounded plumes
                    const float n = vnoise(u * aspect * 1.6f + m_lobeOff, m_lobeAge * 0.02f, 977);
                    const float q = fminf(fmaxf((n - 0.35f) / 0.4f, 0.0f), 1.0f);
                    velV -= lobes * 1.5f * (a.riseSpeed * 0.35f + riseX) * q * q * (3.0f - 2.0f * q);
                }
                if (SR > 0 && velV > 0.0f) velV = 0.0f;
                const float bu = u - vu * dt;
                const float bv = v - velV * dt;          // never above v: from below
                float src;
                {
                    float fx = bu * kMixW - 0.5f, fy = bv * kMixH - 0.5f;
                    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
                    float tx = fx - x0, ty = fy - y0;
                    auto at = [&](int xi, int yi) {
                        xi = xi < 0 ? 0 : (xi >= kMixW ? kMixW - 1 : xi);
                        yi = yi < 0 ? 0 : (yi >= TH ? TH - 1 : yi);
                        return field[(size_t)yi * kMixW + xi];
                    };
                    src = (at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx) * (1 - ty)
                        + (at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx) * ty;
                }
                // Injection happens ONLY below the bottom edge (or everywhere when
                // film_hue2_seed_rows is 0, which is the old behaviour kept for
                // the A/B). On the first frame the whole grid is laid at once --
                // that is the initial condition of a cold start, before anything
                // is on screen, not something appearing in front of the user.
                const bool makeHere = (SR == 0) || (y >= kMixH) || seeding;
                if (pour && !seeding && y >= kMixH && x >= m_refreshX0 && x <= m_refreshX1) {
                    next[(size_t)y * kMixW + x] = m_refreshVal;   // the refresh pour: one dye, hard-edged, below the edge
                } else if (makeHere) {
                    const float nz  = vnoise(u * aspect * fq + t * 0.7f,
                                             v * fq - t * 0.5f, zo);
                    const float nz2 = vnoise(u * aspect * fq * 2.3f - t * 0.4f,
                                             v * fq * 2.3f + t * 0.3f, zo + 7);
                    float tgt = fminf(fmaxf(nz * 0.78f + nz2 * 0.22f, 0.0f), 1.0f);
                    // film_hue2_cover (FINAL-CYCLE C): the value noise has mean ~0.5
                    // and bilinear advection pulls the extremes toward it, so the
                    // share above the 0.62 seam shrinks on the way up (10-15% at
                    // 150 s). A bias on the TARGET moves the whole distribution:
                    // more material past the hue2 seam, less below the hue3 one.
                    // Still made only here, below the edge. 0 skips it (identity).
                    if (lobed) {             // T8: the bias only through the lobes, which widen until they meet
                        const float n = vnoise(u * aspect * 1.6f + m_lobeOff, m_lobeAge * 0.02f, 977);
                        const float th = 0.85f - 1.25f * lobeGrow;
                        const float L = fminf(fmaxf((n - th) / 0.16f + 0.5f, 0.0f), 1.0f);
                        const float mk = 1.0f - lobes * (1.0f - L * L * (3.0f - 2.0f * L));
                        tgt = fminf(fmaxf(tgt + bias * mk, 0.0f), 1.0f);
                    } else if (bias != 0.0f) tgt = fminf(fmaxf(tgt + bias, 0.0f), 1.0f);
                    if (shaped && &field == &m_mixField) {   // T8 connections: the next fluid as growing blobs
                        auto mask = [&](float amt, float off, int z) {      // amt 0: none .. 1: the whole row
                            if (!(amt > 0.0f)) return 0.0f;
                            // skewed lattice (x leans with y), so a blob's top is never a lattice row: no flat tops
                            const float ny = v * 2.2f - m_shapeClock * 0.06f;      // ~4 blobs across, ~0.45 screen tall
                            const float n = vnoise(u * aspect * 2.2f + 0.6f * ny + off, ny, z);
                            const float L = fminf(fmaxf((n - (1.0f - 1.2f * amt)) / 0.12f + 0.5f, 0.0f), 1.0f);
                            return L * L * (3.0f - 2.0f * L);
                        };
                        tgt += (fminf(tgt, 0.55f) - tgt) * mask(m_shapeHold, 31.3f, 995);   // the hold: no new patch
                        tgt += (0.0f - tgt) * mask(m_shapeLo, 13.7f, 991);  // the film end (the third hue at share 0)
                        tgt += (1.0f - tgt) * mask(m_shapeHi, 57.1f, 993);  // the patch end
                    }
                    const float k = seeding ? 1.0f : (1.0f - expf(-decay * dt));
                    next[(size_t)y * kMixW + x] = src + (tgt - src) * k;
                } else {
                    // Visible row: pure advection. Deliberately NO decay here --
                    // with the shipped 0.35 the relaxation time is under three
                    // seconds, so a patch that entered at the bottom would be
                    // gone before it had climbed a tenth of the screen and the
                    // feature would not exist. Decay now shapes the material
                    // while it is still being made, below the edge.
                    next[(size_t)y * kMixW + x] = src;
                }
            }
        }
        field.swap(next);
    };
    stepField(m_mixField, !m_mixSeeded, a.filmHue2Cover, freq, 0, pourOn);
    m_mixSeeded = true;
    // Coverage logger shadow fields: diagnostics only, empty unless the shot
    // run asked for a sweep, and nothing drawn ever reads them.
    if (!m_coverBias.empty()) {
        if (m_coverFields.size() != m_coverBias.size()) {
            m_coverFields.assign(m_coverBias.size(), std::vector<float>());
            m_coverSeeded = false;
        }
        for (auto& f : m_coverFields)
            if ((int)f.size() != N) { f.assign((size_t)N, 0.5f); m_coverSeeded = false; }
        for (size_t i = 0; i < m_coverFields.size(); i++) {
            float fq = freq;
            const float sc = (i < m_coverScale.size()) ? m_coverScale[i] : -1.0f;
            if (sc > 0.0f)
                fq = (1.0f / fmaxf(fminf(fmaxf(sc, 0.08f), 0.90f), 1e-3f))
                   * (1.0f + 0.18f * sinf(m_mixPhase * 0.7f));
            const int zo = (i < m_coverSeed.size()) ? m_coverSeed[i] * 1009 : 0;
            stepField(m_coverFields[i], !m_coverSeeded, m_coverBias[i], fq, zo, false);
        }
        m_coverSeeded = true;
    }
}

bool FluidRenderer::MixCoverage(int i, float out[10]) const {
    const std::vector<float>* f = &m_mixField;
    if (i >= 0) {
        if ((size_t)i >= m_coverFields.size()) return false;
        f = &m_coverFields[(size_t)i];
    }
    const int NV = kMixW * kMixH;          // the VISIBLE rows come first
    if ((int)f->size() < NV) return false;
    int c[7] = {};
    double sum = 0.0;
    for (int k = 0; k < NV; k++) {
        const float v = (*f)[(size_t)k];
        sum += v;
        if (v > 0.62f) c[0]++;
        if (v > 0.68f) c[1]++;
        if (v >= 0.56f && v <= 0.68f) c[2]++;
        if (v < 0.32f) c[3]++;
        if (v < 0.28f) c[4]++;
        if (v < 0.24f) c[5]++;
        if (v < 0.20f) c[6]++;
    }
    for (int k = 0; k < 7; k++) out[k] = (float)c[k] / (float)NV;
    out[7] = (float)(sum / NV);
    // patches: 4-connected groups of visible cells above the hue2 seam
    int lab[kMixW * kMixH];
    for (int k = 0; k < NV; k++) lab[k] = ((*f)[(size_t)k] > 0.62f) ? -1 : 0;
    int groups = 0, biggest = 0;
    int stack[kMixW * kMixH];
    for (int k = 0; k < NV; k++) {
        if (lab[k] != -1) continue;
        groups++;
        int sp = 0, n = 0;
        stack[sp++] = k; lab[k] = groups;
        while (sp > 0) {
            const int q = stack[--sp];
            n++;
            const int qx = q % kMixW, qy = q / kMixW;
            const int nb[4] = { qx > 0 ? q - 1 : -1, qx < kMixW - 1 ? q + 1 : -1,
                                qy > 0 ? q - kMixW : -1, qy < kMixH - 1 ? q + kMixW : -1 };
            for (int e = 0; e < 4; e++)
                if (nb[e] >= 0 && lab[nb[e]] == -1) { lab[nb[e]] = groups; stack[sp++] = nb[e]; }
        }
        if (n > biggest) biggest = n;
    }
    out[8] = (float)groups;
    out[9] = (float)biggest / (float)NV;
    return true;
}

void FluidRenderer::StepAcidBlobs(float dt) {
    const LiquidAcidConfig& a = m_cfg.acid;
    const int n = (int)m_acidBlobs.size();
    if (n == 0) return;
    const float aspect = (float)m_width / fmaxf((float)m_height, 1.0f);
    const float simTexX = 1.0f / fmaxf((float)m_simW, 1.0f);   // sim texels/s -> uv/s
    const float simTexY = 1.0f / fmaxf((float)m_simH, 1.0f);
    const float t = m_time;

    // ---- oil_viscosity: one key, several coupled effects ------------------
    // A thick liquid lags the water it floats on, accelerates slowly, coasts,
    // breathes and sways more slowly, and NECKS into a neighbour over seconds
    // instead of snapping to it. Every factor below is exactly 1 (or the
    // branch is skipped) at oil_viscosity 0, so an existing ini is unmoved.
    const float vis     = fminf(fmaxf(a.oilViscosity, 0.0f), 1.0f);
    const float visFlow = 1.0f - 0.55f * vis;    // flow + curl response
    const float breathS = 1.0f - 0.60f * vis;    // breathing rate
    // Relaxation RATE, not a drag coefficient: a lower rate is what makes the
    // blob lag its target velocity in both directions.
    const float dampRate = fmaxf(a.damping, 0.05f) / (1.0f + 2.0f * vis);
    // The rise wobble is a function of wallpaper time, so "slower" is a slower
    // clock, not a smaller amplitude.
    const float tw = (vis > 1e-4f) ? t * (1.0f - 0.5f * vis) : t;
    const float fg = a.flowGain * visFlow;
    const float cdrift = a.curlDrift * visFlow;

    // ---- rise_parallax ----------------------------------------------------
    // s = lerp(1, clamp(r/disc_max, 0.25, 1), rise_parallax). A small blob is
    // a FAR blob: it rises slowly, sways slowly and picks up less of the near
    // currents. Exactly 1 for every blob when the key is 0.
    const float par  = fminf(fmaxf(a.riseParallax, 0.0f), 1.0f);
    const float rRef = fmaxf(a.discMax, 1e-4f);
    const bool  cons0 = (a.conserveMass > 0.5f);

    // ---- mouse_oil_mode: the pointer's own velocity, uv/s -----------------
    // Recorded by HandleInput, differentiated here (HandleInput has no dt) and
    // smoothed so a single 144 Hz sample does not spike. Nothing in this block
    // touches the ink -- the oil modes are deliberately oil-only.
    const int mm = a.mouseOilMode;
    float ptrVx = 0.0f, ptrVy = 0.0f, ptrSp = 0.0f;
    if (mm > 0) {
        if (m_ptrMoved && m_ptrHave && dt > 1e-5f) {
            const float kk = 1.0f - expf(-12.0f * dt);
            m_ptrVx += ((m_ptrX - m_ptrPx) / dt - m_ptrVx) * kk;
            m_ptrVy += ((m_ptrY - m_ptrPy) / dt - m_ptrVy) * kk;
        } else {
            const float kk = 1.0f - expf(-6.0f * dt);
            m_ptrVx -= m_ptrVx * kk;
            m_ptrVy -= m_ptrVy * kk;
        }
        ptrVx = m_ptrVx; ptrVy = m_ptrVy;
        ptrSp = sqrtf(ptrVx * ptrVx + ptrVy * ptrVy);
    }
    m_ptrPx = m_ptrX; m_ptrPy = m_ptrY; m_ptrHave = true;
    const float mGain = fmaxf(a.mouseOilGain, 0.0f);
    const float mR    = fmaxf(a.mouseOilRadius, 0.01f);

    // bilinear sample of the low-res velocity grid, in uv/s
    auto sampleVel = [&](float x, float y, float& ox, float& oy) {
        float fx = x * kVelW - 0.5f, fy = y * kVelH - 0.5f;
        int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
        float tx = fx - x0, ty = fy - y0;
        auto at = [&](int xi, int yi, int c) {
            xi = xi < 0 ? 0 : (xi >= kVelW ? kVelW - 1 : xi);
            yi = yi < 0 ? 0 : (yi >= kVelH ? kVelH - 1 : yi);
            return m_velCpu[((size_t)yi * kVelW + xi) * 2 + c];
        };
        ox = (at(x0, y0, 0) * (1 - tx) + at(x0 + 1, y0, 0) * tx) * (1 - ty)
           + (at(x0, y0 + 1, 0) * (1 - tx) + at(x0 + 1, y0 + 1, 0) * tx) * ty;
        oy = (at(x0, y0, 1) * (1 - tx) + at(x0 + 1, y0, 1) * tx) * (1 - ty)
           + (at(x0, y0 + 1, 1) * (1 - tx) + at(x0 + 1, y0 + 1, 1) * tx) * ty;
        ox *= simTexX; oy *= simTexY;
    };

    for (int i = 0; i < n; i++) {
        AcidBlob& b = m_acidBlobs[i];
        // rise_parallax: this blob's whole motion budget, by size.
        float pscale = 1.0f;
        if (par > 1e-4f) {
            float rr = b.baseR / rRef;
            rr = rr < 0.25f ? 0.25f : (rr > 1.0f ? 1.0f : rr);
            pscale = 1.0f + (rr - 1.0f) * par;
        }
        float vu = 0, vv = 0;
        sampleVel(b.x, b.y, vu, vv);
        const float fgb = fg * pscale;
        float tx = vu * fgb;
        float ty = vv * fgb;

        // Mild analytic curl drift so the oil still creeps where the fluid is
        // quiet (divergence-free: blobs swirl instead of piling up).
        float a1 = 3.1f * b.x + 0.23f * t + b.s1;
        float a2 = 2.7f * b.y + 0.19f * t;
        float a3 = 5.3f * b.x - 0.11f * t;
        float a4 = 4.1f * b.y + 0.29f * t + b.s2;
        float dpsidx = 3.1f * cosf(a1) * cosf(a2) + 0.7f * 5.3f * cosf(a3) * cosf(a4);
        float dpsidy = -2.7f * sinf(a1) * sinf(a2) - 0.7f * 4.1f * sinf(a3) * sinf(a4);
        tx += dpsidy * cdrift;
        ty += -dpsidx * cdrift;

        // buoyancy (uv y is down, so "up" is negative); holes sink
        ty += (b.wgt > 0.0f ? -1.0f : 1.0f) * a.buoyancy * (b.baseR / 0.12f);

        // ---- LAVA LAMP rise (rise_speed / rise_wobble) -------------------
        // A constant upward drift on the TARGET velocity, so the damping
        // relaxation still smooths it and the fluid can still shove a blob
        // sideways. Holes are water, not oil: they climb at 0.7x, which is
        // what makes a bubble creep across the disc it sits in rather than
        // riding it like a painted dot.
        if (a.riseSpeed > 1e-6f) {
            // rise_parallax scales the climb itself: the small (far) blobs
            // take up to 4x as long to cross the frame as the big near ones,
            // which is the whole parallax cue.
            const float rs = a.riseSpeed * (b.wgt > 0.0f ? 1.0f : 0.7f) * pscale;
            ty -= rs;
            if (a.riseWobble > 1e-4f) {
                // two incommensurate sines on the blob's own curl phases:
                // long lazy sway, never in step with its neighbours.
                tx += a.riseWobble * rs *
                      (0.85f * sinf(0.17f * tw + b.s1) + 0.45f * sinf(0.29f * tw + b.s2));
            }
        }

        // ---- mouse_oil_mode = 1, PUSH ------------------------------------
        // Blobs inside the radius take the pointer's own velocity (kernel
        // weighted) plus a weak radial shove, so the cursor drags AND parts
        // the oil. Added to the TARGET velocity, so the damping relaxation
        // still smooths it and the oil keeps its own inertia; nothing here can
        // move, shrink or delete a blob.
        if (mm == 1 && ptrSp > 1e-5f) {
            const float dx = (b.x - m_ptrX) * aspect, dy = b.y - m_ptrY;
            const float d2 = dx * dx + dy * dy;
            if (d2 < mR * mR) {
                float kq = 1.0f - d2 / (mR * mR);
                kq = kq * kq;
                const float g = mGain * kq;
                tx += ptrVx * g;
                ty += ptrVy * g;
                const float d = sqrtf(d2);
                if (d > 1e-5f) {
                    const float pr = 0.05f * g * ptrSp / (ptrSp + 0.25f);
                    tx += (dx / d) * pr / aspect;
                    ty += (dy / d) * pr;
                }
            }
        }
        // ---- mouse_oil_mode = 2, COMB ------------------------------------
        // The pointer path is a marbling comb: blobs in a NARROW band along it
        // stretch along the drag direction (b.comb / b.cdx,b.cdy, uploaded in
        // AcidBlobGPU.c and turned into the shader's anisotropic kernel), so
        // the cursor leaves thin oil streaks that slowly round back up over
        // ~3 s. Small blobs are towed along the stroke as well.
        if (mm == 2) {
            if (b.comb > 0.0f) {
                b.comb -= b.comb * (1.0f - expf(-dt / 3.0f));
                if (b.comb < 1e-4f) { b.comb = 0.0f; b.cdx = 0.0f; b.cdy = 0.0f; }
            }
            if (ptrSp > 1e-4f) {
                const float ux = ptrVx * aspect, uy = ptrVy;
                const float ul = sqrtf(ux * ux + uy * uy) + 1e-9f;
                const float nx = ux / ul, ny = uy / ul;
                const float dx = (b.x - m_ptrX) * aspect, dy = b.y - m_ptrY;
                const float along = dx * nx + dy * ny;
                const float perp  = fabsf(-dx * ny + dy * nx);
                const float bw = mR * 0.35f;
                if (perp < bw && fabsf(along) < mR) {
                    float kq = (1.0f - perp / bw) * (1.0f - fabsf(along) / mR);
                    kq = kq * kq;
                    const float sp = fminf(ptrSp / 0.6f, 1.0f);
                    const float want = fminf(mGain * sp * kq * 0.9f, 0.80f);
                    if (want > b.comb) {
                        b.comb += (want - b.comb) * (1.0f - expf(-dt / 0.25f));
                        b.cdx = nx; b.cdy = ny;
                    }
                    // (not `small`: <rpcndr.h> #defines that to char)
                    const float tow = fminf(fmaxf(0.10f / fmaxf(b.baseR, 0.01f), 0.3f), 2.0f);
                    tx += ptrVx * kq * tow * 0.5f * mGain;
                    ty += ptrVy * kq * tow * 0.5f * mGain;
                }
            }
        } else if (b.comb > 0.0f) {
            b.comb = 0.0f; b.cdx = 0.0f; b.cdy = 0.0f;
        }

        // Soft repulsion between SAME-SIGN blobs: keeps discs and bubbles from
        // collapsing into one continent (the oil PoC's size-sorting failure),
        // while holes stay free to sit inside oil.
        // oil_viscosity widens this into a NECKING model: outside contact
        // there is now a weak attraction over a band ~2x the contact radius
        // that pulls two approaching blobs together over seconds, and the
        // contact repulsion itself is softened so the neck thickens instead of
        // snapping. At oil_viscosity 0 reachF and soft are exactly 1, the
        // attraction branch is unreachable and this is the original loop.
        if (a.repulsion > 0.0001f || vis > 1e-4f) {
            const float reachF = 1.0f + 0.9f * vis;
            const float soft   = 1.0f - 0.35f * vis;
            for (int j = 0; j < n; j++) {
                if (j == i) continue;
                const AcidBlob& o = m_acidBlobs[j];
                if ((o.wgt > 0.0f) != (b.wgt > 0.0f)) continue;
                float dx = (b.x - o.x) * aspect, dy = b.y - o.y;
                float d2 = dx * dx + dy * dy;
                float rr = (b.baseR + o.baseR) * 0.95f;
                const float reach = rr * reachF;
                if (d2 > reach * reach || d2 < 1e-8f) continue;
                float d = sqrtf(d2);
                float push;
                if (d < rr)            push = a.repulsion * (1.0f - d / rr) * 0.02f * soft;
                else if (vis > 1e-4f)  push = -vis * (1.0f - (d - rr) / (reach - rr)) * 0.006f;
                else                   continue;
                tx += (dx / d) * push / aspect;
                ty += (dy / d) * push;
            }
        }

        float k = 1.0f - expf(-dampRate * dt);   // fps-normalised
        b.vx += (tx - b.vx) * k;
        b.vy += (ty - b.vy) * k;
        b.x += b.vx * dt;
        b.y += b.vy * dt;

        // wrap (population stays constant; no respawn churn)
        // CONSERVATION (brief S). wrap_margin is 0.20 by default, but a big
        // disc's FIELD reaches baseR * support_scale ~ 0.4 in uv-x: at
        // x = -0.20 it still covers the left fifth of the frame, and the wrap
        // teleported it to x = +1.20 where it instantly covered the right
        // fifth. A large mass -- an oil disc, or a black HOLE -- vanished from
        // one edge and appeared at the other in a single frame. Measured on
        // acid-rise-12: blob x ran -0.139..1.149 with support_x up to 0.365,
        // so every side-drifting blob did this. Under conserve_mass the wrap
        // margin is at least the blob's own support reach, so the field is
        // clear of the frame on both sides of the teleport and nothing pops.
        // The support radius is in p-units (y); uv-x is that over the aspect.
        const float supOut = b.baseR * a.supportScale * (1.0f + a.breathAmt) * 0.86f;
        const bool  cons   = (a.conserveMass > 0.5f);
        const float m  = fmaxf(a.wrapMargin, 0.02f);
        const float mx = cons ? fmaxf(m, supOut / fmaxf(aspect, 1e-4f) + 0.02f) : m;
        const float my = cons ? fmaxf(m, supOut + 0.02f) : m;
        if (b.x < -mx)        b.x += 1.0f + 2.0f * mx;
        if (b.x > 1.0f + mx)  b.x -= 1.0f + 2.0f * mx;
        // rise_respawn: a blob that has climbed clear of the TOP re-enters
        // BELOW the bottom edge, at a fresh x and a fresh radius from its own
        // kind's range, instead of reappearing at the same x with the same
        // size (a visible loop once the whole column has cycled once). The
        // population is unchanged either way -- this is a teleport, not a
        // spawn -- so nothing about the field's density moves.
        // A blob's FIELD reaches baseR * support_scale, not baseR, so the old
        // test (centre above -margin-baseR) teleported a big disc while its
        // lower lobe was still a quarter of the way down the screen: the whole
        // top of the frame twitched every time one respawned. That was the
        // user's "there is some flickering sometimes". Use the support radius,
        // cut at the distance where the Wyvill kernel has decayed to ~2% --
        // past that the blob cannot move any isoline. For the small kinds this
        // is a SHORTER trip than the old margin+baseR, so the on-screen
        // population does not thin out.
        const bool respawnMode = (a.riseSpeed > 1e-6f && a.riseRespawn);
        if (respawnMode && b.y < -supOut) {
            auto rf = [&]() {
                m_acidRespawnRng ^= m_acidRespawnRng << 13;
                m_acidRespawnRng ^= m_acidRespawnRng >> 17;
                m_acidRespawnRng ^= m_acidRespawnRng << 5;
                return (m_acidRespawnRng >> 8) * (1.0f / 16777216.0f);
            };
            float lo = a.bubbleMin, hi = a.bubbleMax, bias = a.sizeBias;
            if      (b.kind == 0) { lo = a.discMin; hi = a.discMax; bias = a.bigBias; }
            else if (b.kind == 1) { lo = a.webMin;  hi = a.webMax;  bias = a.bigBias; }
            else if (b.kind == 3) { lo = a.holeMin; hi = a.holeMax; bias = a.sizeBias; }
            // CONSERVATION (brief S residue): a blob that leaves the top and
            // comes back under the bottom is the SAME blob, so under
            // conserve_mass it keeps its radius instead of drawing a new one.
            // The fresh x and the fresh curl phases are what stop the column
            // reading as a loop; re-rolling the size as well was mass
            // appearing and disappearing off-frame, which is the one thing
            // this key exists to forbid. The draws still happen either way,
            // so the respawn stream is identical and nobody else moves.
            const float newR = lo + (hi - lo) * powf(rf(), fmaxf(bias, 0.05f));
            if (!cons) b.baseR = newR;
            // ...but a blob that is on its way OUT (blob_count went down)
            // must stay on its way out. Restoring its target here handed it
            // back its life every time it wrapped, and Adjust then retired
            // somebody else instead: the population churned and the total
            // area bled away instead of settling.
            if (b.rTarget > 0.0f) b.rTarget = b.baseR;
            b.x = rf();
            b.y = 1.0f + b.baseR * a.supportScale * (1.0f + a.breathAmt) * 0.86f;
            b.vx = 0.0f;
            b.vy = 0.0f;
            b.s1 = rf() * 6.2831853f;
            b.s2 = rf() * 6.2831853f;
            // brief AG-b: re-rolled here, while the blob is parked off-screen
            // under the bottom edge -- a mass never changes dye in view.
            b.dyeId = NextAcidDyeId();
        } else if (!respawnMode) {
            if (b.y < -my)        b.y += 1.0f + 2.0f * my;
            if (b.y > 1.0f + my)  b.y -= 1.0f + 2.0f * my;
        }
        // In respawn mode the y wrap is off entirely: a blob is parked past
        // 1+m on purpose and has to climb back in. A hard ceiling on how far
        // below the frame it may be pushed is the only guard needed -- without
        // it a strong downward eddy could carry one off and the rise would
        // take minutes to bring it back, thinning the population for free.
        if (respawnMode) {
            // brief BZ hazard 5b: a blob grown in by the turnover is born at 2%
            // of its radius, so a floor on the CURRENT baseR pulls a future
            // giant up to y ~1.06 and it swells in view at the bottom edge.
            // With turnover on, park it by the radius it is growing to.
            const float supFloor = (a.blobTurnoverS > 0.0f && cons)
                ? fmaxf(b.baseR, b.rTarget) * a.supportScale * (1.0f + a.breathAmt) * 0.86f
                : supOut;
            const float floorY = 1.0f + supFloor + 0.06f;
            if (b.y > floorY) { b.y = floorY; if (b.vy > 0.0f) b.vy = 0.0f; }
        }

        b.phase += b.breathRate * breathS * dt;

        // ---- accent_mode 1: who may carry the complement ------------------
        // The test is on baseR, NOT on the breathing radius: a blob sitting
        // near the line would otherwise flip shade twice a minute as it
        // breathes across it. A hysteresis band on top of that, and the
        // colour itself never switches -- accentMix walks to its target over
        // 2 s, so the only thing that can change a blob's class (a respawn
        // drawing a new radius, or the user moving the slider) crossfades.
        // A blob's baseR only changes on a respawn, which happens below the
        // bottom edge, so in practice the walk is finished before it is seen.
        if (a.accentMode == 1) {
            const float amr = fmaxf(a.accentMaxR, 0.0f) * fmaxf(a.discMax, 1e-4f);
            if (b.accentSmall < 0)               b.accentSmall = (b.baseR < amr) ? 1 : 0;
            else if (b.baseR < amr * 0.96f)      b.accentSmall = 1;
            else if (b.baseR > amr * 1.04f)      b.accentSmall = 0;
            const float frac = fminf(fmaxf(a.accentFrac, 0.0f), 1.0f);
            const float tgt = (b.accentSmall > 0 &&
                               AcidAccentHash((uint32_t)i, 0x51ED2701u) < frac) ? 1.0f : 0.0f;
            if (b.accentMix < -0.5f) b.accentMix = tgt;      // first frame: no fade-in
            else {
                const float step = dt * 0.5f;                // full swing in 2 s
                b.accentMix = (b.accentMix < tgt) ? fminf(b.accentMix + step, tgt)
                                                  : fmaxf(b.accentMix - step, tgt);
            }
        }
        // ---- blob_count, smoothly (brief S residue) ---------------------
        // baseR walks to rTarget so a blob can be grown in or shrunk away.
        // Outside a blob_count change the two are equal and this is a no-op;
        // with conserve_mass off nothing ever sets them apart at all.
        if (b.rTarget != b.baseR) {
            const float tauB = (b.rTarget > b.baseR)
                             ? fmaxf(a.spawnGrowS, 1.0f) : fmaxf(a.dissolveS, 1.0f);
            b.baseR += (b.rTarget - b.baseR) * (1.0f - expf(-dt / tauB));
        }
    }
    // Retire whatever has shrunk past the point of contributing anything: the
    // Wyvill kernel's reach is baseR * support_scale, so a blob under a fifth
    // of a pixel of it cannot move an isoline.
    if (cons0) {
        const float gone = 0.2f / (fmaxf((float)m_height, 1.0f)
                                   * fmaxf(a.supportScale, 0.5f));
        for (size_t i = m_acidBlobs.size(); i-- > 0; )
            if (m_acidBlobs[i].rTarget <= 0.0f && m_acidBlobs[i].baseR < gone)
                m_acidBlobs.erase(m_acidBlobs.begin() + (ptrdiff_t)i);
    }
}

// ===========================================================================
// DROPLET PARTICLE SIM ([liquid_acid] droplets)
//
// The procedural bubble swarms were two cellular layers evaluated in SCREEN
// space. The user, seeing them on the panel: "it still looks like 2 things
// layered. The dots look png'd on. They need to be simulated and attached to
// the oil, actually simulated. Make a surface tension sim and have smaller
// dots organically form."
//
// So these are particles, and -- the important part -- they are rendered by
// being ADDED INTO THE SAME METABALL FIELD as the big blobs, before the
// threshold. A trapped water droplet is a literal hole in the oil surface and
// an oil droplet on the ink (or inside a big hole) is a literal bump of it,
// which is why they get the soft thickness edge, the thin-oil colour fringe,
// the emergent halo and the rim without one line of special-case shading --
// and why two of them approaching NECK together, so a coalescence reads as
// surface tension. Both swarm kinds are replaced: kind 0 = water in oil,
// kind 1 = oil on open ink.
//
// Determinism: a private xorshift seeded off the shot seed, exactly like the
// blob population, so a --shot replays and the fluid's own rand() is untouched.
// ===========================================================================

// CPU twin of the shader's metaball loop: the field, its gradient and the
// local OIL's own velocity (the same soft-max blend the fill colour uses) at
// one uv point. rise_stretch's anisotropy is deliberately not reproduced --
// it only matters for a fast-moving blob's silhouette, and this is used for
// confinement and for carrying a droplet, where a few percent of a radius is
// far below what the damping relaxation smooths out anyway.
void FluidRenderer::AcidFieldAt(float x, float y, float aspect, float& outField,
                                float& outGx, float& outGy,
                                float& outVx, float& outVy,
                                float* outHx, float* outHy) const {
    const LiquidAcidConfig& a = m_cfg.acid;
    const float px = x * aspect, py = y;
    float f = 0.0f, gx = 0.0f, gy = 0.0f;
    float vw = 0.0f, vx = 0.0f, vy = 0.0f;
    float hw2 = 0.0f, hx = 0.0f, hy = 0.0f;   // the MASS side (brief AB)
    for (size_t bi = 0; bi < m_acidBlobs.size(); bi++) {
        const AcidBlob& b = m_acidBlobs[bi];
        const float r = b.baseR * (1.0f + a.breathAmt * sinf(b.phase));
        const float sup = r * a.supportScale;
        const float s2 = sup * sup;
        const float qx = px - b.x * aspect, qy = py - b.y;
        const float d2 = qx * qx + qy * qy;
        if (d2 >= s2 || s2 < 1e-12f) continue;
        const float u = 1.0f - d2 / s2;
        const float u2 = u * u;
        const float w = u2 * u;
        f += w * b.wgt;
        const float k = (-6.0f * u2 / s2) * b.wgt;
        gx += k * qx;
        gy += k * qy;
        if (b.wgt > 0.0f) {
            const float w2 = w * w, w4 = w2 * w2;
            vw += w4; vx += b.vx * w4; vy += b.vy * w4;
        } else if (outHx) {
            const float w2 = w * w, w4 = w2 * w2;
            hw2 += w4; hx += b.vx * w4; hy += b.vy * w4;
        }
    }
    outField = f; outGx = gx; outGy = gy;
    outVx = (vw > 1e-9f) ? vx / vw : 0.0f;
    outVy = (vw > 1e-9f) ? vy / vw : 0.0f;
    if (outHx) *outHx = (hw2 > 1e-9f) ? hx / hw2 : 0.0f;
    if (outHy) *outHy = (hw2 > 1e-9f) ? hy / hw2 : 0.0f;
}

// ---- BUBBLE WEATHER (U9) -------------------------------------------------
// One smooth signal in [-1, 1] per channel, walking between per-phase targets
// drawn from a hash of the phase index. Each phase HOLDS its target for a
// hashed fraction of its length and then eases to the next, so two phases in
// a row that happen to draw similar targets read as one long season and the
// grid of boundaries never shows. Pure function of wallpaper time: nothing to
// carry across a pause, a resume or a --shot replay.
static float WeatherAt(float t, float period, uint32_t salt) {
    period = fmaxf(period, 5.0f);
    const float tau = t / period;
    const int   n   = (int)floorf(tau);
    const float u   = tau - (float)n;
    auto h = [&](int i, uint32_t extra) {
        uint32_t x = (uint32_t)(i * 0x9E3779B9u) ^ salt ^ extra;
        x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15;
        x *= 0x846CA68Bu; x ^= x >> 16;
        return (x >> 8) * (1.0f / 16777216.0f) * 2.0f - 1.0f;   // -1 .. 1
    };
    const float a0 = h(n, 0u), a1 = h(n + 1, 0u);
    // hold for 25-75% of the phase, then ease across the rest
    const float hold = 0.25f + 0.25f * (h(n, 0x5BD1u) + 1.0f);
    float e = (u - hold) / fmaxf(1.0f - hold, 1e-4f);
    e = e < 0.0f ? 0.0f : (e > 1.0f ? 1.0f : e);
    e = e * e * e * (e * (e * 6.0f - 15.0f) + 10.0f);           // smootherstep
    return a0 + (a1 - a0) * e;
}

void FluidRenderer::StepAcidDroplets(float dt) {
    const LiquidAcidConfig& a = m_cfg.acid;
    const float depthRise = EffDepthRise();   // depth_rise + depth_amount
    // Oil3D W3: a pinch-off satellite is born with AcidDrop{}'s depth 0 = the NEAREST
    // plane (the depth field came after the satellite code): at depth_amount 1 that is
    // most of the frame's droplet area (the 2026-11-02 depth debug: the whole cluster of
    // big droplets at depth 0.1). The depth haze needs every droplet's real depth, so
    // while it is on a satellite keeps its parent's; haze 0 = today's population to the
    // bit (the fix for every look moves today's depth of field: the user's call).
    const bool satDepth = a.GrainWanted() && a.murkHaze > 0.0005f;
    const float wAmt = fminf(fmaxf(a.weather, 0.0f), 1.0f);
    const bool  consM = (a.conserveMass > 0.5f);
    // Two independent channels off the same clock: how crowded the frame is,
    // and how much of the swarm is hollow. The density half is gated on
    // conserve_mass, because without it a moving target re-seeds the whole
    // population every frame instead of growing and shrinking it.
    const float wDens = (wAmt > 1e-4f && consM)
                      ? WeatherAt(m_time, a.weatherPeriodS, 0x00B1u) : 0.0f;
    const float wRing = (wAmt > 1e-4f)
                      ? WeatherAt(m_time, a.weatherPeriodS, 0x7A3Cu) : 0.0f;
    // The ring share alone barely reads: a ring has to clear a band-width
    // floor 3.5x the solid one to resolve at all, so only a handful of the
    // rings alive are ever visible and a 45% swing in the share moves two or
    // three of them. The BIG hollow bubbles are what a "spell of rings"
    // actually looks like, so the same channel drives their share too --
    // measured 0.1-1.2% visible rings without it, which is no season at all.
    const float ringFracEff = fminf(fmaxf(a.dropletRingFrac
                                          * (1.0f + wAmt * 0.90f * wRing),
                                          0.0f), 1.0f);
    const float ringBigEff  = fminf(fmaxf(a.dropletRingBigFrac
                                          * (1.0f + wAmt * 1.30f * wRing),
                                          0.0f), 1.0f);
    int target = (int)lroundf((float)a.droplets
                              * (1.0f + wAmt * 0.45f * wDens));
    if (a.droplets > 0 && target < 1) target = 1;
    // ---- BUBBLE CRUST (brief AB) -----------------------------------------
    // The crust gets a target of its OWN, added on top. The user was explicit
    // that this is not a swap -- "it can be inside and outside" -- so the film
    // keeps every droplet it has today and the masses gain a population
    // beside it. 0.45 is roughly the share of the frame a mass covers at the
    // shipped coverage, so droplet_crust_density reads as a density against
    // the film rather than as a raw count.
    const float massBias  = fminf(fmaxf(a.dropletMassBias, 0.0f), 1.0f);
    const float crustDens = fminf(fmaxf(a.dropletCrustDens, 1.0f), 4.0f);
    const float crustRk   = fminf(fmaxf(a.dropletCrustR, 0.05f), 1.0f);
    const int   targetCrust = (int)lroundf((float)target * massBias
                                           * (crustDens - 1.0f) * 0.45f);
    const int   targetFilm  = target;
    target += targetCrust;
    if (target > kAcidMaxDrops) target = kAcidMaxDrops;
    if (target <= 0) {
        // CONSERVATION: turning the population off from the settings window
        // used to delete 1500 visible droplets in one frame. Shrink them away
        // over dissolve_s instead; the upload path keeps drawing them (their
        // grid does not move) until the last one is gone.
        if (!m_acidDrops.empty()) {
            if (a.conserveMass > 0.5f && dt > 0.0f) {
                const float k0 = 1.0f - expf(-dt / fmaxf(a.dissolveS, 0.34f));
                bool any = false;
                for (size_t i = 0; i < m_acidDrops.size(); i++) {
                    AcidDrop& d = m_acidDrops[i];
                    d.rt = 0.0f;
                    d.r -= d.r * k0;
                    if (d.r > 1e-5f) any = true; else d.r = 0.0f;
                }
                if (any) return;
            }
            m_acidDrops.clear();
        }
        m_dropletSeededFor = -1;
        return;
    }
    if (dt <= 0.0f) return;

    const float aspect = (float)m_width / fmaxf((float)m_height, 1.0f);
    const float simTexX = 1.0f / fmaxf((float)m_simW, 1.0f);
    const float simTexY = 1.0f / fmaxf((float)m_simH, 1.0f);
    // The shader walks the 3x3 cells around a pixel, so a droplet's SUPPORT
    // radius may not exceed one cell -- that is what makes 3x3 exact instead
    // of "usually enough", and it is also the per-pixel cost bound.
    const float cellP = fminf(aspect / (float)kDropGridW, 1.0f / (float)kDropGridH);
    const float rCap  = cellP / fmaxf(a.dropletSupport, 0.5f);
    const float rMax  = fminf(fmaxf(a.dropletRMax, 1e-4f), rCap);
    const float rMin  = fminf(fmaxf(a.dropletRMin, 1e-5f), rMax * 0.5f);
    // BIG HOLLOW BUBBLES: rCap exists because the shader walks the 3x3 cells
    // around a pixel, so a droplet's support may not exceed one cell. A big
    // ring breaks that on purpose and buys the invariant back by being
    // registered in every cell its support covers (see buildGrid's `spread`).
    const float ringMul  = fminf(fmaxf(a.dropletRingRMul, 1.0f), 6.0f);
    // MEDIUM BLACK DROPLETS (droplet_big_frac / droplet_big_r_mul, fluid.h):
    // with the key at 0 bigOn is false, no droplet carries the flag and every
    // line below that names it takes the path it took before.
    const float bigMul  = fminf(fmaxf(a.dropletBigRMul, 1.0f), 6.0f);
    const float bigFrac = (bigMul > 1.251f) ? fminf(fmaxf(a.dropletBigFrac, 0.0f), 1.0f) : 0.0f;
    const bool  bigOn   = (bigFrac > 1e-6f);
    // a medium droplet holds its neighbours off at kBigSep x the radius sum:
    // the droplet kernel reaches 2.2x the drawn radius, and at 1.4x (the first
    // try) two fields still summed over the isoline between them, so a pair
    // drew one lump and a small neighbour a knob on the rim. At 1.75x each
    // kernel is under ~0.08 at the midpoint, below the isoline's level even
    // in thin film. That is further than the 3x3 cell walk sees: it walks
    // this many cells instead.
    const float kBigSep = 1.75f;
    const int   bigWalk = bigOn ? (int)fminf(ceilf(kBigSep * rMax * (2.0f * bigMul)
                                                   * (float)kDropGridH) + 1.0f, 8.0f) : 1;
    // ---- RACING MICRO-BUBBLES (brief Y) ----------------------------------
    // An ordinary trapped droplet rides the oil and lags it by droplet_rise,
    // so its NET climb is riseSpeed * (1 - droplet_rise). A racer's bonus is
    // expressed against exactly that, which is what makes droplet_racer_speed
    // read as "x an ordinary droplet" rather than as an opaque uv/s.
    const float racerFrac  = fminf(fmaxf(a.dropletRacerFrac, 0.0f), 1.0f);
    const float racerBase  = a.riseSpeed * fmaxf(1.0f - a.dropletRise, 0.1f);
    const float racerMul   = fminf(fmaxf(a.dropletRacerSpeed, 1.0f), 8.0f);
    const float racerWob   = racerBase * fminf(fmaxf(a.dropletRacerWobble, 0.0f), 2.0f);
    const float racerRMax  = (a.dropletRacerRMax > 1e-6f) ? a.dropletRacerRMax
                                                          : rMin * 2.0f;
    const float tNow = m_time;
    const float thresh = a.threshold;
    // ONE relaxation time for birth, coalescence and dissolution: nothing in
    // this sim changes size or membership discontinuously.
    const float tau   = 0.34f;
    const float relax = 1.0f - expf(-dt / tau);
    // CONSERVATION (brief S): the one relaxation time above makes a droplet
    // appear, and disappear, in about a third of a second. That is a pop at a
    // visible size in the open. conserve_mass gives BIRTH and DEATH their own
    // (much longer) times, so a new droplet spends its first seconds below the
    // resolvable floor and swells into view, and one dying of old age fades
    // out over seconds. Coalescence keeps the 0.34 s: pouring into a
    // neighbour is a transfer, not a disappearance, and it should stay quick.
    const bool  cons     = (a.conserveMass > 0.5f);
    const float growTau  = (cons && a.spawnGrowS > 0.01f) ? a.spawnGrowS : tau;
    const float dieTau   = (cons && a.dissolveS  > 0.01f) ? a.dissolveS  : tau;
    // droplet_coalesce_s: the neck takes this long to close -- the absorbed
    // droplet's slide-in AND both radii move on it, so a coalescence reads as
    // one surface pulling itself round rather than a dot snapping into place.
    const float coalS  = fmaxf(a.dropletCoalesceS, 0.0f);
    const float mrelax = 1.0f - expf(-dt / (coalS > 0.01f ? coalS : 0.18f));
    const float life  = a.dropletLife;
    // oil_viscosity, droplet side: a slower velocity relaxation (so a droplet
    // is carried smoothly and never snaps to a new target) and a damped
    // Brownian term. Both are exactly the shipped values at viscosity 0.
    const float dvis  = fminf(fmaxf(a.oilViscosity, 0.0f), 1.0f);
    const float dvDamp = fmaxf(a.dropletDamping, 0.1f) / (1.0f + 2.0f * dvis);
    const float dvJit  = 1.0f - 0.70f * dvis;

    auto rf = [&]() {
        m_dropletRng ^= m_dropletRng << 13;
        m_dropletRng ^= m_dropletRng >> 17;
        m_dropletRng ^= m_dropletRng << 5;
        return (m_dropletRng >> 8) * (1.0f / 16777216.0f);
    };
    // pow(U, bias) with bias > 1 crowds the small end: a heavy tail of tiny
    // droplets with a few big ones, which is what the macro footage shows.
    auto drawR = [&]() {
        return rMin + (rMax - rMin) * powf(rf(), fmaxf(a.dropletBias, 0.05f));
    };
    // bilinear tap of the 64x36 velocity readback, uv/s (same as StepAcidBlobs)
    auto sampleVel = [&](float x, float y, float& ox, float& oy) {
        float fx2 = x * kVelW - 0.5f, fy2 = y * kVelH - 0.5f;
        int x0 = (int)floorf(fx2), y0 = (int)floorf(fy2);
        float tx = fx2 - x0, ty = fy2 - y0;
        auto at = [&](int xi, int yi, int c) {
            xi = xi < 0 ? 0 : (xi >= kVelW ? kVelW - 1 : xi);
            yi = yi < 0 ? 0 : (yi >= kVelH ? kVelH - 1 : yi);
            return m_velCpu[((size_t)yi * kVelW + xi) * 2 + c];
        };
        ox = (at(x0, y0, 0) * (1 - tx) + at(x0 + 1, y0, 0) * tx) * (1 - ty)
           + (at(x0, y0 + 1, 0) * (1 - tx) + at(x0 + 1, y0 + 1, 0) * tx) * ty;
        oy = (at(x0, y0, 1) * (1 - tx) + at(x0 + 1, y0, 1) * tx) * (1 - ty)
           + (at(x0, y0 + 1, 1) * (1 - tx) + at(x0 + 1, y0 + 1, 1) * tx) * ty;
        ox *= simTexX; oy *= simTexY;
    };
    // Nucleation is a SURFACE effect: a droplet of water comes out of solution
    // where the film is THIN (near an edge, near another droplet's rim) and
    // where the flow is shearing, not uniformly across a slab of oil.
    // kind 2 = a CRUST bubble (brief AB): kind 1 in every other respect, but
    // it must land WELL INSIDE a dye mass rather than anywhere off the oil,
    // it is small, and it is flagged so it rides the mass and never merges.
    auto nucleate = [&](int kind, AcidDrop& out) -> bool {
        const bool crust = (kind == 2);
        if (crust) kind = 1;
        for (int tr = 0; tr < 20; tr++) {
            const float x = rf(), y = rf();
            float f, gx, gy, ovx, ovy;
            AcidFieldAt(x, y, aspect, f, gx, gy, ovx, ovy);
            const float gl = sqrtf(gx * gx + gy * gy) + 1e-6f;
            const float sdf = (f - thresh) / gl;
            float p;
            if (crust) {
                // Deep enough in that the bubble sits in the black body of
                // the mass and not on its rim, where the confinement term
                // would push it straight back out.
                if (sdf >= -0.010f) continue;
                // ...and denser where the mass is thickest, which is what
                // makes the crust patchy instead of an even sprinkle.
                p = 0.25f + 0.75f * (1.0f - expf(sdf / 0.060f));
            } else if (kind == 0) {
                if (sdf <= 0.004f) continue;               // inside the oil only
                float vu, vv; sampleVel(x, y, vu, vv);
                const float sp = sqrtf(vu * vu + vv * vv);
                p = 0.12f + 0.68f * expf(-sdf / 0.055f) + 0.35f * (1.0f - expf(-sp * 18.0f));
            } else {
                if (sdf >= -0.004f) continue;              // open ink only
                p = 0.10f + 0.70f * expf(sdf / 0.070f);
            }
            if (rf() > fminf(p, 1.0f)) continue;
            out = AcidDrop{};
            out.x = x; out.y = y;
            out.r = 0.0f;                                  // born at nothing
            out.rt = drawR();
            if (crust) { out.crust = 1; out.rt *= crustRk; }
            out.tauR = growTau;                            // see growTau above
            out.kind = kind;
            out.mergeTo = -1;
            // HOLLOW or solid, and doubled or not: both are drawn ONCE, here,
            // and fixed for the droplet's life -- a spot that changed its mind
            // frame to frame would flicker, which is the one thing this sim
            // exists to avoid. Drawn from a HASH OF THE BIRTH POSITION rather
            // than from the sim's own stream, deliberately: consuming a random
            // number here would shift every later draw, so merely switching
            // one of these keys on would have moved the whole population and
            // no A/B of them could be read.
            auto hash01 = [](float hx, float hy, uint32_t salt) {
                uint32_t a1, b1;
                memcpy(&a1, &hx, 4); memcpy(&b1, &hy, 4);
                uint32_t h = (a1 * 2654435761u) ^ (b1 * 2246822519u) ^ salt;
                h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
                h *= 3266489917u; h ^= h >> 16;
                return (h >> 8) * (1.0f / 16777216.0f);
            };
            out.ring = (kind == 0 && hash01(x, y, 0x9E3779B9u) < ringFracEff)
                     ? 1 : 0;
            // ...and one more draw from the same place: the seed that makes
            // this ring's own out-of-round shape (ellipse axis, wobble phase,
            // breathing rate). Same reasoning -- hashed, not drawn from the
            // stream, so the population is identical whatever the shape keys
            // say, and fixed for life, so a ring never morphs into another.
            out.seed = hash01(x, y, 0x85EBCA6Bu);
            // ...and the DEPTH, from the same place and for the same reason:
            // a droplet's distance from the lens is part of its identity, so
            // it is fixed for life, and drawing it from a hash rather than
            // from the sim's stream means switching droplet_depth on does not
            // move a single droplet. 0.5 is the masses' own plane; the spread
            // about it is applied where it is USED, so the key can be turned
            // while the sim runs without re-rolling anybody.
            out.depth = hash01(x, y, 0xB5297A4Du);
            // ---- BIG HOLLOW BUBBLE ---------------------------------------
            // Drawn from the position HASH, never from the sim's own stream,
            // for the same reason the ring flag is: switching these keys on
            // must not move the rest of the population. A big bubble also
            // needs ROOM -- the confinement term keeps a droplet's centre at
            // least 0.6 of its radius inside the oil, so one nucleated near a
            // film edge would be shoved out and dissolve before it had grown.
            if (out.ring && ringMul > 1.001f) {
                if (hash01(x, y, 0xC2B2AE35u) < ringBigEff) {
                    const float hs = hash01(x, y, 0x27D4EB2Fu);
                    out.rt = rMax * (1.0f + (ringMul - 1.0f) * (0.35f + 0.65f * hs));
                    // Enough room to grow into, but not a deep-interior
                    // test: the droplet is born at nothing and swells over
                    // spawn_grow_s, and the confinement term steers it
                    // further inside as its radius grows, so demanding the
                    // full final margin up front rejected every candidate.
                    if (sdf < out.rt * 0.30f) continue;
                }
            }
            // ---- MEDIUM SOLID DROPLET (droplet_big_frac) ------------------
            // From the position hash, like the big ring above. It is born
            // only where its whole disc fits inside the film with room to
            // spare: a medium black disc that overlaps the film's edge reads
            // as a bite out of the outline, not as a droplet.
            if (bigOn && kind == 0 && !out.ring) {
                if (hash01(x, y, 0x1B873593u) < bigFrac) {
                    const float hs = hash01(x, y, 0xCC9E2D51u);
                    out.rt = rMax * (1.25f + (bigMul - 1.25f) * hs * sqrtf(hs));
                    out.big = 1;
                    if (sdf < out.rt * 1.30f) continue;
                    // ...and never within reach of another medium one: two
                    // born overlapping stayed one fused lump (their kernels
                    // reach 2.2x the radius). Same spacing as the pair rule.
                    bool crowded = false;
                    for (size_t q = 0; q < m_acidDrops.size() && !crowded; q++) {
                        const AcidDrop& o = m_acidDrops[q];
                        if (!o.big || o.rt <= 0.0f) continue;
                        const float ox = (o.x - x) * aspect, oy = o.y - y;
                        const float rs = kBigSep * (out.rt + fmaxf(o.r, o.rt)) + 0.5f * rMax;
                        crowded = (ox * ox + oy * oy < rs * rs);
                    }
                    if (crowded) continue;
                    // On the masses' plane (0.42..0.5 of the hash's spread): a
                    // medium droplet is BLACK OIL, so the depth haze (which
                    // fades a deep droplet toward the film) must not reach it.
                    out.depth = 0.42f + 0.08f * out.depth;
                }
            }
            // ---- RACING MICRO-BUBBLE ------------------------------------
            // The flag comes from the position hash, like ring/seed/depth, so
            // switching droplet_racer_frac on does not move a single droplet.
            // Only the SMALLEST trapped droplets qualify, and never a ring: a
            // racer is a micro-bubble, not a lens.
            if (racerFrac > 1e-6f && kind == 0 && !out.ring
                && out.rt <= racerRMax
                && hash01(x, y, 0x7F4A7C15u) < racerFrac) {
                out.racer = 1;
                // "never appearing in view": a racer crosses the frame in
                // well under a droplet_life, so if it were nucleated mid-frame
                // like everything else the user would watch it swell into
                // existence and then sprint. Under conserve_mass it is born
                // OFF-FRAME, just below the bottom edge, and climbs in -- the
                // same door its wrapped siblings come back through.
                if (cons) {
                    const float ny = 1.02f + 0.06f * hash01(x, y, 0x9E3779B1u);
                    // Sweep ACROSS the bottom band for a spot that is inside
                    // the oil rather than giving up on the drawn x: only part
                    // of that band is under a mass at any moment, and failing
                    // back into the outer try loop cost most racers their
                    // birth (measured: 4 alive on average instead of dozens).
                    // The sweep is a fixed pattern, not a draw, so the sim's
                    // random stream is still untouched.
                    float nx = x; bool ok = false;
                    for (int k2 = 0; k2 < 12 && !ok; k2++) {
                        nx = x + (float)k2 * (1.0f / 12.0f);
                        if (nx >= 1.0f) nx -= 1.0f;
                        float f4, gx4, gy4, o3, o4;
                        AcidFieldAt(nx, ny, aspect, f4, gx4, gy4, o3, o4);
                        const float gl4 = sqrtf(gx4 * gx4 + gy4 * gy4) + 1e-6f;
                        ok = ((f4 - thresh) / gl4 > 0.004f);
                    }
                    if (!ok) continue;                 // no oil under the edge
                    out.x = nx; out.y = ny;
                }
            }
            return true;
        }
        return false;
    };

    // ---- bulk fill (startup / population change) -------------------------
    // Nucleating 1500 droplets at ~40/s would leave the first 40 s bare, so
    // the initial condition is drawn in one go, at full radius and with
    // staggered ages so they do not all reach droplet_life together.
    if (m_dropletSeededFor != target && cons && m_dropletSeededFor > 0
        && !m_acidDrops.empty()) {
        // CONSERVATION: moving the "droplets" slider used to clear the field
        // and re-seed the whole population at full radius -- 1500 droplets
        // vanishing and 1500 appearing in one frame. Retire only the surplus,
        // by shrinking it, and let the emitter grow any shortfall in.
        int seen = 0;
        for (size_t i = 0; i < m_acidDrops.size(); i++) {
            if (m_acidDrops[i].rt <= 0.0f) continue;
            if (++seen > target) { m_acidDrops[i].rt = 0.0f; m_acidDrops[i].tauR = dieTau; }
        }
        m_dropletSeededFor = target;
    }
    if (m_dropletSeededFor != target) {
        m_dropletRng = (g_randSeed ? (g_randSeed * 2246822519u) ^ 0xD407u : GetTickCount()) | 1u;
        m_acidDrops.clear();
        m_acidDrops.reserve(target);
        for (int i = 0; i < target; i++) {
            AcidDrop d;
            const int kind = (rf() < a.dropletInkFrac) ? 1 : 0;
            if (!nucleate(kind, d)) continue;
            d.r = d.rt;
            d.age = (life > 0.5f) ? rf() * life : 0.0f;
            m_acidDrops.push_back(d);
        }
        m_dropletSeededFor = target;
        m_dropletSpawnAcc = 0.0f;
    }

    const int n = (int)m_acidDrops.size();
    std::vector<float> ax((size_t)n, 0.0f), ay((size_t)n, 0.0f);

    // ---- 1. motion -------------------------------------------------------
    for (int i = 0; i < n; i++) {
        AcidDrop& d = m_acidDrops[i];
        d.age += dt;
        float f, gx, gy, ovx, ovy;
        AcidFieldAt(d.x, d.y, aspect, f, gx, gy, ovx, ovy);
        const float gl = sqrtf(gx * gx + gy * gy) + 1e-6f;
        const float sdf = (f - thresh) / gl;
        float vu, vv; sampleVel(d.x, d.y, vu, vv);

        float tx, ty;
        if (d.kind == 0) {
            // trapped INSIDE the oil: it rides the oil, not the water. ovx/ovy
            // is the local oil's own velocity, so a droplet travels with the
            // disc it sits in and shears when that disc does.
            tx = ovx; ty = ovy;
            // ...and lags it slightly (uv y is down, so +y is down): trapped
            // water is denser than the wax, which is what makes a bubble creep
            // ACROSS the disc instead of riding it like a painted dot.
            ty += a.riseSpeed * a.dropletRise;
        } else if (d.crust) {
            // ---- BUBBLE CRUST: it rides the MASS ------------------------
            // The user's note on the lava-lamp refs: it sits in the wax and
            // travels with it, "it does not race across it". So a crust
            // bubble takes the local dark mass's own velocity, the same
            // soft-max blend over the negative blobs that a trapped droplet
            // takes over the oil -- not the water's flow, which would drag
            // it out of the body it belongs to within seconds.
            float hvx = 0.0f, hvy = 0.0f;
            float f5, gx5, gy5, o5, o6;
            AcidFieldAt(d.x, d.y, aspect, f5, gx5, gy5, o5, o6, &hvx, &hvy);
            tx = hvx; ty = hvy;
            // a touch of the water underneath, so a crust on the open ink
            // (no negative blob over it) still drifts rather than freezing
            tx += vu * a.flowGain * 0.25f;
            ty += vv * a.flowGain * 0.25f;
        } else {
            tx = vu * a.flowGain;
            ty = vv * a.flowGain;
        }
        // ---- depth_rise: the back layer climbs faster ---------------------
        // The user's own idea, watching the live panel: "the back particles
        // going up faster". Parallax the wrong way round on purpose -- a real
        // lens would show the FAR layer moving less -- but this is a thin film
        // seen from above, not a landscape, and what it buys is that the
        // out-of-focus layer separates from the sharp one in MOTION as well as
        // in blur, which is what makes the depth read at all. Symmetric about
        // the masses' own plane (0.5), so the population's mean climb rate is
        // untouched and the rise looks exactly as fast as it did.
        if (depthRise > 1e-5f && a.riseSpeed > 1e-6f)
            ty -= a.riseSpeed * depthRise * (d.depth - 0.5f);
        // ---- racing micro-bubbles -----------------------------------------
        // "still slowish but considerably faster than the others". A flat
        // bonus on the climb (so the ratio to an ordinary droplet is exactly
        // droplet_racer_speed) plus a lateral zigzag on two incommensurate
        // slow sines keyed to the droplet's OWN seed and depth -- no two
        // racers wobble in step, and each one's path is a lazy spiral rather
        // than a straight line, which is what a micro-bubble in water does.
        // depth_rise above is untouched: a racer still belongs to its layer.
        if (d.racer && racerMul > 1.0f) {
            // Its OWN climb, scaled. A flat bonus in uv/s does not read as
            // "x the others": rise_parallax already spreads the population's
            // climb over 4x by size, and a flat term measured 4.4x on the
            // median instead of the 2.5 the key asked for. Multiplying the
            // upward part of the target leaves a racer in slow far oil slower
            // than one in fast near oil, exactly as its neighbours are.
            ty -= fmaxf(-ty, 0.0f) * (racerMul - 1.0f);
            if (racerWob > 1e-9f) {
                const float TAU = 6.2831853f;
                tx += racerWob * (0.9f * sinf(0.90f * tNow + d.seed * TAU)
                                + 0.5f * sinf(1.43f * tNow + d.depth * TAU));
            }
        }
        // slow Brownian jitter -- damped by oil_viscosity: a droplet suspended
        // in a thick liquid does not twitch.
        tx += (rf() * 2.0f - 1.0f) * a.dropletJitter * dvJit;
        ty += (rf() * 2.0f - 1.0f) * a.dropletJitter * dvJit;

        // ---- confinement: kind 0 stays inside the oil, kind 1 outside it --
        // Push back along the field gradient (which points INTO the oil).
        const float want = (d.kind == 0) ? 1.0f : -1.0f;
        // (a medium droplet keeps its whole disc inside, droplet_big_frac)
        const float margin = fmaxf(d.r * ((bigOn && d.big) ? 1.15f : 0.6f), 1e-4f);
        if (sdf * want < margin) {
            const float over = (margin - sdf * want) / margin;
            const float push = 0.05f * fminf(over, 3.0f);
            tx += (gx / gl) * want * push / aspect;
            ty += (gy / gl) * want * push;
            d.out += dt;
        } else {
            d.out = fmaxf(d.out - dt * 2.0f, 0.0f);
        }
        // Stranded on the wrong side for good (the oil moved away): dissolve
        // it. Never delete it outright -- that is exactly the one-frame pop
        // the procedural swarm had.
        if (d.out > 1.2f) { d.rt = 0.0f; d.tauR = dieTau; }

        // merge pull: the absorbed droplet's centre slides into the survivor
        if (d.mergeTo >= 0 && d.mergeTo < n) {
            const AcidDrop& s = m_acidDrops[d.mergeTo];
            d.x += (s.x - d.x) * mrelax;
            d.y += (s.y - d.y) * mrelax;
        }

        const float k = 1.0f - expf(-dvDamp * dt);
        d.vx += (tx - d.vx) * k;
        d.vy += (ty - d.vy) * k;
        d.x += d.vx * dt;
        d.y += d.vy * dt;

        // ---- contribution gate -------------------------------------------
        // A droplet on the WRONG side of the interface is invisible in itself
        // -- a positive one inside the oil only adds oil to oil, a negative
        // one on open ink only subtracts from ink that is already below the
        // isoline -- but its kernel is TINY and therefore very steep, so it
        // still dominates |grad| in its neighbourhood. sdf = (field - thresh)
        // / |grad| then collapses toward 0 there, which drops the pixel into
        // the dark rim band and flattens the film thickness proxy: a droplet
        // that is not there draws a hollow dark RING with an oil centre, and
        // a cluster of them smears a soft dark veil over the sheet. Fade the
        // whole contribution (field AND gradient) out instead, over half the
        // droplet's own radius of travel, so it leaves no trace at all.
        {
            float f2, gx2, gy2, ovx2, ovy2;
            AcidFieldAt(d.x, d.y, aspect, f2, gx2, gy2, ovx2, ovy2);
            const float gl2 = sqrtf(gx2 * gx2 + gy2 * gy2) + 1e-6f;
            const float side = ((d.kind == 0) ? 1.0f : -1.0f) * ((f2 - thresh) / gl2);
            float g = side / fmaxf(0.5f * d.r, 1e-5f);
            g = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);
            g = g * g * (3.0f - 2.0f * g);
            // ---- "an actual hole, or invisible": no rim without a fill ----
            // Being on the right side is not enough. The droplet's own PEAK
            // kernel has to carry the local field across the isoline, or it
            // draws nothing but a rim around nothing -- the hollow rings with
            // an oil centre the user photographed. A positive droplet that
            // has drifted into a deep hole can never lift the field back over
            // the threshold; a negative one has to beat whatever oil it is
            // under (the shader scales its punch by the LOCAL field, which is
            // what normally makes this work at any thickness, but not where
            // the two fight). Fade it out over a small field margin rather
            // than switching it, so nothing pops.
            float over;
            if (d.kind == 0) over = thresh - (f2 - fmaxf(f2, 1.0f) * a.dropletWeight);
            // kind 1 carries the local deficit plus its authored weight (see
            // the shader), so it always crosses -- the test is here for
            // symmetry and to keep the two definitions in one place.
            else             over = a.dropletOilW;
            float pg = over / 0.18f;
            pg = pg < 0.0f ? 0.0f : (pg > 1.0f ? 1.0f : pg);
            g *= pg * pg * (3.0f - 2.0f * pg);
            // ...and fade out below about a pixel. A sub-pixel droplet cannot
            // be resolved -- no pixel lands near enough to its centre for the
            // kernel to reach full strength and punch through -- so it, too,
            // would contribute nothing but a gradient spike.
            // The floor is a pixel OR the dark rim's own width, whichever is
            // larger: a droplet whose hole would come out narrower than the
            // rim band painted around it is all rim and no fill, which is the
            // other half of the ring. rim_width is a half-width in the same
            // p-units as r.
            const float pxY = 1.0f / fmaxf((float)m_height, 1.0f);
            float rFloor = fmaxf(1.2f * pxY, 1.6f * fmaxf(a.rimWidth, 1e-5f));
            // A RING has to clear a higher floor: what must be resolvable is
            // its BAND, which is only a fraction of its radius across, not the
            // whole droplet. Same rule as 7f0188d's ("no rim without a fill"),
            // read for the shape that is deliberately hollow -- an unresolved
            // ring is a grey smudge, and a smudge is what that fix removed.
            if (d.ring) {
                const float bandFrac = fmaxf(2.2f * fminf(fmaxf(a.dropletRingWidth,
                                                               0.02f), 0.60f), 0.25f);
                rFloor /= bandFrac;
            }
            float sp2 = (d.r - 0.35f * rFloor) / fmaxf(0.65f * rFloor, 1e-9f);
            sp2 = sp2 < 0.0f ? 0.0f : (sp2 > 1.0f ? 1.0f : sp2);
            d.gate = g * sp2 * sp2 * (3.0f - 2.0f * sp2);
        }

        if (life > 0.5f && d.age > life) { d.rt = 0.0f; d.tauR = dieTau; }
        // ---- off the frame ------------------------------------------------
        // THE BUG BEHIND "why is the motion randomly grouping in this spot"
        // (user, 2026-09-18, phone video of acid-rise-12 on the panel).
        // Kind 0 droplets ride the RISING oil, so they all leave across the
        // TOP and were killed there -- while their replacements were
        // nucleated UNIFORMLY over the whole frame. A uniform volumetric
        // source with an absorbing boundary at one end is the textbook recipe
        // for a linear density ramp toward that end, and that is exactly what
        // the sim produced: measured over 19 samples of a 300 s headless run,
        // the visible kind-0 droplets per tenth of the frame ran
        //   75 78 73 71 62 61 50 40 24 9   (top -> bottom)
        // -- an 8.5x pile-up in the top band, permanently. The blobs, which
        // DO re-enter (rise_respawn), were flat over the same run.
        // The fix is the conservation rule: a droplet that leaves the frame
        // re-enters from the opposite edge with its own size, ring and seed
        // intact, exactly as the blobs do. No source, no sink, no ramp.
        if (cons) {
            const float kM = 0.06f;      // safely past the droplet's support
            bool wrapped = false;
            if (d.y < -kM)             { d.y += 1.0f + 2.0f * kM; wrapped = true; }
            else if (d.y > 1.0f + kM)  { d.y -= 1.0f + 2.0f * kM; wrapped = true; }
            if (d.x < -kM)             { d.x += 1.0f + 2.0f * kM; wrapped = true; }
            else if (d.x > 1.0f + kM)  { d.x -= 1.0f + 2.0f * kM; wrapped = true; }
            if (wrapped) {
                // Land it on the right side of the interface before it drifts
                // back into view -- a kind 0 droplet must re-enter INSIDE the
                // oil and a kind 1 one on open ink, or the confinement term
                // would strand and dissolve it on the way in. The search runs
                // ALONG the edge it re-entered from, so the re-entry point
                // stays off-frame and is never visible.
                const float want = (d.kind == 0) ? 1.0f : -1.0f;
                float f3, gx3, gy3, o1, o2;
                AcidFieldAt(d.x, d.y, aspect, f3, gx3, gy3, o1, o2);
                float gl3 = sqrtf(gx3 * gx3 + gy3 * gy3) + 1e-6f;
                if (((f3 - thresh) / gl3) * want < d.r) {
                    const bool alongX = (d.y < 0.0f || d.y > 1.0f);
                    for (int tr = 0; tr < 24; tr++) {
                        const float c = rf();
                        const float tx3 = alongX ? c : d.x;
                        const float ty3 = alongX ? d.y : c;
                        AcidFieldAt(tx3, ty3, aspect, f3, gx3, gy3, o1, o2);
                        gl3 = sqrtf(gx3 * gx3 + gy3 * gy3) + 1e-6f;
                        if (((f3 - thresh) / gl3) * want > d.r) {
                            d.x = tx3; d.y = ty3; break;
                        }
                    }
                }
                // Fresh grace on the confinement counter and no inherited
                // momentum from the far edge. Its AGE is deliberately kept:
                // resetting it would make wrapping droplets immortal and the
                // droplet_life turnover would stop.
                d.out = 0.0f; d.vx = 0.0f; d.vy = 0.0f;
            }
        } else {
            // off the frame: dissolve there, never inside the visible area
            if (d.x < -0.04f || d.x > 1.04f || d.y < -0.04f || d.y > 1.04f) d.rt = 0.0f;
        }
        d.r += (d.rt - d.r) * ((d.tauR > 1e-3f)
                               ? (1.0f - expf(-dt / d.tauR)) : relax);
        if (!(d.r > 0.0f)) d.r = 0.0f;              // NaN guard
        if (!(d.x > -10.0f && d.x < 10.0f)) { d.x = 0.5f; d.rt = 0.0f; d.r = 0.0f; }
        if (!(d.y > -10.0f && d.y < 10.0f)) { d.y = 0.5f; d.rt = 0.0f; d.r = 0.0f; }
    }

    // ---- 2. uniform grid (neighbour search AND the shader's cell table) ---
    const int NC = kDropGridW * kDropGridH;
    // `spread`: the table the SHADER reads registers a droplet in every cell
    // its support disc reaches, MINUS one ring of cells, because the shader
    // already walks the 3x3 around each pixel. For everything inside rCap
    // (support <= one cell) that reduces to the home cell and this is exactly
    // the original build; only a BIG HOLLOW BUBBLE takes extra entries, and
    // only a few hundred of them. The table used for the CPU neighbour search
    // is built without it, so a pair is still visited once.
    auto buildGrid = [&](bool spread) {
        const int m = (int)m_acidDrops.size();
        m_dropletCellCount.assign((size_t)NC, 0);
        m_dropletCellStart.assign((size_t)NC + 1, 0);
        const float cwx = 1.0f / (float)kDropGridW;      // cell size, uv
        const float cwy = 1.0f / (float)kDropGridH;
        std::vector<int> cellOf;                 // home-cell build (spread off)
        std::vector<std::pair<int, int>> ins;    // (cell, droplet), spread on
        if (!spread) cellOf.assign((size_t)m, 0);
        else         ins.reserve((size_t)m + 256);
        int placed = 0;
        // droplet_big_frac: the medium droplets go into the shader's table
        // FIRST, so the per-cell cap can never cut one cell out of a big disc.
        // Key off = one pass in the order it always had.
        // (the neighbour table too: a medium droplet cut by the cap would
        // stop holding anybody off)
        const int passes = bigOn ? 2 : 1;
        for (int pass = 0; pass < passes; pass++)
        for (int i = 0; i < m; i++) {
            const AcidDrop& d = m_acidDrops[i];
            if (passes == 2 && ((d.big != 0) != (pass == 0))) continue;
            int cx = (int)floorf(d.x * kDropGridW);
            int cy = (int)floorf(d.y * kDropGridH);
            cx = cx < 0 ? 0 : (cx >= kDropGridW ? kDropGridW - 1 : cx);
            cy = cy < 0 ? 0 : (cy >= kDropGridH ? kDropGridH - 1 : cy);
            if (!spread) {
                const int c = cy * kDropGridW + cx;
                cellOf[i] = c;
                if (m_dropletCellCount[c] < kDropCellCap) m_dropletCellCount[c]++;
                else cellOf[i] = -1;                    // over the per-cell cap
                continue;
            }
            // support disc, uv. The shader culls at exactly this radius.
            const float sup = d.r * fmaxf(a.dropletSupport, 0.5f);
            // ceil(S / cell) - 1, with a hair of slack: a droplet at exactly
            // rCap has S == one cell, and rounding that to 2 cells would
            // spread EVERY droplet over 3x3 and change the shipped look.
            int rx = (int)ceilf((sup / fmaxf(aspect, 1e-4f)) / cwx - 1.0001f);
            int ry = (int)ceilf(sup / cwy - 1.0001f);
            if (rx < 0 || placed + 1 >= kAcidMaxDrops) rx = 0;
            if (ry < 0 || placed + 1 >= kAcidMaxDrops) ry = 0;
            if (rx > 8) rx = 8;
            if (ry > 8) ry = 8;
            // ...and never at the cost of dropping ordinary droplets.
            if ((placed + (2 * rx + 1) * (2 * ry + 1)) > kAcidMaxDrops) { rx = 0; ry = 0; }
            for (int oy = -ry; oy <= ry; oy++) {
                const int yy = cy + oy;
                if (yy < 0 || yy >= kDropGridH) continue;
                for (int ox = -rx; ox <= rx; ox++) {
                    const int xx = cx + ox;
                    if (xx < 0 || xx >= kDropGridW) continue;
                    const int c = yy * kDropGridW + xx;
                    if (m_dropletCellCount[c] >= kDropCellCap) continue;
                    m_dropletCellCount[c]++;
                    ins.push_back(std::make_pair(c, i));
                    placed++;
                }
            }
        }
        int acc = 0;
        for (int c = 0; c < NC; c++) { m_dropletCellStart[c] = acc; acc += m_dropletCellCount[c]; }
        m_dropletCellStart[NC] = acc;
        std::vector<int> fill((size_t)NC, 0);
        m_dropletOrder.assign((size_t)acc, 0);
        if (!spread) {
            for (int i = 0; i < m; i++) {
                const int c = cellOf[i];
                if (c < 0) continue;
                m_dropletOrder[(size_t)m_dropletCellStart[c] + fill[c]] = i;
                fill[c]++;
            }
        } else {
            for (size_t k = 0; k < ins.size(); k++) {
                const int c = ins[k].first;
                m_dropletOrder[(size_t)m_dropletCellStart[c] + fill[c]] = ins[k].second;
                fill[c]++;
            }
        }
    };
    buildGrid(false);

    // ---- 3. surface tension: attraction, contact repulsion, coalescence ---
    const float mergeF = 1.0f - fminf(fmaxf(a.dropletMerge, 0.05f), 0.90f);
    // ---- droplet_coalesce: TOUCHING solids become one --------------------
    // See the key's comment in fluid.h: droplet_merge's overlap distance sits
    // INSIDE the contact repulsion's rest distance, so it is unreachable and
    // pairs park at dd = ri+rj forever -- the lumpy 5-lobed groups the user
    // photographed. This adds the missing rule: a contact is a coin flip per
    // frame at 8 * droplet_coalesce per second, so it lasts a moment and then
    // the two pour into one round droplet of the combined area.
    const float coal  = fminf(fmaxf(a.dropletCoalesce, 0.0f), 1.0f);
    const float coalP = (coal > 1e-4f && dt > 0.0f)
                      ? (1.0f - expf(-8.0f * coal * dt)) : 0.0f;
    const float mergeTau = (coalS > 0.01f) ? coalS : tau;
    std::vector<AcidDrop> satellites;
    // ---- rings behave like BUBBLES, not lone lenses ----------------------
    // The user, with photos of boiling oil and a soap raft: "it kinda looks
    // like this, should probably clump." So two rings ATTRACT over a short
    // range and then stop at a slight overlap instead of coalescing: their
    // two bands overlap into ONE dark wall, which is exactly what a foam does
    // and what a merge would destroy. kRingWall is that overlap (8% of the
    // radius sum), kRingMaxTouch the raft cap -- a ring with that many
    // neighbours is interior to a raft and stops pulling more in, so a raft
    // grows at its edges and never collapses into one solid mass.
    const float ringClump = fminf(fmaxf(a.dropletRingClump, 0.0f), 1.0f);
    const float kRingWall = 0.92f;
    const int   kRingMaxTouch = 6;          // hexagonal packing: fully ringed
    std::vector<int> touchNew((size_t)n, 0);
    for (int ci = 0; ci < NC; ci++) {
        const int cx0 = ci % kDropGridW, cy0 = ci / kDropGridW;
        const int e0 = m_dropletCellStart[ci] + m_dropletCellCount[ci];
        for (int si = m_dropletCellStart[ci]; si < e0; si++) {
            const int i = m_dropletOrder[si];
            if (m_acidDrops[i].r <= 0.0f) continue;
            // wr = 1 for everybody but a medium droplet (droplet_big_frac)
            const int wr = (bigOn && m_acidDrops[i].big) ? bigWalk : 1;
            for (int oy = -wr; oy <= wr; oy++) {
                const int yy = cy0 + oy;
                if (yy < 0 || yy >= kDropGridH) continue;
                for (int ox = -wr; ox <= wr; ox++) {
                    const int xx = cx0 + ox;
                    if (xx < 0 || xx >= kDropGridW) continue;
                    const int cj = yy * kDropGridW + xx;
                    const int e1 = m_dropletCellStart[cj] + m_dropletCellCount[cj];
                    // beyond the 3x3 only a medium droplet looks: it takes
                    // every partner there, and of two medium ones the lower index
                    const bool farCell = (ox < -1 || ox > 1 || oy < -1 || oy > 1);
                    for (int sj = m_dropletCellStart[cj]; sj < e1; sj++) {
                        const int j = m_dropletOrder[sj];
                        if (farCell ? (j == i || (m_acidDrops[j].big && j < i))
                                    : (j <= i)) continue;   // each pair once
                        AcidDrop& di = m_acidDrops[i];
                        AcidDrop& dj = m_acidDrops[j];
                        if (dj.kind != di.kind || dj.r <= 0.0f) continue;
                        // A pair already pouring into one another must not be
                        // shoved apart by the contact repulsion while the neck
                        // closes -- with droplet_coalesce_s that takes about a
                        // second, long enough for the repulsion to win.
                        if (coal > 1e-4f && (di.mergeTo == j || dj.mergeTo == i))
                            continue;
                        const float dx = (di.x - dj.x) * aspect, dy = di.y - dj.y;
                        const float d2 = dx * dx + dy * dy;
                        const float sum = di.r + dj.r;
                        const float reach = 3.0f * sum;
                        if (d2 > reach * reach || d2 < 1e-12f) continue;
                        const float dd = sqrtf(d2), inv = 1.0f / dd;
                        const bool bothRing = (di.ring && dj.ring);
                        const bool bothCrust = (di.crust && dj.crust);
                        if (bothRing && dd < sum * 1.06f) {
                            if (i < n) touchNew[i]++;
                            if (j < n) touchNew[j]++;
                        }
                        // ---- MEDIUM DROPLETS STAY ROUND (droplet_big_frac) --
                        // A droplet parked in contact fuses its field with its
                        // neighbour's: on a small pair that is a moment before
                        // they pour together, on a medium disc it is a knob on
                        // the circle (the user on the hole blobs: "mad werid
                        // and not circular"). So: a droplet whose centre is
                        // under the disc pours into it (it is hidden there),
                        // every other one is held off the rim, and nothing is
                        // pulled toward a medium droplet. The disc keeps its
                        // radius; it moves a quarter as much as what it pushes.
                        if (bigOn && (di.big || dj.big)) {
                            const bool both = (di.big && dj.big);
                            if (!both) {
                                AcidDrop& bg = di.big ? di : dj;
                                AcidDrop& sm = di.big ? dj : di;
                                if (dd < bg.r && sm.mergeTo < 0 && sm.rt > 0.0f && bg.rt > 0.0f) {
                                    sm.rt = 0.0f; sm.tauR = mergeTau;
                                    sm.mergeTo = di.big ? i : j;
                                    continue;
                                }
                            }
                            const float rest = sum * kBigSep;
                            if (dd >= rest) continue;
                            // strong enough that a pair pressed together at a
                            // birth or by the flow is apart within seconds
                            const float accB = (1.0f - dd / rest) * 1.2f;
                            const float ki = (di.big && !both) ? 0.25f : 1.0f;
                            const float kj = (dj.big && !both) ? 0.25f : 1.0f;
                            const float uxB = dx * inv, uyB = dy * inv;
                            ax[i] += uxB * accB * ki; ay[i] += uyB * accB * ki;
                            ax[j] -= uxB * accB * kj; ay[j] -= uyB * accB * kj;
                            continue;
                        }
                        // Two rings never merge (see kRingWall above); a ring
                        // meeting a SOLID droplet is absorbed by it instead,
                        // area-conserving, so the solid survives and the ring
                        // pours into it exactly as a small drop pours into a
                        // big one -- the survivor choice is the only change.
                        // ...or TOUCHING, when droplet_coalesce is on --
                        // SOLIDS ONLY. A ring that merely touches a solid must
                        // not be swallowed: rings are rare and the contact
                        // rule fires within a tenth of a second, so including
                        // them emptied the frame of hollow bubbles (measured:
                        // 14-25 rings alive instead of ~200). The original
                        // deep-overlap absorption still applies to them.
                        // The
                        // || short-circuits, so rf() is reached only when the
                        // key is on and the pair is in contact: with the key
                        // at 0 the random stream is untouched and the whole
                        // sim is bit-identical to before.
                        if (!bothRing
                            && di.mergeTo < 0 && dj.mergeTo < 0
                            && di.rt > 0.0f && dj.rt > 0.0f
                            && (dd < sum * mergeF
                                || (coalP > 0.0f && !di.ring && !dj.ring
                                    && !di.crust && !dj.crust
                                    && dd < sum && rf() < coalP))) {
                            // COALESCE, area-conserving. The survivor is the
                            // larger one; the other pours into it (rt -> 0,
                            // centre pulled in) instead of being deleted, so
                            // the metaball union necks them together.
                            bool iBig = (di.rt >= dj.rt);
                            // ...unless one of them is a ring: then the SOLID
                            // one is always the survivor, whatever the sizes,
                            // and what comes out is a solid droplet of the
                            // combined area. A hollow shell cannot swallow a
                            // filled drop.
                            if (di.ring != dj.ring) iBig = (dj.ring != 0);
                            const int bi = iBig ? i : j, sm = iBig ? j : i;
                            const float rb = m_acidDrops[bi].rt, rs = m_acidDrops[sm].rt;
                            const float area = rb * rb + rs * rs;
                            float rn = sqrtf(area);
                            if (rn > rMax) {
                                // A cap, or every droplet ends up one puddle.
                                // The surplus area leaves as a SATELLITE, the
                                // way a real over-fed drop pinches one off.
                                const float rs2 = area - rMax * rMax;
                                rn = rMax;
                                if (rs2 > rMin * rMin) {
                                    AcidDrop sat = AcidDrop{};
                                    const float ang = rf() * 6.2831853f;
                                    const float rr = sqrtf(rs2);
                                    sat.x = m_acidDrops[bi].x
                                          + cosf(ang) * (rMax + rr) * 1.25f / aspect;
                                    sat.y = m_acidDrops[bi].y + sinf(ang) * (rMax + rr) * 1.25f;
                                    sat.r = 0.0f; sat.rt = rr;
                                    sat.tauR = growTau;   // a pinch-off swells in too
                                    sat.kind = m_acidDrops[bi].kind; sat.mergeTo = -1;
                                    if (satDepth) sat.depth = m_acidDrops[bi].depth;   // W3
                                    satellites.push_back(sat);
                                }
                            }
                            m_acidDrops[bi].rt = rn;
                            m_acidDrops[bi].tauR = mergeTau;
                            m_acidDrops[sm].rt = 0.0f;
                            m_acidDrops[sm].tauR = mergeTau;
                            m_acidDrops[sm].mergeTo = bi;
                            continue;
                        }
                        float acc;
                        if (bothRing) {
                            // BUBBLE RAFT. Stop at kRingWall, so the two bands
                            // overlap into a single shared wall and the pair
                            // reads as foam rather than as two circles that
                            // happen to touch; pull harder than solid droplets
                            // do (droplet_ring_clump) so rings find each other
                            // and pack, and stop pulling once a ring is
                            // surrounded, which is what bounds a raft.
                            const float wall = sum * kRingWall;
                            if (dd < wall) {
                                acc = (1.0f - dd / wall) * 0.55f;
                            } else {
                                const bool full = (di.touch >= kRingMaxTouch
                                                || dj.touch >= kRingMaxTouch);
                                const float pull = full ? 0.0f
                                    : (a.dropletAttract + 3.0f * ringClump);
                                acc = -pull * (1.0f - (dd - wall) / (2.0f * sum)) * 0.05f;
                            }
                        } else if (bothCrust && dd < sum * 1.10f) {
                            // CRUST (brief AB): a crust bubble never
                            // coalesces, so if it is allowed to sit at exactly
                            // touching its metaball field fuses with its
                            // neighbour's and a pair comes out as one capsule
                            // -- a dash, not two bubbles. The lava-lamp refs
                            // show a dense foam of bubbles that stay round and
                            // separate, so crust rests just OUTSIDE contact
                            // and the two fields never merge.
                            acc = (1.0f - dd / (sum * 1.10f)) * 0.45f;
                        } else if (dd < sum) {
                            // contact repulsion: two droplets that are not
                            // merging stay round instead of interpenetrating
                            acc = (1.0f - dd / sum) * 0.35f;
                        } else if (bothCrust) {
                            // CRUST does not pull on itself. Attraction plus
                            // a fixed rest distance is a recipe for rigid
                            // rafts: half a dozen bubbles locked at one
                            // spacing, whose union field reads as a single
                            // lumpy polygon rather than as bubbles (the same
                            // failure the solid droplets had before
                            // droplet_coalesce). A crust is distributed by
                            // where it NUCLEATES -- biased by the mass's own
                            // thickness, which is what makes it patchy -- and
                            // then carried by the mass. So: separation only.
                            continue;
                        } else {
                            // short-range attraction within ~3 radii
                            acc = -a.dropletAttract * (1.0f - (dd - sum) / (2.0f * sum)) * 0.05f;
                        }
                        const float ux = dx * inv, uy = dy * inv;
                        ax[i] += ux * acc; ay[i] += uy * acc;
                        ax[j] -= ux * acc; ay[j] -= uy * acc;
                    }
                }
            }
        }
    }
    for (int i = 0; i < n; i++) {
        m_acidDrops[i].vx += ax[i] * dt;
        m_acidDrops[i].vy += ay[i] * dt;
        m_acidDrops[i].touch = touchNew[i];   // raft occupancy, for next frame
    }

    // ---- 4. retire the fully dissolved, keeping mergeTo consistent -------
    {
        std::vector<int> remap(m_acidDrops.size(), -1);
        std::vector<AcidDrop> keep;
        keep.reserve(m_acidDrops.size());
        // dissolve_s stretches a death from a third of a second to seconds, so
        // the array would fill with droplets that have long since gone under
        // the contribution gate -- dead weight in an O(blobs) per droplet loop.
        // The gate is EXACTLY zero below 0.35 of the resolvable floor (see the
        // sp2 ramp in the motion loop), so anything under that is retired at
        // once and nothing can pop.
        const float pxY0 = 1.0f / fmaxf((float)m_height, 1.0f);
        const float retireR = cons
            ? 0.34f * fmaxf(1.2f * pxY0, 1.6f * fmaxf(a.rimWidth, 1e-5f))
            : rMin * 0.08f;
        for (size_t i = 0; i < m_acidDrops.size(); i++) {
            const AcidDrop& d = m_acidDrops[i];
            if (d.rt <= 0.0f && d.r < retireR) continue;        // gone, invisibly
            remap[i] = (int)keep.size();
            keep.push_back(d);
        }
        for (size_t i = 0; i < keep.size(); i++) {
            AcidDrop& d = keep[i];
            d.mergeTo = (d.mergeTo >= 0 && d.mergeTo < (int)remap.size())
                      ? remap[d.mergeTo] : -1;
        }
        m_acidDrops.swap(keep);
    }
    for (size_t i = 0; i < satellites.size(); i++)
        if ((int)m_acidDrops.size() < kAcidMaxDrops) m_acidDrops.push_back(satellites[i]);

    // ---- 5. nucleation ---------------------------------------------------
    m_dropletSpawnAcc += fmaxf(a.dropletSpawn, 0.0f) * dt;
    if (m_dropletSpawnAcc > 60.0f) m_dropletSpawnAcc = 60.0f;
    // The film and the crust are counted SEPARATELY and topped up against
    // their own targets (brief AB): one population must never be able to
    // starve the other, which is exactly what "keep today's film and ADD the
    // crust" means. With droplet_mass_bias 0 the crust target is 0 and this
    // is the original single-population loop.
    int live = 0, liveCrust = 0;
    for (size_t i = 0; i < m_acidDrops.size(); i++) {
        if (m_acidDrops[i].rt <= 0.0f) continue;
        live++;
        if (m_acidDrops[i].crust) liveCrust++;
    }
    while (m_dropletSpawnAcc >= 1.0f) {
        m_dropletSpawnAcc -= 1.0f;
        if ((int)m_acidDrops.size() >= kAcidMaxDrops) break;
        const bool crustShort = (liveCrust < targetCrust);
        const bool filmShort  = ((live - liveCrust) < targetFilm);
        if (!crustShort && !filmShort) break;
        // When both are short, alternate by the accumulator's parity so
        // neither waits on the other.
        bool doCrust = crustShort && (!filmShort || ((live & 1) == 0));
        AcidDrop d;
        const int k = doCrust ? 2 : ((rf() < a.dropletInkFrac) ? 1 : 0);
        if (nucleate(k, d)) {
            m_acidDrops.push_back(d); live++;
            if (d.crust) liveCrust++;
        }
    }

    // ---- 6. final bin, the one the shader reads --------------------------
    buildGrid(true);
}

// ===========================================================================
// OIL DRAG ([liquid_acid] oil_drag / oil_dye_block)
//
// The user, watching acid-rise-12 live: "make it impossible for the fluid sim
// underneath, the mono ink, to get under the oil, or rather an intense
// friction that makes it hard for it to get under."
//
// Three compute passes, all of them skipped entirely while both keys are 0 --
// including the PSO compiles, which happen the first frame one of them is
// turned on. The mask is the SAME Wyvill field the display shader thresholds,
// which is what makes the holes behave: a negative blob is negative here too,
// so a hole is not oil, is not dragged, and the ink you see through it is the
// ink layer, exactly as it renders.
// ===========================================================================
void FluidRenderer::EnsureOilMask() {
    if (m_oilMaskMade) return;
    if (!m_device || !m_computeRS) return;
    auto makeCS = [&](const char* entry, ComPtr<ID3D12PipelineState>& pso) {
        ComPtr<ID3DBlob> cs = Compile(kComputeSrc, entry, "cs_5_0");
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = m_computeRS.Get();
        cd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        HR(m_device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&pso)));
    };
    makeCS("CSOilMask", m_psoOilMask);
    makeCS("CSOilDrag", m_psoOilDrag);
    makeCS("CSOilDyeBlock", m_psoOilDyeBlock);
    m_oilMaskMade = m_psoOilMask && m_psoOilDrag && m_psoOilDyeBlock;   // a lost device: retry after the rebuild
}

void FluidRenderer::StepOilDrag(float dt) {
    const LiquidAcidConfig& a = m_cfg.acid;
    const float drag  = fmaxf(a.oilDrag, 0.0f);
    const float block = fmaxf(a.oilDyeBlock, 0.0f);
    if (drag <= 1e-4f && block <= 1e-4f) return;
    if (!m_oilMask.res || !m_acidBlobUpload[m_frameIndex] || dt <= 0.0f) return;
    EnsureOilMask();
    if (!m_psoOilMask || !m_psoOilDrag || !m_psoOilDyeBlock) return;

    const int nb = (int)m_acidBlobs.size();
    if (nb <= 0) return;

    // Soft edge of the coverage threshold, in FIELD units. The Wyvill field
    // climbs from 0 to ~1 over a blob's support radius, so a band of a few sim
    // texels is a small fraction of the threshold; 0.10 of it puts the edge at
    // roughly 3-5 texels for the shipped radii, which is the couple of texels
    // the brief asks for without aliasing the mask into a staircase.
    const float edge = fmaxf(a.threshold, 0.05f) * 0.10f;

    SimCB cb = {};
    cb.texelW = 1.0f / m_simW;
    cb.texelH = 1.0f / m_simH;
    cb.dt = dt;
    cb.aspect = (float)m_width / fmaxf((float)m_height, 1.0f);
    cb.value  = a.threshold;
    cb.radius = a.supportScale;
    cb.pointX = (float)nb;
    cb.pointY = edge;

    m_cmd->SetComputeRootSignature(m_computeRS.Get());
    m_cmd->SetComputeRootShaderResourceView(
        7, m_acidBlobUpload[m_frameIndex]->GetGPUVirtualAddress());

    auto run = [&](ID3D12PipelineState* pso, Tex* s0, Tex* s1, Tex* dst) {
        if (s0) Transition(*s0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (s1) Transition(*s1, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(*dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_cmd->SetPipelineState(pso);
        cb.dimsW = dst->w; cb.dimsH = dst->h;
        m_cmd->SetComputeRoot32BitConstants(0, sizeof(SimCB) / 4, &cb, 0);
        m_cmd->SetComputeRootDescriptorTable(1, s0 ? s0->srv : dst->srv);
        m_cmd->SetComputeRootDescriptorTable(2, s1 ? s1->srv : dst->srv);
        m_cmd->SetComputeRootDescriptorTable(3, dst->uav);
        m_cmd->SetComputeRootDescriptorTable(4, dst->uav);
        m_cmd->SetComputeRootDescriptorTable(5, dst->uav);
        m_cmd->SetComputeRootDescriptorTable(6, dst->srv);
        m_cmd->Dispatch(Groups(dst->w), Groups(dst->h), 1);
    };

    // 1. coverage
    run(m_psoOilMask.Get(), nullptr, nullptr, &m_oilMask);
    UavBarrier(m_oilMask.res.Get());

    // 2. friction + rim deflection (+ viscous diffusion under the oil).
    // Applied ONCE per frame with the frame's own dt rather than inside the
    // substep loop: it is a damping term, fps-normalised the same way every
    // other decay in this file is (k = 1 - exp(-rate*dt)), so the result is
    // the same at 60 and at 144 fps and the solver never sees a discontinuity.
    if (drag > 1e-4f) {
        // oil_drag 1 -> ~6/s, i.e. ink under the oil loses ~99% of its speed
        // in half a second. Anything faster is indistinguishable from a wall.
        cb.colorR = 1.0f - expf(-6.0f * drag * dt);
        // The rim push is the mask gradient scaled to sim texels/s. Deliberately
        // gentle: this is meant to make ink SLIDE along the edge of an island,
        // not to blow a hole around it.
        cb.colorB = fminf(fmaxf(a.oilViscosity, 0.0f), 1.0f) * 0.6f;
        // texels/s of push per unit of mask gradient, times dt -- an
        // acceleration integrated over the frame, so it is fps-normalised too.
        cb.colorG = 90.0f * drag * dt;
        run(m_psoOilDrag.Get(), m_velocity.read, &m_oilMask, m_velocity.write);
        m_velocity.Swap();
    }
    // 3. dye under the oil fades out over ~1-2 s at oil_dye_block 1.
    if (block > 1e-4f) {
        cb.cap = 1.0f - expf(-1.2f * block * dt);
        run(m_psoOilDyeBlock.Get(), m_dye.read, &m_oilMask, m_dye.write);
        m_dye.Swap();
    }
}

// ---- THE RIG: one lamp, one lens, two kinds of motion ---------------------
// The LAMP drifts continuously -- a sum of slow sines over seconds to a
// minute, the frame's idle animation (brief Q). The FOCUS does not: the user
// was explicit that the tilt is not a drift but an OCCASIONAL READJUSTMENT.
// It holds still for tens of seconds to minutes, then somebody re-tilts the
// lens -- a second or two of eased movement with a slight overshoot and
// settle, like a hand letting go of a barrel -- and then it is still again.
// Both live on the same body and are stepped here, so everything downstream
// (the haze, the bloom, the depth of field and, later, the lid ghosts, the
// flare and the vignette centre) reads ONE set of numbers and agrees about
// where the rig is.
void FluidRenderer::RigState(float out[8]) const {
    out[0] = m_rig.lampX;  out[1] = m_rig.lampY;
    out[2] = m_rig.axisX;  out[3] = m_rig.axisY;
    out[4] = m_rig.tiltAngle * 57.29577951f;
    out[5] = m_rig.focus;
    out[6] = m_rig.shiftX; out[7] = m_rig.shiftY;
}

void FluidRenderer::StepCameraRig(float dt) {
    const PostConfig& po = m_cfg.post;
    const float DEG = 0.01745329252f;
    auto rf = [this]() {
        m_camRng ^= m_camRng << 13; m_camRng ^= m_camRng >> 17; m_camRng ^= m_camRng << 5;
        return (float)(m_camRng & 0xFFFFFFu) * (1.0f / 16777216.0f);
    };
    // ---- the lamp (was computed in kPostSrc; the numbers are its own, so
    // the haze and the bloom come out exactly as they did before) ----------
    m_rig.lampX = po.lightX + m_camLampOX;
    m_rig.lampY = po.lightY + m_camLampOY;
    if (po.lightDrift > 0.0005f) {
        const float kd = fminf(fmaxf(po.lightDrift, 0.0f), 1.0f);
        const float t  = AnimatorTime(2);
        m_rig.lampX += (0.055f * sinf(t * 0.0171f) + 0.030f * sinf(t * 0.0413f + 1.7f)) * kd;
        m_rig.lampY += (0.040f * sinf(t * 0.0233f + 0.6f) + 0.022f * sinf(t * 0.0561f + 2.3f)) * kd;
    }
    // ---- the LENS CENTRE -------------------------------------------------
    // Where the optical axis meets the dish. It is not the middle of the
    // screen and it does not stay put: the field curvature, the tilt origin
    // and (item Z) the null point of the chromatic split all hang off it, and
    // a null point burnt into one spot of an OLED for hours is exactly what
    // this whole family of effects exists to avoid. Its own slow sines, in
    // step with the lamp's idle drift but not in phase with it -- they are
    // different parts of one rig, not one part copied twice.
    m_rig.axisX = fminf(fmaxf(po.cameraAxisX + m_camAxOX, -2.0f), 3.0f);
    m_rig.axisY = fminf(fmaxf(po.cameraAxisY + m_camAxOY, -2.0f), 3.0f);
    if (po.lightDrift > 0.0005f) {
        const float kd = fminf(fmaxf(po.lightDrift, 0.0f), 1.0f);
        const float t  = AnimatorTime(2);
        m_rig.axisX += (0.026f * sinf(t * 0.0127f + 2.2f)
                      + 0.014f * sinf(t * 0.0331f + 5.1f)) * kd;
        m_rig.axisY += (0.022f * sinf(t * 0.0193f + 0.4f)
                      + 0.012f * sinf(t * 0.0447f + 3.6f)) * kd;
    }
    // ---- OLED PIXEL-SHIFT ORBIT (item V3) --------------------------------
    // The safety net under everything else: whatever the picture is doing,
    // the whole finished frame walks a slow closed orbit so no feature ever
    // holds one pixel. Two incommensurate periods (about 7 and 11 minutes) so
    // the path never repeats exactly, and the step per frame at 144 Hz is a
    // few thousandths of a pixel -- far below anything the eye can follow,
    // which is the entire trick: invisible motion that still moves.
    {
        const float t = AnimatorTime(2);
        m_rig.shiftX = po.pixelShiftPx * (0.72f * sinf(t * 0.0150f)
                                        + 0.28f * sinf(t * 0.0095f + 1.9f));
        m_rig.shiftY = po.pixelShiftPx * (0.72f * cosf(t * 0.0131f + 0.7f)
                                        + 0.28f * sinf(t * 0.0088f + 4.2f));
    }
    m_rig.tiltAmt = po.focusTilt;

    // ---- the LID (task V2) ------------------------------------------------
    // A sheet of glass resting on the dish is never quite still, and on an
    // OLED it had better not be: every reflection in it, every sheen and the
    // lamp's glint hang off this offset, so this is what keeps them off a
    // fixed pixel. Four incommensurate periods between 3 and 12 minutes --
    // the pattern never repeats within a session -- and the amplitude is
    // deliberately large in uv (about a sixth of the frame), because these
    // are the brightest things the lid adds.
    // This runs BEFORE the focus ring's early return: the lens may be bolted
    // down (focus_tilt_period 0) and the lid still has to wander.
    {
        const float tl = AnimatorTime(2);
        const float ix = 0.085f * sinf(tl * 0.0131f + 0.9f)
                       + 0.045f * sinf(tl * 0.0307f + 2.4f);
        const float iy = 0.070f * sinf(tl * 0.0163f + 1.9f)
                       + 0.038f * sinf(tl * 0.0271f + 0.3f);
        const float ir = 0.55f  * sinf(tl * 0.0089f)
                       + 0.28f  * sinf(tl * 0.0193f + 1.2f);
        m_rig.lidX = ix + m_lidTargX;
        m_rig.lidY = iy + m_lidTargY;
        m_rig.lidRot = ir + m_lidTargR;
        m_lidIdleX = ix; m_lidIdleY = iy; m_lidIdleR = ir;
    }

    // ---- the focus ring ---------------------------------------------------
    if (!m_camInit) {
        m_rig.tiltAngle = m_camAngleA = m_camAngleB = po.focusTiltAngle * DEG;
        m_rig.focus = m_camFocusA = m_camFocusB = po.cameraFocus;
        m_camHold = fmaxf(po.focusTiltPeriod, 0.0f) * (0.55f + 0.9f * rf());
        m_camMoveT = -1.0f;
        m_rig.movePhase = 1.0f;
        m_camInit = true;
    }
    // period 0 = the lens is bolted down: the authored angle and focus, and
    // not a single float of state moving. This is the shipped default.
    if (po.focusTiltPeriod < 0.01f) {
        m_rig.tiltAngle = po.focusTiltAngle * DEG;
        m_rig.focus     = po.cameraFocus;
        m_rig.movePhase = 1.0f;
        m_camMoveT = -1.0f;
        return;
    }
    if (m_camMoveT >= 0.0f) {
        m_camMoveT += dt;
        const float u = fminf(m_camMoveT / fmaxf(m_camMoveDur, 0.05f), 1.0f);
        // A damped spring written out in closed form rather than integrated:
        // starts from rest, overshoots by ~7% around the middle of the move,
        // settles. The last tenth is cross-faded to exactly 1 so the machine
        // has a clean end and the next hold starts from the target, not from
        // the spring's 0.7% residual.
        const float ec = expf(-5.2f * u);
        const float w  = 7.2f;
        float e = 1.0f - ec * (cosf(w * u) + (5.2f / w) * sinf(w * u));
        float t = (u - 0.90f) / 0.10f;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        e += (1.0f - e) * (t * t * (3.0f - 2.0f * t));
        m_rig.tiltAngle = m_camAngleA + (m_camAngleB - m_camAngleA) * e;
        m_rig.focus     = m_camFocusA + (m_camFocusB - m_camFocusA) * e;
        // ...and the LID slides with them, on the same spring, so the ghosts,
        // the sheen and the glint arrive when the focus does. This is the
        // user's "all the things move at once": never one effect alone.
        m_rig.lidX = m_lidIdleX + m_lidPrevX + (m_lidTargX - m_lidPrevX) * e;
        m_rig.lidY = m_lidIdleY + m_lidPrevY + (m_lidTargY - m_lidPrevY) * e;
        m_rig.lidRot = m_lidIdleR + m_lidPrevR + (m_lidTargR - m_lidPrevR) * e;
        // ...and the LAMP and the LENS CENTRE ride the same spring, so the
        // whole rig arrives at once. This is the user's model exactly: never
        // one effect moving alone.
        m_camLampOX = m_camLampAX + (m_camLampBX - m_camLampAX) * e;
        m_camLampOY = m_camLampAY + (m_camLampBY - m_camLampAY) * e;
        m_camAxOX   = m_camAxAX + (m_camAxBX - m_camAxAX) * e;
        m_camAxOY   = m_camAxAY + (m_camAxBY - m_camAxAY) * e;
        m_rig.movePhase = u;
        if (u >= 1.0f) {
            m_rig.tiltAngle = m_camAngleA = m_camAngleB;
            m_rig.focus     = m_camFocusA = m_camFocusB;
            m_camLampOX = m_camLampAX = m_camLampBX;
            m_camLampOY = m_camLampAY = m_camLampBY;
            m_camAxOX   = m_camAxAX = m_camAxBX;
            m_camAxOY   = m_camAxAY = m_camAxBY;
            m_camMoveT = -1.0f;
            m_camHold  = po.focusTiltPeriod * (0.55f + 0.9f * rf());
            m_lidPrevX = m_lidTargX; m_lidPrevY = m_lidTargY;
            m_lidPrevR = m_lidTargR;
        }
        return;
    }
    m_rig.movePhase = 1.0f;
    m_camHold -= dt;
    if (m_camHold > 0.0f) return;
    // A new readjustment. The angle is re-aimed by up to +-70 deg off the
    // authored one (a re-tilt, not a spin) and the focus distance by up to
    // +-0.22 of the depth range -- enough to hand the sharp plane to a
    // different layer of droplets, not enough to lose the picture.
    m_camAngleA = m_rig.tiltAngle;
    m_camFocusA = m_rig.focus;
    m_camAngleB = po.focusTiltAngle * DEG + (rf() * 2.0f - 1.0f) * 70.0f * DEG;
    m_camFocusB = po.cameraFocus + (rf() * 2.0f - 1.0f) * 0.22f;
    // The lamp swings further than the lens does, because on a real rig the
    // lamp is the thing somebody actually picks up and puts down again.
    const float rj = fminf(fmaxf(po.rigReadjust, 0.0f), 1.0f);
    m_camLampAX = m_camLampOX; m_camLampAY = m_camLampOY;
    m_camAxAX   = m_camAxOX;   m_camAxAY   = m_camAxOY;
    m_camLampBX = (rf() * 2.0f - 1.0f) * 0.28f * rj;
    m_camLampBY = (rf() * 2.0f - 1.0f) * 0.20f * rj;
    m_camAxBX   = (rf() * 2.0f - 1.0f) * 0.09f * rj;
    m_camAxBY   = (rf() * 2.0f - 1.0f) * 0.09f * rj;
    m_camMoveDur = fmaxf(po.focusTiltMoveS, 0.15f) * (0.8f + 0.4f * rf());
    // ...and the lid is shoved to a new resting offset by the same move.
    m_lidPrevX = m_lidTargX; m_lidPrevY = m_lidTargY; m_lidPrevR = m_lidTargR;
    // ...and the hue2 patches re-lay on the same move (brief AE): the phase
    // jump is eased in StepHueField, so they re-form over a second rather
    // than cutting.
    m_mixPhaseTarget += 3.0f + rf() * 5.0f;
    // ...which only ever grows: past ~1000 both move down by the SAME multiple of
    // 20*pi, so the glide between them is untouched and every sine that reads the
    // phase (m and 0.7*m: 20*pi is a whole turn of both) is unchanged. Only the
    // value-noise offset of the seed rows jumps, on the move that re-lays them
    // anyway (review 2026-10-04 U33).
    if (m_mixPhaseTarget > 1000.0f) {
        const float wrap = 62.831853f * floorf(m_mixPhase / 62.831853f);
        m_mixPhase -= wrap;
        m_mixPhaseTarget -= wrap;
    }
    m_lidTargX = (rf() * 2.0f - 1.0f) * 0.10f;
    m_lidTargY = (rf() * 2.0f - 1.0f) * 0.08f;
    m_lidTargR = (rf() * 2.0f - 1.0f) * 0.9f;
    m_camMoveT = 0.0f;
    m_rig.movePhase = 0.0f;
}

// ---- hue_anchor_weight (brief BV rule 4, pre-flight item 4) -------------
// The rotation lingers at the proven anchors. Density over the ABSOLUTE
// palette hue H: d(H) = 1 + 4*w*sum a_i*g(H - anchor_i), g a wrapped gaussian
// (sigma 20 deg); m_anchorCdf[j] = the integral of d over 0..j*360/256,
// normalised to 0..1 (257 entries). The linear clock u (0..1) advances the
// cumulative position from the palette's own hue h0: F(H) = F(h0) + u (mod
// 1), and the returned rotation is H - h0 (mod 360). u = 0 gives exactly 0
// (same table both ways), u -> 1 gives 360 (seamless wrap), monotone. At
// w = 1 magenta dwells ~5x longer per degree than a plain hue, the
// lime/mustard band ~2.4x shorter than linear. Table rebuilt only when w
// changes; one binary search per frame.
// The anchor density's normalised cumulative table (257 entries) for weight
// w; shared by the warp below and palette_start_hue's random draw (brief BW).
static void BuildAnchorCdf(float w, float cdf[257]) {
    static const float kAnc[4][2] = { {325.0f, 1.0f}, {215.0f, 0.6f},
                                      {355.0f, 0.5f}, {275.0f, 0.4f} };
    const float sig = 20.0f;
    double acc = 0.0;
    double dens[257];
    for (int j = 0; j <= 256; j++) {
        const float H = 360.0f * (float)j / 256.0f;
        double d = 1.0;
        for (int k = 0; k < 4; k++) {
            float dh = fmodf(fabsf(H - kAnc[k][0]), 360.0f);
            if (dh > 180.0f) dh = 360.0f - dh;
            d += 4.0 * w * kAnc[k][1] * exp(-0.5 * (dh / sig) * (dh / sig));
        }
        dens[j] = d;
    }
    cdf[0] = 0.0f;
    double cum[257]; cum[0] = 0.0;
    for (int j = 1; j <= 256; j++) {
        acc += 0.5 * (dens[j - 1] + dens[j]);
        cum[j] = acc;
    }
    for (int j = 0; j <= 256; j++) cdf[j] = (float)(cum[j] / acc);
    cdf[256] = 1.0f;
}

float FluidRenderer::AnchorWarpDeg(float u, float h0Deg, float w) const {
    if (fabsf(w - m_anchorCdfW) > 1e-6f) {
        BuildAnchorCdf(w, m_anchorCdf);
        m_anchorCdfW = w;
    }
    auto F = [&](float H) {          // H in [0,360) -> 0..1, piecewise linear
        const float x = H * (256.0f / 360.0f);
        int j = (int)floorf(x);
        if (j < 0) j = 0; if (j > 255) j = 255;
        const float t = x - (float)j;
        return m_anchorCdf[j] + (m_anchorCdf[j + 1] - m_anchorCdf[j]) * t;
    };
    auto Finv = [&](float y) {       // 0..1 -> H in [0,360]
        int lo = 0, hi = 256;
        while (hi - lo > 1) { const int mid = (lo + hi) >> 1; if (m_anchorCdf[mid] <= y) lo = mid; else hi = mid; }
        const float span = m_anchorCdf[lo + 1] - m_anchorCdf[lo];
        const float t = span > 1e-9f ? (y - m_anchorCdf[lo]) / span : 0.0f;
        return 360.0f * ((float)lo + t) / 256.0f;
    };
    float h0 = fmodf(h0Deg, 360.0f); if (h0 < 0.0f) h0 += 360.0f;
    const float f0 = F(h0);
    float y = f0 + u;
    if (y >= 1.0f) y -= 1.0f;
    float d = Finv(y) - h0;
    if (d < 0.0f) d += 360.0f;
    if (u > 0.5f && d < 1e-3f) d = 360.0f;   // the wrap end, never a snap back
    return d;
}

// The effective oil/ink/meniscus palette at palette-clock time clk (the
// sweep cross-fade + the hue_rotate_period rotation), exactly the code the
// constant upload runs with clk = AnimatorTime(0); factored out (brief BW)
// so palette_start_hue can read the palette's hue at any clock phase.
void FluidRenderer::EffectivePalette(float clk, float effOil[12], float effInk[12], float effMen[3]) const {
    const LiquidAcidConfig& a = m_cfg.acid;
    memcpy(effOil, a.oilColors, 12 * sizeof(float));
    memcpy(effInk, a.inkRamp, 12 * sizeof(float));
    memcpy(effMen, a.meniscusCol, 3 * sizeof(float));
    if (a.hueSweepPeriod > 0.01f) {
        int np = a.sweepCount;
        if (np < 1) np = 1;
        if (np > LiquidAcidConfig::kSweepMax) np = LiquidAcidConfig::kSweepMax;
        const float u = fmodf(clk / a.hueSweepPeriod, 1.0f) * np;
        const int k0 = (int)u % np, k1 = (k0 + 1) % np;
        // smoothstep the cross-fade so each pair gets a long settled stretch
        float f = u - floorf(u);
        f = f * f * (3.0f - 2.0f * f);
        float oilA[3], inkA[3];
        HsvLerp3(&a.sweepOil[k0 * 3], &a.sweepOil[k1 * 3], f, oilA);
        HsvLerp3(&a.sweepInk[k0 * 3], &a.sweepInk[k1 * 3], f, inkA);
        BuildAcidPalette(a, oilA, inkA, effOil, effInk, effMen);
        // Hand-authored four-shade entries (sweep_oil_N given twelve floats)
        // override the derived oil family: cross-fade the four shades one for
        // one, so the user's tile9 palettes land on screen exactly as they
        // were rendered instead of being rebuilt from a single anchor. An
        // entry without a full set contributes the family just derived for
        // it, so the two forms mix freely inside one list. Nothing here runs
        // unless an ini actually carries a full set.
        if (a.sweepOilFullSet[k0] || a.sweepOilFullSet[k1]) {
            float fam0[12], fam1[12], dummyInk[12], dummyMen[3];
            if (a.sweepOilFullSet[k0]) memcpy(fam0, &a.sweepOilFull[k0 * 12], sizeof(fam0));
            else BuildAcidPalette(a, &a.sweepOil[k0 * 3], &a.sweepInk[k0 * 3],
                                  fam0, dummyInk, dummyMen);
            if (a.sweepOilFullSet[k1]) memcpy(fam1, &a.sweepOilFull[k1 * 12], sizeof(fam1));
            else BuildAcidPalette(a, &a.sweepOil[k1 * 3], &a.sweepInk[k1 * 3],
                                  fam1, dummyInk, dummyMen);
            for (int ci = 0; ci < 4; ci++)
                HsvLerp3(&fam0[ci * 3], &fam1[ci * 3], f, &effOil[ci * 3]);
        }
    }
    // ---- global hue rotation (hue_rotate_period) -------------------------
    // The user's ask was "the hue of the ENTIRE screen can shift", so this is
    // one angle applied to the WHOLE oil family, never per blob: neighbouring
    // discs keep their relative shades and the frame reads as one palette
    // turning, not as confetti. The ink is untouched -- on the mono-ink
    // tile9 family it stays grey and the holes stay black through every hue.
    if (a.hueRotatePeriod > 0.01f) {
        float deg = 360.0f * fmodf(clk / a.hueRotatePeriod, 1.0f);
        // brief BV rule 4, hue_anchor_weight: a WARPED clock. 0 keeps the
        // line above exactly (identity by construction: this block is skipped).
        if (a.hueAnchorWeight > 0.0005f) {
            float h0, s0, v0;
            RgbToHsv(&effOil[0], h0, s0, v0);   // the palette's own hue, 0..1
            deg = AnchorWarpDeg(fmodf(clk / a.hueRotatePeriod, 1.0f),
                                h0 * 360.0f, a.hueAnchorWeight);
        }
        for (int ci = 0; ci < 4; ci++) HsvHueShiftCpu(&effOil[ci * 3], deg);
    }
}

// ---- Oil3D L1: LAMP 1 AS A SPECTRAL WINDOW -- the LUT ---------------------
// The model (port chat KEY SPEC v2 / SLOT SPEC FINAL): a continuous hue circle
// sampled at 48 points (7.5 deg), RELATIVE to the anchor (the film's own hue).
// A dye of hue H, band sigma and strength k at amount c transmits
//     T(h) = exp(-c k (1 - exp(-d^2 / (2 sigma^2)))),   d = h - H wrapped,
// and the film transmits the PRODUCT over its dyes (Beer-Lambert: the
// absorbances add), so two dyes walk the hue toward each other and opposites
// darken. What reaches the eye at sample h is E(h) T(h), E = lamp 1's window:
// ranges [lo, hi] (wrapping through 0, a second one while lo2 != hi2) with
// smoothstep edges `feather` wide -- monotone, no ringing -- box-filtered over
// each 7.5 deg bin so a gliding edge never steps -- times the level.
// The 64 entries run over the hue2 share u = k2 in 0..1:
//   TODAY (the reference): dye A1 (the palette's 'self' band) at the anchor
//     ROTATED by hue2Deg x u -- the spectral twin of today's AcidHueShift of
//     the film by HUE2_DEG x k2 -- under a white lamp;
//   NOW: the same base dye x dye_a2^(amount (1 - fB)) x dye_b1^(amount fB) x
//     dye_b2^(amount fB) under the window, fB = min(1, u / hue2Amt) = the
//     fluid-B (patch) fraction: A2 lives in the film, B1/B2 in the patches.
// Each entry is the CHANGE now vs today:
//   x  hue shift (deg): the move of the spectrum's CIRCULAR MEAN on the hue
//      circle (unwrapped along u so the shader's lerp never crosses +-180);
//   y  chroma: the spectrum's purity, the circular RESULTANT LENGTH R (1 = one
//      hue, 0 = flat = grey), mapped into today's headroom: >= 0 = that
//      fraction of the way from today's saturation to 1, < 0 = saturation x
//      (1 + y);
//   z  luminance ratio x level, capped at 1 (a window only removes light:
//      never above today): each sample weighted by the linear-light Y of the
//      pure hue anchor + h, so a window on green passes more than one on blue;
//   w  the bare window's own luminance ratio (a white surface under the lamp;
//      logged, read by nothing yet).
// Hue and chroma come from the circle itself, NOT from the RGB of the summed
// spectrum: the hexcone's sextants pulled the encoded-HSV centroid of a
// symmetric window 8 deg off the dye near yellow-green (the first live render
// turned the green film olive), and the 2020 gamut stretch amplifies any such
// shift near the gamut edge. On the circle a window symmetric about a dye
// moves nothing, at any anchor.
// Today's per-pixel colour (palette shades, lamp ramp, hue3) is what the
// shader applies the change to, so a white window with no dyes is today.
// Oil3D L2: lamp = 2 builds LAMP 2's LUT the same way from the lamp2_* keys
// (laLamp2; its z holds lamp2_level x the window's luminance ratio, <= 1, which
// the OIL_LAMP2 shader reads as lamp 2's light against lamp 1's); glow is
// lamp 1's alone (pass nullptr for lamp 2). For lamp = 1 every number below
// is computed exactly as before: only the source of the window keys changed.
// Oil3D L3 (2): an ADDED dye's passband on the SPECTRAL LINE (red = absolute
// hue 0 .. blue = 240; x a spectral hue on it). A dye whose own hue is on the
// line has one Gaussian band there, measured along the line (no wrap through
// the purples). A PURPLE dye (240..360) is two-lobed: a lobe beyond the blue
// end (centre H) and one beyond the red end (centre H - 360), weighted
// min(1, 2 (1 - p)) and min(1, 2 p), p = (H - 240) / 120, so it is exactly the
// blue dye at 240 and the red dye at 360 / 0 (continuous as a dye drifts across
// either end) and passes both ends fully at magenta (300).
static float LineBand(float x, float H, float sigma) {
    const float kEnd = 240.0f;
    const float s2 = 2.0f * fmaxf(sigma, 1.0f) * fmaxf(sigma, 1.0f);
    if (H <= kEnd) return expf(-(x - H) * (x - H) / s2);
    const float p = (H - kEnd) / (360.0f - kEnd);
    const float dB = x - H, dR = x - (H - 360.0f);
    return fmaxf(fminf(1.0f, 2.0f * (1.0f - p)) * expf(-dB * dB / s2), fminf(1.0f, 2.0f * p) * expf(-dR * dR / s2));
}

void FluidRenderer::BuildLampLut(int lamp, float anchorDeg, float hue2Deg, float hue2Amt,
                                 float out[64][4], float glow[4]) const {
    const LiquidAcidConfig& a = m_cfg.acid;
    const bool  l2 = (lamp == 2);
    // Oil3D L3b: lamp 0 = the WHITE state with the same dyes (a full window at
    // level 1, no glow, no log line): the white track's film (OIL_DYEWHITE)
    const bool  wh = (lamp == 0);
    const float wLo  = wh ? 0.0f : (l2 ? a.lamp2Lo  : a.lamp1Lo),  wHi  = wh ? 360.0f : (l2 ? a.lamp2Hi  : a.lamp1Hi);
    const float wLo2 = wh ? 0.0f : (l2 ? a.lamp2Lo2 : a.lamp1Lo2), wHi2 = wh ? 0.0f   : (l2 ? a.lamp2Hi2 : a.lamp1Hi2);
    const float wFeather = l2 ? a.lamp2Feather : a.lamp1Feather;
    const float wLevel   = wh ? 1.0f : (l2 ? a.lamp2Level : a.lamp1Level);
    const int   NS = 48;
    const float STEP = 360.0f / NS;
    const float D2R = 3.14159265f / 180.0f;
    auto wrap180 = [](float d) { d = fmodf(d + 180.0f, 360.0f); if (d < 0.0f) d += 360.0f; return d - 180.0f; };
    auto lin = [](float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); };
    auto sstep = [](float e0, float e1, float x) {
        float t = fminf(fmaxf((x - e0) / fmaxf(e1 - e0, 1e-6f), 0.0f), 1.0f); return t * t * (3.0f - 2.0f * t); };
    // per sample: the luminance of the pure hue anchor + h (linear light) and
    // the sample's direction on the circle
    float bY[NS], cs[NS], sn[NS], cR[NS], cG[NS], cB[NS];
    for (int i = 0; i < NS; i++) {
        float h = fmodf(anchorDeg + i * STEP, 360.0f); if (h < 0.0f) h += 360.0f;
        RGB c = HSVtoRGB(h / 360.0f, 1.0f, 1.0f);
        cR[i] = lin(c.r); cG[i] = lin(c.g); cB[i] = lin(c.b);
        bY[i] = 0.2126f * cR[i] + 0.7152f * cG[i] + 0.0722f * cB[i];
        cs[i] = cosf(i * STEP * D2R); sn[i] = sinf(i * STEP * D2R);
    }
    // the window, per bin (16 sub-samples), relative degrees
    const float F = fminf(fmaxf(wFeather, 0.5f), 90.0f);
    auto range = [&](float h, float lo, float hi) -> float {
        const float span = hi - lo;
        if (span >= 360.0f) return 1.0f;
        float w = fmodf(span, 360.0f); if (w < 0.0f) w += 360.0f;
        if (w <= 0.0f) return 0.0f;
        const float d = fabsf(wrap180(h - (lo + 0.5f * w)));
        return 1.0f - sstep(0.5f * w - 0.5f * F, 0.5f * w + 0.5f * F, d);
    };
    const bool two = wLo2 != wHi2;
    const float level = fminf(fmaxf(wLevel, 0.0f), 4.0f);
    float W[NS];
    float wY = 0.0f, tY = 0.0f;
    for (int i = 0; i < NS; i++) {
        float acc = 0.0f;
        for (int sI = 0; sI < 16; sI++) {
            const float h = i * STEP + ((sI + 0.5f) / 16.0f - 0.5f) * STEP;
            float v = range(h, wLo, wHi);
            if (two) v = fmaxf(v, range(h, wLo2, wHi2));
            acc += v;
        }
        W[i] = acc / 16.0f;
        wY += W[i] * bY[i]; tY += bY[i];
    }
    const float bareY = fminf(level * wY / fmaxf(tY, 1e-9f), 1.0f);
    // Oil3D L1b: the window's own COLOUR (the fog = lamp x medium and the
    // bloom_warmth veil in the post pass): each channel's share of the white
    // lamp the window passes, x level, luminance capped at 1 like bareY (a
    // window removes light). Per channel against the same white sums, so a
    // white window is exactly (1, 1, 1). Lamp 1 only (Oil3D L2: glow nullptr).
    if (glow) {
        float wr = 0.0f, wg = 0.0f, wb = 0.0f, tr = 0.0f, tg = 0.0f, tb = 0.0f;
        for (int i = 0; i < NS; i++) {
            wr += W[i] * cR[i]; wg += W[i] * cG[i]; wb += W[i] * cB[i];
            tr += cR[i];        tg += cG[i];        tb += cB[i];
        }
        float gr = level * wr / fmaxf(tr, 1e-9f), gg = level * wg / fmaxf(tg, 1e-9f),
              gb = level * wb / fmaxf(tb, 1e-9f);
        const float gy = 0.2126f * gr + 0.7152f * gg + 0.0722f * gb;
        if (gy > 1.0f) { gr /= gy; gg /= gy; gb /= gy; }
        glow[0] = gr; glow[1] = gg; glow[2] = gb;
        glow[3] = a.LampGlowWanted() ? 1.0f : 0.0f;
    }
    // the dyes (k 0 or amount 0 = absent)
    auto T = [&](float h, float H, float sig, float k, float c) {
        if (c <= 0.0f || k <= 0.0f) return 1.0f;
        const float d = wrap180(h - H);
        const float s2 = fmaxf(sig, 1.0f);
        return expf(-c * k * (1.0f - expf(-d * d / (2.0f * s2 * s2))));
    };
    const float a2 = fminf(fmaxf(a.dyeA2Amount, 0.0f), 1.0f);
    const float b1 = fminf(fmaxf(a.dyeB1Amount, 0.0f), 1.0f);
    const float b2 = fminf(fmaxf(a.dyeB2Amount, 0.0f), 1.0f);
    // ---- Oil3D L3 (2): FLUID B, and dye MIXING along the spectral line ------
    // fField: film_hue2_amt 0 with fluid B's dyes in: the OIL_FLUIDB shader
    // indexes the LUT by the field share f itself (u = f) and the reference
    // has no patch rotation (today shows no patch there). Otherwise as L1.
    const bool fField = a.FluidBWanted() && hue2Amt <= 0.0005f;
    // Fluid B's own dyes DISPLACE its 0-state dye (today's patch: the 'self'
    // band rotated by film_hue2; the film itself with hue2 off) as they come
    // in: exponent 1 - lamB f, lamB = the stronger B dye's amount, so at full
    // amounts fluid B is exactly B1 x B2 (two dyes per fluid). 0 = L1 exactly.
    const float lamB = fmaxf((a.filmDyeK[2] > 0.0f) ? b1 : 0.0f, (a.filmDyeK[3] > 0.0f) ? b2 : 0.0f);
    // The ADDED dyes (A2, B1, B2) mix on the SPECTRAL LINE, not the hue circle:
    // light is a line from red (absolute hue 0) to blue (kEnd = 240); the
    // purples between 240 and 360 are not light of their own but a MIX of the
    // line's two ends (weights linear across the purple arc). On a circle two
    // dyes 180 deg apart pass two symmetric overlaps (green AND magenta) that
    // cancel to grey; on the line the second overlap does not exist, so yellow
    // + blue passes green (the port chat's spectral proof). So: an added dye's
    // band is measured ALONG the line (LineBand: a purple dye is two-lobed,
    // continuous across both ends), and a purple sample transmits its two end
    // components through the added dyes separately: T = base(h) x sum_c w_c
    // base(c) E(c) / sum_c w_c base(c). The base ('self', the film's own dye) keeps L1's circle, so a
    // window over the film alone (no added dye) is L1 to the bit.
    const bool extra = (a2 > 0.0f && a.filmDyeK[1] > 0.0f) || (b1 > 0.0f && a.filmDyeK[2] > 0.0f)
                    || (b2 > 0.0f && a.filmDyeK[3] > 0.0f);
    const float kEnd = 240.0f;
    auto absH = [](float x) { x = fmodf(x, 360.0f); return x < 0.0f ? x + 360.0f : x; };
    float dyeAbs[4];
    for (int d = 0; d < 4; d++) dyeAbs[d] = absH(anchorDeg + a.filmDyeHue[d]);
    auto Tline = [&](float x, int d, float c) {   // x = absolute spectral hue in [0, kEnd]
        if (c <= 0.0f || a.filmDyeK[d] <= 0.0f) return 1.0f;
        return expf(-c * a.filmDyeK[d] * (1.0f - LineBand(x, dyeAbs[d], a.filmDyeSigma[d])));
    };
    float prevH = 0.0f;
    for (int j = 0; j < 64; j++) {
        const float u  = j / 63.0f;
        const float fB = fField ? u : (hue2Amt > 1e-4f ? fminf(u / hue2Amt, 1.0f) : 0.0f);
        const float Hb = fField ? 0.0f : hue2Deg * u;
        const float cBase = 1.0f - lamB * fB;               // fluid B displaces its 0-state dye
        const float cA2 = a2 * (1.0f - fB), cB1 = b1 * fB, cB2 = b2 * fB;
        auto E = [&](float x) { return Tline(x, 1, cA2) * Tline(x, 2, cB1) * Tline(x, 3, cB2); };
        float rP = 0, rX = 0, rY = 0, rL = 0;     // reference: power, circle sums, luminance
        float cP = 0, cX = 0, cY = 0, cL = 0;     // now
        for (int i = 0; i < NS; i++) {
            const float h  = i * STEP;
            const float tr = T(h, Hb, a.filmDyeSigma[0], a.filmDyeK[0], 1.0f);
            float tc = tr;
            if (extra) {
                const float tb = (cBase < 1.0f) ? T(h, Hb, a.filmDyeSigma[0], a.filmDyeK[0], cBase) : tr;
                const float hA = absH(anchorDeg + h);
                if (hA <= kEnd) {
                    tc = tb * E(hA);
                } else {
                    const float wb = (360.0f - hA) / (360.0f - kEnd), wr = 1.0f - wb;
                    const float bB = T(kEnd - anchorDeg, Hb, a.filmDyeSigma[0], a.filmDyeK[0], cBase);
                    const float bR = T(-anchorDeg, Hb, a.filmDyeSigma[0], a.filmDyeK[0], cBase);
                    tc = tb * (wb * bB * E(kEnd) + wr * bR * E(0.0f)) / fmaxf(wb * bB + wr * bR, 1e-12f);
                }
            }
            tc *= W[i];
            rP += tr; rX += tr * cs[i]; rY += tr * sn[i]; rL += tr * bY[i];
            cP += tc; cX += tc * cs[i]; cY += tc * sn[i]; cL += tc * bY[i];
        }
        const float Rr = sqrtf(rX * rX + rY * rY) / fmaxf(rP, 1e-12f);
        const float Rc = sqrtf(cX * cX + cY * cY) / fmaxf(cP, 1e-12f);
        const float lum = fminf(level * cL / fmaxf(rL, 1e-12f), 1.0f);
        // a black or flat (grey) spectrum has no hue: keep the last entry's shift
        float dH = (Rc > 1e-4f && cP > 1e-9f)
                 ? wrap180((atan2f(cY, cX) - atan2f(rY, rX)) / D2R) : prevH;
        if (j > 0) dH = prevH + wrap180(dH - prevH);          // unwrap along u
        prevH = dH;
        float y;
        if (Rc >= Rr) y = (Rc - Rr) / fmaxf(1.0f - Rr, 1e-4f);
        else          y = Rc / fmaxf(Rr, 1e-4f) - 1.0f;
        out[j][0] = dH;
        out[j][1] = fminf(fmaxf(y, -1.0f), 1.0f);
        out[j][2] = lum;
        out[j][3] = bareY;
    }
    // the shot log: what the lamp does to the plain film (k2 = 0) and to the
    // patch cores (k2 = 1), every 10 s (the anchor drifts with the palette).
    // Lamp 1's line is unchanged; lamp 2's ([lamp2]) has its geometry instead
    // of the glow.
    static double s_lastLog[2] = { -1e9, -1e9 };
    double& lastLog = s_lastLog[l2 ? 1 : 0];
    if (!wh && (m_time - lastLog >= 10.0 || m_time < lastLog)) {
        lastLog = m_time;
        if (!l2) {
            printf("[lamp1] t=%.1f anchor %.1f hue2 %.1f x %.2f window %.0f..%.0f r2 %.0f..%.0f feather %.0f level %.2f"
                   " | k2=0: hue %+.1f chroma %+.2f lum %.3f | k2=1: hue %+.1f chroma %+.2f lum %.3f | bare lum %.3f"
                   " | glow %.3f %.3f %.3f\n",
                   (double)m_time, anchorDeg, hue2Deg, hue2Amt, a.lamp1Lo, a.lamp1Hi, a.lamp1Lo2, a.lamp1Hi2,
                   F, level, out[0][0], out[0][1], out[0][2], out[63][0], out[63][1], out[63][2], bareY,
                   glow ? glow[0] : 1.0f, glow ? glow[1] : 1.0f, glow ? glow[2] : 1.0f);
        } else {
            printf("[lamp2] t=%.1f anchor %.1f hue2 %.1f x %.2f window %.0f..%.0f r2 %.0f..%.0f feather %.0f level %.2f"
                   " | k2=0: hue %+.1f chroma %+.2f lum %.3f | k2=1: hue %+.1f chroma %+.2f lum %.3f | bare lum %.3f"
                   " | elev %.1f azim %+.1f (rel. lamp 1) size %.1f\n",
                   (double)m_time, anchorDeg, hue2Deg, hue2Amt, wLo, wHi, wLo2, wHi2,
                   F, level, out[0][0], out[0][1], out[0][2], out[63][0], out[63][1], out[63][2], bareY,
                   a.lamp2Elev, a.lamp2Azim, a.lamp2Size);
        }
    }
}

// ---- Oil3D L3 (3): THE MASSES UNDER THE LAMPS -- laMass[4] ------------------
// Port chat L3 pre-flight (iii). Both are per-frame CONSTANTS, one per lamp
// (not the f LUT): a mass is one dye, not a field.
//   DYED MASS (dye_lum > 0): the mass dye = ONE dye of the 'self' band at its
//     own absolute hue (dye_hue, with dye_hue_follow already folded in);
//     reference = it under a white lamp, now = it under lamp j's window x
//     level: hue shift / chroma / luminance ratio exactly as one LUT entry
//     (circular statistics; lum <= 1, lamp 2's x lamp2_level). The shader
//     applies it to today's per-pixel dye colour (OilMassWin).
//   DARK-DYE OIL (oil_dye_amount > 0): the oil's transmittance is 0 except ONE
//     band at oil_dye (rel. to the anchor), sigma 8.5 deg (~20 deg FWHM),
//     peak 0.20 x amount. The light that crosses it came through the film it
//     floats on (fluid A, f = 0: the 'self' dye x dye_a2 on the spectral line,
//     as the LUT's "now") under lamp j's window: rgb_j = sum E_j T_A T_oil
//     rgb(h) / sum T_ref bY (T_ref = the white film, bY the hue's linear Y),
//     i.e. per unit of the WHITE film's luminance: Y(rgb_j) <= 0.2 x amount x
//     the windowed film's own ratio, so the dark oil peaks at <= 20 % of the
//     adjacent open film under the same lamp. Two lamps: (rgb_1 + rgb_2) / (1 +
//     lamp2Z), the film sum's normaliser. White state: rgb_w (T_A = T_ref, E =
//     1). A window off the band or a film passing nothing in it = black oil.
void FluidRenderer::BuildMassConsts(float anchorDeg, float massHueDeg, float lamp2Z,
                                    float out[4][4]) const {
    const LiquidAcidConfig& a = m_cfg.acid;
    const int   NS = 48;
    const float STEP = 360.0f / NS;
    const float D2R = 3.14159265f / 180.0f;
    auto wrap180 = [](float d) { d = fmodf(d + 180.0f, 360.0f); if (d < 0.0f) d += 360.0f; return d - 180.0f; };
    auto lin = [](float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); };
    auto sstep = [](float e0, float e1, float x) {
        float t = fminf(fmaxf((x - e0) / fmaxf(e1 - e0, 1e-6f), 0.0f), 1.0f); return t * t * (3.0f - 2.0f * t); };
    auto absH = [](float x) { x = fmodf(x, 360.0f); return x < 0.0f ? x + 360.0f : x; };
    float bY[NS], cs[NS], sn[NS], cR[NS], cG[NS], cB[NS];
    for (int i = 0; i < NS; i++) {
        RGB c = HSVtoRGB(absH(anchorDeg + i * STEP) / 360.0f, 1.0f, 1.0f);
        cR[i] = lin(c.r); cG[i] = lin(c.g); cB[i] = lin(c.b);
        bY[i] = 0.2126f * cR[i] + 0.7152f * cG[i] + 0.0722f * cB[i];
        cs[i] = cosf(i * STEP * D2R); sn[i] = sinf(i * STEP * D2R);
    }
    // the two lamps' windows x level (BuildLampLut's window, verbatim)
    float E[2][NS];
    for (int l = 0; l < 2; l++) {
        const bool l2 = (l == 1);
        const float wLo  = l2 ? a.lamp2Lo  : a.lamp1Lo,  wHi  = l2 ? a.lamp2Hi  : a.lamp1Hi;
        const float wLo2 = l2 ? a.lamp2Lo2 : a.lamp1Lo2, wHi2 = l2 ? a.lamp2Hi2 : a.lamp1Hi2;
        const float F = fminf(fmaxf(l2 ? a.lamp2Feather : a.lamp1Feather, 0.5f), 90.0f);
        const float level = fminf(fmaxf(l2 ? a.lamp2Level : a.lamp1Level, 0.0f), 4.0f);
        auto range = [&](float h, float lo, float hi) -> float {
            const float span = hi - lo;
            if (span >= 360.0f) return 1.0f;
            float w = fmodf(span, 360.0f); if (w < 0.0f) w += 360.0f;
            if (w <= 0.0f) return 0.0f;
            const float d = fabsf(wrap180(h - (lo + 0.5f * w)));
            return 1.0f - sstep(0.5f * w - 0.5f * F, 0.5f * w + 0.5f * F, d);
        };
        const bool two = wLo2 != wHi2;
        for (int i = 0; i < NS; i++) {
            float acc = 0.0f;
            for (int sI = 0; sI < 16; sI++) {
                const float h = i * STEP + ((sI + 0.5f) / 16.0f - 0.5f) * STEP;
                float v = range(h, wLo, wHi);
                if (two) v = fmaxf(v, range(h, wLo2, wHi2));
                acc += v;
            }
            E[l][i] = (l2 && !a.Lamp2Wanted()) ? 0.0f : level * acc / 16.0f;
        }
    }
    auto T = [&](float d, float sig, float k, float c) {
        if (c <= 0.0f || k <= 0.0f) return 1.0f;
        const float s2 = fmaxf(sig, 1.0f);
        return expf(-c * k * (1.0f - expf(-d * d / (2.0f * s2 * s2))));
    };
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) out[r][c] = 0.0f;
    // ---- the dyed mass: one sample product per lamp
    if (a.dyeSat > 1e-4f && a.dyeLum > 1e-4f) {
        const float relM = wrap180(massHueDeg - anchorDeg);
        for (int l = 0; l < 2; l++) {
            if (l == 1 && !a.Lamp2Wanted()) break;
            float rP = 0, rX = 0, rY = 0, rL = 0, cP = 0, cX = 0, cY = 0, cL = 0;
            for (int i = 0; i < NS; i++) {
                const float tr = T(wrap180(i * STEP - relM), 55.0f, 1.8f, 1.0f);
                const float tc = tr * E[l][i];
                rP += tr; rX += tr * cs[i]; rY += tr * sn[i]; rL += tr * bY[i];
                cP += tc; cX += tc * cs[i]; cY += tc * sn[i]; cL += tc * bY[i];
            }
            const float Rr = sqrtf(rX * rX + rY * rY) / fmaxf(rP, 1e-12f);
            const float Rc = sqrtf(cX * cX + cY * cY) / fmaxf(cP, 1e-12f);
            const float dH = (Rc > 1e-4f && cP > 1e-9f) ? wrap180((atan2f(cY, cX) - atan2f(rY, rX)) / D2R) : 0.0f;
            float y = (Rc >= Rr) ? (Rc - Rr) / fmaxf(1.0f - Rr, 1e-4f) : Rc / fmaxf(Rr, 1e-4f) - 1.0f;
            out[l][0] = dH;
            out[l][1] = fminf(fmaxf(y, -1.0f), 1.0f);
            out[l][2] = fminf(cL / fmaxf(rL, 1e-12f), 1.0f);
        }
    }
    // ---- the dark-dye oil
    const float amt = fminf(fmaxf(a.oilDyeAmount, 0.0f), 1.0f);
    if (amt > 0.0005f) {
        const float a2 = fminf(fmaxf(a.dyeA2Amount, 0.0f), 1.0f);
        const bool  hasA2 = a2 > 0.0f && a.filmDyeK[1] > 0.0f;
        const float kEnd = 240.0f;
        const float H2 = absH(anchorDeg + a.filmDyeHue[1]);
        auto TA2 = [&](float x) {   // dye_a2 on the spectral line (the LUT's "now" at f = 0)
            if (!hasA2) return 1.0f;
            return expf(-a2 * a.filmDyeK[1] * (1.0f - LineBand(x, H2, a.filmDyeSigma[1])));
        };
        float wr = 0, wg = 0, wb = 0, sy = 0;
        float nr[2] = {}, ng[2] = {}, nb[2] = {};
        for (int i = 0; i < NS; i++) {
            const float h  = i * STEP;
            const float tR = T(wrap180(h), a.filmDyeSigma[0], a.filmDyeK[0], 1.0f);     // the white film
            float tA = tR;
            if (hasA2) {
                const float hA = absH(anchorDeg + h);
                if (hA <= kEnd) tA = tR * TA2(hA);
                else {
                    const float wB = (360.0f - hA) / (360.0f - kEnd), wR = 1.0f - wB;
                    const float bB = T(wrap180(kEnd - anchorDeg), a.filmDyeSigma[0], a.filmDyeK[0], 1.0f);
                    const float bR = T(wrap180(-anchorDeg), a.filmDyeSigma[0], a.filmDyeK[0], 1.0f);
                    tA = tR * (wB * bB * TA2(kEnd) + wR * bR * TA2(0.0f)) / fmaxf(wB * bB + wR * bR, 1e-12f);
                }
            }
            const float d  = wrap180(h - a.oilDye);
            const float tO = 0.20f * amt * expf(-d * d / (2.0f * 8.5f * 8.5f));
            sy += tR * bY[i];
            wr += tR * tO * cR[i]; wg += tR * tO * cG[i]; wb += tR * tO * cB[i];
            for (int l = 0; l < 2; l++) {
                const float t = E[l][i] * tA * tO;
                nr[l] += t * cR[i]; ng[l] += t * cG[i]; nb[l] += t * cB[i];
            }
        }
        const float inv = 1.0f / fmaxf(sy, 1e-12f);
        const float n2 = 1.0f / (1.0f + fmaxf(lamp2Z, 0.0f));
        out[2][0] = (nr[0] + nr[1]) * inv * n2;
        out[2][1] = (ng[0] + ng[1]) * inv * n2;
        out[2][2] = (nb[0] + nb[1]) * inv * n2;
        out[3][0] = wr * inv; out[3][1] = wg * inv; out[3][2] = wb * inv;
        out[3][3] = 1.0f;
    }
    static double s_lastLog = -1e9;
    if (m_time - s_lastLog >= 10.0 || m_time < s_lastLog) {
        s_lastLog = m_time;
        const float yW = 0.2126f * out[3][0] + 0.7152f * out[3][1] + 0.0722f * out[3][2];
        const float yN = 0.2126f * out[2][0] + 0.7152f * out[2][1] + 0.0722f * out[2][2];
        printf("[mass] t=%.1f anchor %.1f | dyed mass hue %.1f: lamp1 hue %+.1f chroma %+.2f lum %.3f,"
               " lamp2 hue %+.1f chroma %+.2f lum %.3f | dark oil %+.0f x %.2f: Y/film windowed %.4f white %.4f"
               " rgb %.4f %.4f %.4f\n",
               (double)m_time, anchorDeg, massHueDeg, out[0][0], out[0][1], out[0][2], out[1][0], out[1][1],
               out[1][2], a.oilDye, amt, yN, yW, out[2][0], out[2][1], out[2][2]);
    }
}

void FluidRenderer::UploadAcidConstants() {
    const LiquidAcidConfig& a = m_cfg.acid;
    const UINT fi = m_frameIndex;
    if (!m_acidBlobData[fi] || !m_acidParamData[fi]) return;

    // ---- effective palette for this frame -------------------------------
    // With the sweep off this is just the authored palette. With it on, the
    // oil anchor and the ink mid tone cross-fade between curated vivid pairs
    // and the rest of the palette is rebuilt around them using the authored
    // palette's own S/V ratios.
    float effOil[12], effInk[12], effMen[3];
    EffectivePalette(AnimatorTime(0), effOil, effInk, effMen);
    // Oil3D L4: the colour director holds the film hue (src/director.cpp): the
    // whole oil family turns so its hue lands there, the same one-angle turn as
    // hue_rotate_period. < 0 (the default) = untouched.
    if (m_dirHue >= 0.0f) {
        float h0, s0, v0;
        RgbToHsv(&effOil[0], h0, s0, v0);
        for (int ci = 0; ci < 4; ci++) HsvHueShiftCpu(&effOil[ci * 3], m_dirHue - h0 * 360.0f);
    }
    // ---- oil_saturation: one vividness knob, whatever fed the palette ----
    if (fabsf(a.oilSaturation - 1.0f) > 0.001f) {
        for (int ci = 0; ci < 4; ci++) {
            float h, s, v;
            RgbToHsv(&effOil[ci * 3], h, s, v);
            RGB c = HSVtoRGB(h, fminf(fmaxf(s * a.oilSaturation, 0.0f), 1.0f), v);
            effOil[ci * 3 + 0] = c.r; effOil[ci * 3 + 1] = c.g; effOil[ci * 3 + 2] = c.b;
        }
    }
    // ---- DYE THE BLACK (brief AG / AM) -----------------------------------
    // "Add ability to dye the black ink." The references are lava lamps: the
    // dark body is not black, it is a DEEP translucent colour with the lamp
    // showing through it.
    //
    // This used to dye the ink RAMP (effInk) here, on the theory that the dark
    // masses are drawn from it. They are not. Measured on 2026-09-22 (branch
    // dye4, trace written out in full at the dye block in shaders.h): the LIVE
    // preset runs ink_mode=water, and in water mode the mass colour comes from
    // InkWater()'s paper_color, never from laInk[] -- which this shader reads
    // in exactly two places, the (dead) bands branch and the toe_tint lift,
    // and acid-rise-12 leaves toe_tint at 0. That is why two rounds of edits
    // to the ramp rendered byte-identical. So all this does now is hand the
    // shader the dye COLOUR and amount; the wax itself is built in the display
    // pass, where the mass's own depth (-sdf) is available and the thickness
    // can actually read as translucency.
    //
    // dye_sat 0 or dye_lum 0 sends amount 0, the shader skips the branch, and
    // the frame is byte-identical -- as is style=fluid, which never gets here.
    // Hue (0..1) and saturation go over as they are and the shader builds the
    // colour: the acid cbuffer has single scalars free, not a whole vector,
    // and the acid PSO already carries InkHsv2Rgb from the shared ink block.
    float dyeHueN = 0.0f, dyeSat = 0.0f, dyeAmt = 0.0f;
    if (a.dyeSat > 1e-4f && a.dyeLum > 1e-4f) {
        float hue = a.dyeHue;
        if (a.dyeHueFollow) {
            // ...as an OFFSET from the film's own current hue, so the pair
            // turns together under hue_rotate_period and the sweep instead of
            // drifting into whatever clash the clock happens to land on.
            float fh, fs, fv;
            RgbToHsv(&effOil[0], fh, fs, fv);
            hue += fh * 360.0f;
        }
        float hn = fmodf(hue, 360.0f);
        if (hn < 0.0f) hn += 360.0f;
        hn *= (1.0f / 360.0f);
        // The shader takes it at value 1 and scales by the amount and by the
        // wax's own thickness, so the hue is carried at full vividness and
        // only the light level changes with depth.
        dyeHueN = hn;
        dyeSat  = fminf(fmaxf(a.dyeSat, 0.0f), 1.0f);
        dyeAmt  = fminf(fmaxf(a.dyeLum, 0.0f), 1.0f);
    }
    // ---- brief BK: masses and droplets as two elements -------------------
    // The droplet holes take their own dye (dye_droplet_hue/sat/lum, each -1
    // = inherit the mass dye) times dye_droplets, and the masses take theirs
    // times dye_masses. The droplet colour is packed as HUE, SATURATION and
    // LEVEL, 8 bits each, in one float (h8 + 256 s8 + 65536 l8, exact below
    // 2^24) -- the acid cbuffer has one scalar left, not a vector -- and the
    // shader rebuilds it as InkHsv2Rgb(h, s, 1) * l. Brief BP: it used to be
    // quantised to 8-bit RGB AFTER the level multiply, i.e. a 0.3-level dye
    // had ~77 codes per channel and a 10-20% hue/sat error; HSV keeps the hue
    // to 1.4 deg and the saturation to 1/255 at any level. -1 = split off, and
    // then the shader runs exactly the old dye line. While the split is on,
    // dye_masses rides on DYE_AMT, which the shader's split path reads as the
    // mass colour's level and nothing else.
    float dyeDropRgb = -1.0f;
    const bool dyeSplit = fabsf(a.dyeDroplets - 1.0f) > 1e-4f || fabsf(a.dyeMasses - 1.0f) > 1e-4f
                       || a.dyeDropHue >= 0.0f || a.dyeDropSat >= 0.0f || a.dyeDropLum >= 0.0f;
    if (dyeSplit) {
        const float dSat = (a.dyeDropSat >= 0.0f) ? a.dyeDropSat : a.dyeSat;
        const float dLum = (a.dyeDropLum >= 0.0f) ? a.dyeDropLum : a.dyeLum;
        float h8 = 0.0f, s8 = 0.0f, l8 = 0.0f;
        auto q8 = [](float v) { return floorf(fminf(fmaxf(v, 0.0f), 1.0f) * 255.0f + 0.5f); };
        if (dSat > 1e-4f && dLum > 1e-4f) {
            float hue = (a.dyeDropHue >= 0.0f) ? a.dyeDropHue : a.dyeHue;
            if (a.dyeHueFollow) {
                float fh, fs, fv;
                RgbToHsv(&effOil[0], fh, fs, fv);
                hue += fh * 360.0f;
            }
            float hn = fmodf(hue, 360.0f);
            if (hn < 0.0f) hn += 360.0f;
            const float k = fminf(fmaxf(dLum, 0.0f), 1.0f) * fminf(fmaxf(a.dyeDroplets, 0.0f), 1.0f);
            h8 = fmodf(floorf(hn / 360.0f * 256.0f + 0.5f), 256.0f);   // hue = h8 / 256
            s8 = q8(dSat);
            l8 = q8(k);
        }
        dyeDropRgb = h8 + 256.0f * s8 + 65536.0f * l8;
        dyeAmt *= fminf(fmaxf(a.dyeMasses, 0.0f), 1.0f);
    }

    AcidBlobGPU* dst = (AcidBlobGPU*)m_acidBlobData[fi];
    const int n = (int)m_acidBlobs.size();
    for (int i = 0; i < n && i < kAcidMaxBlobs; i++) {
        const AcidBlob& b = m_acidBlobs[i];
        dst[i].a[0] = b.x;
        dst[i].a[1] = b.y;
        dst[i].a[2] = b.baseR * (1.0f + a.breathAmt * sinf(b.phase));
        dst[i].a[3] = b.wgt;
        int ci = (b.colIdx < 0 || b.colIdx > 3) ? 0 : b.colIdx;
        if (a.accentMode == 1) {
            // Shade 4 is the ACCENT and belongs to the accent set alone. A
            // blob that drew it at seed time and is not in that set is folded
            // back into the base three, which is the half of this that
            // matters: it is what makes a big mass in the complement
            // impossible rather than merely unlikely.
            if (ci == 3) ci = (int)(AcidAccentHash((uint32_t)i, 0x9E3779B1u) * 3.0f) % 3;
            const float mx = fminf(fmaxf(b.accentMix, 0.0f), 1.0f);
            for (int ch = 0; ch < 3; ch++)
                dst[i].b[ch] = effOil[ci * 3 + ch]
                             + (effOil[9 + ch] - effOil[ci * 3 + ch]) * mx;
        } else {
            dst[i].b[0] = effOil[ci * 3 + 0];
            dst[i].b[1] = effOil[ci * 3 + 1];
            dst[i].b[2] = effOil[ci * 3 + 2];
        }
        // .w = per-blob anisotropy for rise_stretch (0 = round, the shipped
        // look). A rising drop in a lamp is a teardrop while it is moving and
        // relaxes round as it slows, and a small one is dragged out far more
        // than a heavy disc -- so the amount is speed relative to the rise
        // speed, scaled by how small the blob is. Capped: past ~0.8 the
        // metaball stops reading as a blob and starts reading as a smear.
        float stretch = 0.0f;
        if (a.riseStretch > 0.0005f && a.riseSpeed > 1e-6f) {
            const float sp = sqrtf(b.vx * b.vx + b.vy * b.vy);
            const float sf = fminf(sp / (a.riseSpeed * 1.6f), 1.0f);
            const float rf2 = fminf(fmaxf(0.075f / fmaxf(b.baseR, 0.012f), 0.25f), 1.6f);
            stretch = fminf(a.riseStretch * sf * rf2, 0.80f);
        }
        dst[i].b[3] = stretch;
        // rise_parallax_dim: a far (small) blob is slightly dimmer, its colour
        // pulled toward the background as a depth cue. Same s as the motion
        // scale in StepAcidBlobs, so the two cues agree.
        if (a.riseParallaxDim > 1e-4f && a.riseParallax > 1e-4f) {
            float rr = b.baseR / fmaxf(a.discMax, 1e-4f);
            rr = rr < 0.25f ? 0.25f : (rr > 1.0f ? 1.0f : rr);
            const float s = 1.0f + (rr - 1.0f) * fminf(a.riseParallax, 1.0f);
            const float dim = 1.0f - (1.0f - s) * fminf(a.riseParallaxDim, 1.0f);
            dst[i].b[0] *= dim; dst[i].b[1] *= dim; dst[i].b[2] *= dim;
        }
        // mouse_oil_mode = 2 (comb): stretch amount + direction. Zero unless
        // the comb is running, and the shader then skips the whole branch.
        dst[i].c[0] = b.comb;
        dst[i].c[1] = b.cdx;
        dst[i].c[2] = b.cdy;
        // brief AG-b: the blob's dye identity. Read by the shader for HOLE
        // blobs only, and only while dye_lum_vary / dye_hue_vary are on.
        dst[i].c[3] = b.dyeId;
    }

    // DIAGNOSTIC ONLY (FW_ACID_DUMP, brief AG-b): keep what was uploaded so
    // DumpAcidCsv can write the shader's own blob records. Off = no cost.
    static const bool kAcidDumpOn = [] {
        wchar_t e[8] = {};
        return GetEnvironmentVariableW(L"FW_ACID_DUMP", e, 8) > 0 && e[0] != L'0';
    }();
    if (kAcidDumpOn) {
        const int nc = n < kAcidMaxBlobs ? n : kAcidMaxBlobs;
        m_acidBlobGpuCopy.assign((const float*)dst, (const float*)dst + (size_t)nc * 12);
    }

    AcidParamsGPU p = {};
    for (int i = 0; i < 4; i++) {
        p.oil[i][0] = effOil[i * 3 + 0];
        p.oil[i][1] = effOil[i * 3 + 1];
        p.oil[i][2] = effOil[i * 3 + 2];
        p.ink[i][0] = effInk[i * 3 + 0];
        p.ink[i][1] = effInk[i * 3 + 1];
        p.ink[i][2] = effInk[i * 3 + 2];
    }
    // ---- the packed scalars, BY NAME (brief BE) --------------------------
    // Every laP<n>.<c> is written here, once, through its name in
    // src\acid_slots.h -- the same table the shader's LA_<NAME> defines come
    // from. tools\slot-check.ps1 fails the build review if a name is written
    // twice, never written, or written and never read. Components nobody
    // writes stay 0 from the = {} above (the free slots the checker lists).
    float* const V[kAcidSlotVecs] = {
        p.p0,  p.p1,  p.p2,  p.p3,  p.p4,  p.p5,  p.p6,  p.p7,  p.p8,  p.p9,
        p.p10, p.p11, p.p12, p.p13, p.p14, p.p15, p.p16, p.p17, p.p18, p.p19,
        p.p20, p.p21, p.p22, p.p23, p.p24, p.p25, p.p26, p.p27, p.p28, p.p29,
        p.p30, p.p31, p.p32, p.p33, p.p34, p.p35, p.p36, p.p37, p.p38, p.p39,
        p.p40, p.p41, p.p42, p.p43, p.p44, p.p45 };
    auto slot = [&](AcidSlot s, float v) { V[s >> 2][s & 3] = v; };

    const float aspect = (float)m_width / fmaxf((float)m_height, 1.0f);
    slot(LA_BLOB_COUNT,   (float)(n < kAcidMaxBlobs ? n : kAcidMaxBlobs));
    slot(LA_THRESHOLD,    a.threshold);
    slot(LA_SUPPORT_SCALE, a.supportScale);
    slot(LA_AA_SCALE,     a.aaScale);
    slot(LA_RIM_WIDTH,    a.rimWidth);
    slot(LA_RIM_INSET,    a.rimInset);
    slot(LA_RIM_DARK,     a.rimDark);
    slot(LA_REFRACTION,   a.refraction);
    slot(LA_MENISCUS,     a.meniscus);
    slot(LA_MENISCUS_W,   a.meniscusW);
    slot(LA_TRANSLUCENCY, a.translucency);
    slot(LA_OIL_TEXTURE,  a.oilTexture);
    slot(LA_INK_LEVELS,   a.inkLevels);
    slot(LA_INK_SOFT,     a.inkSoft);
    slot(LA_INK_MIX,      a.inkMix);
    slot(LA_INK_HUE_VARY, a.inkHueVary);
    slot(LA_INK_GAIN,     a.inkGain);
    slot(LA_INK_BIAS,     a.inkBias);
    slot(LA_SEAM_STR,     a.seamStrength);
    slot(LA_SEAM_SCALE,   a.seamScale);
    // brief BD: the look's own grain is DEFERRED exactly as [post] film_grain
    // is when the image-space pass runs -- not moved there, dropped. It was a
    // SECOND stock on top of the [post] one, unpaced, and it lived in the
    // texture the post pass resamples, so the lateral aberration was taking a
    // first difference of it on R and B and printing it as colour. One stock.
    // (asked of PostActive() directly rather than of m_postGrainDeferred:
    // this upload runs during the sim step, long before the draw sets that
    // flag, so the flag would still read false here.)
    // ...and only when the post pass has a grain of its OWN to replace this
    // one. "One stock" means whichever exists, not none: the "(80-20, NO lens
    // effects)" twin runs the post pass for dither alone with film_grain 0,
    // and this grain is the only one it has.
    const bool postGrain = PostActive() && m_cfg.post.filmGrain > 0.0005f;
    slot(LA_SEAM_LO,      a.seamLo);
    slot(LA_SEAM_HI,      a.seamHi);
    slot(LA_GRAIN,        postGrain ? 0.0f : a.grainAmt);
    slot(LA_GRAIN_SCALE,  a.grainScale);
    slot(LA_SPECKLE,      a.speckle);
    slot(LA_SPECKLE_SCALE, a.speckScale);
    slot(LA_TIME,         m_time);
    slot(LA_ASPECT,       aspect);
    slot(LA_OIL_HDR,      a.oilHdr);
    slot(LA_RIM_HDR,      a.rimHdr);
    slot(LA_MENISCUS_OFF, a.meniscusOff);
    slot(LA_INK_SHADING,  a.inkShading);
    // With the droplet particle sim on, the procedural swarms are forced OFF:
    // ONE system is the whole point (the user, on the two layered together:
    // "it still looks like 2 things layered. The dots look png'd on").
    const bool dropsOn = (a.droplets > 0);
    slot(LA_SWARM_HOLES,  dropsOn ? 0.0f : a.swarmHoles);
    slot(LA_SWARM_DROPS,  dropsOn ? 0.0f : a.swarmDrops);
    slot(LA_SWARM_DENSITY, a.swarmDensity);
    slot(LA_SWARM_RIM_DARK, a.swarmRimDark);
    slot(LA_SWARM_SCALE_HOLES, a.swarmScaleA);
    slot(LA_SWARM_SCALE_DROPS, a.swarmScaleB);
    slot(LA_SWARM_R_MIN,  a.swarmRMin);
    slot(LA_SWARM_R_MAX,  a.swarmRMax);
    slot(LA_SWARM_CLUMP,  a.swarmClump);
    slot(LA_SWARM_DARK,   a.swarmDark);
    slot(LA_INK_WATER,    (a.inkMode == 1 ? 1.0f : 0.0f));
    slot(LA_TOE_TINT,     fmaxf(a.toeTint, 0.0f));
    // Target ink hue = the oil family's mean hue, swept, plus 180 degrees.
    // (laP11.w is free: the hue sweep is applied to the palette above.)
    float targetHue = fmodf(OilMeanHueDeg(effOil) + 180.0f, 360.0f);
    slot(LA_INK_LOCK,     a.inkComplementLock ? 1.0f : 0.0f);
    slot(LA_INK_LOCK_SPAN, a.inkComplementSpan);
    slot(LA_INK_TARGET_HUE, targetHue);
    slot(LA_RIM_VARY,     a.rimVary);
    slot(LA_RIM_INK_FOLLOW, a.rimInkFollow);
    slot(LA_RIM_ORDER,    a.rimOrder ? 1.0f : 0.0f);
    slot(LA_GRAIN_SHADOW_W, fmaxf(a.grainShadowW, 0.0f));
    slot(LA_OIL_THIN_EDGE, fmaxf(a.oilThinEdge, 0.0f));
    slot(LA_OIL_EDGE_FRAC, fmaxf(a.oilEdgeFrac, 0.0f));
    slot(LA_OIL_SPECULAR, fmaxf(a.oilSpecular, 0.0f));
    slot(LA_OIL_IRID,     fmaxf(a.oilIrid, 0.0f));
    slot(LA_SWARM_LENS,   fmaxf(a.swarmLens, 0.0f));
    slot(LA_MEN_FROM_INK, fmaxf(a.meniscusFromInk, 0.0f));
    slot(LA_OIL_GLOW,     fmaxf(a.oilGlow, 0.0f));
    slot(LA_REFR_WIDTH,   fmaxf(a.refractionWidth, 0.0f));
    float men[4] = { effMen[0], effMen[1], effMen[2], 0.0f };
    memcpy(p.men, men, 16);
    slot(LA_OIL_TRANSP,   fmaxf(a.oilTransparency, 0.0f));
    slot(LA_OIL_ABSORB,   fmaxf(a.oilAbsorb, 0.0f));
    slot(LA_OIL_FILM_BUMP, fmaxf(a.oilFilmBump, 0.0f));
    slot(LA_OIL_REFR_BODY, fmaxf(a.oilRefractBody, 0.0f));
    // DYE_DEPTH_W: how much the dye layer weighs against the droplets in the
    // per-pixel depth blend (item AA; 0.25 = the old constant prior).
    // MEN_FILM_MIX = meniscus_film_mix: how much of the meniscus halo's colour
    // (and its ink-brightness gate) comes from the FILM instead of the ink.
    // 0 = today.
    slot(LA_OIL_INK_BLUR, fmaxf(a.oilInkBlur, 0.0f));
    slot(LA_DYE_DEPTH_W,  fminf(fmaxf(a.dyeDepthW, 0.01f), 4.0f));
    slot(LA_MEN_FILM_MIX, fminf(fmaxf(a.meniscusFilmMix, 0.0f), 1.0f));
    slot(LA_RISE_BOTTOM_LIGHT, fmaxf(a.riseBottomLight, 0.0f));
    slot(LA_POST_CHROMA,  fmaxf(a.postChroma, 0.0f));
    slot(LA_POST_LIFT,    fmaxf(a.postLift, 0.0f));
    slot(LA_DROPS_ON,     dropsOn ? 1.0f : 0.0f);
    slot(LA_DROP_GRID_W,  (float)kDropGridW);
    slot(LA_DROP_GRID_H,  (float)kDropGridH);
    slot(LA_OIL_EDGE_MODE, (a.oilEdgeMode == 1) ? 1.0f : 0.0f);
    slot(LA_DROP_SUPPORT, fmaxf(a.dropletSupport, 0.5f));
    slot(LA_DROP_WEIGHT,  fmaxf(a.dropletWeight, 0.0f));
    slot(LA_DROP_OIL_W,   fmaxf(a.dropletOilW, 0.0f));
    // The index the RING SHAPE records start at, so the shader can find a
    // droplet's second float4 without a second SRV.
    slot(LA_DROP_RING_BASE, (float)kAcidMaxDrops);
    // Ring band half-width as a fraction of the droplet's SUPPORT radius, so
    // the shader can build the annulus without a per-droplet size; the visible
    // ring is about this fraction of the droplet across.
    slot(LA_DROP_RING_WIDTH, fminf(fmaxf(a.dropletRingWidth, 0.02f), 0.60f));
    slot(LA_DROP_RING_LIFT, fminf(fmaxf(a.dropletRingLift, 0.0f), 1.0f));
    slot(LA_OIL_EDGE_CURVE, fminf(fmaxf(a.oilEdgeCurve, 0.0f), 1.0f));
    // DIFFR_SCALE = the DIFFRACTION scale: the point spread times the constant
    // k that sets where the size curve bends, carried in uv-y like every other
    // optical width here so a preview and the panel diffract the same. A
    // droplet of this radius comes out at 63% of its full darkness; twice it,
    // 98%. k = 2.5 puts the knee on the sim's small droplets (r_min is about
    // 1.6 px at 1440p) and leaves anything above ~8 px fully black.
    slot(LA_DIFFR_SCALE,  2.5f * fmaxf(a.diffractionPx, 0.05f) / 1440.0f);
    // [post] halo / halo_px / softness. All three are optical widths authored
    // in px at 1440p and handed to the shader as a FRACTION of the frame, so
    // a 960x540 preview and the 1440p panel show the same lens.
    const PostConfig& po = m_cfg.post;
    slot(LA_HALO,         fminf(fmaxf(po.halo, 0.0f), 1.0f));
    slot(LA_HALO_W,       fmaxf(po.haloPx, 0.5f) / 1440.0f);
    slot(LA_SOFTNESS,     fmaxf(po.softness, 0.0f) / 1440.0f);
    slot(LA_BAND_MIN,     fminf(fmaxf(po.bandMin, 0.0f), 1.0f));
    // oil_penumbra: width authored in px at 1440p, carried as a fraction of
    // the frame so a preview and the panel show the same band.
    slot(LA_PENUMBRA,     fminf(fmaxf(a.oilPenumbra, 0.0f), 1.0f));
    slot(LA_PENUMBRA_W,   fmaxf(a.oilPenumbraPx, 0.5f) / 1440.0f);
    slot(LA_PENUMBRA_HUE, a.oilPenumbraHue);
    slot(LA_PENUMBRA_DARK, fminf(fmaxf(a.oilPenumbraDark, 0.0f), 1.0f));
    // cellulose: the master folded into the two side weights, so the shader
    // can skip the whole block on one test; the feature size is authored in
    // px at 1440p and carried as a fraction of the frame (same convention as
    // every other optical width here), and the drift is the rise speed times
    // cellulose_drift, in uv/s, so the fibres travel with the masses.
    const float cellM = fminf(fmaxf(a.cellulose, 0.0f), 1.0f);
    slot(LA_CELL_INK,     cellM * fmaxf(a.celluloseInk, 0.0f));
    slot(LA_CELL_OIL,     cellM * fmaxf(a.celluloseOil, 0.0f));
    slot(LA_CELL_SCALE,   fmaxf(a.celluloseScale, 2.0f) / 1440.0f);
    slot(LA_CELL_DRIFT,   fmaxf(a.riseSpeed, 0.0f) * fminf(fmaxf(a.celluloseDrift, 0.0f), 1.0f));
    // ---- PERSPECTIVE CAMERA + DEPTH OF FIELD + TILT ----------------------
    // The optical axis is given in uv and used in the shader's p-space (x
    // times the aspect), so it is handed over already scaled. camera_fov is
    // authored in degrees ACROSS the frame and carried as the tangent per
    // p-unit of distance from the axis: tan(theta) = fovK * r, which is what
    // the ring branch wants and costs it no trig. focus_band_px is a WIDTH in
    // the picture, so the shader turns it into a depth tolerance by
    // multiplying it by the local gradient of the focus plane -- the sharp
    // strip really is that many pixels wide however hard the lens is tilted.
    const float DEG = 0.01745329252f;
    const float dofPx = EffDofMaxPx();   // dof_max_px + depth_amount
    const bool  dofOn = (dofPx > 0.01f);
    const float fovK  = (po.cameraFov > 0.01f)
                      ? tanf(fminf(po.cameraFov, 170.0f) * 0.5f * DEG) / 0.5f : 0.0f;
    slot(LA_CAM_AXIS_X,   m_rig.axisX);
    slot(LA_CAM_AXIS_Y,   m_rig.axisY);
    slot(LA_FOCUS_DEPTH,  dofOn ? m_rig.focus : 0.5f);
    slot(LA_DOF_MAX_PX,   dofOn ? fmaxf(dofPx, 0.0f) : 0.0f);
    slot(LA_FIELD_CURVE,  po.cameraFieldCurve);
    slot(LA_TILT_AMT,     m_rig.tiltAmt);
    slot(LA_TILT_COS,     cosf(m_rig.tiltAngle));
    slot(LA_TILT_SIN,     sinf(m_rig.tiltAngle));
    // COC_SPAN_INV = 1 / the depth interval over which the CoC ramps from the
    // edge of the sharp band to the full dof_max_px. A TENTH of the depth
    // range, and the shader's ramp is shouldered on top of that: the user's
    // reference is a macro lens whose one sharp plane resolves fine texture
    // while everything off it is already a wash, so the falloff has to be
    // steep. Fixed rather than another key -- it is the aperture, and
    // camera_focus, focus_band_px and droplet_depth already aim the plane.
    slot(LA_FOCUS_BAND,   fmaxf(EffFocusBandPx(), 0.0f) / 1440.0f);
    slot(LA_COC_SPAN_INV, 1.0f / 0.12f);
    slot(LA_FOV_K,        fovK);
    slot(LA_DIFFRACTION,  fminf(fmaxf(a.diffraction, 0.0f), 1.0f));
    // ---- DROPLET LENS SHADING (item X) -----------------------------------
    // The band width is authored in px at 1440p like every other optical
    // width here and carried as a fraction of the frame. The LAMP comes off
    // the rig, so the specular swings with the same light the haze and the
    // bloom hang off instead of inventing a second one.
    slot(LA_LENS,         fminf(fmaxf(a.dropletLens, 0.0f), 1.0f));
    slot(LA_LENS_CENTRE,  fminf(fmaxf(a.dropletLensCentre, 0.0f), 1.0f));
    slot(LA_LENS_BAND,    fmaxf(a.dropletLensBand, 0.25f) / 1440.0f);
    slot(LA_DROP_SPEC,    fminf(fmaxf(a.dropletSpec, 0.0f), 1.0f));
    // DYE_DEPTH / DYE_TILT = the DYE LAYER's own depth and its slope (item
    // AA). The slope runs along the direction from the lens centre to the
    // lamp, which drifts, so the dye slab is never parallel to the focus
    // surface and the line where they cross travels with the rig instead of
    // sitting on one row of pixels.
    slot(LA_LAMP_X,       m_rig.lampX);
    slot(LA_LAMP_Y,       m_rig.lampY);
    slot(LA_DYE_DEPTH,    fminf(fmaxf(a.dyeDepth, 0.0f), 1.0f));
    slot(LA_DYE_TILT,     a.dyeDepthTilt);
    // mass_rim (brief AB): strength, and its width as a fraction of the frame
    // from px at 1440p -- the same convention as every other optical width
    // (3 px, hardcoded: there is no key). DYE_AMT / DYE_HUE / DYE_SAT = the
    // mass dye (brief AG/AM). Amount 0 = the shader skips the wax branch
    // entirely, which is what keeps dye_sat 0 / dye_lum 0 byte-identical.
    slot(LA_MASS_RIM,     fminf(fmaxf(a.massRim, 0.0f), 1.0f));
    slot(LA_MASS_RIM_W,   3.0f / 1440.0f);
    slot(LA_DYE_AMT,      dyeAmt);
    slot(LA_DYE_HUE,      dyeHueN);
    // ---- brief AE: the second (and third) dye hue ------------------------
    // ---- the contrast hue WOBBLES (brief AE-b) ---------------------------
    // The user: "the contrast colour wiggles around like 170-190 degrees
    // opposite the primary". So the angle is not a constant: it breathes a
    // few degrees either side of the authored one. Two sines whose periods
    // are in the golden ratio never come back into phase, so the pair never
    // repeats exactly; m_mixPhase is the rig's readjustment phase, already
    // eased, so the contrast angle re-aims at the same moment the lamp, the
    // lid, the focus and the hue2 patches do -- nothing moves alone.
    // film_hue2 itself is applied in the shader to the film colour AFTER the
    // global hue_rotate_period rotation has turned the palette, so the two
    // hues rotate together as a PAIR and the combo drifts round the wheel.
    float hue2Eff = a.filmHue2;
    if (fabsf(a.filmHue2Wobble) > 0.001f) {
        const float P = fmaxf(a.filmHue2WobbleP, 5.0f);
        const float TAU = 6.2831853f;
        const float th = AnimatorTime(1);   // ANIM_HUE2 freeze holds the swing
        const float w = 0.62f * sinf(TAU * th / P + m_mixPhase)
                      + 0.38f * sinf(TAU * th / (P * 0.61803399f)
                                     + 1.7f + m_mixPhase * 0.7f);
        hue2Eff += a.filmHue2Wobble * w;
    }
    slot(LA_HUE2_AMT,     fminf(fmaxf(a.filmHue2Amt, 0.0f), 1.0f));
    slot(LA_HUE2_DEG,     hue2Eff);
    slot(LA_HUE3_AMT,     fminf(fmaxf(a.filmHue3Amt, 0.0f), 1.0f));
    slot(LA_HUE3_DEG,     a.filmHue3);
    // brief BV: hue3 share (1 = today's thresholds), equal load (base film).
    // 1 / 0 = today; the shader branches them out.
    slot(LA_HUE3_SHARE,   fminf(fmaxf(a.filmHue3Share, 0.0f), 1.0f));
    slot(LA_EQUAL_LOAD,   fminf(fmaxf(a.filmEqualLoad, 0.0f), 1.0f));
    // brief BW: patch-core equal load (0 = today) and the seam band width
    // factor (exactly 1.0 = today: the shader compares against 1.0 and takes
    // the literal band, so the clamp must leave 1 at 1).
    slot(LA_EQUAL_LOAD_P, fminf(fmaxf(a.filmEqualLoadP, 0.0f), 1.0f));
    slot(LA_HUE2_SEAM,    fminf(fmaxf(a.filmHue2Seam, 0.1f), 1.0f));
    // REFLECT_R / REFLECT_AMT: brief AJ, the boundary-reflection reach
    // (screen heights) and how much of the seam's hue a rim takes. 0 reach =
    // today, and the shader skips the whole probe.
    slot(LA_CRUST_HUE_MIX, fminf(fmaxf(a.crustHueMix, 0.0f), 1.0f));
    slot(LA_REFLECT_R,    fmaxf(a.boundaryReflectR, 0.0f));
    slot(LA_REFLECT_AMT,  fminf(fmaxf(a.boundaryReflectAmt, 0.0f), 1.0f));
    slot(LA_DYE_SAT,      dyeSat);
    // brief BK: the packed droplet dye (-1 = split off) and dye_smoke (0 =
    // today). With both at those values the shader skips its BK branch.
    slot(LA_DYE_DROP_RGB, dyeDropRgb);
    // film_level: the flat film's own level (1 = today, the shader skips it).
    slot(LA_FILM_LEVEL,   fminf(fmaxf(a.filmLevel, 0.0f), 1.0f));
    slot(LA_DYE_SMOKE,    fminf(fmaxf(a.dyeSmoke, 0.0f), 1.0f));
    // ---- CAST SHADOWS (brief BC) ----------------------------------------
    // shadow_len is a fraction of the screen HEIGHT, which is exactly the
    // unit the shader's p-space y carries, so it goes across untouched.
    // light_z lives in [post] beside light_x / light_y -- it is a property
    // of the lamp, not of the oil -- and rides over here because the only
    // pass that can see an occluder is the acid display pass.
    slot(LA_SHADOW_AMT,   fminf(fmaxf(a.shadowAmt, 0.0f), 1.0f));
    slot(LA_SHADOW_LEN,   fmaxf(a.shadowLen, 0.0f));
    slot(LA_SHADOW_SOFT,  fminf(fmaxf(a.shadowSoft, 0.0f), 1.0f));
    slot(LA_LIGHT_Z,      fminf(fmaxf(po.lightZ, -1.0f), 1.0f));
    // ---- brief BL: fluorescent oil under the lamp, ambient dye ----------
    // oil_fluor 0 and dye_lamp_follow 1 are today: the shader skips both
    // branches, so every existing preset is byte-identical. The reach is in
    // screen heights (p-space y), the unit the lamp distance is measured in.
    slot(LA_OIL_FLUOR,       fminf(fmaxf(a.oilFluor, 0.0f), 1.0f));
    slot(LA_OIL_FLUOR_REACH, fmaxf(a.oilFluorReach, 0.05f));
    slot(LA_DYE_LAMP_FOLLOW, fminf(fmaxf(a.dyeLampFollow, 0.0f), 1.0f));
    // brief BP: dark_sat, chroma compensation keyed on each element's LEVEL
    // (film_level for the film, the dye level for masses / droplets). 0 =
    // today and the shader skips it.
    slot(LA_DARK_SAT,        fminf(fmaxf(a.darkSat, 0.0f), 1.0f));
    // brief AG-b: per-mass dye variation. All three 0 = today and the shader
    // skips both the identity accumulation and the variation block. The
    // rise speed (uv/s) drifts the identity fallback with the column, so a
    // gap-mass keeps its shade while it climbs instead of rising through a
    // fixed screen-space pattern.
    slot(LA_DYE_LUM_VARY,  fminf(fmaxf(a.dyeLumVary, 0.0f), 0.5f));
    slot(LA_DYE_HUE_VARY,  fminf(fmaxf(a.dyeHueVary, 0.0f), 180.0f));
    slot(LA_DYE_THICK_HUE, fminf(fmaxf(a.dyeThickHue, -180.0f), 180.0f));
    slot(LA_DYE_ID_RISE,   fmaxf(a.riseSpeed, 0.0f));
    // brief BR: dye_core, the mass core's brightness as a fraction of its
    // rim (the floor of lerp(core, 1, Tw)). 0.42 was the hard-wired literal;
    // a clamp leaves the default's float32 bits untouched, so every preset
    // that does not set the key renders byte-identical. 1 = even fill.
    slot(LA_DYE_CORE,      fminf(fmaxf(a.dyeCore, 0.0f), 1.0f));
    // ---- brief BU: LAMP GREY -------------------------------------------
    // The user: "graying out from the lamp ... as if the dye is getting less
    // light ... rule of thirds, one corner at a time". The shader greys a
    // soft disc around (GREY_CX, GREY_CY); this picks WHERE: the corner
    // diagonally opposite the lamp, by the sign of lamp - 0.5 on each axis
    // with +-0.04 hysteresis (a lamp drifting across the middle must not
    // flicker the region). A continuous far-from-lamp weight would light two
    // corners at once, hence the CPU pick. When the far corner changes, the
    // centre SLIDES to it along the frame edge (never through the middle)
    // over kGreySlideS, eased, so there is always exactly one region. The
    // state runs whatever lamp_grey is, so turning the key on shows the
    // right corner at once; GREY_K 0 skips the shader branch (identity).
    {
        const float A = aspect;
        const float P = 2.0f * A + 2.0f;                // perimeter, p-units
        const float cornerS[4] = { 0.0f, A, A + 1.0f, 2.0f * A + 1.0f };   // TL TR BR BL
        const float dtG = (m_greyLastT < 0.0f) ? 0.0f
                        : fminf(fmaxf(m_time - m_greyLastT, 0.0f), 1.0f);
        m_greyLastT = m_time;
        const float lx = m_rig.lampX - 0.5f, ly = m_rig.lampY - 0.5f;
        const float Hy = 0.04f;
        if (m_greySX == 0) {
            m_greySX = (lx >= 0.0f) ? 1 : -1;
            m_greySY = (ly >= 0.0f) ? 1 : -1;
        } else {
            if (lx >  Hy) m_greySX =  1; else if (lx < -Hy) m_greySX = -1;
            if (ly >  Hy) m_greySY =  1; else if (ly < -Hy) m_greySY = -1;
        }
        // lamp right (+1) -> the LEFT edge is far; lamp below (+1) -> the TOP.
        const bool right = (m_greySX < 0), bottom = (m_greySY < 0);
        const int  want  = bottom ? (right ? 2 : 3) : (right ? 1 : 0);
        if (m_greyCorner < 0) {
            m_greyCorner = want;
            m_greyS = cornerS[want];
            m_greySlideT = -1.0f;
        } else if (want != m_greyCorner) {
            // the shorter way round the frame from wherever the centre is now
            // (a readjust mid-slide simply re-aims it)
            float d = fmodf(cornerS[want] - m_greyS, P);
            if (d >  0.5f * P + 1e-4f) d -= P;
            if (d < -0.5f * P - 1e-4f) d += P;
            m_greyCorner = want;
            m_greySFrom  = m_greyS;
            m_greySDelta = d;
            m_greySlideT = 0.0f;
        }
        if (m_greySlideT >= 0.0f) {
            m_greySlideT += dtG;
            const float u = fminf(m_greySlideT / kGreySlideS, 1.0f);
            m_greyS = m_greySFrom + m_greySDelta * (u * u * (3.0f - 2.0f * u));
            if (u >= 1.0f) { m_greyS = cornerS[m_greyCorner]; m_greySlideT = -1.0f; }
        } else {
            m_greyS = cornerS[m_greyCorner];   // at rest: absorbs a resize
        }
        float s = fmodf(m_greyS, P);
        if (s < 0.0f) s += P;
        float gx, gy;
        if (s < A)                 { gx = s;                    gy = 0.0f; }
        else if (s < A + 1.0f)     { gx = A;                    gy = s - A; }
        else if (s < 2.0f * A + 1.0f) { gx = A - (s - (A + 1.0f)); gy = 1.0f; }
        else                       { gx = 0.0f;                 gy = 1.0f - (s - (2.0f * A + 1.0f)); }
        m_greyCx = gx; m_greyCy = gy;
        // Oil3D C1: the lamp footprint replaces lamp_grey as tank_depth rises
        slot(LA_GREY_K,    0.6f * fminf(fmaxf(a.lampGrey, 0.0f), 1.0f)
                           * (a.tankDepth > 0.0005f ? 1.0f - fminf(a.tankDepth / 0.1f, 1.0f) : 1.0f));
        slot(LA_GREY_SIZE, fminf(fmaxf(a.lampGreySize, 0.05f), 1.0f));
        slot(LA_GREY_COOL, fminf(fmaxf(a.lampGreyCool, 0.0f), 1.0f));
        slot(LA_GREY_CX,   gx);
        slot(LA_GREY_CY,   gy);
        // brief BX: the grey heart (centred mask on the same block). No 0.6
        // factor: amount 1 = fully grey inside the core. 0 skips the branch.
        // Fix round: with the [post] pass running, the DESATURATION moves to its
        // end (RunPostPass, rg1.y) and the display pass does only the film lift:
        // HEART_K is written NEGATIVE (-amount) to say so.
        const float hk = fminf(fmaxf(a.lampGreyHeart, 0.0f), 1.0f);
        slot(LA_HEART_K,    (PostActive() && (int)(hk * 63.0f + 0.5f) >= 1) ? -hk : hk);
        slot(LA_HEART_SIZE, fminf(fmaxf(a.lampGreyHeartSize, 0.8f), 2.2f));
        slot(LA_HEART_LIFT, fminf(fmaxf(a.lampGreyHeartLift, 0.0f), 0.8f));
    }
    // ---- brief BU: SPLIT TONE --------------------------------------------
    // The user: "make it the opposite of the main hue, and obviously rotate
    // with the main hue. Make it turn on/off, out of sync with the hue, the
    // same way gears are like 3 and 5 so they always switch teeth." The main
    // hue is effOil[0] AFTER the sweep and hue_rotate_period (above), so the
    // tint turns with the film; its complement is taken in OKLab (see below).
    // Folded here into one
    // pre-multiplied add (level lift x gate x amount) so the shader does one
    // test and one multiply-add. The gate (ShadowToneGate) is a 50% duty
    // on/off on its OWN clock; 2225 s against the 3600 s hue period is the
    // golden ratio, which never repeats a (hue, on/off) pairing. Gate 0 (the
    // off half) uploads 0 and the shader branch is skipped, exactly as at
    // shadow_tone 0. Dark tones of the MASSES only (the shader's mask); this
    // is the one BU piece that lifts true black, by at most the lift key.
    {
        float tr = 0.0f, tg = 0.0f, tb = 0.0f;
        const float amt  = fminf(fmaxf(a.shadowTone, 0.0f), 1.0f);
        const float lift = fminf(fmaxf(a.shadowToneLift, 0.0f), 0.25f);
        const float hAmt = fminf(fmaxf(a.highlightToneAmt, 0.0f), 1.0f);
        const float hDes = fminf(fmaxf(a.highlightToneDesat, 0.0f), 1.0f);
        // brief BU-b: turning the tone on from 0 STARTS the ON half of the
        // gate (no dark wait of up to period/2). The clock's origin m_toneT0
        // moves to now when the tone keys go from all-zero to any nonzero, or
        // when the acid look comes back after a gap (> 1 s without an upload,
        // e.g. the cycle director was on another look). At launch (never
        // seen) and while the keys stay put, m_toneT0 stays 0: the gate is
        // BU's exactly.
        {
            const int on = (amt > 1e-4f || hAmt > 1e-4f || hDes > 1e-4f) ? 1 : 0;
            int prev = m_tonePrevOn;
            if (prev >= 0 && m_toneSeenT >= 0.0f && m_time - m_toneSeenT > 1.0f) prev = 0;
            if (prev == 0 && on) m_toneT0 = m_time;
            m_tonePrevOn = on;
            m_toneSeenT  = m_time;
        }
        const float g = ShadowToneGate((double)m_time - (double)m_toneT0,
                                       a.shadowTonePeriod, a.shadowToneFade);
        m_toneGate = g;
        if (amt > 1e-4f && lift > 1e-6f) {
            if (g > 1e-5f) {
                // The tint is an HSV colour (value 1, shadow_tone_sat) whose
                // HSV hue is SOLVED so that what lands on the black -- the
                // encoded add at this frame's amplitude k -- sits at the film's
                // OKLab hue + 180 deg: 96 coarse steps, then a 1/4608 refine.
                // ~220 cbrt per frame on the CPU, nothing on the GPU.
                const float k   = lift * g * amt;
                const float sat = fminf(fmaxf(a.shadowToneSat, 0.0f), 1.0f);
                const float tgt = BuOkHueEnc(effOil[0], effOil[1], effOil[2]) + 3.14159265f;
                auto err = [&](float hh) {
                    const RGB c = HSVtoRGB(hh - floorf(hh), sat, 1.0f);
                    float d = BuOkHueEnc(c.r * k, c.g * k, c.b * k) - tgt;
                    d = fmodf(d, 6.2831853f);
                    if (d >  3.14159265f) d -= 6.2831853f;
                    if (d < -3.14159265f) d += 6.2831853f;
                    return fabsf(d);
                };
                float fh, fs, fv;
                RgbToHsv(&effOil[0], fh, fs, fv);
                float best = fh + 0.5f, be = 1e9f;
                if (a.shadowToneHue >= 0.0f) {
                    // shadow_tone_hue: a FIXED absolute hue (brief BV), no
                    // rotation with the film, no solve.
                    best = fmodf(a.shadowToneHue, 360.0f) / 360.0f;
                } else if (sat > 1e-3f) {
                    for (int i = 0; i < 96; i++) {
                        const float hh = i / 96.0f, e = err(hh);
                        if (e < be) { be = e; best = hh; }
                    }
                    const float c0 = best;
                    for (int i = -24; i <= 24; i++) {
                        const float hh = c0 + i / 4608.0f, e = err(hh);
                        if (e < be) { be = e; best = hh; }
                    }
                }
                const RGB c = HSVtoRGB(best - floorf(best), sat, 1.0f);
                tr = c.r * k; tg = c.g * k; tb = c.b * k;
            }
        }
        m_toneAdd[0] = tr; m_toneAdd[1] = tg; m_toneAdd[2] = tb;
        slot(LA_TONE_R, tr);
        slot(LA_TONE_G, tg);
        slot(LA_TONE_B, tb);

        // ---- brief BU-b: HIGHLIGHT tint (Lightroom split toning) ----------
        // The film (alpha) above the split takes a white-balance MULTIPLY
        // toward the tint, renormalised in the shader to the pixel's own
        // linear Y (as lamp grey does): colour moves, luminance does not.
        // Folded here into one RGB U = M x (1 + desat x gate):
        //   T = linear HSV(h, highlight_tone_sat, 1) scaled to dot(T,Y709)=1
        //   M = lerp(1, T, amount x gate)   (still dot(M,Y709) = 1)
        // The shader recovers desat = dot(U,Y709) - 1 and M = U / dot(U,Y709).
        // Hue: highlight_tone_hue >= 0 = absolute HSV degrees (Lightroom's
        // Hue slider); < 0 = the OKLab complement of the RESOLVED shadow hue:
        // with shadow_tone_hue fixed, OKLab(HSV(it, shadow sat, 1)) + 180;
        // with the shadow hue itself the film's complement, that is the
        // film's own OKLab hue. The HSV hue of the tint is SOLVED (96 steps +
        // 1/4608 refine, as above) so that the tint's OKLab hue hits it.
        float hr = 0.0f, hg = 0.0f, hb = 0.0f;
        if ((hAmt > 1e-4f || hDes > 1e-4f) && g > 1e-5f) {
            const float hs = fminf(fmaxf(a.highlightToneSat, 0.0f), 1.0f);
            float best = 0.0f;
            if (a.highlightToneHue >= 0.0f) {
                best = fmodf(a.highlightToneHue, 360.0f) / 360.0f;
            } else if (hs > 1e-3f) {
                const float sSat = fminf(fmaxf(a.shadowToneSat, 0.0f), 1.0f);
                float tgt;
                if (a.shadowToneHue >= 0.0f && sSat > 1e-3f) {
                    const float sh = fmodf(a.shadowToneHue, 360.0f) / 360.0f;
                    const RGB sc = HSVtoRGB(sh - floorf(sh), sSat, 1.0f);
                    tgt = BuOkHueEnc(sc.r, sc.g, sc.b) + 3.14159265f;
                } else {
                    tgt = BuOkHueEnc(effOil[0], effOil[1], effOil[2]);
                }
                auto herr = [&](float hh) {
                    const RGB c = HSVtoRGB(hh - floorf(hh), hs, 1.0f);
                    float d = BuOkHueEnc(c.r, c.g, c.b) - tgt;
                    d = fmodf(d, 6.2831853f);
                    if (d >  3.14159265f) d -= 6.2831853f;
                    if (d < -3.14159265f) d += 6.2831853f;
                    return fabsf(d);
                };
                float be = 1e9f;
                for (int i = 0; i < 96; i++) {
                    const float hh = i / 96.0f, e = herr(hh);
                    if (e < be) { be = e; best = hh; }
                }
                const float c0 = best;
                for (int i = -24; i <= 24; i++) {
                    const float hh = c0 + i / 4608.0f, e = herr(hh);
                    if (e < be) { be = e; best = hh; }
                }
            }
            const RGB c = HSVtoRGB(best - floorf(best), hs, 1.0f);
            auto lin = [](float v) {
                v = fmaxf(v, 0.0f);
                return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
            };
            float T[3] = { lin(c.r), lin(c.g), lin(c.b) };
            const float tY = 0.2126f * T[0] + 0.7152f * T[1] + 0.0722f * T[2];
            const float k  = hAmt * g;
            const float u  = 1.0f + hDes * g;
            float M[3];
            for (int i = 0; i < 3; i++) M[i] = (1.0f + (T[i] / fmaxf(tY, 1e-6f) - 1.0f) * k) * u;
            hr = M[0]; hg = M[1]; hb = M[2];
        }
        m_toneHi[0] = hr; m_toneHi[1] = hg; m_toneHi[2] = hb;
        slot(LA_HTONE_R, hr);
        slot(LA_HTONE_G, hg);
        slot(LA_HTONE_B, hb);
        slot(LA_TONE_BAL, fminf(fmaxf(a.toneBalance, -1.0f), 1.0f));
    }
    // ---- Oil3D Stage A: oil_height (read only by the OIL_HEIGHT PSO) -----
    // lambda = the wax's shoulder width, authored in px at 1440p, carried in
    // uv-y like every other width; the toe is a fixed 1 px; the debug mode
    // comes from --shot-debug (never from an ini).
    slot(LA_OIL_H_LAMBDA, fminf(fmaxf(a.oilHeightPx, 1.0f), 200.0f) / 1440.0f);
    slot(LA_OIL_H_TOE,    1.0f / 1440.0f);
    slot(LA_OIL_H_DEBUG,  (float)m_oilHeightDebug);
    slot(LA_OIL3D_ABSORB, fminf(fmaxf(a.oil3dAbsorb, 0.0f), 64.0f));
    // B re-ruling: CROSS-FADE, don't stack. The transmission IS the mass's lit
    // edge band; mass_rim is its 2D fake. The band the transmission adds falls
    // as ~1/k (measured), so mass_rim keeps (1 - c/k) of itself and the TOTAL
    // band stays at today's level. c from the stageB2 calibration renders.
    {
        const float k = fminf(fmaxf(a.oil3dAbsorb, 0.0f), 64.0f);
        // measured (stageB2): mass_rim carries only ~0.8 % of today's band, so
        // the cross-fade is complete by k = 2 and cannot offset the transmission
        const float kShareC = 2.0f;
        slot(LA_ABS_SHARE, k > 0.0005f ? fminf(kShareC / k, 1.0f) : 0.0f);
    }
    // ---- Oil3D C1: one lamp over a tank (all 0 at tank_depth 0) -----------
    // C2 note 2: the azimuth's DRIFT is integrated. azim + drift * m_time / 60
    // jumped the lamp (and with it the footprint, the shadows and the shafts)
    // by d(drift) * m_time whenever light_azim_drift changed: a slider, a
    // stage lerp, a clock. Here the phase is phase0 + drift * (m_time - t0) /
    // 60, rebased at every change of drift, so the lamp only ever GLIDES. The
    // first value is taken as is (phase0 0, t0 0): the same arithmetic as
    // before, to the bit, for every run whose drift never changes.
    if (!m_azDriftSet) {
        m_azDriftSet = true; m_azDriftLast = a.lightAzimDrift; m_azPhase0 = 0.0f; m_azT0 = 0.0f;
    } else if (a.lightAzimDrift != m_azDriftLast) {
        m_azPhase0 += m_azDriftLast * (m_time - m_azT0) / 60.0f;
        m_azPhase0  = fmodf(m_azPhase0, 360.0f);
        m_azT0 = m_time; m_azDriftLast = a.lightAzimDrift;
    }
    {
        const float D = fminf(fmaxf(a.tankDepth, 0.0f), 1.0f);       // frame heights
        float sx = 0.0f, sy = 0.0f, pen = 0.0f, under = 0.0f, fade = 0.0f, stretch = 1.0f;
        float csReach = 0.0f;
        float l2x = 0.0f, l2y = 0.0f, l2pen = 0.0f, l2reach = 0.0f;   // Oil3D L2 (0 = lamp 2 off)
        float wsShare = 0.0f, wsTau = 0.0f;
        float covAx = 1.0f, covAy = 1.0f, covBx = 0.0f, covBy = 0.0f;
        if (D > 0.0005f) {
            const float d2r  = 3.14159265f / 180.0f;
            const float elev = fminf(fmaxf(a.lightElev, 10.0f), 89.0f) * d2r;
            const float az   = (a.lightAzim + (m_azPhase0 + a.lightAzimDrift * (m_time - m_azT0) / 60.0f)) * d2r;
            const float len  = D / tanf(elev);                         // D cot(elev), p-units
            // toward the light: the oil that shadows this receiver sits there
            sx = len * cosf(az) / fmaxf(aspect, 1e-4f);                 // uv x
            sy = -len * sinf(az);                                       // uv y (screen up = -y)
            const float half = 0.5f * fminf(fmaxf(a.lightSize, 0.5f), 40.0f) * d2r;
            pen = D * tanf(half);
            // C1 fix: the under-layer shadow (read THROUGH the film, displaced
            // by the whole tank depth) is the SECONDARY term: capped so a full
            // occlusion moves the film plateau by at most 5 %. It was
            // transparency x (1 - e^-D/0.2) x 0.6 (0.25 at the live 0.5 / 0.35):
            // enough to print a dark copy of every mass ~290 px up-left.
            under = fminf(fminf(fmaxf(a.oilTransparency, 0.0f), 1.0f)
                          * (1.0f - expf(-D / 0.2f)) * 0.12f, 0.05f);
            fade = fminf(D / 0.1f, 1.0f);
            stretch = 1.0f / fmaxf(sinf(elev), 0.2f);
            // C1 fix: shadow_on_film is a CONTACT shadow. The oil floats ON the
            // film, so its shadow there travels only the wax's own height
            // (lambda = oil_height_px) x cot(elev), not the tank depth: a wax
            // slab lambda high ends its shadow at lambda cot(elev), +- the
            // lamp's penumbra lambda (cot(elev - size/2) - cot(elev + size/2));
            // the meniscus rounds that end into a bell over [0, reach],
            // reach = 2 lambda cot(elev) + penumbra (~21 px at 1440p for
            // lambda 16, elev 60, size 8). uv-y, like lambda.
            const float lam = fminf(fmaxf(a.oilHeightPx, 1.0f), 200.0f) / 1440.0f;
            const float eLo = fmaxf(elev - half, 2.0f * d2r);
            const float eHi = fminf(elev + half, 89.5f * d2r);
            csReach = lam * (2.0f / tanf(elev) + 1.0f / tanf(eLo) - 1.0f / tanf(eHi));
            // ---- Oil3D L2: lamp 2's geometry (read by the OIL_LAMP2 PSOs only) --
            // Its azimuth is lamp 1's INCLUDING the integrated drift (az above)
            // plus lamp2_azim, so the pair keeps its angle as lamp 1 swings. The
            // same laws as lamp 1's, with lamp 2's own elevation and size: the
            // under-layer offset D cot(e2) toward lamp 2 with the PCSS radius
            // D tan(s2/2), and the contact shadow's reach lambda (2 cot(e2) +
            // cot(e2 - s2/2) - cot(e2 + s2/2)). All 0 while lamp 2 is off.
            if (a.Lamp2Wanted()) {
                const float elev2 = fminf(fmaxf(a.lamp2Elev, 10.0f), 89.0f) * d2r;
                const float az2   = az + a.lamp2Azim * d2r;
                const float len2  = D / tanf(elev2);
                l2x = len2 * cosf(az2) / fmaxf(aspect, 1e-4f);
                l2y = -len2 * sinf(az2);
                const float half2 = 0.5f * fminf(fmaxf(a.lamp2Size, 0.5f), 40.0f) * d2r;
                l2pen = D * tanf(half2);
                const float eLo2 = fmaxf(elev2 - half2, 2.0f * d2r);
                const float eHi2 = fminf(elev2 + half2, 89.5f * d2r);
                l2reach = lam * (2.0f / tanf(elev2) + 1.0f / tanf(eLo2) - 1.0f / tanf(eHi2));
            }
            // ---- Oil3D C2: the lit column (water_scatter > 0) ------------
            // In-scatter over z in [0, D]: beta * T(z) * lit, T(z) =
            // exp(-sigma z (1 + 1/sin(elev))): the light walks z / sin(elev)
            // down to depth z, the view walks z back up. tau = the optical
            // depth at the floor; the unoccluded column (lit = 1) integrates
            // to L = beta D (1 - e^-tau) / tau against the direct light
            // (beta = water_scatter per frame height). Port chat re-ruling:
            // TODAY is the fully lit state, so the column adds nothing; a
            // fully shadowed column removes its share L / (1 + L) of the
            // film's light, seen through the film (x oil_transparency). The
            // shader scales that share by the occluded fraction of its depth
            // taps (importance-sampled in T, so every tap weighs the same).
            const float beta = fminf(fmaxf(a.waterScatter, 0.0f), 64.0f);
            if (beta > 0.0005f) {
                const float sig = fminf(fmaxf(a.waterSigma, 0.0f), 64.0f);
                wsTau   = sig * D * (1.0f + 1.0f / fmaxf(sinf(elev), 0.05f));
                float L = beta * D * (wsTau > 1e-4f ? (1.0f - expf(-wsTau)) / wsTau : 1.0f);
                // Oil3D W1: the murk's particles scatter too (OIL_MURK frames only)
                if (a.MurkWanted()) {
                    float wm = fminf(fmaxf(a.waterMurk, 0.0f), 1.0f);
                    if (m_dirWaterOn) wm *= m_dirMurkK;   // post-night T3: director_water's murk level
                    L *= 1.0f + fminf(fmaxf(a.murkScatter, 0.0f), 8.0f) * wm;
                }
                wsShare = fminf(fmaxf(a.oilTransparency, 0.0f), 1.0f) * L / (1.0f + L);
                // The PADDED mass coverage: quarter res (4 px per texel), the
                // frame centred in a symmetric pad P >= D cot(elev) + the
                // penumbra (+2 texels for the bilinear tap and the AA band),
                // rounded up to 16 texels so the azimuth's drift never resizes
                // it (only the scissor, the light-side region, follows it).
                // Grows at once, shrinks only with 32 texels of hysteresis.
                const float Hpx = (float)m_height, Wpx = (float)m_width;
                const int need = (int)ceilf((len + pen) * Hpx / 4.0f) + 2;
                int P = m_massCovWantP;
                if (P < need || P > need + 32 || m_massCovFrameW != m_width || m_massCovFrameH != m_height)
                    P = ((need + 15) / 16) * 16;
                m_massCovWantP = P;
                const int bw = (m_width + 3) / 4, bh = (m_height + 3) / 4;
                const float cw = (float)(bw + 2 * P), ch = (float)(bh + 2 * P);
                covAx = (Wpx / 4.0f) / cw;  covBx = (float)P / cw;
                covAy = (Hpx / 4.0f) / ch;  covBy = (float)P / ch;
                // scissor: the frame + the light-side reach + the penumbra on
                // every side (+2 texels): the only texels any tap can reach.
                const float sxp = sx * Wpx, syp = sy * Hpx, penp = pen * Hpx;
                m_massCovSc[0] = P - ((int)ceilf((fmaxf(-sxp, 0.0f) + penp) / 4.0f) + 2);
                m_massCovSc[1] = P - ((int)ceilf((fmaxf(-syp, 0.0f) + penp) / 4.0f) + 2);
                m_massCovSc[2] = P + bw + ((int)ceilf((fmaxf(sxp, 0.0f) + penp) / 4.0f) + 2);
                m_massCovSc[3] = P + bh + ((int)ceilf((fmaxf(syp, 0.0f) + penp) / 4.0f) + 2);
            }
        }
        slot(LA_WS_SHARE,   wsShare);
        slot(LA_WS_TAU,     wsTau);
        slot(LA_COV_AX,     covAx);
        slot(LA_COV_AY,     covAy);
        slot(LA_COV_BX,     covBx);
        slot(LA_COV_BY,     covBy);
        // Oil3D W1: the murk tile mapping (read by the OIL_MURK variants only;
        // zero = unread otherwise). One tile = murk_scale frame heights across;
        // the depth maps to D / (6 S) of a tile, so a clump is six times as tall
        // as it is wide and a column mostly crosses ONE (at D / 2S the 16 depth
        // taps averaged three clumps away: MAD 0.7 codes at water_murk 0.7, the
        // first proof shots). The field rises with the
        // oil (moves UP the screen), wanders sideways and slides through the
        // column (it evolves): never still, the OLED rule.
        if (a.MurkWanted() && wsShare > 0.0f) {
            const float S  = fminf(fmaxf(a.murkScale, 0.05f), 8.0f);
            const float dr = fminf(fmaxf(a.murkDrift, 0.0f), 10.0f) * m_time;
            p.murk[0][0] = aspect / S;
            p.murk[0][1] = 1.0f / S;
            p.murk[0][2] = fmodf(0.0021f * dr, 1.0f);
            p.murk[0][3] = fmodf(0.0090f * dr / S, 1.0f);
            p.murk[1][0] = D / (6.0f * S);
            p.murk[1][1] = fmodf(0.0040f * dr, 1.0f);
            // the useful range (murk sheet 2026-10-30): at 0 the dense silt never shades itself and the
            // frame rises +1.92 % (over the 1.5 % fence, hot yellow spots); 0.15 sits near +1.2 %; past 2
            // only darker
            p.murk[1][2] = fminf(fmaxf(a.murkShadow, 0.15f), 2.0f) * wsTau;
            // murk_light: the silt shades the film's own light (1 + murk_light (d_col - 1)); 0 = today
            p.murk[1][3] = fminf(fmaxf(a.murkLight, 0.0f), 1.0f);
            // the same mapping for the CPU (MurkDensityCpu: where the motes spawn)
            m_murkMap[0] = p.murk[0][0]; m_murkMap[1] = p.murk[0][1];
            m_murkMap[2] = p.murk[0][2]; m_murkMap[3] = p.murk[0][3];
            m_murkMap[4] = p.murk[1][0]; m_murkMap[5] = p.murk[1][1];
        }
        // Oil3D W3: the fine silt + depth haze (read by the OIL_GRAIN variants
        // only). [0] = murk_grain, the lattice spacing in px at this frame's
        // height, the haze's optical depth over the column (4 x murk_haze: a
        // droplet at the deepest live depth, z 0.8, keeps e^-1.6 = 20 % at haze
        // 0.5), murk_haze_lift; [1] = slices 0 and 1's lattice offsets, [2].xy =
        // slice 2's, .z = the slice count, .w = the churn clock (s, wrapped at
        // 4096: the wobble's one-frame twitch an hour is below a pixel). Each
        // slice drifts with the murk (its rise, 0.009 frame heights a second x
        // murk_drift, and its sideways 0.0021 x murk_scale) at its own rate, the
        // near slice 1.3x, the middle 1x, the deep one 0.7x (the parallax), plus
        // a slow sideways sway of its own; integrated in closed form from the
        // clock (no state: frame-rate free, shots reproduce), in lattice CELLS
        // wrapped at 1024 (the shader hashes the cells mod 1024: seamless).
        if (a.GrainWanted() && wsShare > 0.0f) {
            const double H   = (double)m_height;
            // the spacing scales with the frame (px at 1440p), never under 1.25 px: a
            // Gaussian of sigma 0.42 cell narrower than ~0.5 px shimmers as it moves
            const double cpx = fmax(fmin(fmax((double)a.murkGrainPx, 0.5), 8.0) * H / 1440.0, 1.25);
            const double S   = fmin(fmax((double)a.murkScale, 0.05), 8.0);
            const double dr  = fmin(fmax((double)a.murkDrift, 0.0), 10.0);
            const double T   = m_timeD;
            static const double kRate[3] = { 1.3, 1.0, 0.7 }, kGrow[3] = { 1.0, 1.45, 2.0 };
            static const double kSwayS[3] = { 47.0, 71.0, 103.0 }, kSwayPh[3] = { 0.0, 2.1, 4.4 };
            float off[6];
            for (int s = 0; s < 3; s++) {
                const double cs = cpx * kGrow[s];
                const double w  = 6.283185307 / kSwayS[s];
                const double sway = 0.004 * H * dr;   // px/s of sideways sway
                const double ox = (0.0021 * dr * S * H * kRate[s] * T + sway / w * sin(w * T + kSwayPh[s])) / cs;
                const double oy = (0.0090 * dr * H * kRate[s] * T) / cs;
                off[2 * s]     = (float)(ox - 1024.0 * floor(ox / 1024.0));
                off[2 * s + 1] = (float)(oy - 1024.0 * floor(oy / 1024.0));
            }
            p.grain[0][0] = fminf(fmaxf(a.murkGrain, 0.0f), 1.0f);
            p.grain[0][1] = (float)cpx;
            p.grain[0][2] = 4.0f * fminf(fmaxf(a.murkHaze, 0.0f), 1.0f);
            p.grain[0][3] = fminf(fmaxf(a.murkHazeLift, 0.0f), 2.0f);
            p.grain[1][0] = off[0]; p.grain[1][1] = off[1]; p.grain[1][2] = off[2]; p.grain[1][3] = off[3];
            p.grain[2][0] = off[4]; p.grain[2][1] = off[5];
            p.grain[2][2] = (float)(a.murkGrainSlices < 1 ? 1 : (a.murkGrainSlices > 3 ? 3 : a.murkGrainSlices));
            p.grain[2][3] = (float)(T - 4096.0 * floor(T / 4096.0));
        }
        // Oil3D C3: the motes (read by the OIL_MOTES variants only). Records at
        // AcidDrops[2 kAcidMaxDrops + 2k], cells at DropCells[64 x 36 + c] (the
        // tail, written below); the frame in px; the blur = the oil's own
        // dof_max_px (scaled from 1440p like mote_px), reached at half the depth.
        // (director_water keeps them on at any count; [2].y = its murk level k, read by OIL_MURKK alone)
        if (MotesWanted() && wsShare > 0.0f) {
            if (m_dirWaterOn) p.mote[2][1] = m_dirMurkK;
            p.mote[0][0] = (float)kMoteGridW;
            p.mote[0][1] = (float)kMoteGridH;
            p.mote[0][2] = (float)(kAcidMaxDrops * 2);
            p.mote[0][3] = (float)(kDropGridW * kDropGridH);
            p.mote[1][0] = (float)m_width;
            p.mote[1][1] = (float)m_height;
            p.mote[1][2] = (dofOn ? fmaxf(dofPx, 0.0f) : 0.0f) * (float)m_height / 1440.0f;
            p.mote[1][3] = 2.0f;
            p.mote[2][0] = fminf(fmaxf(a.moteBright, 0.0f), 32.0f);
        }
        // Oil3D W2: the water colour (read by the OIL_WTINT variants only). Each
        // hue is relative to the anchor (the film's own hue, as the lamps read it),
        // a full-saturation colour in linear light, blended from white by its
        // amount: [0] = the water's gain, [1] = water x silt (the silt's light
        // crosses the water too). .w of [0] = the cap on the shader's per-pixel
        // normaliser (the gain that keeps the film's luminance): past it the
        // share goes darker instead (an opposite colour has little to pass on).
        if (a.WaterTintWanted() && wsShare > 0.0f) {
            float fh, fs, fv;
            RgbToHsv(&effOil[0], fh, fs, fv);
            auto tintRgb = [&](float relDeg, float amt, float out[3]) {
                float h = fmodf(fh * 360.0f + relDeg, 360.0f);
                if (h < 0.0f) h += 360.0f;
                const RGB c = HSVtoRGB(h / 360.0f, 1.0f, 1.0f);
                const float k = fminf(fmaxf(amt, 0.0f), 1.0f), cc[3] = { c.r, c.g, c.b };
                for (int ch = 0; ch < 3; ch++) out[ch] = 1.0f + (powf(cc[ch], 2.2f) - 1.0f) * k;
            };
            float w[3], s[3];
            tintRgb(a.waterHue, a.waterTint, w);
            tintRgb(a.siltHue, a.siltTint, s);
            for (int ch = 0; ch < 3; ch++) { p.water[0][ch] = w[ch]; p.water[1][ch] = w[ch] * s[ch]; }
            p.water[0][3] = 2.0f;
        }
        // DROP_LIFE (read by the DROP_LIFE variants only): droplet_shadow's lamp is lamp 1 -- the same
        // azimuth (+ the integrated drift), elevation and size as the C1 shadows above, but not gated on
        // tank_depth: a droplet's shadow lands on the film it floats over, not on the tank's floor.
        if (a.DropLifeWanted()) {
            const float d2r = 3.14159265f / 180.0f;
            const float el  = fminf(fmaxf(a.lightElev, 10.0f), 89.0f) * d2r;
            const float az  = (a.lightAzim + (m_azPhase0 + a.lightAzimDrift * (m_time - m_azT0) / 60.0f)) * d2r;
            const float hs  = 0.5f * fminf(fmaxf(a.lightSize, 0.5f), 40.0f) * d2r;
            const float ds  = fminf(fmaxf(a.dropletShadow, 0.0f), 1.0f);
            p.drop[0][0] = ds;
            p.drop[0][1] = 1.0f / tanf(el);
            p.drop[0][2] = tanf(hs);
            p.drop[0][3] = fminf(fmaxf(a.dropletWobble, 0.0f), 1.0f);
            // toward the light is (cos az, -sin az) in p-space (sx / sy above before the uv-x divide)
            p.drop[1][0] = -cosf(az);
            p.drop[1][1] = sinf(az);
            p.drop[1][2] = ds > 0.0005f ? 1.0f : 0.0f;   // the glint turns to this lamp
            p.drop[1][3] = ds > 0.0005f ? 0.0f : 1.0f;   // the old rig-lamp droplet shadow stays only without it
            // droplet_core: the lens opens between a 3-px and a 14-px radius (1440p, carried in uv-y)
            p.drop[2][0] = fminf(fmaxf(a.dropletCore, 0.0f), 1.0f);
            p.drop[2][1] = 3.0f / 1440.0f;
            p.drop[2][2] = 14.0f / 1440.0f;
            // post-night T4: droplet_haze_rim (read by the DROP_HAZERIM variants only)
            if (a.dropletHazeRim > 0.0005f) p.drop[2][3] = fminf(a.dropletHazeRim, 1.0f);
        }
        slot(LA_TANK_FADE,  fade);
        slot(LA_SH_OFF_X,   sx);
        slot(LA_SH_OFF_Y,   sy);
        slot(LA_PEN_R,      pen);
        slot(LA_SH_UNDER,   under);
        slot(LA_SH_FILM,    D > 0.0005f ? fminf(fmaxf(a.shadowOnFilm, 0.0f), 1.0f) : 0.0f);
        slot(LA_FP_STRETCH, stretch);
        slot(LA_CS_REACH,   csReach);
        slot(LA_L2_OFF_X,   l2x);
        slot(LA_L2_OFF_Y,   l2y);
        slot(LA_L2_PEN_R,   l2pen);
        slot(LA_L2_CS_REACH, l2reach);
        // Oil3D L3 (1): a WHITE lamp 2's share, the filled reference's weight
        // (BuildLampLut's level clamp, then its luminance cap at 1)
        slot(LA_L2_WLEVEL,  a.Lamp2Wanted() ? fminf(fminf(fmaxf(a.lamp2Level, 0.0f), 4.0f), 1.0f) : 0.0f);
    }
    {
        // Only the VISIBLE rows are uploaded: the hidden seed rows under the
        // bottom edge exist on the CPU alone, so the cbuffer layout and the
        // shader are untouched by the off-screen generation.
        const int NV = kMixW * kMixH;
        for (int i = 0; i < 60 * 4; i++)
            ((float*)p.mix)[i] = (i < NV && (int)m_mixField.size() >= (size_t)NV)
                               ? m_mixField[(size_t)i] : 0.0f;
    }
    // ---- Oil3D L1: lamp 1's spectral window (read by the OIL_LAMP PSOs only)
    // White window + no film dyes = today: the LUT stays zero and unread.
    // Oil3D L2: lamp 2 on = the OIL_LAMP2 PSO, which reads lamp 1's LUT too (a
    // white lamp 1 builds the identity: hue 0, chroma 0, lum 1) and lamp 2's,
    // the same construction from the lamp2_* keys. lamp2_level 0: neither runs
    // because of lamp 2, and laLamp2 stays zero and unread.
    if (a.LampPathWanted()) {
        float fh, fs, fv;
        RgbToHsv(&effOil[0], fh, fs, fv);    // the anchor: the film's own hue
        BuildLampLut(1, fh * 360.0f, V[LA_HUE2_DEG >> 2][LA_HUE2_DEG & 3],
                     V[LA_HUE2_AMT >> 2][LA_HUE2_AMT & 3], p.lamp, p.glow);
        if (a.Lamp2Wanted())
            BuildLampLut(2, fh * 360.0f, V[LA_HUE2_DEG >> 2][LA_HUE2_DEG & 3],
                         V[LA_HUE2_AMT >> 2][LA_HUE2_AMT & 3], p.lamp2, nullptr);
        // Oil3D L3 (3): the masses under the lamps (zero = unread unless
        // OIL_MASSDYE): the dye_hue dye's one-sample change per lamp, the
        // dark-dye oil's colour per unit film Y
        if (a.MassDyeWanted())
            BuildMassConsts(fh * 360.0f, V[LA_DYE_HUE >> 2][LA_DYE_HUE & 3] * 360.0f,
                            a.Lamp2Wanted() ? p.lamp2[0][2] : 0.0f, p.mass);
        // Oil3D L3b: the white track's film (OIL_DYEWHITE): the same dyes, white light
        if (a.FilmDyesWanted())
            BuildLampLut(0, fh * 360.0f, V[LA_HUE2_DEG >> 2][LA_HUE2_DEG & 3],
                         V[LA_HUE2_AMT >> 2][LA_HUE2_AMT & 3], p.lampW, nullptr);
    }
    memcpy(m_acidParamData[fi], &p, sizeof(p));
    m_acidLastFi = (int)fi;    // --shot-dump re-draws this exact frame

    // ---- droplet particle buffers ---------------------------------------
    // Sorted by grid cell, with a (first, count) table per cell, so the pixel
    // shader walks only the 3x3 cells around it. Both buffers stay all-zero
    // when the sim is off, which is a count of 0 in every cell.
    if (m_dropletData[fi] && m_dropletCellData[fi]) {
        AcidDropGPU* dd = (AcidDropGPU*)m_dropletData[fi];
        DropCellGPU* dc = (DropCellGPU*)m_dropletCellData[fi];
        const int NC = kDropGridW * kDropGridH;
        if (!dropsOn || m_dropletOrder.empty()) {
            memset(dc, 0, sizeof(DropCellGPU) * NC);
        } else {
            const int total = (int)m_dropletOrder.size();
            for (int k = 0; k < total && k < kAcidMaxDrops; k++) {
                const AcidDrop& d = m_acidDrops[m_dropletOrder[k]];
                dd[k].a[0] = d.x;
                dd[k].a[1] = d.y;
                // SIGN carries the kind: negative = a hole in the oil.
                dd[k].a[2] = (d.kind == 0) ? -d.r : d.r;
                // .w packs the contribution gate (0..1, see StepAcidDroplets),
                // the one per-droplet FLAG the shader needs, and now the
                // DEPTH, quantised to 8 bits and shifted above both:
                //   w = 4*dq + 2*ring + gate,   dq = 0 or 1 + round(depth*254)
                // The particle stays ONE float4, which is the whole point: the
                // droplet loop is the hottest code in the frame and a second
                // fetch per droplet per pixel would cost more than the whole
                // depth of field does. A float32 holds this exactly enough --
                // at w ~ 1023 the spacing is 6e-5, four orders below anything
                // the gate can express. dq = 0 is the SENTINEL for "no depth",
                // so with droplet_depth 0 this line writes bit-for-bit the
                // value it wrote before the feature existed.
                const float dsp = fminf(fmaxf(EffDropletDepth(), 0.0f), 1.0f);
                float dq = 0.0f;
                if (dsp > 1e-5f) {
                    const float dep = fminf(fmaxf(0.5f + (d.depth - 0.5f) * dsp, 0.0f), 1.0f);
                    dq = 1.0f + floorf(dep * 254.0f + 0.5f);
                }
                // droplet_big_frac: a MEDIUM droplet adds 256 to dq (the shader
                // reads dq > 255 as the flag and takes it off again). 4 x 511
                // + 3 < 2048 keeps the gate to ~1e-4. Key off: never written.
                const bool bigD = (d.big != 0 && !d.ring && a.dropletBigFrac > 1e-6f
                                   && a.dropletBigRMul > 1.251f);
                if (bigD) dq += 256.0f;
                dd[k].a[3] = 4.0f * dq + d.gate + (d.ring ? 2.0f : 0.0f);
                // ---- droplet_wobble: a SOLID droplet's life (DROP_LIFE only) --
                // The ring's Fourier record below, reused for the solids while
                // the DROP_LIFE variant reads it (only with droplet_wobble on):
                // a breathing radius (+-3..5 %, a 2..6 s period, its own phase)
                // carried on the radius itself, plus a mode-2 out-of-round that
                // swings like a real drop's shape oscillation while its axis
                // turns, and a faint 3-lobe ripple. Big droplets wobble, specks
                // are stiff (surface tension): 0.35 of it at nothing, all of it
                // from half droplet_r_max. Evaluated here, once per droplet per
                // frame, off its birth seed, so it never jumps.
                if (!d.ring && a.dropletWobble > 0.0005f && a.DropLifeWanted()) {
                    const float TAU = 6.28318531f;
                    const float g1 = DropHash(d.seed, 11.0f, 0x7FEB352Du);
                    const float g2 = DropHash(d.seed, 12.0f, 0x846CA68Bu);
                    const float g3 = DropHash(d.seed, 13.0f, 0x2C1B3C6Du);
                    const float g4 = DropHash(d.seed, 14.0f, 0x297A2D39u);
                    const float g5 = DropHash(d.seed, 15.0f, 0x6B43A9B5u);
                    const float g6 = DropHash(d.seed, 16.0f, 0x165667B1u);
                    const float rmx = fmaxf(a.dropletRMax, 1e-4f);
                    float kw  = fminf(a.dropletWobble, 1.0f)
                              * (0.35f + 0.65f * fminf(d.r / (0.5f * rmx), 1.0f));
                    // a medium droplet stays a clean circle (+-1 % at most)
                    if (bigD) kw *= 0.2f;
                    const float bP  = 2.0f + 4.0f * g2;                       // breath period, s
                    dd[k].a[2] *= 1.0f + kw * (0.03f + 0.02f * g1) * sinf(TAU * (m_time / bP + g3));
                    const float sw  = sinf(TAU * (m_time / (bP * (1.3f + 0.9f * g6)) + g1));
                    const float A2  = kw * ((0.012f + 0.028f * g4) + (0.02f + 0.02f * g5) * sw);
                    const float t2  = TAU * g2 + m_time * TAU / (40.0f + 80.0f * g3) * (g4 < 0.5f ? -1.0f : 1.0f);
                    const float A3  = kw * (0.006f + 0.008f * g5)
                                    * (0.6f + 0.4f * sinf(TAU * (m_time / (0.7f * bP) + g6)));
                    const float t3  = TAU * g6 - m_time * 0.05f * (g4 < 0.5f ? -1.0f : 1.0f);
                    AcidDropGPU& sw4 = dd[kAcidMaxDrops + k];
                    sw4.a[0] = A2 * cosf(2.0f * t2); sw4.a[1] = A2 * sinf(2.0f * t2);
                    sw4.a[2] = A3 * cosf(3.0f * t3); sw4.a[3] = A3 * sinf(3.0f * t3);
                }
                if (!d.ring) continue;
                // ---- the ring's SHAPE, in the second half of the buffer ---
                // "These are pixel perfect circles." A ring's radius is a
                // Fourier series in the polar angle rather than a constant:
                //   R(phi) = R * (1 + A2*cos(2(phi-t2)) + A3*cos(3(phi-t3)))
                // -- a slowly turning ELLIPSE (the 2-lobe term; A2 = 0.01 is
                // an axis ratio of 1.02, A2 = 0.111 is 1.25) plus a breathing
                // 3-lobe WOBBLE of a few percent. The shader needs no trig
                // and no per-pixel angle for this: cos/sin of 2phi and 3phi
                // come out of the radial unit vector by the double- and
                // triple-angle identities, so all it wants is the four
                // coefficients A*cos(n*t), A*sin(n*t), which is one float4.
                // Everything that MOVES (the axis turning, the wobble
                // breathing) is therefore evaluated here, on the CPU, once
                // per ring per frame, where it can be read.
                const float TAU = 6.28318531f;
                const float wob = fminf(fmaxf(a.dropletRingWobble, 0.0f), 1.0f);
                const float h1 = DropHash(d.seed, 1.0f, 0xA511E9B3u);
                const float h2 = DropHash(d.seed, 2.0f, 0x2545F491u);
                const float h3 = DropHash(d.seed, 3.0f, 0x9E3779B1u);
                const float h4 = DropHash(d.seed, 4.0f, 0xC2B2AE35u);
                const float h5 = DropHash(d.seed, 5.0f, 0x27D4EB2Fu);
                const float h6 = DropHash(d.seed, 6.0f, 0x165667B1u);
                const float ecc = 1.02f + 0.23f * h1;          // 1.02 .. 1.25
                const float A2  = wob * (ecc - 1.0f) / (ecc + 1.0f);
                // 38 s to 115 s for a full turn of the long axis, either way
                const float t2  = TAU * h2 + m_time * (0.055f + 0.11f * h3)
                                           * (h3 < 0.5f ? -1.0f : 1.0f);
                // 1.5..4% of the radius, breathing over 12..31 s
                const float br  = 0.62f + 0.38f * sinf(m_time * (0.20f + 0.30f * h6)
                                                       + TAU * h5);
                const float A3  = wob * (0.015f + 0.025f * h4) * br;
                const float t3  = TAU * h5 + m_time * 0.035f
                                           * (h6 < 0.5f ? -1.0f : 1.0f);
                AcidDropGPU& sh = dd[kAcidMaxDrops + k];
                sh.a[0] = A2 * cosf(2.0f * t2); sh.a[1] = A2 * sinf(2.0f * t2);
                sh.a[2] = A3 * cosf(3.0f * t3); sh.a[3] = A3 * sinf(3.0f * t3);
            }
            for (int c = 0; c < NC; c++) {
                int first = m_dropletCellStart[c], cnt = m_dropletCellCount[c];
                if (first >= kAcidMaxDrops) { first = 0; cnt = 0; }
                else if (first + cnt > kAcidMaxDrops) cnt = kAcidMaxDrops - first;
                dc[c].first = (uint32_t)first;
                dc[c].count = (uint32_t)(cnt < 0 ? 0 : cnt);
            }
        }
    }

    // ---- Oil3D C3: the motes, in the droplet buffers' TAIL --------------
    // Counting-sorted into a 160x90 grid (16 px cells at 1440p; the shader
    // walks the 2x2 cells around a pixel and clamps a disc at half a cell,
    // 7.5 px), a cell holding at most kMoteCellCap. Two records per mote: (screen uv,
    // depth t, radius px in focus) and (gain = the spawn fade x the glint
    // flash). Only while motes > 0: the tail is read by OIL_MOTES alone.
    // (director_water: also with none, so a count that glided to 0 leaves empty cells, not last frame's)
    if (MotesWanted() && (!m_motes.empty() || m_dirWaterOn) && m_dropletData[fi] && m_dropletCellData[fi]) {
        AcidDropGPU* md = (AcidDropGPU*)m_dropletData[fi] + kAcidMaxDrops * 2;
        DropCellGPU* mc = (DropCellGPU*)m_dropletCellData[fi] + kDropGridW * kDropGridH;
        const int NC = kMoteGridW * kMoteGridH, n = (int)m_motes.size();
        m_moteCellCount.assign(NC, 0);
        m_moteCellStart.assign(NC, 0);
        m_moteOrder.assign(n, -1);
        for (int i = 0; i < n; i++) {
            const Mote& m = m_motes[i];
            if (m.x < 0.0f || m.x >= 1.0f || m.y < 0.0f || m.y >= 1.0f) continue;
            const int c = (int)(m.y * kMoteGridH) * kMoteGridW + (int)(m.x * kMoteGridW);
            if (m_moteCellCount[c] >= kMoteCellCap) continue;
            m_moteCellCount[c]++;
            m_moteOrder[i] = c;
        }
        for (int c = 1; c < NC; c++) m_moteCellStart[c] = m_moteCellStart[c - 1] + m_moteCellCount[c - 1];
        std::vector<int> fill(NC, 0);
        const float pxScale = fminf(fmaxf(a.motePx, 0.2f), 6.0f) * (float)m_height / 1440.0f;
        for (int i = 0; i < n; i++) {
            const int c = m_moteOrder[i];
            if (c < 0) continue;
            const int k = m_moteCellStart[c] + fill[c]++;
            const Mote& m = m_motes[i];
            float gain = m.fade;
            if (m_dirWaterOn) gain *= fminf(fmaxf(m_dirMotes - (float)i, 0.0f), 1.0f);   // director_water
            if (m.glint) gain *= 1.0f + 6.0f * powf(fmaxf(0.0f, sinf(m.phase + m.rate * m_time)), 24.0f);
            md[2 * k].a[0] = m.x;  md[2 * k].a[1] = m.y;
            md[2 * k].a[2] = m.t;  md[2 * k].a[3] = m.r * pxScale;
            md[2 * k + 1].a[0] = gain; md[2 * k + 1].a[1] = md[2 * k + 1].a[2] = md[2 * k + 1].a[3] = 0.0f;
        }
        for (int c = 0; c < NC; c++) {
            mc[c].first = (uint32_t)m_moteCellStart[c];
            mc[c].count = (uint32_t)m_moteCellCount[c];
        }
    }
}

void FluidRenderer::BindAcid() {
    const UINT fi = m_frameIndex;
    if (m_acidParamUpload[fi])
        m_cmd->SetGraphicsRootConstantBufferView(2, m_acidParamUpload[fi]->GetGPUVirtualAddress());
    if (m_acidBlobUpload[fi])
        m_cmd->SetGraphicsRootShaderResourceView(3, m_acidBlobUpload[fi]->GetGPUVirtualAddress());
    if (m_dropletUpload[fi])
        m_cmd->SetGraphicsRootShaderResourceView(7, m_dropletUpload[fi]->GetGPUVirtualAddress());
    if (m_dropletCellUpload[fi])
        m_cmd->SetGraphicsRootShaderResourceView(8, m_dropletCellUpload[fi]->GetGPUVirtualAddress());
}

// ===========================================================================
// "Ink in water" â€” the SHARED render block's constants, and the drop emitter.
// Both are used by style=ink and (the constants) by liquid_acid ink_mode=water.
// ===========================================================================

void FluidRenderer::UploadInkConstants() {
    const InkConfig& k = m_cfg.ink;
    const UINT fi = m_frameIndex;
    if (!m_inkParamData[fi]) return;
    InkParamsGPU p = {};
    const float p0[4] = { fmaxf(k.density, 0.0f), fmaxf(k.chroma, 0.0f),
                          k.edgeStrength, fmaxf(k.edgeScale, 0.25f) };
    const float p1[4] = { k.edgeLo, k.edgeHi, k.inverted ? 1.0f : 0.0f, k.vignette };
    const float p2[4] = { k.parallax, k.parallaxScale, k.parallaxDrift, m_time };
    const float p3[4] = { k.coreKnee, k.hdrCore, k.motionLo, k.motionHi };
    const float p4[4] = { k.motionOpacity, k.veilFloor, k.tintMidDip,
                          (k.tonemap > 0) ? 1.0f : 0.0f };
    memcpy(p.p0, p0, 16); memcpy(p.p1, p1, 16);
    memcpy(p.p2, p2, 16); memcpy(p.p3, p3, 16); memcpy(p.p4, p4, 16);

    // ---- [ink] tonemap: resolve the nits targets into scRGB -------------
    // The display shader works in scRGB, where 1.0 = 80 nits by definition,
    // so every "_nits" key is just a divide. white_nits = 0 means "the SDR
    // white level we already sit at" (80 * sdr_scale), which makes tonemap=1
    // a pure curve change; the presets set it explicitly.
    // With HDR OFF the swap chain clips at 1.0, so both ceilings come down to
    // SDR white â€” the look must survive HDR on AND off (AGENTS.md).
    const float sdrScale = fmaxf(m_sdrScale, 0.01f);
    float whiteSc = (k.whiteNits > 0.5f) ? k.whiteNits / 80.0f : sdrScale;
    float peakSc  = (m_cfg.hdrPeakNits > 0.5f) ? m_cfg.hdrPeakNits / 80.0f
                                               : whiteSc;
    if (!m_hdrActive) { whiteSc = fminf(whiteSc, sdrScale); peakSc = sdrScale; }
    // peak_nits is the CEILING, so the tray's peak menu (CMD_PEAK_*) caps the
    // ink curve too: a peak set below white_nits pulls the white point down
    // with it rather than being ignored.
    whiteSc = fminf(whiteSc, peakSc);
    float blackSc = fmaxf(k.blackNits, 0.0f) / 80.0f;
    blackSc = fminf(blackSc, whiteSc);
    // The ink tonemap writes absolute scRGB (it never multiplies by the b0
    // sdrScale), so the cycle fade scales its three targets directly. x1.0
    // (the default) leaves every value bit-identical.
    const float p5[4] = { whiteSc * m_fade, blackSc * m_fade,
                          fminf(fmaxf(k.toneKnee, 0.0f), 1.0f),
                          fmaxf(k.toneChroma, 0.0f) };
    const float p6[4] = { fminf(fmaxf(k.tintHueBlend, 0.0f), 1.0f),
                          peakSc * m_fade,
                          k.blackOnly ? 1.0f : 0.0f,                       // black_only
                          k.blackOnly ? fminf(fmaxf(k.filmColour, 0.0f), 1.0f) : 0.0f };
    memcpy(p.p5, p5, 16); memcpy(p.p6, p6, 16);

    // ---- duotone PAIR ROTATION ([ink] pair_sweep_period) -----------------
    // With the sweep off these are just the authored tints. With it on, the
    // pair cross-fades through the SAME curated complementary anchors the
    // liquid_acid palette sweep uses, with the same hue-preserving HSV lerp
    // and the same smoothstepped fade (so each pair gets a long settled
    // stretch). The ink anchor drives the THIN veil and the oil anchor the
    // THICK core: the ink anchors are the darker, cooler half of every pair,
    // which keeps veils darker than cores â€” the assignment the user's
    // hand-picked duotone inis already use (teal veil / vermillion core).
    float tThin[3], tThick[3];
    memcpy(tThin,  k.tintThin,  sizeof(tThin));
    memcpy(tThick, k.tintThick, sizeof(tThick));
    if (k.pairSweepPeriod > 0.01f) {
        const LiquidAcidConfig& a = m_cfg.acid;   // the curated pair list
        int np = a.sweepCount;                    // sweep_count, default 5
        if (np < 1) np = 1;
        if (np > LiquidAcidConfig::kSweepMax) np = LiquidAcidConfig::kSweepMax;
        const float u = fmodf(AnimatorTime(0) / k.pairSweepPeriod, 1.0f) * np;
        const int k0 = (int)u % np, k1 = (k0 + 1) % np;
        float f = u - floorf(u);
        f = f * f * (3.0f - 2.0f * f);
        HsvLerp3(&a.sweepInk[k0 * 3], &a.sweepInk[k1 * 3], f, tThin);
        HsvLerp3(&a.sweepOil[k0 * 3], &a.sweepOil[k1 * 3], f, tThick);
    }
    for (int i = 0; i < 3; i++) {
        p.paper[i]     = k.paper[i];
        p.tintThin[i]  = tThin[i];
        p.tintThick[i] = tThick[i];
    }
    memcpy(m_inkParamData[fi], &p, sizeof(p));
}

void FluidRenderer::BindInk() {
    const UINT fi = m_frameIndex;
    if (m_inkParamUpload[fi])
        m_cmd->SetGraphicsRootConstantBufferView(4, m_inkParamUpload[fi]->GetGPUVirtualAddress());
    if (m_velLow.res) {
        Transition(m_velLow, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_cmd->SetGraphicsRootDescriptorTable(5, m_velTable);
    }
}

void FluidRenderer::InjectDrop(float x, float y, float vx, float vy,
                               float radiusPct, float density,
                               const float* rgb, int spatter) {
    const DropConfig& d = m_cfg.drops;
    if (vy < 0.0f)        vy = d.speed;
    if (radiusPct < 0.0f) radiusPct = d.radius;
    if (density < 0.0f)   density = d.density;
    if (spatter < 0)      spatter = d.spatter;
    if (spatter > 12)     spatter = 12;

    float col[3];
    if (rgb) { col[0] = rgb[0]; col[1] = rgb[1]; col[2] = rgb[2]; }
    else if (d.colorMode == 1) PickSplatColor(0.0f, col);
    else { col[0] = d.color[0]; col[1] = d.color[1]; col[2] = d.color[2]; }

    // The head. One Gaussian velocity impulse pointing down: after the
    // pressure projection it is a vortex dipole, and that dipole is what rolls
    // the head into the mushroom cap in ref 1. The dye cap is raised to the
    // drop density so a 1.35 core is not clipped by a lower max_brightness.
    const float cap = fmaxf(density, m_cfg.maxBrightness);
    const float asym = d.asymmetry;
    if (asym > 0.01f) {
        // Two unequal, off-centre velocity impulses around ONE dye stamp. The
        // velocity grid is 256 wide, so this is nearly free; a second dye stamp
        // at 4096 would not be. The result is an unequal vortex pair with one
        // lobe leading, like the references, instead of the textbook
        // mirror-symmetric one a single Gaussian impulse produces.
        const float side = (RandF() < 0.5f) ? -1.0f : 1.0f;
        const float off  = 0.5f * asym * radiusPct * 0.01f * (float)m_width;
        const float lead = 1.0f + 0.5f * asym * side;
        const float sp   = fmaxf(d.impulseSpread, 0.25f);
        const float vr   = radiusPct * sp * sp;   // radius is a squared scale
        // [drops] impulse (round 3): 1 = these two calls as they always were; 0 = no velocity at all
        if (d.impulse >= 0.9999f) {
            SplatVelocity(x - off, y, vx - 0.25f * asym * vy * side, vy * lead,
                          vr * (1.0f - 0.25f * asym));
            SplatVelocity(x + off, y, vx + 0.25f * asym * vy * side, vy * (2.0f - lead),
                          vr * (1.0f + 0.25f * asym));
        } else if (d.impulse > 0.0001f) {
            const float ik = d.impulse;
            SplatVelocity(x - off, y, (vx - 0.25f * asym * vy * side) * ik, vy * lead * ik,
                          vr * (1.0f - 0.25f * asym));
            SplatVelocity(x + off, y, (vx + 0.25f * asym * vy * side) * ik, vy * (2.0f - lead) * ik,
                          vr * (1.0f + 0.25f * asym));
        }
        SplatDye(x, y, col[0] * density, col[1] * density, col[2] * density,
                 radiusPct, cap, true);
    } else {
        const float sp = fmaxf(d.impulseSpread, 0.25f);
        if (d.impulse >= 0.9999f) SplatVelocity(x, y, vx, vy, radiusPct * sp * sp);
        else if (d.impulse > 0.0001f) SplatVelocity(x, y, vx * d.impulse, vy * d.impulse, radiusPct * sp * sp);
        SplatDye(x, y, col[0] * density, col[1] * density, col[2] * density,
                 radiusPct, cap, true);
    }

    // Splash satellites (ref 2). Not 2D physics â€” a fake, and a cheap one:
    // each is a full dye-res pass, so the count is capped at 12.
    for (int i = 0; i < spatter; i++) {
        const float ang = RandF() * 6.2831853f;
        const float rr  = d.spatterSpread * (0.35f + 0.65f * RandF());
        const float sx  = x + cosf(ang) * rr * (float)m_width;
        const float sy  = y + sinf(ang) * rr * (float)m_height;
        Splat(sx, sy, cosf(ang) * d.spatterSpeed * 0.35f,
              sinf(ang) * d.spatterSpeed * 0.35f + d.spatterSpeed * 0.5f,
              col[0] * density, col[1] * density, col[2] * density,
              d.spatterRadius, cap);
    }

    // The thin trail hanging from the entry point: dye only, no velocity.
    m_dropTailLeft = d.tailSec;
    m_dropTailX = x; m_dropTailY = y;
    m_dropTailCol[0] = col[0]; m_dropTailCol[1] = col[1]; m_dropTailCol[2] = col[2];
}

void FluidRenderer::UpdateDrops(float dt) {
    const DropConfig& d = m_cfg.drops;
    if (m_dropQueued) {              // QueueDrop() from outside the render loop
        m_dropQueued = false;
        InjectDrop(m_dropQx, m_dropQy, 0.0f, m_dropQvy);
    }
    // The entry trail keeps painting even after the emitter is switched off,
    // so a half-finished drop is never truncated.
    if (m_dropTailLeft > 0.0f) {
        const float amt = d.tailDensity * m_emitScale;
        const float tr = fmaxf(d.radius * d.tailRadiusFrac, 0.01f);
        // fps-normalised: this runs every frame for tail_sec, so an un-scaled
        // impulse would add ~86 kicks at 144 fps and fire the plume into the floor
        if (d.tailSpeed > 0.001f && d.impulse > 0.0001f)
            SplatVelocity(m_dropTailX, m_dropTailY, 0.0f, (d.impulse >= 0.9999f ? 1.0f : d.impulse) *
                          d.speed * d.tailSpeed * m_emitScale * 0.1f, tr * 4.0f);
        SplatDye(m_dropTailX, m_dropTailY,
                 m_dropTailCol[0] * amt, m_dropTailCol[1] * amt, m_dropTailCol[2] * amt,
                 tr, fmaxf(d.density, m_cfg.maxBrightness), true);
        m_dropTailLeft -= dt;
    }
    if (!d.enabled) return;
    if (!m_dropPrimed) {
        // Seeded from RandF() AFTER InitWanderers, so --seed replay is stable
        // and existing seeded sequences are not shifted.
        m_dropTimer = d.interval * (0.3f + 0.7f * RandF());
        m_dropPrimed = true;
    }
    m_dropTimer -= dt;
    if (m_dropTimer > 0.0f) return;
    m_dropTimer = fmaxf(d.interval * (1.0f + (RandF() - 0.5f) * 0.7f), 0.5f);
    if (d.obeyGovernor && m_screenTooFull) return;   // water already full of ink
    float x = (d.xMin + (d.xMax - d.xMin) * RandF()) * (float)m_width;
    float y = (d.yMin + (d.yMax - d.yMin) * RandF()) * (float)m_height;
    // in_black (fluid.h): the entry point goes where the oil field is below
    // its threshold with room to spare, i.e. inside the black. Key at 0: not
    // one extra random draw, the drop lands where it always did.
    if (d.inBlack > 0.0f && m_cfg.acid.enabled) {
        const float aspect = (float)m_width / fmaxf((float)m_height, 1.0f);
        bool found = false;
        for (int tr = 0; tr < 40 && !found; tr++) {
            const float ux = 0.04f + 0.92f * RandF(), uy = 0.04f + 0.92f * RandF();
            float f, gx, gy, ovx, ovy;
            AcidFieldAt(ux, uy, aspect, f, gx, gy, ovx, ovy);
            const float gl = sqrtf(gx * gx + gy * gy) + 1e-6f;
            if ((f - m_cfg.acid.threshold) / gl < -d.inBlack) {
                x = ux * (float)m_width;
                y = uy * (float)m_height;
                found = true;
            }
        }
        if (!found) return;          // no black with room right now: skip this drop
    }
    InjectDrop(x, y, (RandF() - 0.5f) * 120.0f, d.speed);
}

void FluidRenderer::UpdateCoverage() {
    if ((!m_cfg.autoPause || !m_cfg.wanderers) && !m_coverageWanted) {
        m_screenTooFull = false;
        m_survivorTooFull = false;
        return;
    }
    // Headless capture: the harvest must land on a FIXED frame, not "whenever
    // the GPU happened to finish" â€” otherwise the governor flips a frame or
    // two earlier/later between runs and the images diverge. Pin it to the
    // same 1 Hz cadence as the issue, blocking if the copy isn't done yet.
    if (m_headless && m_covPending && m_time - m_lastCovTime >= 1.0f &&
        m_fence->GetCompletedValue() < m_covFence) {
        HR(m_fence->SetEventOnCompletion(m_covFence, m_fenceEvent));
        if (!m_deviceLost) WaitForSingleObject(m_fenceEvent, INFINITE);   // R3
    }
    // harvest a finished readback (issued a frame or more ago; 1 Hz cadence
    // makes the staleness irrelevant and avoids any GPU sync)
    if (m_covPending && m_fence->GetCompletedValue() >= m_covFence &&
        (!m_headless || m_time - m_lastCovTime >= 1.0f)) {
        uint8_t* data = nullptr;
        D3D12_RANGE range = { 0, (SIZE_T)m_covPitch * kCovH };
        if (SUCCEEDED(m_covReadback->Map(0, &range, (void**)&data))) {
            ProcessCoverage(data, m_covPitch);
            D3D12_RANGE none = { 0, 0 };
            m_covReadback->Unmap(0, &none);
        }
        m_covPending = false;
    }
    if (!m_covPending && m_time - m_lastCovTime >= 1.0f) {
        m_lastCovTime = m_time;
        SimCB cb = {};
        m_cmd->SetComputeRootSignature(m_computeRS.Get());
        Transition(*m_dye.read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(m_coverage, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_cmd->SetPipelineState(m_psoDownsample.Get());
        cb.dimsW = kCovW; cb.dimsH = kCovH;
        m_cmd->SetComputeRoot32BitConstants(0, sizeof(SimCB) / 4, &cb, 0);
        m_cmd->SetComputeRootDescriptorTable(1, m_dye.read->srv);
        m_cmd->SetComputeRootDescriptorTable(2, m_dye.read->srv);
        m_cmd->SetComputeRootDescriptorTable(3, m_coverage.uav);
        m_cmd->SetComputeRootDescriptorTable(4, m_coverage.uav);
        m_cmd->SetComputeRootDescriptorTable(5, m_coverage.uav);
        m_cmd->Dispatch(Groups(kCovW), Groups(kCovH), 1);

        Transition(m_coverage, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
        src.pResource = m_coverage.res.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = m_covReadback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        dst.PlacedFootprint.Footprint.Width = kCovW;
        dst.PlacedFootprint.Footprint.Height = kCovH;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = m_covPitch;
        m_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        m_covFence = m_nextFence;   // EndFrameAndPresent signals this value
        m_covPending = true;
    }
}

// Port of measureCoverage(): dark-area %, 6x3 tile contrast, hysteresis.
void FluidRenderer::ProcessCoverage(const uint8_t* data, UINT pitch) {
    int dark = 0;
    const int total = kCovW * kCovH;
    float tiles[18] = {};   // 6x3
    const int TX = 6, TY = 3;
    const float tw = kCovW / (float)TX, th = kCovH / (float)TY;
    float hueX = 0, hueY = 0, hueW = 0;   // brightness-weighted circular mean

    for (int y = 0; y < kCovH; y++) {
        const uint16_t* row = (const uint16_t*)(data + (SIZE_T)y * pitch);
        for (int x = 0; x < kCovW; x++) {
            float r = HalfToFloat(row[x * 4 + 0]);
            float g = HalfToFloat(row[x * 4 + 1]);
            float b = HalfToFloat(row[x * 4 + 2]);
            float m = fmaxf(r, fmaxf(g, b));
            if (m <= m_cfg.darkLevel) dark++;
            else {
                float mn = fminf(r, fminf(g, b));
                float d = m - mn;
                if (d > 1e-5f) {
                    float h;
                    if (m == r) h = fmodf((g - b) / d, 6.0f);
                    else if (m == g) h = (b - r) / d + 2.0f;
                    else h = (r - g) / d + 4.0f;
                    float rad = h * 60.0f * (3.14159265f / 180.0f);
                    hueX += cosf(rad) * m;
                    hueY += sinf(rad) * m;
                    hueW += m;
                }
            }
            int ti = (int)fminf(TX - 1.0f, x / tw) + TX * (int)fminf(TY - 1.0f, y / th);
            tiles[ti] += m;
        }
    }
    float darkPct = 100.0f * dark / total;
    m_darkPct = darkPct;
    if (hueW > 0.001f) {
        float ang = atan2f(hueY, hueX) * 180.0f / 3.14159265f;
        m_avgHue = fmodf(ang + 720.0f, 360.0f);
    }

    float maxTile = 0, sumTiles = 0;
    for (int t = 0; t < TX * TY; t++) {
        tiles[t] /= tw * th;
        sumTiles += tiles[t];
        if (tiles[t] > maxTile) maxTile = tiles[t];
    }
    float avgRest = (sumTiles - maxTile) / (TX * TY - 1);
    bool contrastOK = m_cfg.contrastReq <= 0 ||
                      maxTile >= avgRest * (1.0f + m_cfg.contrastReq / 100.0f);

    // hysteresis: pause below the floor, resume only once 10 points above it
    if (!m_screenTooFull && darkPct < m_cfg.darkFloor) m_screenTooFull = true;
    else if (m_screenTooFull && darkPct > fminf(m_cfg.darkFloor + 10.0f, 95.0f)) m_screenTooFull = false;
    if (!m_survivorTooFull && darkPct < m_cfg.survDarkFloor) m_survivorTooFull = true;
    else if (m_survivorTooFull && darkPct > fminf(m_cfg.survDarkFloor + 10.0f, 95.0f)) m_survivorTooFull = false;

    // flat dim mush (no standout region): cancel pauses, keep painting
    if (!contrastOK) {
        m_screenTooFull = false;
        m_survivorTooFull = false;
    }
}
