#include "compiler.h"
#include "widget_factory.h"
#include "svg_widget.h"
#include "../css/selector.h"
#include "../css/cascade.h"
#include "../css/css_parser.h"
#include "../theme.h"
#ifdef small
#undef small
#endif
#include "../controls.h"

#include "../css/value.h"

#include <functional>
#include <optional>
#include <set>
#include <sstream>

namespace ui::page {

namespace {

// Build a MatchNode from the HTML element's static attrs, plus its parent.
ui::css::MatchNode BuildMatchNode(const ui::uix::Node& n, const ui::css::MatchNode* parent) {
    ui::css::MatchNode m;
    m.tag = n.tag;
    m.parent = parent;

    // Collect classes and id from static attrs
    for (const auto& a : n.attrs) {
        if (a.kind != ui::uix::AttrKind::Static) continue;
        if (a.name == "id") {
            m.id = a.rawValue;
        } else if (a.name == "class") {
            // Split by whitespace
            std::istringstream iss(a.rawValue);
            std::string token;
            while (iss >> token) m.classes.push_back(token);
        } else {
            m.attrs[a.name] = a.rawValue;
        }
    }
    return m;
}

// Pick up plain-text children and concatenate; children with interpolations produce bindings.
/* 检测一段 text 是否是 "@i18nKey" 形式 (整段 trim 后剩 @ 开头 + 标识符
   字符)。返回 key（去掉 @），不是的话返回空 optional。
   匹配 .ui markup 的 @key 语义: 整个文本节点是单一 i18n 引用。
   "Hello @x" 这种部分混合不算（保持普通字面量，用户用 {{ }} 显式拼接）。 */
static std::optional<std::string> AsI18nKey(const std::string& text) {
    size_t i = 0, n = text.size();
    while (i < n && (text[i] == ' ' || text[i] == '\t' ||
                     text[i] == '\r' || text[i] == '\n')) ++i;
    if (i >= n || text[i] != '@') return std::nullopt;
    ++i;  // skip '@'
    size_t keyStart = i;
    auto isKeyCh = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    };
    while (i < n && isKeyCh(text[i])) ++i;
    if (i == keyStart) return std::nullopt;  // "@" 后无内容
    size_t keyEnd = i;
    while (i < n && (text[i] == ' ' || text[i] == '\t' ||
                     text[i] == '\r' || text[i] == '\n')) ++i;
    if (i != n) return std::nullopt;  // trailing 非空白 → 不是纯 i18n key
    return text.substr(keyStart, keyEnd - keyStart);
}

// The `outHasInterp` flag indicates if any interpolations existed (or
// @key i18n shorthand, which is desugared to $t('key') at compile time).
// Returns concatenated static text (may be empty).
std::string ExtractStaticText(const ui::uix::Node& node, bool& outHasInterp) {
    outHasInterp = false;
    std::string s;
    for (const auto& c : node.children) {
        if (c->kind == ui::uix::NodeKind::Text) {
            if (!s.empty()) s += " ";
            s += c->text;
        } else if (c->kind == ui::uix::NodeKind::Interpolation) {
            outHasInterp = true;
        }
    }
    /* @key 形式的纯文本 → 编译期改写为 $t() 调用，跟正常 interpolation
       走同一条 binding 路径，locale 切换时自动 re-evaluate */
    if (!outHasInterp && AsI18nKey(s)) outHasInterp = true;
    return s;
}

// JS-quote a static text fragment for the QuickJS path's BuildTextSourceJs.
// Output is a double-quoted JS string literal; backslash, double-quote,
// newline, CR, and tab escaped.
std::string JsQuoteString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    out += '"';
    return out;
}

// Build a JS expression (raw, NOT yet this.-prefixed) that concatenates the
// node's text + {{ interp }} children. Mirrors BuildTextExpr but emits source
// text for the QuickJS path.
//
//   <div>Hi, {{ user.name }}!</div>     →  "Hi, " + (user.name) + "!"
//   <p>{{ count * 2 }}</p>              →  "" + (count * 2) + ""
//   <label>@app.title</label>           →  $t("app.title")
std::string BuildTextSourceJs(const ui::uix::Node& node) {
    std::string out;
    bool first = true;
    auto append = [&](std::string piece) {
        if (first) { out = std::move(piece); first = false; }
        else       { out += " + "; out += std::move(piece); }
    };
    for (const auto& c : node.children) {
        if (c->kind == ui::uix::NodeKind::Text) {
            if (auto key = AsI18nKey(c->text)) {
                append("$t(" + JsQuoteString(*key) + ")");
            } else {
                append(JsQuoteString(c->text));
            }
        } else if (c->kind == ui::uix::NodeKind::Interpolation) {
            // Interpolation Attr stored its raw expression — but template_parser.cpp
            // only keeps the parsed AST + raw text on the *attribute* form. For
            // text interpolations we have NodeKind::Interpolation with `expr`.
            // We still need the raw source. Easiest: serialize from the
            // {{ ... }} bookend via the original line/col if we had it; but
            // the parser collapsed it. Workaround: re-stringify by walking
            // the AST is heavy. For now, store the raw text on the parser
            // side. → see the companion change in template_parser.cpp.
            append("(" + c->text + ")");
        }
    }
    return out.empty() ? std::string("\"\"") : out;
}

struct CompilerCtx {
    const ui::css::Stylesheet& sheet;
    // Shared pointer so widget recomputeStyle lambdas and the page can both
    // see runtime updates (theme switch rewrites the contents in place).
    std::shared_ptr<std::vector<std::pair<std::string, std::string>>> cssVars;
    CompiledPage* out;
    bool skipRootVFor = false;
    bool skipRootVIf  = false;
};

// Parse a v-for expression "x in items" or "(x, i) in items"
// Returns true on success; fills loopVar, indexVar (may be empty), listName.
bool ParseVFor(const std::string& expr, std::string& loopVar, std::string& indexVar, std::string& listName) {
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        size_t b = s.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) return std::string();
        return s.substr(a, b - a + 1);
    };
    std::string s = trim(expr);
    // Find " in " separator
    size_t inPos = std::string::npos;
    for (size_t i = 0; i + 3 < s.size(); ++i) {
        if ((s[i] == ' ' || s[i] == '\t') && s[i+1] == 'i' && s[i+2] == 'n' &&
            (s[i+3] == ' ' || s[i+3] == '\t')) {
            inPos = i;
            break;
        }
    }
    if (inPos == std::string::npos) return false;
    std::string lhs = trim(s.substr(0, inPos));
    std::string rhs = trim(s.substr(inPos + 4));
    listName = rhs;
    if (!lhs.empty() && lhs.front() == '(') {
        if (lhs.back() != ')') return false;
        std::string inner = trim(lhs.substr(1, lhs.size() - 2));
        size_t comma = inner.find(',');
        if (comma == std::string::npos) {
            loopVar = trim(inner);
        } else {
            loopVar = trim(inner.substr(0, comma));
            indexVar = trim(inner.substr(comma + 1));
        }
    } else {
        loopVar = lhs;
        indexVar.clear();
    }
    return !loopVar.empty() && !listName.empty();
}

