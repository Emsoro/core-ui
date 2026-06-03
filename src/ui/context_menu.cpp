#include "context_menu.h"
#include "ui_context.h"
#include <algorithm>
#include <windowsx.h>
#include <dwmapi.h>

/* Windows 10 1607+ 提供 GetDpiForWindow。本项目 CMakeLists 定义 _WIN32_WINNT=0x0A00
 * 后系统头会声明它，此处 fallback 只在老 SDK 上启用。*/
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0A00
static inline UINT GetDpiForWindow(HWND hwnd) {
    HMODULE hModule = LoadLibraryW(L"user32.dll");
    if (hModule) {
        typedef UINT(WINAPI* PFN_GetDpiForWindow)(HWND);
        PFN_GetDpiForWindow pfn = (PFN_GetDpiForWindow)GetProcAddress(hModule, "GetDpiForWindow");
        if (pfn) {
            UINT dpi = pfn(hwnd);
            FreeLibrary(hModule);
            return dpi;
        }
        FreeLibrary(hModule);
    }
    HDC hdc = GetDC(hwnd);
    UINT dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(hwnd, hdc);
    return dpi ? dpi : 96;
}
#endif

namespace ui {

bool ContextMenu::popupClassRegistered_ = false;
bool ContextMenu::g_debugSuppressAutoClose = false;

// ---- Build (build 75 BREAKING: widget-tree based items) ----

void ContextMenu::AddItemContent(int id, const std::wstring& shortcut,
                                  WidgetPtr content) {
    MenuItem item;
    item.id = id;
    item.shortcut = shortcut;
    item.customContent = std::move(content);
    items_.push_back(std::move(item));
}

void ContextMenu::SetLastItemMeta(std::string strId,
                                   std::vector<std::pair<std::string,std::string>> attrs) {
    if (items_.empty()) return;
    items_.back().strId = std::move(strId);
    items_.back().attrs = std::move(attrs);
}

void ContextMenu::AddSeparator() {
    MenuItem item;
    item.isSeparator = true;
    items_.push_back(std::move(item));
}

void ContextMenu::AddSubmenu(WidgetPtr entryContent, ContextMenuPtr submenu) {
    MenuItem item;
    item.id = -1;
    item.customContent = std::move(entryContent);
    item.submenu = std::move(submenu);
    items_.push_back(std::move(item));
}

void ContextMenu::SetEnabled(int id, bool enabled) {
    for (auto& item : items_) {
        if (item.id == id) item.enabled = enabled;
    }
}

// ---- Reactive rebuild (build 73 / L17) ----

ContextMenu* ContextMenu::OpenSubmenuAt(int index) {
    /* Build 85: mimic HandleMouseMove submenu-open path. 调试 / 测试用,
     * 没真鼠标 hover 时也能打开 submenu (拍截图验证). */
    if (index < 0 || index >= (int)items_.size()) return nullptr;
    auto& item = items_[index];
    if (item.isSeparator || !item.submenu) return nullptr;

    // Close previously open submenu (跟实际 hover 逻辑一致)
    if (openSubmenuIndex_ >= 0 && openSubmenuIndex_ < (int)items_.size()) {
        auto& prevSub = items_[openSubmenuIndex_].submenu;
        if (prevSub) prevSub->Close();
    }
    openSubmenuIndex_ = index;
    auto& sub = item.submenu;
    sub->parentMenu_ = this;
    if (popupHwnd_) {
        RECT rc;
        GetWindowRect(popupHwnd_, &rc);
        D2D1_RECT_F ir = ItemRect(index);
        UINT subDpi = GetDpiForWindow(popupHwnd_);
        float subScale = (float)subDpi / 96.0f;
        int marginPx = (int)(kShadowMargin * subScale);
        /* Build 86+: 减 kSubmenuOverlap 让 submenu 往左挪, 跟 parent 右边重叠. */
        int overlapPx = (int)(kSubmenuOverlap * subScale);
        int subX = rc.right - marginPx - overlapPx;
        int subY = rc.top + (int)(ir.top * subScale);
        sub->ShowPopup(parentHwnd_ ? parentHwnd_ : popupHwnd_, subX, subY);
    } else {
        D2D1_RECT_F ir = ItemRect(index);
        D2D1_RECT_F vp = Bounds();
        vp.right += 400; vp.bottom += 400;
        sub->Show(ir.right - 4, ir.top, vp);
    }
    return sub.get();
}

void ContextMenu::Clear() {
    /* 反应式 rebuild 入口: 把 items_ 全清掉, 让 PopulateMenu 重新喂.
     * submenu shared_ptr 在这一刻 release; 如果用户正打开 submenu, 那条
     * pointer 会变 dangling — 但 reactive 重 build 只发生在 Show 入口,
     * 那时 Close 已经把 openSubmenuIndex_ 重置. 安全. */
    items_.clear();
    hoveredIndex_ = -1;
    openSubmenuIndex_ = -1;
    clickedId_ = -1;
}

// ---- Show / Hide ----

void ContextMenu::Show(float x, float y, const D2D1_RECT_F& viewport) {
    /* Build 73 (L17): reactive .uix 菜单先 rebuild items 再算宽高. 老的
     * imperative 路径没设 hook, items 早就建好, no-op. */
    if (beforeShowHook_) beforeShowHook_();

    float w = MenuWidth();
    float h = MenuHeight();
    if (x + w > viewport.right) x = viewport.right - w;
    if (y + h > viewport.bottom) y = viewport.bottom - h;
    if (x < viewport.left) x = viewport.left;
    if (y < viewport.top) y = viewport.top;
    x_ = x; y_ = y;
    visible_ = true;
    hoveredIndex_ = -1;
    openSubmenuIndex_ = -1;
    clickedId_ = -1;
}

void ContextMenu::ShowPopup(HWND parentHwnd, int screenX, int screenY) {
    /* 同 Show: 先 reactive rebuild, 再走原 popup 路径 (clamp/sizing 才能拿
     * 到最新 items 的宽高). */
    if (beforeShowHook_) beforeShowHook_();

    parentHwnd_ = parentHwnd;

    // Get DPI scale from parent window
    UINT dpi = GetDpiForWindow(parentHwnd);
    float dpiScale = (float)dpi / 96.0f;

    int pw = (int)(MenuWidth() * dpiScale);
    int ph = (int)(MenuHeight() * dpiScale);

    // Clamp to screen — based on visual menu rect (not the shadow-padded hwnd).
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (screenX + pw > sw) screenX = sw - pw;
    if (screenY + ph > sh) screenY = sh - ph;
    if (screenX < 0) screenX = 0;
    if (screenY < 0) screenY = 0;

    // Menu content is drawn at (kShadowMargin, kShadowMargin) inside the
    // hwnd; the surrounding margin holds the blurred drop shadow.
    x_ = kShadowMargin;
    y_ = kShadowMargin;
    visible_ = true;
    hoveredIndex_ = -1;
    openSubmenuIndex_ = -1;
    clickedId_ = -1;

    // Position the hwnd offset by -kShadowMargin so the visible card lands
    // at the requested screen coordinate.
    int marginPx = (int)(kShadowMargin * dpiScale);
    CreatePopupWindow(parentHwnd, screenX - marginPx, screenY - marginPx);
}

void ContextMenu::Close() {
    visible_ = false;
    hoveredIndex_ = -1;
    if (openSubmenuIndex_ >= 0 && openSubmenuIndex_ < (int)items_.size()) {
        auto& sub = items_[openSubmenuIndex_].submenu;
        if (sub) sub->Close();
    }
    openSubmenuIndex_ = -1;
    DestroyPopupWindow();
    /* Build 88+: debug flag 自动恢复 — `ui_debug_set_menu_autoclose(0)` 是
     * 调试用一次性 helper, 不该跨菜单 session 持续生效. 否则脚本测一次后
     * GuoheView 用户右键所有菜单都不会自动关, 调试痕迹污染业务. 每次菜单
     * 关闭重置, caller 想下一个菜单也抑制就再调一次. */
    g_debugSuppressAutoClose = false;
}

// ---- Debug / simulation accessors ----

int ContextMenu::ItemIdAt(int index) const {
    if (index < 0 || index >= (int)items_.size()) return -1;
    if (items_[index].isSeparator) return -1;
    return items_[index].id;
}

bool ContextMenu::ItemEnabled(int index) const {
    if (index < 0 || index >= (int)items_.size()) return false;
    return items_[index].enabled && !items_[index].isSeparator;
}

bool ContextMenu::ItemIsSeparator(int index) const {
    if (index < 0 || index >= (int)items_.size()) return false;
    return items_[index].isSeparator;
}

int ContextMenu::FindIndexById(int id) const {
    for (int i = 0; i < (int)items_.size(); i++) {
        if (!items_[i].isSeparator && items_[i].id == id) return i;
    }
    return -1;
}

bool ContextMenu::SimulateClickIndex(int index) {
    if (index < 0 || index >= (int)items_.size()) return false;
    const MenuItem& it = items_[index];
    if (it.isSeparator || !it.enabled) return false;
    clickedId_ = it.id;
    clickedStrId_ = it.strId;
    clickedAttrs_ = it.attrs;
    // 复刻 PopupWndProc 里 WM_LBUTTONUP 的分派路径：把 item id + 属性载荷回传父窗口
    if (parentHwnd_) {
        PostMessageW(parentHwnd_, WM_APP + 100, (WPARAM)it.id,
                     (LPARAM)new MenuClickInfo{it.strId, it.attrs});
    }
    Close();
    return true;
}

ContextMenuPtr ContextMenu::SubmenuAt(int index) const {
    if (index < 0 || index >= (int)items_.size()) return nullptr;
    return items_[index].submenu;
}

// 沿 path 前 depth-1 层走到内层菜单；若路径在中途断裂返回 nullptr。
// 最后一层不要求是 submenu —— 调用方根据需要自行处理叶子。
static const ContextMenu* WalkPath(const ContextMenu* root, const int* path, int depth) {
    if (depth < 0 || (!path && depth > 0)) return nullptr;
    const ContextMenu* cur = root;
    for (int i = 0; i < depth - 1; i++) {
        if (!cur) return nullptr;
        int idx = path[i];
        auto sub = cur->SubmenuAt(idx);
        if (!sub) return nullptr;
        if (!cur->ItemEnabled(idx)) return nullptr;
        cur = sub.get();
    }
    return cur;
}

int ContextMenu::ItemCountAtPath(const int* path, int depth) const {
    const ContextMenu* m = WalkPath(this, path, depth + 1);
    // 当 depth==0，要返回自身 count；WalkPath(root, path, 1) 走 0 圈后返回自身。OK。
    return m ? m->ItemCount() : -1;
}

int ContextMenu::ItemIdAtPath(const int* path, int depth) const {
    if (depth < 1) return -1;
    const ContextMenu* m = WalkPath(this, path, depth);
    if (!m) return -1;
    return m->ItemIdAt(path[depth - 1]);
}

bool ContextMenu::HasSubmenuAtPath(const int* path, int depth) const {
    if (depth < 1) return false;
    const ContextMenu* m = WalkPath(this, path, depth);
    if (!m) return false;
    return m->SubmenuAt(path[depth - 1]) != nullptr;
}

bool ContextMenu::SimulateClickPath(const int* path, int depth) {
    if (depth < 1 || !path) return false;
    // 先一路把 path 走到叶子所在的菜单层。
    ContextMenu* cur = this;
    for (int i = 0; i < depth - 1; i++) {
        int idx = path[i];
        if (idx < 0 || idx >= (int)cur->items_.size()) return false;
        const MenuItem& it = cur->items_[idx];
        if (it.isSeparator || !it.enabled || !it.submenu) return false;
        cur = it.submenu.get();
    }
    int leafIdx = path[depth - 1];
    if (leafIdx < 0 || leafIdx >= (int)cur->items_.size()) return false;
    const MenuItem& leaf = cur->items_[leafIdx];
    if (leaf.isSeparator || !leaf.enabled) return false;
    cur->clickedId_ = leaf.id;
    cur->clickedStrId_ = leaf.strId;
    cur->clickedAttrs_ = leaf.attrs;
    // 回传给 ROOT 菜单的 parentHwnd（主窗口）。
    if (parentHwnd_) {
        PostMessageW(parentHwnd_, WM_APP + 100, (WPARAM)leaf.id,
                     (LPARAM)new MenuClickInfo{leaf.strId, leaf.attrs});
    }
    Close();
    return true;
}

// ---- Popup Window ----

void ContextMenu::CreatePopupWindow(HWND parent, int screenX, int screenY) {
    if (!popupClassRegistered_) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = PopupWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
        wc.lpszClassName = L"UiCore_MenuPopup";
        RegisterClassExW(&wc);
        popupClassRegistered_ = true;
    }

