#pragma once
//
// gh_img_view.h — 通用瓦块画布 widget（核心 UI 内建组件）
//
// 设计目标：消费形如 "BGRA8 premultiplied + 256×256 瓦块 + 多级 pyramid" 的
// 像素流，调用方负责喂数据 (set_tile / set_preview)，widget 负责显示与交互。
// 不依赖任何外部库；API 形状对齐 gh-img-decode 的输出（见 CLAUDE.md "数据形
// 状契约" 段），但绝不 include / link 解码器。
//
// 不做：
//   - 文件 IO / 解码（widget 不会自己加载图）
//   - HDR fp16（v1 仅 BGRA8 premul，HDR 留 v2）
//   - 动画多帧（v1 单帧静态）
//   - 文字 / 装饰 / 状态栏（画布纯净）

#include "widget.h"
#include "renderer.h"

#include <d2d1.h>
#include <d2d1_3.h>      /* ID2D1SvgDocument — Build 70+ (L20) SVG 原生路径 */
#include <wrl/client.h>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include <string>

namespace ui {

class UI_API GhImgViewWidget : public Widget {
public:
    GhImgViewWidget();
    ~GhImgViewWidget() override;

    // ---- 数据形状 ----
    struct Info {
        uint32_t fullWidth   = 0;
        uint32_t fullHeight  = 0;
        uint32_t tileSize    = 256;
        uint32_t levels      = 1;       // 1 = 单级；N = pyramid N 级（顶级 = 0）
        uint32_t pixelFormat = 0;       // 0 = BGRA8 premul（v1 仅此一种）
    };

    // 宣告图像数据形状。调用后清空所有已喂瓦块/preview，进入"等数据"状态。
    void Begin(const Info& info, Renderer& r);

    // 全图缩略图兜底（在任何瓦块到位前作背景画面）。stride = pw * 4 时可省。
    void SetPreview(const void* bgra, uint32_t pw, uint32_t ph, uint32_t stride,
                    Renderer& r);

    // 喂某级某瓦块。tx/ty 为该 level 网格坐标；最右一列/最下一行允许 tw < tileSize。
    void SetTile(uint32_t level, uint32_t tx, uint32_t ty,
                 const void* bgra, uint32_t tw, uint32_t th, uint32_t stride,
                 Renderer& r);

    // 清单级瓦块（切级前调，避免新旧 level 串图）
    void ClearLevel(uint32_t level);

    // 清空所有数据（卸载图）
    void Clear();

    // ---- SVG 矢量源 (Build 70+ L20) ----
    // 喂一个 SVG 文件作为渲染源. 进入 "SVG 模式" — 跳过瓦块逻辑, OnDraw 用
    // ID2D1DeviceContext5::DrawSvgDocument + 当前 viewport (zoom/pan/rotation)
    // transform 直接矢量光栅化. info_.fullWidth/Height 由 SVG natural size
    // (优先 viewBox, fallback width/height 属性) 喂入, 所以 Fit/Reset/zoom
    // 等几何全部复用原有路径.
    //
    // 调用方在加载 .svg 文件时调本函数, 替代 Begin + SetTile 系列. 之前
    // 已加载的瓦块状态会被清空 (转换 source type).
    //
    // 失败返 false (文件不存在 / SVG 解析失败 / D2D 渲染器不支持
    // CreateSvgDocument — Win10 1607 前不支持). 失败时不切 mode.
    //
    // 调用方拿到 info_.fullWidth/Height 通过 GetInfo() 或直接读 widget 的
    // C API ui_gh_img_view_get_full_size (如果已暴露).
    bool SetSvgFromFile(const std::wstring& path, Renderer& r);

    // SVG 模式判断 — 当前 source 是 SVG 还是瓦块 pyramid.
    bool IsSvgMode() const { return svgDoc_ != nullptr; }