void CompileElement(CompilerCtx& ctx,
                    const ui::uix::Node& node,
                    const ui::css::MatchNode* parentMatch,
                    Widget* parentWidget) {
    if (node.kind != ui::uix::NodeKind::Element) return;

    // ---- v-if detection (mount/unmount semantics) ----
    // When true, the template becomes a real widget; when false, nothing is built.
    // The `skipRootVIf` flag is set by the runtime when it's about to mount a
    // conditional's template as a real widget.
    {
        bool checkVIf = !ctx.skipRootVIf;
        ctx.skipRootVIf = false;  // one-shot
        if (checkVIf) {
            for (const auto& a : node.attrs) {
                if (a.kind == ui::uix::AttrKind::Directive && a.name == "if") {
                    if (a.rawValue.empty()) {
                        ctx.out->errors.push_back("v-if requires an expression");
                        return;
                    }
                    CompiledConditional cond;
                    cond.parentWidget = parentWidget;
                    cond.templateNode = &node;
                    cond.condSourceJs = a.rawValue;
                    if (parentWidget) cond.insertIndex = parentWidget->Children().size();
                    ctx.out->conditionals.push_back(std::move(cond));
                    return;  // skip widget creation; runtime mounts it on demand
                }
            }
        }
    }

    // ---- v-for detection (before building widget) ----
    // If this element has v-for, register a CompiledLoop and DO NOT build its widget now.
    // The runtime will compile per-iteration copies.
    // The `skipRootVFor` flag suppresses this on the FIRST call of a per-iteration
    // compile so the template itself becomes a real widget.
    bool checkVFor = !ctx.skipRootVFor;
    ctx.skipRootVFor = false;  // only affects the root of the call
    if (checkVFor) {
        for (const auto& a : node.attrs) {
            if (a.kind == ui::uix::AttrKind::Directive && a.name == "for") {
                std::string loopVar, indexVar, listName;
                if (!ParseVFor(a.rawValue, loopVar, indexVar, listName)) {
                    ctx.out->errors.push_back("malformed v-for value: '" + a.rawValue + "'");
                    return;
                }
                CompiledLoop loop;
                loop.parentWidget = parentWidget;
                loop.templateNode = &node;
                loop.loopVar = loopVar;
                loop.indexVar = indexVar;
                loop.listName = listName;
                loop.listSourceJs = listName;
                for (const auto& ka : node.attrs) {
                    if (ka.kind == ui::uix::AttrKind::Bind && ka.name == "key" &&
                        !ka.rawValue.empty()) {
                        loop.keySourceJs = ka.rawValue;
                        break;
                    }
                }
                if (parentWidget) loop.insertIndex = parentWidget->Children().size();
                ctx.out->loops.push_back(std::move(loop));
                return;
            }
        }
    }

    // ---- Component instantiation ----
    // ---- <menuitem> / <separator> 出现在 <menu> 外 ----
    // <menu> 的子节点由下面的拦截块直接处理, 永远走不到这里. 出现在这里
    // 说明用户写错了位置 — 报错给提示, 不再悄悄变成空 VBox.
    if (node.tag == "menuitem") {
        ctx.out->errors.push_back(
            "<menuitem> must be a direct child of <menu>; got it at top level. "
            "Wrap with <menu id=\"...\">...</menu>.");
        return;
    }

    // ---- <menu> detection ----
    // <menu id="..." trigger="#elem" event="click|rclick"> 不创建 widget,
    // 而是把 children (<menuitem>/<separator>) 收集到一个 CompiledMenu, 让
    // PageState 在 Attach 时实例化成 ContextMenu 并挂到 trigger 上.
    if (node.tag == "menu") {
        /* Phase B: 把 <svg ...><path .../></svg> 子节点序列化回字符串, 让
           ContextMenu / Renderer::ParseSvgIcon 处理. 仅处理静态 attrs + child
           Text + 嵌套 element; 表达式绑定/事件不会出现在 menu icon 里. */
        std::function<void(const ui::uix::Node&, std::string&)> serializeNode;
        serializeNode = [&serializeNode](const ui::uix::Node& n, std::string& out) {
            if (n.kind == ui::uix::NodeKind::Text) { out += n.text; return; }
            if (n.kind != ui::uix::NodeKind::Element) return;
            out += "<"; out += n.tag;
            for (const auto& a : n.attrs) {
                if (a.kind != ui::uix::AttrKind::Static) continue;
                out += " "; out += a.name; out += "=\""; out += a.rawValue; out += "\"";
            }
            if (n.children.empty()) { out += "/>"; return; }
            out += ">";
            for (const auto& c : n.children) if (c) serializeNode(*c, out);
            out += "</"; out += n.tag; out += ">";
        };

        /* 递归编译: 顶层 <menu> 进 ctx.out->menus; 嵌套 <menu text="..."> 作为
           submenu 通过 lambda 共享 ctx (id/handler 收集都用得到). */
        std::function<CompiledMenuPtr(const ui::uix::Node&, int /*autoId*/, bool /*top*/)>
            compileMenu;
        compileMenu = [&](const ui::uix::Node& mNode, int /*startId*/, bool top) -> CompiledMenuPtr {
            auto cm = std::make_shared<CompiledMenu>();
            for (const auto& a : mNode.attrs) {
                if (a.kind == ui::uix::AttrKind::Static) {
                    if (a.name == "id") cm->id = a.rawValue;
                    else if (a.name == "trigger") cm->triggerSelector = a.rawValue;
                    else if (a.name == "event")   cm->triggerEvent    = a.rawValue;
                } else if (a.kind == ui::uix::AttrKind::Directive) {
                    /* <menu v-if / v-show>: 整体条件渲染, show 不出来时
                     * PopulateMenu skip 不 build items. v-show 在 menu 语境
                     * 等价 v-if (popup 模型没"占位但不显" 概念, 见 build 73 CHANGELOG). */
                    if (a.name == "if" || a.name == "show") cm->vIfExpr = a.rawValue;
                }
            }
            if (top && cm->triggerEvent.empty()) cm->triggerEvent = "click";

            int autoId = 1;
            for (const auto& cptr : mNode.children) {
                if (!cptr) continue;
                const auto& c = *cptr;
                if (c.kind != ui::uix::NodeKind::Element) continue;

                /* BREAKING (build 75): 嵌套 <menu title="..." v-if="..."> 当
                 * submenu. submenu 的 "parent 入口" 视觉跟 menuitem 同款
                 * widget-template 渲染: 取 title (静态或 :title 反应式) 包到
                 * 自动 <label> 子节点, 跟 <menuitem><label>title</label></menuitem>
                 * 等价. 没有 menuitem 那么自由 (用户暂不能写 <menu><svg/>title</menu>
                 * 形式定 submenu 入口外观, 若需要可后续扩展). */
                if (c.tag == "menu") {
                    CompiledMenuItem mi;
                    std::string titleStatic;
                    std::string titleBound;
                    for (const auto& a : c.attrs) {
                        if (a.kind == ui::uix::AttrKind::Static) {
                            if (a.name == "title" || a.name == "text")
                                titleStatic = a.rawValue;
                        } else if (a.kind == ui::uix::AttrKind::Bind) {
                            if (a.name == "title" || a.name == "text")
                                titleBound = a.rawValue;
                        } else if (a.kind == ui::uix::AttrKind::Directive) {
                            if (a.name == "if" || a.name == "show")
                                mi.vIfExpr = a.rawValue;
                        }
                    }
                    /* 合成 wrapper: <div class="menuitem-row"><label>{title}</label></div>
                     * label 内放静态 text node 或 Interpolation node (反应式). */
                    auto wrapper = std::make_unique<ui::uix::Node>();
                    wrapper->kind = ui::uix::NodeKind::Element;
                    wrapper->tag = "div";
                    {
                        ui::uix::Attr cls;
                        cls.kind = ui::uix::AttrKind::Static;
                        cls.name = "class";
                        cls.rawValue = "menuitem-row";
                        wrapper->attrs.push_back(cls);
                    }
                    auto labelNode = std::make_unique<ui::uix::Node>();
                    labelNode->kind = ui::uix::NodeKind::Element;
                    labelNode->tag = "label";
                    if (!titleBound.empty()) {
                        auto interp = std::make_unique<ui::uix::Node>();
                        interp->kind = ui::uix::NodeKind::Interpolation;
                        interp->text = titleBound;
                        labelNode->children.push_back(std::move(interp));
                    } else {
                        auto textNode = std::make_unique<ui::uix::Node>();
                        textNode->kind = ui::uix::NodeKind::Text;
                        textNode->text = titleStatic;
                        labelNode->children.push_back(std::move(textNode));
                    }
                    wrapper->children.push_back(std::move(labelNode));
                    mi.contentRoot = std::move(wrapper);

                    mi.submenu = compileMenu(c, 1, false /*not top*/);
                    if (mi.itemId == 0) mi.itemId = autoId++;
                    cm->items.push_back(std::move(mi));
                    continue;
                }

                CompiledMenuItem mi;
                if (c.tag == "separator" || c.tag == "hr") {
                    mi.separator = true;
                    /* separator 支持 v-if / v-show. */
                    for (const auto& a : c.attrs) {
                        if (a.kind == ui::uix::AttrKind::Directive &&
                            (a.name == "if" || a.name == "show")) {
                            mi.vIfExpr = a.rawValue;
                        }
                    }
                    cm->items.push_back(std::move(mi));
                    continue;
                }
                if (c.tag != "menuitem") {
                    ctx.out->errors.push_back(
                        "<menu> child must be <menuitem>, <separator>, or nested <menu>, got <"
                        + c.tag + ">.");
                    continue;
                }

                /* BREAKING (build 75): 老的 icon/style/text 静态属性 + :text/
                 * :icon/:style 反应式属性 全 deprecated 砍掉. menuitem body
                 * 直接当 widget content slot. 保留:
                 *   id (静态 int) — 派发 callback
                 *   shortcut (静态 / :shortcut 反应式) — 右对齐显示, 不可点
                 *   onclick / @click — JS method 名 (跟 id callback 等价)
                 *   :enabled — 反应式 disable
                 *   v-if / v-show / v-for — 同 widget 标准 */
                for (const auto& a : c.attrs) {
                    if (a.kind == ui::uix::AttrKind::Static) {
                        mi.attrs.push_back({a.name, a.rawValue});  // 捕获全部静态属性 → callback 可读
                        if (a.name == "id") {
                            mi.strId = a.rawValue;                 // 字符串 id (key 如 "cmd_delete" 或数字串)
                            /* 数字 id 时也填 int (内部 hit-test/debug); 非数字 key
                             * 留 itemId=0, 下面 autoId 兜底。不再报错。 */
                            try { mi.itemId = std::stoi(a.rawValue); }
                            catch (...) {}
                        }
                        else if (a.name == "shortcut") mi.shortcut = a.rawValue;
                        else if (a.name == "onclick")  mi.onClick  = a.rawValue;
                    }
                    else if (a.kind == ui::uix::AttrKind::Bind) {
                        if      (a.name == "shortcut") mi.boundShortcutExpr = a.rawValue;
                        else if (a.name == "enabled")  mi.boundEnabledExpr  = a.rawValue;
                        /* 注: :text / :icon / :style 不再支持, 改写 menuitem
                         * body 内放 <svg :style="..."/> 或 <label>{{ x }}</label>
                         * 等任意 widget. */
                    }
                    else if (a.kind == ui::uix::AttrKind::Directive) {
                        if (a.name == "if" || a.name == "show") mi.vIfExpr = a.rawValue;
                        else if (a.name == "for") {
                            std::string lv, iv, ln;
                            if (ParseVFor(a.rawValue, lv, iv, ln)) {
                                mi.vForIterVar   = lv;
                                mi.vForIndexVar  = iv;
                                mi.vForArrayExpr = ln;
                            } else {
                                ctx.out->errors.push_back(
                                    "<menuitem v-for='" + a.rawValue +
                                    "'>: expected \"x in items\" or \"(x, i) in items\"");
                            }
                        }
                    }
                    else if (a.kind == ui::uix::AttrKind::Event && a.name == "click" &&
                             !a.rawValue.empty()) {
                        mi.onClick = a.rawValue;
                    }
                }

                /* 把 menuitem 的 children **深拷贝** 到一个合成 <div class="menuitem-row">
                 * wrapper, 让 widget_factory 当普通 hbox 编译. 用 deep-clone 而
                 * 不是 move 因为外层 mNode 是 const 引用 (AST 不可变契约). Text /
                 * Interpolation 子节点自动包到 <label> 里 (方便 <menuitem>文字</menuitem>
                 * 这种简单写法直接 work). 元素节点 (svg / label / div / button / ...)
                 * 原样 clone 进 wrapper. */
                std::function<ui::uix::NodePtr(const ui::uix::Node&)> cloneNode;
                cloneNode = [&cloneNode](const ui::uix::Node& src) -> ui::uix::NodePtr {
                    auto n = std::make_unique<ui::uix::Node>();
                    n->kind = src.kind;
                    n->line = src.line;
                    n->col  = src.col;
                    n->tag  = src.tag;
                    n->selfClosed = src.selfClosed;
                    n->text = src.text;
                    n->attrs = src.attrs;
                    for (const auto& ch : src.children) {
                        if (ch) n->children.push_back(cloneNode(*ch));
                    }
                    return n;
                };
                auto wrapper = std::make_unique<ui::uix::Node>();
                wrapper->kind = ui::uix::NodeKind::Element;
                wrapper->tag = "div";
                {
                    ui::uix::Attr cls;
                    cls.kind = ui::uix::AttrKind::Static;
                    cls.name = "class";
                    cls.rawValue = "menuitem-row";
                    wrapper->attrs.push_back(cls);
                    /* 默认 layout (flex-row / gap / padding / max-width) 走 lib
                     * 内置 .menuitem-row CSS (page_api.cpp 在 user CSS 前 inject),
                     * 用户可在 <style> 写同选择器覆盖. */
                }
                for (const auto& cptr2 : c.children) {
                    if (!cptr2) continue;
                    if (cptr2->kind == ui::uix::NodeKind::Text) {
                        bool nonWs = false;
                        for (char ch : cptr2->text) {
                            if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
                                nonWs = true; break;
                            }
                        }
                        if (!nonWs) continue;
                        auto labelNode = std::make_unique<ui::uix::Node>();
                        labelNode->kind = ui::uix::NodeKind::Element;
                        labelNode->tag = "label";
                        labelNode->children.push_back(cloneNode(*cptr2));
                        wrapper->children.push_back(std::move(labelNode));
                    } else if (cptr2->kind == ui::uix::NodeKind::Interpolation) {
                        auto labelNode = std::make_unique<ui::uix::Node>();
                        labelNode->kind = ui::uix::NodeKind::Element;
                        labelNode->tag = "label";
                        labelNode->children.push_back(cloneNode(*cptr2));
                        wrapper->children.push_back(std::move(labelNode));
                    } else {
                        wrapper->children.push_back(cloneNode(*cptr2));
                    }
                }
                mi.contentRoot = std::move(wrapper);

                if (mi.itemId == 0) mi.itemId = autoId++;
                cm->items.push_back(std::move(mi));
            }
            return cm;
        };

        auto top = compileMenu(node, 1, true);
        if (top) ctx.out->menus.push_back(std::move(*top));
        return;  // 不创建 widget, 不递归 children
    }

    // Build match descriptor + computed style
    ui::css::MatchNode m = BuildMatchNode(node, parentMatch);

    // Extract inline style="..." (static attr only — bound :style is reactive
    // and lives elsewhere in the binding system).
    std::vector<ui::css::Declaration> inlineDecls;
    for (const auto& a : node.attrs) {
        if (a.kind != ui::uix::AttrKind::Static) continue;
        if (a.name == "style") {
            inlineDecls = ui::css::ParseInlineStyle(a.rawValue);
            break;
        }
    }
    ui::css::ComputedStyle style = ui::css::ComputeStyle(ctx.sheet, m, inlineDecls, *ctx.cssVars);

    // Build widget (captures first static text automatically)
    WidgetPtr w = BuildWidget(node, style);
    if (!w) {
        ctx.out->errors.push_back("failed to build widget for tag <" + node.tag + ">");
        return;
    }

    // Save the widget's identity so the runtime recompute can rebuild a
    // MatchNode chain (with parent links) when state or :class changes.
    w->cssTag = node.tag;
    w->classList = m.classes;

    // Install the recompute hook. Each call rebuilds an ancestor chain by
    // walking the widget tree, so descendant-combinator selectors like
    // `.tabs .tab.active` keep matching after dynamic-class toggles.
    {
        auto varsRef = ctx.cssVars;  // shared_ptr — survives runtime theme rewrites
        auto inlineDeclsCopy = inlineDecls;
        const ui::css::Stylesheet* sheetPtr = &ctx.sheet;
        Widget* rawW = w.get();
        w->recomputeStyle = [sheetPtr, varsRef, inlineDeclsCopy, rawW](uint32_t bits) {
            // Build an ancestor chain from the widget tree.
            std::vector<ui::css::MatchNode> chain;
            for (Widget* p = rawW->Parent(); p; p = p->Parent()) {
                ui::css::MatchNode pm;
                pm.tag = p->cssTag;
                pm.id  = p->id;
                pm.classes = p->classList;
                for (const auto& c : p->dynamicClasses) pm.classes.push_back(c);
                chain.push_back(std::move(pm));
            }
            for (size_t i = 0; i < chain.size(); ++i) {
                chain[i].parent = (i + 1 < chain.size()) ? &chain[i + 1] : nullptr;
            }
            ui::css::MatchNode self;
            self.tag = rawW->cssTag;
            self.id  = rawW->id;
            self.classes = rawW->classList;
            for (const auto& c : rawW->dynamicClasses) self.classes.push_back(c);
            self.stateBits = bits;
            self.parent = chain.empty() ? nullptr : &chain[0];

            auto s = ui::css::ComputeStyle(*sheetPtr, self, inlineDeclsCopy, *varsRef);
            rawW->css = Widget::CssOverride{};
            rawW->bgColor = D2D1_COLOR_F{0, 0, 0, 0};
            rawW->boxShadow.set = false;
            rawW->boxShadows.clear();
            rawW->hasBgGradient = false;
            rawW->bgGradients.clear();
            rawW->rotateDeg = 0.0f;
            rawW->scaleX = 1.0f;
            rawW->scaleY = 1.0f;
            rawW->transformX = 0.0f;
            rawW->transformY = 0.0f;
            rawW->overflowHidden = false;
            // Reset flex-container fields so a class toggle that removes a
            // gap/justify-content/align-items rule actually clears the value
            // (Apply* functions only set on s.Has(prop)). Container type
            // (VBox vs HBox) is locked at factory time and can't change here.
            if (auto* vb = dynamic_cast<VBoxWidget*>(rawW)) {
                vb->Gap(0);
                vb->MainJustify(LayoutJustify::Start);
                vb->CrossAlign(LayoutAlign::Stretch);
            } else if (auto* hb = dynamic_cast<HBoxWidget*>(rawW)) {
                hb->Gap(0);
                hb->MainJustify(LayoutJustify::Start);
                hb->CrossAlign(LayoutAlign::Stretch);
                hb->FlexWrap(false);
            }
            // Reset flex-item fields (expanding/flex from flex-grow / flex shorthand)
            rawW->expanding = false;
            rawW->flex = 1.0f;
            ApplyCommonStyle(*rawW, s);
            ApplyFlexContainerStyle(*rawW, s);
            ApplyFlexItemStyle(*rawW, s);
            // SVG-specific reactive hook: re-run the CSS-to-shape pipeline
            // so :hover / .sel descendant selectors flip path fill at runtime.
            if (auto* svg = dynamic_cast<SvgWidget*>(rawW)) {
                if (svg->recomputeShapes) svg->recomputeShapes();
            }
        };
    }

    // ---- <select>: collect <option> children into ComboBox items ----
    // Also prevents options from being compiled as separate child widgets.
    bool isSelect = (node.tag == "select");
    if (isSelect) {
        if (auto* cb = dynamic_cast<ComboBoxWidget*>(w.get())) {
            std::vector<std::wstring> items;
            std::vector<std::string>  i18nKeys;   // L83: per-option i18n key ("" = literal)
            for (const auto& c : node.children) {
                if (c->kind != ui::uix::NodeKind::Element) continue;
                if (c->tag != "option") continue;
                // Read first text child of the <option>
                std::string  raw;        // narrow source (for @key detection)
                std::wstring wtext;
                for (const auto& oc : c->children) {
                    if (oc->kind == ui::uix::NodeKind::Text) {
                        // narrow UTF-8 → wide; lightweight version
                        const std::string& s = oc->text;
                        raw = s;
                        for (size_t i = 0; i < s.size(); ) {
                            unsigned char b = static_cast<unsigned char>(s[i]);
                            uint32_t cp = 0; int len = 1;
                            if ((b & 0x80) == 0)       { cp = b;          len = 1; }
                            else if ((b & 0xE0) == 0xC0){ cp = b & 0x1F;   len = 2; }
                            else if ((b & 0xF0) == 0xE0){ cp = b & 0x0F;   len = 3; }
                            else if ((b & 0xF8) == 0xF0){ cp = b & 0x07;   len = 4; }
                            for (int k = 1; k < len && i + k < s.size(); ++k)
                                cp = (cp << 6) | (static_cast<unsigned char>(s[i+k]) & 0x3F);
                            i += len;
                            if (cp < 0x10000) wtext += static_cast<wchar_t>(cp);
                            else { cp -= 0x10000;
                                   wtext += static_cast<wchar_t>(0xD800 | (cp >> 10));
                                   wtext += static_cast<wchar_t>(0xDC00 | (cp & 0x3FF)); }
                        }
                        break;  // first text only
                    }
                }
                // L83 i18n: `<option>@key</option>` → record key + show key as
                // placeholder (resolved on the first ui_page_set_locale, like
                // <label> @key fallback). Non-@key options stay literal.
                if (auto key = AsI18nKey(raw)) {
                    i18nKeys.push_back(*key);
                    items.emplace_back(key->begin(), key->end());  // ASCII key → wide
                } else {
                    i18nKeys.push_back(std::string{});
                    items.push_back(std::move(wtext));
                }
            }
            cb->SetItems(std::move(items));
            cb->SetI18nItemKeys(std::move(i18nKeys));
        }
    }

    // ---- <tabs>: each <tab title="..."> child becomes a TabControl tab whose
    //        body is a compiled VBox of inner children. Default child compile
    //        is suppressed for the <tabs> element below.
    bool isTabs = (node.tag == "tabs" || node.tag == "TabControl");
    if (isTabs) {
        if (auto* tc = dynamic_cast<TabControlWidget*>(w.get())) {
            auto utf8ToWide = [](const std::string& s) {
                std::wstring r;
                for (size_t i = 0; i < s.size(); ) {
                    unsigned char b = (unsigned char)s[i];
                    uint32_t cp = 0; int len = 1;
                    if ((b & 0x80) == 0)        { cp = b;        len = 1; }
                    else if ((b & 0xE0) == 0xC0){ cp = b & 0x1F; len = 2; }
                    else if ((b & 0xF0) == 0xE0){ cp = b & 0x0F; len = 3; }
                    else if ((b & 0xF8) == 0xF0){ cp = b & 0x07; len = 4; }
                    for (int k = 1; k < len && i + k < s.size(); ++k)
                        cp = (cp << 6) | ((unsigned char)s[i+k] & 0x3F);
                    i += len;
                    if (cp < 0x10000) r += (wchar_t)cp;
                    else { cp -= 0x10000;
                           r += (wchar_t)(0xD800 | (cp >> 10));
                           r += (wchar_t)(0xDC00 | (cp & 0x3FF)); }
                }
                return r;
            };
            for (const auto& c : node.children) {
                if (c->kind != ui::uix::NodeKind::Element) continue;
                if (c->tag != "tab") continue;
                std::wstring title;
                for (const auto& a : c->attrs) {
                    if (a.kind == ui::uix::AttrKind::Static && a.name == "title") {
                        title = utf8ToWide(a.rawValue);
                    }
                }
                // Compile inner children into a VBox container.
                auto body = std::make_shared<VBoxWidget>();
                ui::css::MatchNode tabM = BuildMatchNode(*c, &m);
                for (const auto& sub : c->children) {
                    if (sub->kind == ui::uix::NodeKind::Element) {
                        CompileElement(ctx, *sub, &tabM, body.get());
                    }
                }
                tc->AddTab(title, body);
            }
        }
    }

    // ---- <svg>: fold child elements into SvgShape / SvgGradient entries ----
    // Children like <circle>/<rect>/... do NOT become widgets. Their static
    // attrs populate the shape; :attr bindings become CompiledBindings whose
    // property is "shape[N].<attr>" on the parent SvgWidget.
    // <defs>/<linearGradient>/<radialGradient>/<stop> are registered on the
    // SvgWidget's gradient dictionary.
    bool isSvg = (node.tag == "svg");
    if (isSvg) {
        if (auto* svg = dynamic_cast<SvgWidget*>(w.get())) {
            auto kindOf = [](const std::string& t) -> int {
                if (t == "circle")   return (int)SvgShapeKind::Circle;
                if (t == "rect")     return (int)SvgShapeKind::Rect;
                if (t == "ellipse")  return (int)SvgShapeKind::Ellipse;
                if (t == "line")     return (int)SvgShapeKind::Line;
                if (t == "polygon")  return (int)SvgShapeKind::Polygon;
                if (t == "polyline") return (int)SvgShapeKind::Polyline;
                if (t == "path")     return (int)SvgShapeKind::Path;
                return -1;
            };

            auto parseStop = [](const ui::uix::Node& stopNode) -> SvgGradientStop {
                SvgGradientStop st;
                for (const auto& a : stopNode.attrs) {
                    if (a.kind != ui::uix::AttrKind::Static) continue;
                    if (a.name == "offset") {
                        std::string v = a.rawValue;
                        bool pct = (!v.empty() && v.back() == '%');
                        if (pct) v.pop_back();
                        try {
                            float f = std::stof(v);
                            st.offset = pct ? f * 0.01f : f;
                        } catch (...) {}
                    } else if (a.name == "stop-color") {
                        ui::css::Color c;
                        if (ui::css::ParseColor(a.rawValue, c)) {
                            st.color = D2D1::ColorF(c.r, c.g, c.b, c.a);
                        }
                    } else if (a.name == "stop-opacity") {
                        try { st.opacity = std::stof(a.rawValue); } catch (...) {}
                    }
                }
                return st;
            };

            auto parseGradient = [&](const ui::uix::Node& gNode, SvgGradient::Kind kind) {
                SvgGradient g;
                g.kind = kind;
                for (const auto& a : gNode.attrs) {
                    if (a.kind != ui::uix::AttrKind::Static) continue;
                    const std::string& n = a.name;
                    const std::string& v = a.rawValue;
                    try {
                        if      (n == "id") g.id = v;
                        else if (n == "x1") g.x1 = std::stof(v);
                        else if (n == "y1") g.y1 = std::stof(v);
                        else if (n == "x2") g.x2 = std::stof(v);
                        else if (n == "y2") g.y2 = std::stof(v);
                        else if (n == "cx") g.cx = std::stof(v);
                        else if (n == "cy") g.cy = std::stof(v);
                        else if (n == "r")  g.radius = std::stof(v);
                        else if (n == "fx") g.fx = std::stof(v);
                        else if (n == "fy") g.fy = std::stof(v);
                    } catch (...) {}
                }
                // Default focal point to center if unset.
                if (g.fx == 0.5f && g.cx != 0.5f) g.fx = g.cx;
                if (g.fy == 0.5f && g.cy != 0.5f) g.fy = g.cy;
                for (const auto& sc : gNode.children) {
                    if (sc->kind != ui::uix::NodeKind::Element) continue;
                    if (sc->tag == "stop") g.stops.push_back(parseStop(*sc));
                }
                if (!g.id.empty()) svg->AddGradient(std::move(g));
            };

            // Apply CSS-cascaded properties to a shape. Browser semantics:
            //   inline style="..."  > stylesheet rule  > presentation attr  > default.
            // Caller already applied presentation attrs to `shape`, so any value
            // in the computed style overrides them.
            auto applyShapeCss = [&](SvgShape& shape, const ui::uix::Node& shapeNode,
                                     const ui::css::MatchNode* shapeParent) {
                ui::css::MatchNode pm = BuildMatchNode(shapeNode, shapeParent);
                std::vector<ui::css::Declaration> inlineDecls;
                for (const auto& a : shapeNode.attrs) {
                    if (a.kind == ui::uix::AttrKind::Static && a.name == "style") {
                        inlineDecls = ui::css::ParseInlineStyle(a.rawValue);
                        break;
                    }
                }
                auto cs = ui::css::ComputeStyle(ctx.sheet, pm, inlineDecls, *ctx.cssVars);
                static const char* kSvgProps[] = {
                    "fill", "stroke",
                    "fill-opacity", "stroke-opacity",
                    "stroke-width", "stroke-dasharray",
                    "stroke-linecap", "stroke-linejoin",
                    "opacity",
                };
                for (const char* p : kSvgProps) {
                    if (cs.Has(p)) ApplySvgShapeAttr(shape, p, cs.Get(p));
                }
            };

            // Track each shape's source HTML node so the runtime recompute
            // hook can re-run CSS without re-parsing markup.
            std::vector<const ui::uix::Node*> shapeSourceNodes;
            // Per-shape snapshot of inherited presentation attrs (from <svg>
            // root + ancestor <g> chain). Parallel to shapeSourceNodes —
            // recompute closure re-applies these BEFORE the shape's own attrs
            // so SVG inheritance (1.1 §6.4) survives state-driven recomputes.
            using InheritedAttrs = std::vector<std::pair<std::string, std::string>>;
            std::vector<InheritedAttrs> shapeInheritedAttrs;

            // SVG presentation attrs that inherit by default (per SVG 1.1 §6.4).
            // `opacity` is intentionally excluded — it's NOT inherited.
            static const std::set<std::string> kInheritableSvgAttrs = {
                "fill", "stroke",
                "fill-opacity", "stroke-opacity",
                "stroke-width", "stroke-dasharray",
                "stroke-linecap", "stroke-linejoin",
                "color",
            };

            // Merge an element's inheritable static attrs into a vec.
            // Later entries with the same name overwrite earlier ones (child
            // overrides ancestor when walking down).
            auto mergeInherited = [&](const ui::uix::Node& el, InheritedAttrs& vec) {
                for (const auto& a : el.attrs) {
                    if (a.kind != ui::uix::AttrKind::Static) continue;
                    if (!kInheritableSvgAttrs.count(a.name)) continue;
                    bool replaced = false;
                    for (auto& p : vec) {
                        if (p.first == a.name) { p.second = a.rawValue; replaced = true; break; }
                    }
                    if (!replaced) vec.emplace_back(a.name, a.rawValue);
                }
            };

            // Walker: handles nested <defs> (where gradients usually live) but
            // also tolerates gradients declared directly under <svg>.
            // `parentMatch` is the css::MatchNode of the current container —
            // starts at the SvgWidget's own match node `m`, descends through
            // <g>/<defs> if we ever support them.
            // `inherited` carries the accumulated presentation attrs from
            // <svg> + any ancestor <g> chain.
            std::function<void(const ui::uix::Node&, const ui::css::MatchNode*, const InheritedAttrs&)> walk =
                [&](const ui::uix::Node& parent, const ui::css::MatchNode* parentMatch,
                    const InheritedAttrs& inherited) {
                for (const auto& c : parent.children) {
                    if (c->kind != ui::uix::NodeKind::Element) continue;
                    if (c->tag == "defs") { walk(*c, parentMatch, inherited); continue; }
                    if (c->tag == "g") {
                        // build 90: <g> presentation attrs 参与级联到 child.
                        // 合并 <g> 自己的 inheritable attrs 到一份副本, 向下传.
                        InheritedAttrs merged = inherited;
                        mergeInherited(*c, merged);
                        walk(*c, parentMatch, merged);
                        continue;
                    }
                    if (c->tag == "linearGradient") { parseGradient(*c, SvgGradient::Kind::Linear); continue; }
                    if (c->tag == "radialGradient") { parseGradient(*c, SvgGradient::Kind::Radial); continue; }
                    int k = kindOf(c->tag);
                    if (k < 0) continue;
                    SvgShape shape;
                    shape.kind = static_cast<SvgShapeKind>(k);
                    // 1a. Inherited presentation attrs (lowest priority).
                    for (const auto& [n, v] : inherited) ApplySvgShapeAttr(shape, n, v);
                    // 1b. Shape's own presentation attrs (override inherited).
                    for (const auto& a : c->attrs) {
                        if (a.kind != ui::uix::AttrKind::Static) continue;
                        ApplySvgShapeAttr(shape, a.name, a.rawValue);
                    }
                    // 2. CSS cascade (stylesheet rule > inline style > presentation attr).
                    applyShapeCss(shape, *c, parentMatch);
                    size_t shapeIdx = svg->Shapes().size();
                    svg->AddShape(std::move(shape));
                    shapeSourceNodes.push_back(c.get());
                    shapeInheritedAttrs.push_back(inherited);
                    for (const auto& a : c->attrs) {
                        if (a.kind != ui::uix::AttrKind::Bind) continue;
                        if (a.rawValue.empty()) continue;
                        CompiledBinding cb;
                        cb.target = w.get();
                        cb.property = "shape[" + std::to_string(shapeIdx) + "]." + a.name;
                        cb.sourceJs = a.rawValue;
                        ctx.out->bindings.push_back(std::move(cb));
                    }
                }
            };
            // Initial inherited set: <svg> root's own inheritable attrs.
            InheritedAttrs svgInherited;
            mergeInherited(node, svgInherited);
            walk(node, &m, svgInherited);

            // Install the runtime recompute closure. Captures by value — sheet
            // ptr is stable (CompiledPage owns it), shape nodes are owned by
            // CompiledPage::ownedHtml, both outlive the widget.
            // 重算逻辑：从 widget 树活体重建 ancestor MatchNode 链（拿到此时
            // 的 dynamicClasses + state bits），再为每个 shape 用其 html node
            // 跑一遍 CSS → ApplySvgShapeAttr。
            {
                auto varsRef = ctx.cssVars;  // shared_ptr — survives runtime rewrites
                const ui::css::Stylesheet* sheetPtr = &ctx.sheet;
                SvgWidget* rawSvg = svg;
                auto nodesCopy = shapeSourceNodes;
                auto inheritedCopy = shapeInheritedAttrs;
                rawSvg->recomputeShapes = [rawSvg, sheetPtr, varsRef, nodesCopy, inheritedCopy]() {
                    // Build live ancestor chain (mirrors recomputeStyle for
                    // regular widgets so descendant selectors stay correct
                    // through dynamic-class toggles + :hover/:focus).
                    // Read CurrentStateBits() instead of lastStateBits — hover
                    // propagation refreshes widgets in unordered set order, so
                    // a parent's lastStateBits may not have updated yet when
                    // we get here. CurrentStateBits reads hovered/pressed/...
                    // directly from the widget's bool members which ARE set
                    // before RefreshCssState() is called.
                    std::vector<ui::css::MatchNode> chain;
                    for (Widget* p = rawSvg->Parent(); p; p = p->Parent()) {
                        ui::css::MatchNode pm;
                        pm.tag = p->cssTag;
                        pm.id  = p->id;
                        pm.classes = p->classList;
                        for (const auto& c : p->dynamicClasses) pm.classes.push_back(c);
                        pm.stateBits = p->CurrentStateBits();
                        chain.push_back(std::move(pm));
                    }
                    for (size_t i = 0; i < chain.size(); ++i) {
                        chain[i].parent = (i + 1 < chain.size()) ? &chain[i + 1] : nullptr;
                    }
                    ui::css::MatchNode self;
                    self.tag = rawSvg->cssTag;
                    self.id  = rawSvg->id;
                    self.classes = rawSvg->classList;
                    for (const auto& c : rawSvg->dynamicClasses) self.classes.push_back(c);
                    self.stateBits = rawSvg->CurrentStateBits();
                    self.parent = chain.empty() ? nullptr : &chain[0];

                    auto& shapes = rawSvg->Shapes();
                    for (size_t i = 0; i < shapes.size() && i < nodesCopy.size(); ++i) {
                        const ui::uix::Node* shapeNode = nodesCopy[i];
                        if (!shapeNode) continue;
                        // Reset to presentation-attr baseline so CSS overrides
                        // apply on top deterministically (otherwise a removed
                        // CSS rule's value would stick).
                        SvgShape fresh;
                        fresh.kind = shapes[i].kind;
                        fresh.points = shapes[i].points;       // geometry preserved
                        fresh.pathData = shapes[i].pathData;
                        // Inherited presentation attrs (lowest priority) first,
                        // then shape's own — mirrors compile-time cascade order.
                        if (i < inheritedCopy.size()) {
                            for (const auto& [n, v] : inheritedCopy[i]) {
                                ApplySvgShapeAttr(fresh, n, v);
                            }
                        }
                        for (const auto& a : shapeNode->attrs) {
                            if (a.kind != ui::uix::AttrKind::Static) continue;
                            ApplySvgShapeAttr(fresh, a.name, a.rawValue);
                        }
                        // CSS cascade with the live MatchNode chain.
                        ui::css::MatchNode pm;
                        pm.tag = shapeNode->tag;
                        for (const auto& a : shapeNode->attrs) {
                            if (a.kind != ui::uix::AttrKind::Static) continue;
                            if (a.name == "id") pm.id = a.rawValue;
                            else if (a.name == "class") {
                                std::istringstream iss(a.rawValue);
                                std::string tok;
                                while (iss >> tok) pm.classes.push_back(tok);
                            } else pm.attrs[a.name] = a.rawValue;
                        }
                        pm.parent = &self;
                        std::vector<ui::css::Declaration> inlineDecls;
                        for (const auto& a : shapeNode->attrs) {
                            if (a.kind == ui::uix::AttrKind::Static && a.name == "style") {
                                inlineDecls = ui::css::ParseInlineStyle(a.rawValue);
                                break;
                            }
                        }
                        auto cs = ui::css::ComputeStyle(*sheetPtr, pm, inlineDecls, *varsRef);
                        static const char* kSvgProps[] = {
                            "fill","stroke","fill-opacity","stroke-opacity",
                            "stroke-width","stroke-dasharray",
                            "stroke-linecap","stroke-linejoin","opacity",
                        };
                        for (const char* p : kSvgProps) {
                            if (cs.Has(p)) ApplySvgShapeAttr(fresh, p, cs.Get(p));
                        }
                        // 把运行时反应式 binding (:fill/:stroke 等) 设的值补回 ——
                        // 最高优先级, 覆盖 static/CSS。否则 hover/state 重建 shape 会
                        // 丢掉 binding 值 (反应式 SVG 图标一 hover 就透明)。
                        rawSvg->ReapplyBoundAttrs(i, fresh);
                        shapes[i] = std::move(fresh);
                    }
                };
            }
        }
    }

    // ---- Compile attrs ----
    // (v-if is handled above via CompiledConditional; v-for above via CompiledLoop.)
    std::string vModelTarget;

    for (const auto& a : node.attrs) {
        switch (a.kind) {
            case ui::uix::AttrKind::Static:
                // id handled in factory; class used for matching only
                break;

            case ui::uix::AttrKind::Bind: {
                if (a.rawValue.empty()) break;
                CompiledBinding cb;
                cb.target = w.get();
                cb.property = a.name;
                cb.sourceJs = a.rawValue;
                ctx.out->bindings.push_back(std::move(cb));
                break;
            }

            case ui::uix::AttrKind::Event: {
                if (a.rawValue.empty()) break;
                CompiledEvent ev;
                ev.target = w.get();
                ev.event = a.name;
                ev.sourceJs = a.rawValue;
                ctx.out->events.push_back(std::move(ev));
                break;
            }

            case ui::uix::AttrKind::Directive: {
                if (a.name == "if") {
                    // Handled at the top of CompileElement via CompiledConditional.
                } else if (a.name == "for") {
                    // Handled at the top of CompileElement via CompiledLoop.
                } else if (a.name == "model") {
                    vModelTarget = a.rawValue;
                } else if (a.name == "show") {
                    if (a.rawValue.empty()) break;
                    CompiledBinding cb;
                    cb.target = w.get();
                    cb.property = "visible";
                    cb.sourceJs = a.rawValue;
                    ctx.out->bindings.push_back(std::move(cb));
                } else {
                    ctx.out->errors.push_back("unknown directive v-" + a.name);
                }
                break;
            }
        }
    }

    // v-model → :value + @change writing to target name
    if (!vModelTarget.empty()) {
        CompiledBinding cb;
        cb.target = w.get();
        cb.property = "value";
        cb.sourceJs = vModelTarget;
        ctx.out->bindings.push_back(std::move(cb));

        CompiledModelWrite mw;
        mw.target = w.get();
        mw.propertyName = vModelTarget;
        ctx.out->modelWrites.push_back(std::move(mw));
    }

    // ---- Text content binding (if any interpolation) ----
    bool hasInterp = false;
    (void)ExtractStaticText(node, hasInterp);
    if (hasInterp) {
        CompiledBinding cb;
        cb.target = w.get();
        cb.property = "text";
        cb.sourceJs = BuildTextSourceJs(node);
        ctx.out->bindings.push_back(std::move(cb));
    }

    // Attach to parent
    if (parentWidget) {
        parentWidget->AddChild(w);
    } else {
        ctx.out->root = w;
    }

    // ---- Recurse children ----
    // <select>/<svg>/<tabs> children are consumed above — skip default compile.
    if (!isSelect && !isSvg && !isTabs) {
        // Inline-text tags can't host nested element children. core-ui has no
        // inline text flow — each <label>/<span> compiles to an independent
        // widget rect, so nesting causes silent visual overlap. Catch this at
        // compile time, push a clear error with the offender's line/col + a
        // copy-pasteable fix, and SKIP the child compile so only the outer
        // label's own text renders (no overlay garbage).
        const std::string& t = node.tag;
        // Text widgets that compile to LabelWidget — any nested element will
        // overlap. <p> is multi-line capable (wrap=true) but still single
        // LabelWidget under the hood, so same problem applies.
        bool isInlineText = (t == "label" || t == "span" || t == "small" ||
                             t == "strong" || t == "em" || t == "a" ||
                             t == "p");
        bool reportedNest = false;
        if (isInlineText) {
            for (const auto& c : node.children) {
                if (c->kind != ui::uix::NodeKind::Element) continue;
                if (!reportedNest) {
                    std::ostringstream msg;
                    msg << "HTML: nested inline element <" << c->tag
                        << "> inside <" << t << "> at line " << c->line
                        << ", col " << c->col
                        << ". core-ui has no inline text flow — each text tag "
                           "(label/span/small/strong/em/a/p) compiles to its "
                           "own widget rect, so nesting causes visual overlap. "
                           "Skipping nested element. Fix: split into siblings "
                           "inside a flex row:\n"
                        << "  <div style=\"flex-direction:row; align-items:baseline; gap:6px\">\n"
                        << "    <" << t << " ...>main text</" << t << ">\n"
                        << "    <" << c->tag << " ...>nested text</" << c->tag << ">\n"
                        << "  </div>";
                    ctx.out->errors.push_back(msg.str());
                    reportedNest = true;
                }
            }
        }
        if (!reportedNest) {
            for (const auto& c : node.children) {
                if (c->kind == ui::uix::NodeKind::Element) {
                    CompileElement(ctx, *c, &m, w.get());
                }
            }
        }
    }

    // ---- <ScrollView> post-process ----
    // Children were AddChild'd during recursion. ScrollViewWidget expects a
    // single content widget via SetContent().
    //
    // Build 109 (L29): 不论 kids.size() **总是**包 wrapper VBox + patch
    // v-if/v-for conditional 的 parentWidget = wrapper. 之前 size==1 时直接
    // SetContent(only) + size==0 时啥都不做, 导致 caller 在 ScrollView 内
    // 混用 v-if/v-for sibling 时 layout 错乱: conditional 注册时记的
    // parentWidget 仍是 sv (compiler 阶段记的), mount 时 InsertChild 到
    // sv.children 而不是 wrapper, ScrollView 同时持 wrapper + N 个 mounted
    // panel, layout 算错 (实测 GuoheView settings 窗 4-panel ScrollView 显示
    // shortcuts panel 抢 general panel 的 viewport).
    //
    // 现在 wrapper 总是收纳所有 sibling, sv.children 永远只有 wrapper, v-if
    // /v-for mount 时 InsertChild 到 wrapper, 一致.
    //
    // 行为变化: 单 child ScrollView 现在多一层 VBox wrapper (之前 only widget
    // 直接是 sv child). VBoxWidget 默认无 margin/padding/border/bg, 视觉无差;
    // CPU 多一次 DoLayout 调用, 可忽略.
    if (auto* sv = dynamic_cast<ScrollViewWidget*>(w.get())) {
        auto& kids = w->Children();
        auto wrapper = std::make_shared<VBoxWidget>();
        // Move kids into wrapper (copy pointers first, then re-parent — kids
        // is a reference to sv.children_ which SetContent will mutate).
        std::vector<WidgetPtr> copy = kids;
        for (auto& c : copy) wrapper->AddChild(c);
        sv->SetContent(wrapper);  // SetContent clears children_ and AddChild(wrapper)

        // patch conditionals / loops parentWidget = sv → wrapper. v-if/v-for
        // mount runtime 在 page_state.cpp:1380 处 InsertChild 到 parentWidget,
        // 我们这里改成 wrapper 才能让 mounted panel 跟 sv 的 sibling 一起
        // 落在 wrapper 内, ScrollView 永远只看到 wrapper 一个 child.
        Widget* wrapper_raw = wrapper.get();
        for (auto& cd : ctx.out->conditionals) {
            if (cd.parentWidget == sv) cd.parentWidget = wrapper_raw;
        }
        for (auto& lp : ctx.out->loops) {
            if (lp.parentWidget == sv) lp.parentWidget = wrapper_raw;
        }
    }
}