    UINT dpi = parent ? GetDpiForWindow(parent) : 96;
    float dpiScale = (float)dpi / 96.0f;
    // Hwnd encompasses both the menu card and the surrounding shadow halo.
    int w = (int)((MenuWidth()  + 2 * kShadowMargin) * dpiScale);
    int h = (int)((MenuHeight() + 2 * kShadowMargin) * dpiScale);

    // Plain WS_POPUP with no NC frame, no thickframe — DirectComposition
    // owns the visual presentation, the hwnd is just a hit-test surface.
    popupHwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        L"UiCore_MenuPopup", L"",
        WS_POPUP,
        screenX, screenY, w, h,
        parent, nullptr, GetModuleHandleW(nullptr), this);

    if (!popupHwnd_) return;

    // Init renderer with shared factories — composition mode (transparent
    // swap chain + DComp) so anti-aliased round corners just work via D2D.
    auto& ctx = GetContext();
    popupRenderer_.Init(ctx.D2DFactory(), ctx.DWFactory(), ctx.WICFactory());
    popupRenderer_.CreateRenderTargetForLayered(popupHwnd_);
    // Force GRAYSCALE text antialias on the popup regardless of the global
    // theme mode. ClearType assumes an opaque background and produces
    // red/blue subpixel fringes when composited onto an alpha-aware surface;
    // Smooth (= GRAYSCALE) is the only safe choice for transparent popups.
    popupRenderer_.SetTextRenderMode(theme::TextRenderMode::Smooth);

    // Paint BEFORE showing to avoid white flash
    PaintPopup();
    ShowWindow(popupHwnd_, SW_SHOWNOACTIVATE);
    SetTimer(popupHwnd_, 1, 50, nullptr);
}