    // 把当前加载的 SVG 矢量源光栅化到 caller 缓冲, 用于鸟瞰图缩略图等场景.
    // fit 到 target_w×target_h 保 aspect (短边贴边, 长边等比缩), 实际像素尺寸
    // 写回 out_w/out_h, BGRA8 premul 数据写到 out_bgra (大小 = out_w*out_h*4, packed).
    // out_bgra 由 caller 分配, 至少 target_w*target_h*4 字节.
    // 必须先 SetSvgFromFile, 否则返 -1. 其他错误返非 0.
    // 内部使用 ID2D1DeviceContext5 + off-screen target bitmap + CPU_READ bitmap
    // CopyFromBitmap, 不影响外层 BeginDraw 状态 (调用方应在 paint cycle 外调).
    int RenderSvgToBgra(uint32_t target_w, uint32_t target_h,
                         uint8_t* out_bgra,
                         uint32_t* out_w, uint32_t* out_h,
                         Renderer& r);

    // ---- 采样 / 滚轮 ----
    // Antialias: 放大 (screen-per-source >= 1) 时是否平滑. true (默认) = 现有
    // HQ_CUBIC/LINEAR 平滑; false = NEAREST_NEIGHBOR, 像素边界清晰 (像素画 /
    // 逐像素查看). 下采始终走平滑路径不受此影响.
    void     SetAntialias(bool on)        { antialias_ = on; }
    bool     Antialias() const            { return antialias_; }
    // WheelZoomEnabled: 内部 OnMouseWheel 是否缩放. true (默认) = 滚轮缩放;
    // false = 不缩放, 让宿主用 ui_widget_on_mouse_wheel hook 自行接管滚轮
    // (hook 在 OnMouseWheel 分发开头无条件 fire, 不受此开关影响).
    void     SetWheelZoomEnabled(bool on) { wheelZoomEnabled_ = on; }
    bool     WheelZoomEnabled() const     { return wheelZoomEnabled_; }

    // ---- level 选择 ----
    void     SetAutoLevel(bool on)   { autoLevel_ = on; }
    bool     AutoLevel() const       { return autoLevel_; }
    void     SetActiveLevel(uint32_t level);
    uint32_t ActiveLevel() const     { return activeLevel_; }

    // ---- 视口 ----
    float Zoom() const               { return zoom_; }
    void  SetZoom(float z);
    void  SetZoomAround(float z, float anchorX, float anchorY);
    float PanX() const               { return panX_; }
    float PanY() const               { return panY_; }
    void  SetPan(float x, float y);   // L47 follow-up: fire NotifyViewport, 见 .cpp
    void  SetZoomRange(float lo, float hi) { minZoom_ = lo; maxZoom_ = hi; }
    void  Fit();    // 长边贴合视口（rotation-aware: 用 effective W/H）
    void  Reset();  // 1:1 居中

    // ---- 旋转（90° 倍数）----
    // pan 存屏幕空间, 不随 rotation 变 — 鼠标拖动方向永远匹配视觉方向,
    // 跟 image_view_plus 行为一致. zoom 保留, fit 用 rotated 视觉 AABB.
    void  SetRotation(int angle);    // 任意 int → ((a%360)+360)%360, 非 90° 倍数 round-to-90
    int   Rotation() const           { return rotation_; }

    // ---- 视口反馈 ----
    struct Viewport {
        uint32_t activeLevel;
        float    zoom;
        float    panX;
        float    panY;
        // 当前 level 网格下，可见瓦块半开区间 [tx0, tx1) × [ty0, ty1)
        uint32_t visibleTx0, visibleTy0;
        uint32_t visibleTx1, visibleTy1;
    };
    std::function<void(const Viewport&)> onViewportChanged;

    // ---- 数据形状查询 ----
    bool      HasInfo() const        { return info_.fullWidth > 0; }
    const Info& GetInfo() const      { return info_; }