// Format a D2D1_COLOR_F as #rrggbb when fully opaque, #rrggbbaa otherwise.
// Theme tokens like sidebarItemHover use semi-transparent overlays (e.g.
// rgba(255,255,255,0.08) in dark mode); dropping the alpha here would
// surface them as opaque #ffffff in CSS, which is exactly the "white
// hover" bug. ui::css::ParseColor already accepts 8-char hex.
std::string FormatHex(const D2D1_COLOR_F& c) {
    auto to8 = [](float v) {
        int n = (int)(v * 255.0f + 0.5f);
        if (n < 0) n = 0; if (n > 255) n = 255; return n;
    };
    char buf[12];
    int a = to8(c.a);
    if (a >= 255) {
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", to8(c.r), to8(c.g), to8(c.b));
    } else {
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x",
                      to8(c.r), to8(c.g), to8(c.b), a);
    }
    return buf;
}

// Collect :root vars from the stylesheet, then prepend library-provided
// theme tokens. .uix authors can write `background: var(--bg)`,
// `color: var(--fg)` etc. and the library overwrites these in place when
// `ui_theme_set_mode` flips Light/Dark, so a single CSS source covers both
// modes — no `.dark`-prefixed duplicate rules required.
//
// User-defined :root vars win (the `has(...)` guards skip the library
// default), so embedders can pin a token to a specific brand value if
// needed.
std::vector<std::pair<std::string, std::string>>
CollectVarsWithTheme(const ui::css::Stylesheet& sheet) {
    auto vars = ui::css::CollectRootVars(sheet);
    auto has = [&](const char* name) {
        for (auto& kv : vars) if (kv.first == name) return true;
        return false;
    };
    auto& T = ::theme::Current();

    // Brand / accent
    if (!has("--accent"))           vars.emplace_back("--accent",          FormatHex(T.accent));
    if (!has("--accent-hover"))     vars.emplace_back("--accent-hover",    FormatHex(T.accentHover));
    if (!has("--accent-press"))     vars.emplace_back("--accent-press",    FormatHex(T.accentPress));
    if (!has("--accent-text"))      vars.emplace_back("--accent-text",     FormatHex(T.accentText));
    if (!has("--accent-selected"))  vars.emplace_back("--accent-selected", FormatHex(T.accentSelected));

    // Window / content surfaces
    if (!has("--window-bg"))        vars.emplace_back("--window-bg",       FormatHex(T.windowBg));
    if (!has("--window-border"))    vars.emplace_back("--window-border",   FormatHex(T.windowBorder));
    if (!has("--bg"))               vars.emplace_back("--bg",              FormatHex(T.contentBg));
    if (!has("--bg-2"))             vars.emplace_back("--bg-2",            FormatHex(T.background2));
    if (!has("--bg-3"))             vars.emplace_back("--bg-3",            FormatHex(T.background3));
    if (!has("--bg-4"))             vars.emplace_back("--bg-4",            FormatHex(T.background4));
    if (!has("--bg-elevated"))      vars.emplace_back("--bg-elevated",     FormatHex(T.elevatedBg));

    // Text foregrounds
    if (!has("--fg"))               vars.emplace_back("--fg",              FormatHex(T.foreground1));
    if (!has("--fg-2"))             vars.emplace_back("--fg-2",            FormatHex(T.foreground2));
    if (!has("--fg-3"))             vars.emplace_back("--fg-3",            FormatHex(T.foreground3));
    if (!has("--fg-4"))             vars.emplace_back("--fg-4",            FormatHex(T.foreground4));
    if (!has("--fg-on-accent"))     vars.emplace_back("--fg-on-accent",    FormatHex(T.foregroundOnBrand));

    // Strokes / dividers
    if (!has("--border"))           vars.emplace_back("--border",          FormatHex(T.divider));
    if (!has("--border-subtle"))    vars.emplace_back("--border-subtle",   FormatHex(T.dividerSubtle));

    // Sidebar
    if (!has("--sidebar-bg"))       vars.emplace_back("--sidebar-bg",      FormatHex(T.sidebarBg));
    if (!has("--sidebar-text"))     vars.emplace_back("--sidebar-text",    FormatHex(T.sidebarText));
    if (!has("--sidebar-hover"))    vars.emplace_back("--sidebar-hover",   FormatHex(T.sidebarItemHover));

    // Input
    if (!has("--input-bg"))         vars.emplace_back("--input-bg",        FormatHex(T.inputBg));
    if (!has("--input-border"))     vars.emplace_back("--input-border",    FormatHex(T.inputBorder));
    if (!has("--input-border-hover"))
                                     vars.emplace_back("--input-border-hover",  FormatHex(T.inputBorderHover));
    if (!has("--input-border-focus"))
                                     vars.emplace_back("--input-border-focus",  FormatHex(T.inputBorderFocus));

    // Card / surface
    if (!has("--card-bg"))          vars.emplace_back("--card-bg",         FormatHex(T.cardBg));
    if (!has("--card-border"))      vars.emplace_back("--card-border",     FormatHex(T.cardBorder));

    // Buttons (subtle background)
    if (!has("--btn-bg"))           vars.emplace_back("--btn-bg",          FormatHex(T.btnNormal));
    if (!has("--btn-hover"))        vars.emplace_back("--btn-hover",       FormatHex(T.btnHover));
    if (!has("--btn-press"))        vars.emplace_back("--btn-press",       FormatHex(T.btnPress));
    if (!has("--btn-text"))         vars.emplace_back("--btn-text",        FormatHex(T.btnText));

    // Disabled
    if (!has("--disabled-bg"))      vars.emplace_back("--disabled-bg",     FormatHex(T.disabledBg));
    if (!has("--disabled-text"))    vars.emplace_back("--disabled-text",   FormatHex(T.disabledText));

    return vars;
}

}  // namespace

