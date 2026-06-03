#pragma once
#include <d2d1_1.h>
#include <d2d1_3.h>   /* ID2D1DeviceContext5 / ID2D1SvgDocument (Win10 1607+) */
#include <d3d11.h>
#include <d3d11_4.h>  /* ID3D11Multithread */
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <dwrite.h>
#include <dwrite_2.h>   /* IDWriteFontFallback / Builder (DWrite 1.2+) */
#include <dwrite_3.h>   /* IDWriteTextFormat3 (DWrite 1.3+, Win10) */
#include <wincodec.h>
#include <wrl/client.h>
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include "theme.h"

using Microsoft::WRL::ComPtr;

namespace ui {

// Parsed SVG icon (cached geometry for efficient rendering)
struct SvgPathLayer {
    ComPtr<ID2D1PathGeometry> geometry;
    float opacity = 1.0f;     // per-path opacity from SVG
    float strokeWidth = 0.0f; // >0 means stroke instead of fill
    /* 累积的 SVG transform（来自所有外层 <g transform> + 自身 transform）。
     * 行向量乘法语义：屏幕点 = 路径局部点 * pathTransform * iconScale。 */
    D2D1_MATRIX_3X2_F transform = D2D1::Matrix3x2F::Identity();
};

/* L75: SVG 文字 run —— <text>/<tspan> 或 <foreignObject> 内嵌 HTML 文字.
 * D2D ID2D1SvgDocument 只渲染形状 (shapes-only, 不画文字), core-ui 自己把文字
 * 解析出来用 DirectWrite 叠加补渲 —— 跟形状走同一个 user→screen 变换, 保证
 * 对齐 + 任意缩放矢量清晰. 覆盖 Mermaid (foreignObject) / draw.io / Graphviz
 * (<text>) 等图表 SVG. */
// L87: SVG 渐变 fill (linear/radial), 给 <text> 叠加渲染用.
struct SvgGradientStop {
    float        offset = 0.0f;                 // 0..1
    D2D1_COLOR_F color  = D2D1::ColorF(0, 0, 0, 1);
};
struct SvgTextGradient {
    bool  radial    = false;
    bool  userSpace = false;                    // gradientUnits: false=objectBoundingBox(默认)
    D2D1_MATRIX_3X2_F transform = D2D1::Matrix3x2F::Identity();  // gradientTransform
    float x1 = 0, y1 = 0, x2 = 1, y2 = 0;       // linear 向量 (objectBB 默认水平 0→1)
    float cx = 0.5f, cy = 0.5f, r = 0.5f;       // radial 中心/半径
    std::vector<SvgGradientStop> stops;
};

struct SvgTextRun {
    std::wstring      text;        // 已解码 (实体 + <br>/</p> → \n), 可多行
    D2D1_MATRIX_3X2_F transform = D2D1::Matrix3x2F::Identity();  // 累积 <g> transform
    float             x = 0.0f;    // user-space 锚点 (在 transform 之前的局部坐标)
    float             y = 0.0f;
    float             fontSize = 16.0f;
    std::wstring      fontFamily;  // 空 / 通用名(sans-serif…) = 主题默认字体
    DWRITE_FONT_WEIGHT fontWeight = DWRITE_FONT_WEIGHT_NORMAL;  // L87: font-weight
    D2D1_COLOR_F      color = D2D1::ColorF(D2D1::ColorF::Black);
    float             opacity = 1.0f;   // L87: 累积父 <g opacity> + 自身 (fill-)opacity
    bool              hasGradient = false;  // L87: fill="url(#id)" 命中渐变
    SvgTextGradient   gradient;             // L87: hasGradient 时有效
    int               anchor = 0;  // text-anchor: 0=start 1=middle 2=end
    float             maxWidth = 0.0f;  // >0 = 换行宽度 (foreignObject width)
    bool              block = false;    // true = foreignObject (块级, 围绕 (x,y) 居中)
};

struct SvgIcon {
    ComPtr<ID2D1PathGeometry> geometry;  // combined (legacy, used if layers empty)
    std::vector<SvgPathLayer> layers;    // per-path with opacity
    std::vector<SvgTextRun>   textRuns;  // L75: <text> / <foreignObject> 文字
    float viewBoxW = 24;
    float viewBoxH = 24;
    bool valid = false;
};

class Renderer {
public:
    // Create and own factories internally (standalone mode)
    bool Init();