    // ---- Tile evict callback (L48: viewport trim 触发) ----
    // NotifyViewport 内自动 trim viewport 外 tile 时, 对每个被 trim 的 tile
    // fire 一次, caller 同步自己端 "已 push" 跟踪状态 (典型 pushed_tiles_
    // erase), 让下次 viewport callback 能重新 enqueue 该 tile 解码.
    //
    // 设计语义 (L48): 跟用户记忆中的"按区加载内存就小"行为对齐 — viewport
    // 变化时 lib 主动 evict viewport 外的 tile, 内存稳定在
    //   widget 内 tile bytes ≈ viewport 内 tile 数 × 256KB
    // 不随 zoom 历史累积. 代价: zoom out / pan 跨 level 重解码 viewport tile,
    // 单 tile 0.38ms × 4 worker 并发 ≈ 10ms 不可感知.
    //
    // 之前 (build 116 v1-v3 LRU 实现) cap 32MB 在 viewport tile 数 > 128 时
    // (4K 屏 / 大 zoom level) 自相残杀 — evict 自己刚 push 的 tile, 用户
    // 报告 "中间清晰边缘模糊". 改成 viewport 严格管 + caller pushed_tiles_
    // 通过 callback 同步, 该 bug 消除.
    //
    // callback 在 NotifyViewport (UI 线程) 同步 fire. caller 典型实现一行
    // pushed_tiles_.erase(key).
    std::function<void(uint32_t level, uint32_t tx, uint32_t ty)> onTileEvicted;

    // ---- Widget 虚函数 ----
    void OnDraw(Renderer& r) override;
    bool OnMouseDown(const MouseEvent& e) override;
    bool OnMouseMove(const MouseEvent& e) override;
    bool OnMouseUp(const MouseEvent& e) override;
    bool OnMouseWheel(const MouseEvent& e) override;
    D2D1_SIZE_F SizeHint() const override;

private:
    // ---- 状态 ----
    Info     info_{};
    bool     autoLevel_ = true;
    bool     antialias_ = true;        // 放大平滑 (默认); false = NEAREST 像素清晰
    bool     wheelZoomEnabled_ = true; // 内部滚轮缩放 (默认); false 让宿主接管
    uint32_t activeLevel_ = 0;

    float zoom_ = 1.0f;
    float panX_ = 0, panY_ = 0;
    float minZoom_ = 0.01f, maxZoom_ = 64.0f;
    int   rotation_ = 0;       // 0/90/180/270 (CW degrees)
    /* 屏幕物理 px / DIP. 100% 缩放=1.0, 125%=1.25, 150%=1.5. PickAutoLevel
     * 内部需要乘 dpi_scale 才能算到屏幕真实像素密度, 不然 DPI>100% 时按
     * DIP 算阈值会选错 level (永远偏向上采样, fit 大图模糊). Begin 时从
     * Renderer.RT()->GetDpi 读 + cache. */
    float dpi_scale_ = 1.0f;

    bool  dragging_ = false;
    float dragStartX_ = 0, dragStartY_ = 0;
    float dragPanX_ = 0, dragPanY_ = 0;
    /* L48 followup: 上次 NotifyViewport fire 时的 widget rect, OnDraw 检测
     * rect 变化 (typical: window resize 让 parent layout 重 size widget) 时
     * 自动 fire NotifyViewport, 让 caller push 新 viewport tile. 不依赖
     * caller 在 resize handler 里手动 invalidate viewport. */
    D2D1_RECT_F lastNotifiedRect_{};

    // ---- 瓦块存储（按 level 分桶）----
    struct TileKey {
        uint32_t level;
        uint32_t tx;
        uint32_t ty;
        bool operator==(const TileKey& o) const noexcept {
            return level == o.level && tx == o.tx && ty == o.ty;
        }
    };
    struct TileKeyHash {
        size_t operator()(const TileKey& k) const noexcept {
            // 简单合并 hash
            uint64_t h = (uint64_t)k.level;
            h = (h * 0x9E3779B97F4A7C15ULL) ^ (uint64_t)k.tx;
            h = (h * 0x9E3779B97F4A7C15ULL) ^ (uint64_t)k.ty;
            return (size_t)h;
        }
    };
    struct Tile {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bmp;
        uint32_t w = 0, h = 0;
    };
    std::unordered_map<TileKey, Tile, TileKeyHash> tiles_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> preview_;
    uint32_t previewW_ = 0, previewH_ = 0;