// Public counterpart of CollectVarsWithTheme — rewrites the library-owned
// keys in place. PageState calls this from a theme-change notifier.
void RebuildThemeVars(std::vector<std::pair<std::string, std::string>>& vars) {
    static const char* kThemeKeys[] = {
        "--accent","--accent-hover","--accent-press","--accent-text","--accent-selected",
        "--window-bg","--window-border","--bg","--bg-2","--bg-3","--bg-4","--bg-elevated",
        "--fg","--fg-2","--fg-3","--fg-4","--fg-on-accent",
        "--border","--border-subtle",
        "--sidebar-bg","--sidebar-text","--sidebar-hover",
        "--input-bg","--input-border","--input-border-hover","--input-border-focus",
        "--card-bg","--card-border",
        "--btn-bg","--btn-hover","--btn-press","--btn-text",
        "--disabled-bg","--disabled-text",
    };
    auto isThemeKey = [](const std::string& k) {
        for (const char* tk : kThemeKeys) if (k == tk) return true;
        return false;
    };
    vars.erase(std::remove_if(vars.begin(), vars.end(),
                              [&](const auto& kv) { return isThemeKey(kv.first); }),
               vars.end());
    auto& T = ::theme::Current();
    auto add = [&](const char* k, const D2D1_COLOR_F& c) {
        vars.emplace_back(k, FormatHex(c));
    };
    add("--accent",          T.accent);
    add("--accent-hover",    T.accentHover);
    add("--accent-press",    T.accentPress);
    add("--accent-text",     T.accentText);
    add("--accent-selected", T.accentSelected);
    add("--window-bg",       T.windowBg);
    add("--window-border",   T.windowBorder);
    add("--bg",              T.contentBg);
    add("--bg-2",            T.background2);
    add("--bg-3",            T.background3);
    add("--bg-4",            T.background4);
    add("--bg-elevated",     T.elevatedBg);
    add("--fg",              T.foreground1);
    add("--fg-2",            T.foreground2);
    add("--fg-3",            T.foreground3);
    add("--fg-4",            T.foreground4);
    add("--fg-on-accent",    T.foregroundOnBrand);
    add("--border",          T.divider);
    add("--border-subtle",   T.dividerSubtle);
    add("--sidebar-bg",      T.sidebarBg);
    add("--sidebar-text",    T.sidebarText);
    add("--sidebar-hover",   T.sidebarItemHover);
    add("--input-bg",        T.inputBg);
    add("--input-border",    T.inputBorder);
    add("--input-border-hover", T.inputBorderHover);
    add("--input-border-focus", T.inputBorderFocus);
    add("--card-bg",         T.cardBg);
    add("--card-border",     T.cardBorder);
    add("--btn-bg",          T.btnNormal);
    add("--btn-hover",       T.btnHover);
    add("--btn-press",       T.btnPress);
    add("--btn-text",        T.btnText);
    add("--disabled-bg",     T.disabledBg);
    add("--disabled-text",   T.disabledText);
}