    // Use externally-owned factories (DLL shared mode)
    bool Init(ID2D1Factory1* factory, IDWriteFactory* dwFactory, IWICImagingFactory* wicFactory);

    bool CreateRenderTarget(HWND hwnd);
    // Composition-mode RT for transparent/layered windows (popup menus,
    // tooltips). Builds a SwapChainForComposition with premultiplied alpha
    // and binds it via DirectComposition so per-pixel alpha is honored on
    // the desktop without WS_EX_LAYERED's GDI bottleneck. Anti-aliased
    // round corners come from D2D's normal FillRoundedRectangle path.
    bool CreateRenderTargetForLayered(HWND hwnd);
    void Resize(UINT width, UINT height);
    void BeginDraw();
    HRESULT EndDraw();
    void FlushAndTrimGpu();

    // Skip VSync for next Present (used during resize for immediate feedback)
    bool skipVSync = false;

    void Clear(const D2D1_COLOR_F& color);
    void FillRect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& color);
    void FillRoundedRect(const D2D1_RECT_F& rect, float rx, float ry, const D2D1_COLOR_F& color);
    void DrawRect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float width = 1.0f);
    void DrawRoundedRect(const D2D1_RECT_F& rect, float rx, float ry, const D2D1_COLOR_F& color, float width = 1.0f);
    void DrawLine(float x1, float y1, float x2, float y2, const D2D1_COLOR_F& color, float width = 1.0f);
    void DrawText(const std::wstring& text, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color,
                  float fontSize, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
                  DWRITE_PARAGRAPH_ALIGNMENT vAlign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                  bool wordWrap = false);
    float MeasureTextWidth(const std::wstring& text, float fontSize,
                           const wchar_t* family = nullptr,  /* nullptr = 用 DefaultFontFamily() */
                           DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL);
    float MeasureTextHeight(const std::wstring& text, float maxWidth, float fontSize,
                            DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL);
    void DrawIcon(const std::wstring& glyph, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float fontSize);

    // Image support
    ComPtr<ID2D1Bitmap> LoadImageFromFile(const std::wstring& path);
    // 从内存里的 PNG/JPG/BMP/ICO/... 字节解码出位图。配合 ui::asset
    // 解析器使用：HTML <img src="logo.png"> → asset::Resolve → 这里。
    // bytes 在调用期间需有效；返回的位图自带 GPU 资源、跟字节解耦。
    ComPtr<ID2D1Bitmap> LoadImageFromBytes(const void* bytes, size_t size);

    // Animated image (GIF) support — 按需解码：维护一张 CPU 画布 + 元数据表，
    // 调用方在动画 timer 里推进帧。内存随 GIF 尺寸而非帧数增长。
    class AnimatedPlayer {
    public:
        ~AnimatedPlayer();
        int FrameCount() const { return (int)meta_.size(); }
        int CanvasWidth() const { return canvasW_; }
        int CanvasHeight() const { return canvasH_; }
        int DelayMs(int frameIndex) const;
        /* 把第 frameIndex 帧合成到画布，返回 BGRA 指针（stride = width*4）。
         * 顺序前进时 O(1)；出现跳帧或循环回绕时会从头重放至目标帧。 */
        const uint8_t* ComposeTo(int frameIndex);
    private:
        friend class Renderer;
        struct FrameMeta {
            UINT x = 0, y = 0, w = 0, h = 0;
            int delayMs = 100;
            int disposal = 0;
        };
        ComPtr<IWICImagingFactory> wic_;
        ComPtr<IWICBitmapDecoder> decoder_;
        int canvasW_ = 0, canvasH_ = 0;
        std::vector<FrameMeta> meta_;
        std::vector<uint8_t> canvas_;
        std::vector<uint8_t> prevCanvas_;  // disposal=3 备份
        int lastComposed_ = -1;
        bool DecodeOne(int index);
        void ResetCanvas();
    };
    /* 打开一个动图（目前仅 GIF）。返回 nullptr 表示非动画或失败。 */
    std::unique_ptr<AnimatedPlayer> OpenAnimatedImage(const std::wstring& path);
    ComPtr<ID2D1Bitmap> CreateBitmapFromPixels(const void* pixels, int width, int height, int stride);
    /* 跟 CreateBitmapFromPixels 一样, 但接收 straight (non-premultiplied) alpha
       BGRA — 内部做 RGB *= A/255 转 premul 再创建 D2D bitmap. 适合直接从 PNG/
       JPEG 解码器拿到的 BGRA (ghde / WIC GUID_WICPixelFormat32bppBGRA / libpng
       / stb_image 等). 不这么做的话, straight 字节被 D2D PREMULTIPLIED bitmap
       按 premul 公式渲染, 抗锯齿边缘 (alpha 中间值) 颜色被错误广播 → chroma
       fringe (透明 PNG 大倍率放大尤其明显). */
    ComPtr<ID2D1Bitmap> CreateBitmapFromPixelsStraight(const void* pixels, int width, int height, int stride);
    ComPtr<ID2D1Bitmap> CreateEmptyBitmap(int width, int height);
    /* HICON → D2D 位图。用于把 EXE 嵌入的图标资源 / Win32 加载的图标转成
       D2D 可绘制位图。caller 仍持有 HICON 所有权，函数内不 DestroyIcon。 */
    ComPtr<ID2D1Bitmap> CreateBitmapFromHICON(HICON hicon);
    void DrawBitmap(ID2D1Bitmap* bitmap, const D2D1_RECT_F& destRect, float opacity = 1.0f,
                    D2D1_BITMAP_INTERPOLATION_MODE interp = D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    /* 高质量插值绘制（缩小时用 HIGH_QUALITY_CUBIC，文字/线条更清晰） */
    void DrawBitmapHQ(ID2D1Bitmap* bitmap, const D2D1_RECT_F& destRect, float opacity = 1.0f,
                      D2D1_INTERPOLATION_MODE interp = D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    /* 带锐化的位图绘制（用于图片查看器） */
    void DrawBitmapSharpened(ID2D1Bitmap* bitmap, const D2D1_RECT_F& destRect, float sharpenAmount = 0.3f,
                              D2D1_INTERPOLATION_MODE interp = D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    void FillRectWithBitmap(ID2D1Bitmap* bitmap, const D2D1_RECT_F& rect);
    void PushClip(const D2D1_RECT_F& rect);
    void PopClip();
    // Rounded-rect clip: anything drawn between Push/Pop is masked to the
    // rounded shape. Required to keep state-layer hovers (e.g. up/down buttons
    // on a rounded NumberBox, items in a rounded ComboBox popup) from spilling
    // past the container's corners. Must be paired with PopRoundedClip().
    void PushRoundedClip(const D2D1_RECT_F& rect, float rx, float ry);
    void PopRoundedClip();
    void PushOpacity(float opacity, const D2D1_RECT_F& bounds);
    void PopOpacity();

    // SVG icon support
    SvgIcon ParseSvgIcon(const std::string& svgContent);
    void DrawSvgIcon(const SvgIcon& icon, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color);
    /* L75: 只解析 SVG 文字 run (不建 path 几何) —— 给 D2D 原生 SVG 路径用:
     * D2D 画形状, 这个补文字. */
    std::vector<SvgTextRun> ParseSvgTextRuns(const std::string& svgContent);
    /* L75: DirectWrite 渲染文字 run. baseXf = SVG user-space → 屏幕 的变换
     * (跟 DrawSvgDocument / DrawSvgIcon 同一个), 每个 run 再左乘自身累积 transform.
     * 内部 save/restore 当前 RT transform. */
    void DrawSvgTextRuns(const std::vector<SvgTextRun>& runs,
                         const D2D1_MATRIX_3X2_F& baseXf);

    ID2D1DeviceContext* RT() { return ctx_.Get(); }
    /* ID2D1DeviceContext5（支持 CreateSvgDocument / DrawSvgDocument）。
     * Windows 10 1607 以下返回 nullptr，调用方应做 null 检查并降级。
     * ctx5_ 与 ctx_ 指向同一 D2D 对象，通过 QueryInterface 获得。*/
    ID2D1DeviceContext5* RT5() { return ctx5_.Get(); }
    ID2D1Factory1* Factory() { return factory_; }
    IDWriteFactory* DWFactory() { return dwFactory_; }
    IWICImagingFactory* WIC() { return wicFactory_; }

    /* Build an IDWriteTextLayout for the given text. Used by widgets that
     * need to share one layout between hit-test, caret positioning,
     * selection rendering and draw (TextArea). `wrap` controls
     * DWRITE_WORD_WRAPPING_WRAP vs NO_WRAP. Returns nullptr on failure.
     * Layout uses theme::kFontFamily (and per-window override if set). */
    ComPtr<IDWriteTextLayout> CreateTextLayout(const std::wstring& text,
                                                float maxWidth, float maxHeight,
                                                float fontSize,
                                                bool wrap = false,
                                                DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL);

    // 临时设置渲染目标（用于缓存等场景）
    void SetTarget(ID2D1DeviceContext* target) { ctx_ = target; }

    // ---- 字体 / 渲染模式 per-window 状态 (since 1.3.0) ----
    // 空串 = 跟随 theme:: 全局默认
    void        SetDefaultFontFamily(const wchar_t* family);
    void        SetCjkFonts(const wchar_t* latin, const wchar_t* cjk);
    void        SetTextRenderMode(theme::TextRenderMode mode);
    const wchar_t* DefaultFontFamily() const;   // 实际生效的默认字体族（窗口 > theme > "Segoe UI"）
    const wchar_t* LatinFontFamily() const;     // 中英分离的拉丁字体，nullptr=未设
    const wchar_t* CjkFontFamily() const;       // 中英分离的中文字体，nullptr=未设
    theme::TextRenderMode TextRenderMode() const;

    // 应用当前文字渲染模式到 ctx_（在 BeginDraw 之前调用或 CreateRenderTarget 后立即调用）
    void ApplyTextRenderMode();

private:
    struct ColorKey {
        uint32_t r = 0;
        uint32_t g = 0;
        uint32_t b = 0;
        uint32_t a = 0;

        bool operator==(const ColorKey& other) const {
            return r == other.r && g == other.g && b == other.b && a == other.a;
        }
    };

    struct ColorKeyHash {
        size_t operator()(const ColorKey& k) const {
            size_t h = 1469598103934665603ull;
            h = (h ^ k.r) * 1099511628211ull;
            h = (h ^ k.g) * 1099511628211ull;
            h = (h ^ k.b) * 1099511628211ull;
            h = (h ^ k.a) * 1099511628211ull;
            return h;
        }
    };

    struct TextFormatKey {
        uint32_t sizeBits = 0;
        uint32_t weight = 0;
        std::wstring family;

        bool operator==(const TextFormatKey& other) const {
            return sizeBits == other.sizeBits &&
                   weight == other.weight &&
                   family == other.family;
        }
    };

    struct TextFormatKeyHash {
        size_t operator()(const TextFormatKey& k) const {
            size_t h = std::hash<uint32_t>{}(k.sizeBits);
            h ^= (std::hash<uint32_t>{}(k.weight) + 0x9e3779b9 + (h << 6) + (h >> 2));
            h ^= (std::hash<std::wstring>{}(k.family) + 0x9e3779b9 + (h << 6) + (h >> 2));
            return h;
        }
    };

    // Owned factories (standalone mode)
    ComPtr<ID2D1Factory1>      ownedFactory_;
    ComPtr<IDWriteFactory>     ownedDwFactory_;
    ComPtr<IWICImagingFactory> ownedWicFactory_;

    // Active pointers (may point to owned or external)
    ID2D1Factory1*      factory_    = nullptr;
    IDWriteFactory*     dwFactory_  = nullptr;
    IWICImagingFactory* wicFactory_ = nullptr;

    // D3D11/D2D 设备（全局共享，首个窗口创建时初始化）
    static ComPtr<ID3D11Device>  s_d3dDevice;
    static ComPtr<ID2D1Device>   s_d2dDevice;
    static int                   s_deviceRefCount;
    bool EnsureSharedDevice();

    // 每窗口独立资源
    ComPtr<ID2D1DeviceContext>  ctx_;
    /* ctx_ QueryInterface 到 ID2D1DeviceContext5 的副本，Win10 1607 以下为空。
     * 同对象多接口，不增加实际资源，仅提供 SVG 等 1607+ API 的入口。*/
    ComPtr<ID2D1DeviceContext5> ctx5_;
    ComPtr<IDXGISwapChain1>    swapChain_;
    ComPtr<ID2D1Bitmap1>       targetBitmap_;
    HWND                       hwnd_ = nullptr;
    // DirectComposition objects — only populated for layered/composition
    // mode (CreateRenderTargetForLayered). Held alive so the visual tree
    // stays bound to the hwnd; destruction order is automatic via ComPtr.
    ComPtr<IUnknown>           dcompDevice_;
    ComPtr<IUnknown>           dcompTarget_;
    ComPtr<IUnknown>           dcompVisual_;
    std::unordered_map<ColorKey, ComPtr<ID2D1SolidColorBrush>, ColorKeyHash> brushCache_;
    std::unordered_map<TextFormatKey, ComPtr<IDWriteTextFormat>, TextFormatKeyHash> textFormatCache_;
    /* SVG 描边图标共用一个 round-cap/round-join 风格，避免锐角处出尖刺 */
    ComPtr<ID2D1StrokeStyle> roundStrokeStyle_;
    ID2D1StrokeStyle* GetRoundStrokeStyle();

    ComPtr<ID2D1SolidColorBrush> GetBrush(const D2D1_COLOR_F& color);
    ComPtr<IDWriteTextFormat> GetTextFormat(float fontSize, const wchar_t* family = nullptr,
                                            DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL);

    // ---- Per-window font / render 状态 (since 1.3.0) ----
    // 空串 = 跟随 theme:: 全局
    std::wstring          defaultFontOverride_;
    std::wstring          latinFontOverride_;
    std::wstring          cjkFontOverride_;
    bool                  hasRenderModeOverride_ = false;
    theme::TextRenderMode renderModeOverride_ = theme::TextRenderMode::Smooth;

    // 根据当前 latin/cjk 设置构造 IDWriteFontFallback；如果两个都空则返回 nullptr。
    // 构造后由 Apply... 在 TextFormat3 上 SetFontFallback。
    ComPtr<IDWriteFontFallback> fontFallback_;
    void RebuildFontFallback();
};

/* 当前正在 paint 的窗口 Renderer (BeginDraw 进入时设, EndDraw 时清).
   定义在 ui_window.cpp. 跨文件用得到的几个地方都以前是 inline extern, 这里
   集中提一下 (避免在嵌套 namespace 里漏写命名空间限定). */
extern Renderer* g_activeRenderer;

} // namespace ui