void ContextMenu::DestroyPopupWindow() {
    if (popupHwnd_) {
        DestroyWindow(popupHwnd_);
        popupHwnd_ = nullptr;
    }
}

void ContextMenu::PaintPopup() {
    if (!popupHwnd_ || !popupRenderer_.RT()) return;
    popupRenderer_.BeginDraw();
    // Transparent surface — Draw() then paints the rounded card on top.
    // Anything outside the rounded shape stays alpha=0 and the desktop
    // composer punches through cleanly (no aliased GDI region clip).
    popupRenderer_.Clear({0, 0, 0, 0});
    Draw(popupRenderer_);
    popupRenderer_.EndDraw();
}

LRESULT CALLBACK ContextMenu::PopupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ContextMenu* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<ContextMenu*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<ContextMenu*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_PAINT: {
        self->PaintPopup();
        ValidateRect(hwnd, nullptr);
        return 0;
    }
    case WM_MOUSEMOVE: {
        // Convert physical pixels to DIPs
        UINT dpi = GetDpiForWindow(hwnd);
        float scale = (float)dpi / 96.0f;
        float x = (float)GET_X_LPARAM(lParam) / scale;
        float y = (float)GET_Y_LPARAM(lParam) / scale;
        self->HandleMouseMove(x, y);
        InvalidateRect(hwnd, nullptr, FALSE);

        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        self->hoveredIndex_ = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        UINT dpi = GetDpiForWindow(hwnd);
        float scale = (float)dpi / 96.0f;
        float x = (float)GET_X_LPARAM(lParam) / scale;
        float y = (float)GET_Y_LPARAM(lParam) / scale;
        self->HandleMouseDown(x, y);
        return 0;
    }
    case WM_LBUTTONUP: {
        UINT dpi = GetDpiForWindow(hwnd);
        float scale = (float)dpi / 96.0f;
        float x = (float)GET_X_LPARAM(lParam) / scale;
        float y = (float)GET_Y_LPARAM(lParam) / scale;
        if (self->HandleMouseUp(x, y)) {
            int clickedId = self->ClickedItemId();
            // Post message to parent so it can handle the callback (id + 属性载荷)
            if (clickedId >= 0 && self->parentHwnd_) {
                PostMessage(self->parentHwnd_, WM_APP + 100, (WPARAM)clickedId,
                            (LPARAM)new MenuClickInfo{self->clickedStrId_, self->clickedAttrs_});
            }
            self->Close();
            // 如果本身是子菜单 popup（parentMenu_ 非空），沿链向上关闭父菜单，
            // 否则 leaf 点完后 root popup 还留在屏上。
            ContextMenu* p = self->parentMenu_;
            while (p) { p->Close(); p = p->parentMenu_; }
        }
        return 0;
    }
    case WM_NCCALCSIZE:
        if (wParam) {
            // Remove all non-client area (WS_THICKFRAME border) — keep only DWM shadow
            return 0;
        }
        break;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_SIZE:
        if (self->popupRenderer_.RT()) {
            self->popupRenderer_.Resize(LOWORD(lParam), HIWORD(lParam));
        }
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_ACTIVATEAPP:
        /* Build 86+: app 失去激活立即关菜单, 不等 WM_TIMER 50ms 轮询.
         * wParam=FALSE 表示 app 进入非激活态 (用户切到其它软件). 这条
         * 跟 timer Check 2 互补 — timer 有 50ms 延迟, 这里立即响应. */
        if (!wParam && !g_debugSuppressAutoClose) {
            KillTimer(hwnd, 1);
            self->Close();
            return 0;
        }
        break;
    case WM_TIMER:
        // Poll: if mouse is pressed outside menu, close it
        if (wParam == 1) {
            if (g_debugSuppressAutoClose) return 0;  // 测试模式：不做自动关闭检查
            if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
                POINT pt;
                GetCursorPos(&pt);
                /* Build 86+: 用 "可见菜单矩形" 而不是 popup hwnd 整框 ——
                 * hwnd 比可见 menu 大 kShadowMargin (18 DIP / each side) 一圈,
                 * 用整框做命中导致 click 在阴影带 (visual 外) 不关菜单. 算
                 * 可见 rect: hwnd.topLeft + kShadowMargin × dpiScale. */
                auto visibleRect = [](HWND h) -> RECT {
                    RECT rc; GetWindowRect(h, &rc);
                    UINT dpi = GetDpiForWindow(h);
                    float scale = (float)dpi / 96.0f;
                    int marginPx = (int)(kShadowMargin * scale);
                    return RECT{rc.left + marginPx, rc.top + marginPx,
                                 rc.right - marginPx, rc.bottom - marginPx};
                };
                RECT rc = visibleRect(hwnd);
                if (pt.x < rc.left || pt.x >= rc.right || pt.y < rc.top || pt.y >= rc.bottom) {
                    bool inSubmenu = false;
                    if (self->openSubmenuIndex_ >= 0 && self->openSubmenuIndex_ < (int)self->items_.size()) {
                        auto& sub = self->items_[self->openSubmenuIndex_].submenu;
                        if (sub && sub->popupHwnd_) {
                            RECT src = visibleRect(sub->popupHwnd_);
                            if (pt.x >= src.left && pt.x < src.right && pt.y >= src.top && pt.y < src.bottom)
                                inSubmenu = true;
                        }
                    }
                    bool inAncestor = false;
                    for (ContextMenu* p = self->parentMenu_; p && !inAncestor; p = p->parentMenu_) {
                        if (!p->popupHwnd_) continue;
                        RECT pc = visibleRect(p->popupHwnd_);
                        if (pt.x >= pc.left && pt.x < pc.right && pt.y >= pc.top && pt.y < pc.bottom)
                            inAncestor = true;
                    }
                    if (!inSubmenu && !inAncestor) {
                        KillTimer(hwnd, 1);
                        self->Close();
                        return 0;
                    }
                }
            }
            // Also check if another window got focus
            HWND fg = GetForegroundWindow();
            if (fg && fg != hwnd && fg != self->parentHwnd_) {
                // Check it's not a submenu
                bool isSubmenu = false;
                if (self->openSubmenuIndex_ >= 0 && self->openSubmenuIndex_ < (int)self->items_.size()) {
                    auto& sub = self->items_[self->openSubmenuIndex_].submenu;
                    if (sub && sub->popupHwnd_ == fg) isSubmenu = true;
                }
                // 或是任一祖先菜单 popup —— 用户点击父菜单时不应把子菜单关掉
                bool isAncestor = false;
                for (ContextMenu* p = self->parentMenu_; p && !isAncestor; p = p->parentMenu_) {
                    if (p->popupHwnd_ == fg) isAncestor = true;
                }
                if (!isSubmenu && !isAncestor) {
                    KillTimer(hwnd, 1);
                    self->Close();
                    return 0;
                }
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---- Geometry ----

bool ContextMenu::HasAnyIcon() const {
    /* BREAKING (build 75): MenuItem 没有 hasIcon 字段了, icon 是 customContent
     * widget tree 的一部分. 始终预留 icon 列宽度 — 不预留会让用户写
     * <menuitem><svg/><label/></menuitem> 时 svg 紧贴左边. */
    return !items_.empty();
}

float ContextMenu::MenuWidth() const {
    /* BREAKING (build 75): 老的 text/shortcut 文字宽度估算改成: 用 customContent
     * widget tree 的 SizeHint() 取 max. customContent 还没 DoLayout 过, SizeHint
     * 在大多数 widget 上返 (fixedW, fixedH) 或 (0,0), 估算可能不准. 兜底走
     * kMinWidth = 200. 后续如要更准, 应在 PopulateMenuItem 末尾对每个 customContent
     * 做一遍 measure pass 写 fixedW. */
    float maxContent  = 0;
    float maxShortcut = 0;
    bool  hasSubmenu  = false;
    for (auto& item : items_) {
        if (item.isSeparator) continue;
        if (item.customContent) {
            auto h = item.customContent->SizeHint();
            float w = h.width;
            /* HBox/VBox SizeHint 不自己 clamp 到 maxW (那是 Layout 期的事),
             * MenuWidth 这里手动 cap — 让 .uix CSS 写 `.menuitem-row {
             * max-width: 240px }` 真正约束菜单宽度. minW 同样 honor. */
            if (item.customContent->maxW > 0 && w > item.customContent->maxW) {
                w = item.customContent->maxW;
            }
            if (item.customContent->minW > 0 && w < item.customContent->minW) {
                w = item.customContent->minW;
            }
            if (w > maxContent) maxContent = w;
        }
        float sw = item.shortcut.length() * 6.5f;
        if (sw > maxShortcut) maxShortcut = sw;
        if (item.submenu) hasSubmenu = true;
    }
    /* Build 77+/80+/81+: 反应式菜单 v-if 在不同 state 隐不同 items, 让
     * visible items 的 max content / shortcut / hasSubmenu 都跟着变 → 两
     * 态自然宽度差. PageState 计算 "全 items (含 v-if=false 的)" 喂三个
     * reserved 字段, 这里 floor 住保证两态宽度完全一致. */
    if (reservedShortcutWidth_ > maxShortcut) maxShortcut = reservedShortcutWidth_;
    if (reservedHasSubmenu_) hasSubmenu = true;
    if (reservedContentWidth_ > maxContent) maxContent = reservedContentWidth_;
    /* shortcut 跟 content 之间的呼吸: 16 DIP. */
    float shortcutCol = maxShortcut > 0 ? maxShortcut + 16.0f : 0;
    float arrowCol    = hasSubmenu ? kSubmenuArrowWidth : 0;
    float w = maxContent + shortcutCol + arrowCol + kPadding * 2;
    /* Build 85+: 跨菜单树共享宽度 — submenu 撑到 parent 的 MenuWidth, 整族
     * 菜单视觉一致. minPropagatedWidth_ 由 PageState 在 PopulateMenu 末尾
     * 算整树最大 MenuWidth 后回写到每个 ContextMenu. */
    if (minPropagatedWidth_ > w) w = minPropagatedWidth_;
    return std::max(w, kMinWidth);
}

float ContextMenu::MenuHeight() const {
    float h = kPadding * 2;
    for (auto& item : items_) {
        h += item.isSeparator ? kSepHeight : kItemHeight;
    }
    return h;
}

D2D1_RECT_F ContextMenu::Bounds() const {
    return {x_, y_, x_ + MenuWidth(), y_ + MenuHeight()};
}

bool ContextMenu::Contains(float x, float y) const {
    auto b = Bounds();
    return x >= b.left && x < b.right && y >= b.top && y < b.bottom;
}

D2D1_RECT_F ContextMenu::ItemRect(int index) const {
    float y = y_ + kPadding;
    for (int i = 0; i < index && i < (int)items_.size(); i++) {
        y += items_[i].isSeparator ? kSepHeight : kItemHeight;
    }
    float h = items_[index].isSeparator ? kSepHeight : kItemHeight;
    return {x_, y, x_ + MenuWidth(), y + h};
}

int ContextMenu::HitTest(float x, float y) const {
    if (!Contains(x, y)) return -1;
    float iy = y_ + kPadding;
    for (int i = 0; i < (int)items_.size(); i++) {
        float h = items_[i].isSeparator ? kSepHeight : kItemHeight;
        if (y >= iy && y < iy + h) return i;
        iy += h;
    }
    return -1;
}

// ---- Draw ----

void ContextMenu::Draw(Renderer& r) {
    if (!visible_) return;

    float w = MenuWidth();
    float h = MenuHeight();

    // Soft drop shadow — concentric semi-transparent rounded rects expand
    // outward in 1 px steps. Each ring is a slightly larger rounded rect
    // with quadratically-falloff alpha. Cheaper than a Gaussian-blur D2D
    // effect (no offscreen bitmap) and the curve is smooth enough for the
    // 16-18 px halo we have around popup mode.
    constexpr int   kShadowSteps      = 12;
    constexpr float kShadowVerticalY  = 2.0f;
    // Per-ring peak alpha. With 12 stacked rings the inner edge accumulates
    // alpha quickly; 0.012 keeps the halo a whisper-faint depth cue.
    constexpr float kShadowPeakAlpha  = 0.012f;
    for (int i = kShadowSteps; i >= 1; --i) {
        float spread = (float)i;
        float t = (float)i / (float)kShadowSteps;
        float alpha = kShadowPeakAlpha * (1.0f - t) * (1.0f - t);
        D2D1_COLOR_F sc{0, 0, 0, alpha};
        D2D1_RECT_F sr{
            x_ - spread,
            y_ - spread + kShadowVerticalY,
            x_ + w + spread,
            y_ + h + spread + kShadowVerticalY,
        };
        float rr = CornerRadius() + spread;
        r.FillRoundedRect(sr, rr, rr, sc);
    }

    // Single-fill rounded card. No outer stroke — on a transparent DComp
    // surface a 0.5px stroke crosses the alpha-blended corner band and
    // shows up as a darker rim against contrast-y desktops.
    D2D1_RECT_F bgRect = {x_, y_, x_ + w, y_ + h};
    // Card surface — pure white in Light mode, an elevated grey in Dark
    // mode (slightly above page bg so the popup reads as "above" the
    // canvas). hasBgColor_ override still wins for explicit tints.
    D2D1_COLOR_F cardBg;
    if (hasBgColor_) {
        cardBg = bgColor_;
    } else if (theme::CurrentMode() == theme::Mode::Dark) {
        cardBg = theme::Rgb(0x2C, 0x2C, 0x2C);   // a touch above background1 (#1F1F1F)
    } else {
        cardBg = D2D1_COLOR_F{1.0f, 1.0f, 1.0f, 1.0f};
    }
    {
        const float cr = CornerRadius();
        r.FillRoundedRect(bgRect, cr, cr, cardBg);
    }

    // BREAKING (build 75): items 走 widget-tree 渲染. 每个 menuitem 的
    // customContent (WidgetPtr) 在 item 的 content-rect 内 DoLayout + DrawTree.
    // ContextMenu 自己只画: separator / hover 高亮 / shortcut 文字 (右对齐) /
    // submenu arrow. disabled 通过临时 set opacity dim.
    float iy = y_ + kPadding;
    for (int i = 0; i < (int)items_.size(); i++) {
        auto& item = items_[i];

        if (item.isSeparator) {
            float sepY = iy + kSepHeight / 2.0f;
            r.DrawLine(x_ + kPadding, sepY, x_ + w - kPadding, sepY,
                       theme::kDividerSubtle());
            iy += kSepHeight;
            continue;
        }

        if (i == hoveredIndex_ && item.enabled) {
            D2D1_RECT_F hlRect = {x_ + kPadding, iy,
                                   x_ + w - kPadding, iy + kItemHeight};
            D2D1_COLOR_F hlColor = (theme::CurrentMode() == theme::Mode::Dark)
                ? D2D1_COLOR_F{1.0f, 1.0f, 1.0f, 0.10f}
                : D2D1_COLOR_F{0.0f, 0.0f, 0.0f, 0.06f};
            r.FillRoundedRect(hlRect, 6.0f, 6.0f, hlColor);
        }

        if (item.customContent) {
            float contentLeft  = x_ + kPadding;
            float contentRight = x_ + w - kPadding;
            if (!item.shortcut.empty()) contentRight -= 100.0f;
            if (item.submenu) contentRight -= kSubmenuArrowWidth;
            item.customContent->rect = {contentLeft, iy, contentRight, iy + kItemHeight};
            float savedOpacity = item.customContent->opacity;
            if (!item.enabled) item.customContent->opacity = 0.4f;
            item.customContent->DoLayout();
            item.customContent->DrawTree(r);
            item.customContent->opacity = savedOpacity;
        }

        if (!item.shortcut.empty()) {
            D2D1_COLOR_F base = theme::kForeground3();
            D2D1_COLOR_F shortcutColor = {base.r, base.g, base.b, item.enabled ? 1.0f : 0.4f};
            D2D1_RECT_F scRect = {x_ + w - kPadding - 100, iy,
                                   x_ + w - 14, iy + kItemHeight};
            r.DrawText(item.shortcut, scRect, shortcutColor, kShortcutFont,
                       DWRITE_TEXT_ALIGNMENT_TRAILING);
        }

        if (item.submenu) {
            D2D1_COLOR_F arrowColor = item.enabled
                ? theme::kBtnText() : D2D1_COLOR_F{0.5f, 0.5f, 0.5f, 0.6f};
            D2D1_RECT_F arrowRect = {x_ + w - kPadding - 16, iy,
                                      x_ + w - kPadding - 2, iy + kItemHeight};
            /* Build 86: › "›" (single right-pointing angle quotation) —
             * 跟 macOS / Win11 submenu arrow 同款 outline 风格. 比 ▸ ▸
             * (黑色实心三角) 看着更克制. */
            r.DrawText(L"›", arrowRect, arrowColor, theme::kFontSizeBody,
                       DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        iy += kItemHeight;
    }

    // Draw open submenu (only in overlay mode, popup submenus have their own window)
    if (!popupHwnd_ && openSubmenuIndex_ >= 0 && openSubmenuIndex_ < (int)items_.size()) {
        auto& sub = items_[openSubmenuIndex_].submenu;
        if (sub && sub->IsVisible()) {
            sub->Draw(r);
        }
    }
}

// ---- Event Handling ----

bool ContextMenu::HandleMouseMove(float x, float y) {
    if (!visible_) return false;

    // Check submenu first
    if (openSubmenuIndex_ >= 0 && openSubmenuIndex_ < (int)items_.size()) {
        auto& sub = items_[openSubmenuIndex_].submenu;
        if (sub && sub->IsVisible()) {
            if (sub->popupHwnd_) {
                // Submenu has its own window — it handles its own events
            } else if (sub->Contains(x, y)) {
                sub->HandleMouseMove(x, y);
                return true;
            }
        }
    }

    int hit = HitTest(x, y);
    hoveredIndex_ = hit;

    // Open/close submenus on hover
    if (hit >= 0 && hit < (int)items_.size() && !items_[hit].isSeparator) {
        if (items_[hit].submenu && items_[hit].enabled) {
            if (openSubmenuIndex_ != hit) {
                // Close previous submenu
                if (openSubmenuIndex_ >= 0 && openSubmenuIndex_ < (int)items_.size()) {
                    auto& prevSub = items_[openSubmenuIndex_].submenu;
                    if (prevSub) prevSub->Close();
                }
                // Open new submenu
                openSubmenuIndex_ = hit;
                auto& sub = items_[hit].submenu;
                sub->parentMenu_ = this;  // leaf 点击后可沿链 Close
                if (popupHwnd_) {
                    // Open submenu as popup window too
                    RECT rc;
                    GetWindowRect(popupHwnd_, &rc);
                    D2D1_RECT_F ir = ItemRect(hit);
                    // rc 是 hwnd 外圈, 含 kShadowMargin 的 drop-shadow padding.
                    // 减掉 marginPx 拿"可见卡片"的右边作 submenu anchor — 否则
                    // submenu 跟父菜单之间留 ~18px (高 DPI 更大) 透明空白 (L17).
                    UINT subDpi = GetDpiForWindow(popupHwnd_);
                    float subScale = (float)subDpi / 96.0f;
                    int marginPx = (int)(kShadowMargin * subScale);
                    /* Build 86+: 减 kSubmenuOverlap 让 submenu 往左挪, 跟
                     * parent 右边重叠. 跟 OpenSubmenuAt 同款公式. */
                    int overlapPx = (int)(kSubmenuOverlap * subScale);
                    int subX = rc.right - marginPx - overlapPx;
                    int subY = rc.top + (int)(ir.top * subScale);
                    sub->ShowPopup(parentHwnd_ ? parentHwnd_ : popupHwnd_, subX, subY);
                } else {
                    D2D1_RECT_F ir = ItemRect(hit);
                    D2D1_RECT_F vp = Bounds();
                    vp.right += 400; vp.bottom += 400;
                    sub->Show(ir.right - 4, ir.top, vp);
                }
            }
        } else {
            if (openSubmenuIndex_ >= 0 && openSubmenuIndex_ < (int)items_.size()) {
                auto& prevSub = items_[openSubmenuIndex_].submenu;
                if (prevSub) prevSub->Close();
                openSubmenuIndex_ = -1;
            }
        }
    }

    return Contains(x, y);
}

bool ContextMenu::HandleMouseDown(float x, float y) {
    if (!visible_) return false;

    if (openSubmenuIndex_ >= 0 && openSubmenuIndex_ < (int)items_.size()) {
        auto& sub = items_[openSubmenuIndex_].submenu;
        if (sub && sub->IsVisible() && !sub->popupHwnd_ && sub->Contains(x, y)) {
            return sub->HandleMouseDown(x, y);
        }
    }

    return Contains(x, y);
}

bool ContextMenu::HandleMouseUp(float x, float y) {
    if (!visible_) return false;

    // Check inline submenu
    if (openSubmenuIndex_ >= 0 && openSubmenuIndex_ < (int)items_.size()) {
        auto& sub = items_[openSubmenuIndex_].submenu;
        if (sub && sub->IsVisible() && !sub->popupHwnd_ && sub->Contains(x, y)) {
            bool handled = sub->HandleMouseUp(x, y);
            if (handled && sub->ClickedItemId() >= 0) {
                clickedId_ = sub->ClickedItemId();
                clickedStrId_ = sub->clickedStrId_;     // 沿链向上传播 id+属性
                clickedAttrs_ = sub->clickedAttrs_;
                Close();
                return true;
            }
            return handled;
        }
    }

    int hit = HitTest(x, y);
    if (hit >= 0 && hit < (int)items_.size()) {
        auto& item = items_[hit];
        if (!item.isSeparator && !item.submenu && item.enabled) {
            clickedId_ = item.id;
            clickedStrId_ = item.strId;
            clickedAttrs_ = item.attrs;
            Close();
            return true;
        }
    }

    if (!Contains(x, y)) {
        Close();
        return true;
    }

    return false;
}

int ContextMenu::Screenshot(const wchar_t* outPath) {
    if (!visible_ || !popupRenderer_.RT() || !outPath) return -1;
    auto* rt = popupRenderer_.RT();

    /* 重画两次保证 swap chain 当前 back buffer 有最新内容 (跟 Window 同款
       BeginDraw + Draw + EndDraw 走两轮). */
    auto repaint = [&]() {
        popupRenderer_.BeginDraw();
        popupRenderer_.Clear(hasBgColor_ ? bgColor_ : theme::kToolbarBg());
        Draw(popupRenderer_);
        popupRenderer_.EndDraw();
    };
    repaint();
    repaint();

    auto pxSize = rt->GetPixelSize();
    UINT pxw = pxSize.width, pyh = pxSize.height;
    if (pxw == 0 || pyh == 0) return -2;

    /* CPU-readable bitmap */
    D2D1_BITMAP_PROPERTIES1 cpuProps = {};
    cpuProps.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                              D2D1_ALPHA_MODE_PREMULTIPLIED);
    cpuProps.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ |
                             D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    ComPtr<ID2D1Bitmap1> cpuBmp;
    HRESULT hr = rt->CreateBitmap(D2D1::SizeU(pxw, pyh), nullptr, 0, cpuProps, &cpuBmp);
    if (FAILED(hr)) return -3;

    ComPtr<ID2D1Bitmap1> target;
    rt->GetTarget(reinterpret_cast<ID2D1Image**>(target.GetAddressOf()));
    if (!target) return -4;

    D2D1_POINT_2U dst = {0, 0};
    D2D1_RECT_U  src = {0, 0, pxw, pyh};
    hr = cpuBmp->CopyFromBitmap(&dst, target.Get(), &src);
    if (FAILED(hr)) return -5;

    D2D1_MAPPED_RECT mapped;
    hr = cpuBmp->Map(D2D1_MAP_OPTIONS_READ, &mapped);
    if (FAILED(hr)) return -6;

    auto& gctx = GetContext();
    ComPtr<IWICStream>           stream;
    ComPtr<IWICBitmapEncoder>    encoder;
    ComPtr<IWICBitmapFrameEncode> frame;

    hr = gctx.WICFactory()->CreateStream(&stream);
    if (FAILED(hr)) { cpuBmp->Unmap(); return -7; }
    hr = stream->InitializeFromFilename(outPath, GENERIC_WRITE);
    if (FAILED(hr)) { cpuBmp->Unmap(); return -8; }
    hr = gctx.WICFactory()->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) { cpuBmp->Unmap(); return -9; }
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) { cpuBmp->Unmap(); return -10; }
    hr = encoder->CreateNewFrame(&frame, nullptr);
    if (FAILED(hr)) { cpuBmp->Unmap(); return -11; }
    hr = frame->Initialize(nullptr);
    if (FAILED(hr)) { cpuBmp->Unmap(); return -12; }
    hr = frame->SetSize(pxw, pyh);
    if (FAILED(hr)) { cpuBmp->Unmap(); return -13; }
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&fmt);
    if (FAILED(hr)) { cpuBmp->Unmap(); return -14; }
    hr = frame->WritePixels(pyh, mapped.pitch, mapped.pitch * pyh, mapped.bits);
    cpuBmp->Unmap();
    if (FAILED(hr)) return -15;
    hr = frame->Commit();
    if (FAILED(hr)) return -16;
    hr = encoder->Commit();
    return SUCCEEDED(hr) ? 0 : -17;
}

} // namespace ui