CompiledPage Compile(const ui::uix::Node& root,
                     const ui::css::Stylesheet& sheet) {
    CompiledPage page;
    page.cssVars = std::make_shared<std::vector<std::pair<std::string, std::string>>>(
        CollectVarsWithTheme(sheet));
    CompilerCtx ctx{ sheet, page.cssVars, &page, false, false };
    CompileElement(ctx, root, nullptr, nullptr);
    return page;
}

CompiledPage CompileIterationTemplate(const ui::uix::Node& root,
                                      const ui::css::Stylesheet& sheet,
                                      std::shared_ptr<std::vector<std::pair<std::string, std::string>>> parentVars) {
    CompiledPage page;
    page.cssVars = parentVars ? parentVars
                              : std::make_shared<std::vector<std::pair<std::string, std::string>>>(
                                    CollectVarsWithTheme(sheet));
    CompilerCtx ctx{ sheet, page.cssVars, &page, true, false };
    CompileElement(ctx, root, nullptr, nullptr);
    return page;
}

CompiledPage CompileConditionalTemplate(const ui::uix::Node& root,
                                        const ui::css::Stylesheet& sheet,
                                        std::shared_ptr<std::vector<std::pair<std::string, std::string>>> parentVars) {
    CompiledPage page;
    page.cssVars = parentVars ? parentVars
                              : std::make_shared<std::vector<std::pair<std::string, std::string>>>(
                                    CollectVarsWithTheme(sheet));
    CompilerCtx ctx{ sheet, page.cssVars, &page, false, true };
    CompileElement(ctx, root, nullptr, nullptr);
    return page;
}

}  // namespace ui::page