    // L48: NotifyViewport 内调, 清掉 tiles_ 中不在 viewport 范围内的 tile
    // + 非 active level 的 tile. 每个被清的 tile fire 一次 onTileEvicted,
    // caller 同步自己端 pushed_tiles_ erase. 内存稳定 = viewport tile × 256KB,
    // 不随 zoom 历史累积.
    void TrimToViewport_(uint32_t active_level,
                          uint32_t visible_tx0, uint32_t visible_tx1,
                          uint32_t visible_ty0, uint32_t visible_ty1);

    /* Build 70+ (L20): SVG 矢量源. 非空时进入 SVG 模式, OnDraw 走
     * DrawSvgDocument 不再画瓦块. svgW_/svgH_ 是 SVG natural size, 跟
     * info_.fullWidth/Height 同步. */
    Microsoft::WRL::ComPtr<ID2D1SvgDocument> svgDoc_;
    uint32_t svgW_ = 0;
    uint32_t svgH_ = 0;
    /* L75: D2D ID2D1SvgDocument 不渲染 <text>/<foreignObject> 文字 (shapes-only) →
     * core-ui 自己解析出来, OnDraw 里 DrawSvgDocument 之后 DirectWrite 叠加补渲. */
    std::vector<SvgTextRun> svgTextRuns_;

    // ---- 渲染辅助 ----
    void     DrawLevel(Renderer& r, uint32_t level, const D2D1_RECT_F& dest);

    // ---- 几何辅助 ----
    // 当前 level 下的图像总尺寸（顶级 = info.fullW/H；每降一级 /2）
    uint32_t LevelWidth(uint32_t level) const;
    uint32_t LevelHeight(uint32_t level) const;
    // 当前 level 下，1 像素 == 屏幕多少像素（pic→screen 的比例因子）
    float    LevelToScreenScale(uint32_t level) const;
    // Logical dest rect: 假设 rotation=0 时, 未旋转 bitmap 在屏幕上的 AABB.
    // 中心 = widget center + pan, 大小 = fullW*zoom × fullH*zoom.
    // OnDraw 在此 dest 内用 logical 坐标 DrawBitmap, D2D rotation transform
    // 完成视觉旋转. 对外 (fit/hit-test) 用 ComputeVisualDestRect.
    D2D1_RECT_F ComputeDestRect() const;
    // Visual dest rect: rotation 应用后, 屏幕上图像可见区域的 AABB.
    // 中心同 logical, 大小 = effW*zoom × effH*zoom (90/270 时 W/H 互换).
    // 用于 fit (zoom 计算用 effW/effH) / 早期剔除 / 命中测试.
    D2D1_RECT_F ComputeVisualDestRect() const;
    // 把 widget 像素坐标转成顶级图像像素坐标 (rotation-aware).
    void     ScreenToImage(float sx, float sy, float& ix, float& iy) const;
    // 顶级图像坐标 → widget 像素 (rotation-aware).
    void     ImageToScreen(float ix, float iy, float& sx, float& sy) const;
    // 旋转后图像有效宽高 (90/270 互换). fullW/H 为 0 时返 0.
    uint32_t EffectiveImageWidth() const;
    uint32_t EffectiveImageHeight() const;
    // 根据当前 zoom + level metric 决定 auto level
    uint32_t PickAutoLevel() const;
    // 切 level（不动 zoom，等同于换数据来源；外部应清旧 level 瓦块）
    void     SwitchLevel(uint32_t level);
    void     ConstrainPan();
    void     NotifyViewport();
    void     InvalidateAllWindows();
};

} // namespace ui
