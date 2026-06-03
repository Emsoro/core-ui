#include "svg_style_inliner.h"

#include <windows.h>

#include <cctype>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace ui {

namespace {

inline bool IsWs(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

inline bool IsClassNameChar(unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_';
}

/* 找以 start ('<' 位置) 起的 tag 结束 '>' 位置. 引号内的 '>' 不算 (XML 规范).
 * 找不到返 npos. */
size_t FindTagEnd(const std::string& s, size_t start) {
    size_t i = start + 1;
    char quote = 0;
    while (i < s.size()) {
        char c = s[i];
        if (quote) {
            if (c == quote) quote = 0;
        } else {
            if (c == '"' || c == '\'') quote = c;
            else if (c == '>') return i;
        }
        i++;
    }
    return std::string::npos;
}

/* 在 [tag_start, tag_end] 范围内 (s[tag_start]=='<', s[tag_end]=='>') 找名为
 * name 的属性. 返 true + value 区间 [vstart, vend) (不含开闭引号).
 * 只匹配 ASCII attr name 大小写敏感 (XML spec). */
bool FindAttr(const std::string& s, size_t tag_start, size_t tag_end,
              const char* name, size_t& vstart, size_t& vend) {
    size_t name_len = std::strlen(name);
    size_t i = tag_start + 1;
    // 跳元素名
    while (i < tag_end && !IsWs((unsigned char)s[i]) && s[i] != '/' && s[i] != '>') i++;

    while (i < tag_end) {
        while (i < tag_end && IsWs((unsigned char)s[i])) i++;
        if (i >= tag_end) break;
        if (s[i] == '/') { i++; continue; }

        size_t an_s = i;
        while (i < tag_end && s[i] != '=' && !IsWs((unsigned char)s[i])
               && s[i] != '/' && s[i] != '>') i++;
        size_t an_e = i;
        if (an_s == an_e) { i++; continue; }  // 不应该, 保险跳一步

        // 跳到 '=' (允许 attr= value 中间空白)
        while (i < tag_end && IsWs((unsigned char)s[i])) i++;
        if (i >= tag_end || s[i] != '=') continue;  // 无值属性, 跳过
        i++;  // skip '='
        while (i < tag_end && IsWs((unsigned char)s[i])) i++;
        if (i >= tag_end) break;

        size_t v_s, v_e;
        char qc = s[i];
        if (qc == '"' || qc == '\'') {
            i++;
            v_s = i;
            while (i < tag_end && s[i] != qc) i++;
            v_e = i;
            if (i < tag_end) i++;  // skip closing quote
        } else {
            v_s = i;
            while (i < tag_end && !IsWs((unsigned char)s[i])
                   && s[i] != '/' && s[i] != '>') i++;
            v_e = i;
        }

        if (an_e - an_s == name_len &&
            std::memcmp(s.data() + an_s, name, name_len) == 0) {
            vstart = v_s;
            vend = v_e;
            return true;
        }
    }
    return false;
}

// CSS scanner: 跳空白 + C 风格注释.
void SkipCssWs(const std::string& css, size_t& i) {
    while (i < css.size()) {
        if (IsWs((unsigned char)css[i])) { i++; }
        else if (i + 1 < css.size() && css[i] == '/' && css[i + 1] == '*') {
            i += 2;
            while (i + 1 < css.size() && !(css[i] == '*' && css[i + 1] == '/')) i++;
            if (i + 1 < css.size()) i += 2;
        } else break;
    }
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && IsWs((unsigned char)s[a])) a++;
    while (b > a && IsWs((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

/* 从 SVG 文本中抽出所有 <style ...>...</style> 块的 CSS 内容拼接.
 * 剥掉 <![CDATA[ ... ]]>. */
std::string ExtractStyleBlocks(const std::string& svg) {
    std::string css;
    size_t pos = 0;
    while (true) {
        size_t s = svg.find("<style", pos);
        if (s == std::string::npos) break;
        // 确认 <style 后是空白 / '>' (避免 <styleX> 误识)
        size_t after = s + 6;
        if (after < svg.size() && svg[after] != ' ' && svg[after] != '\t'
            && svg[after] != '\n' && svg[after] != '\r' && svg[after] != '>'
            && svg[after] != '/') {
            pos = after;
            continue;
        }
        size_t gt = FindTagEnd(svg, s);
        if (gt == std::string::npos) break;
        // 自闭合 <style/> 没内容
        if (gt > 0 && svg[gt - 1] == '/') { pos = gt + 1; continue; }
        size_t end = svg.find("</style>", gt + 1);
        if (end == std::string::npos) break;
        std::string block = svg.substr(gt + 1, end - gt - 1);
        // 剥 CDATA
        size_t cda = block.find("<![CDATA[");
        if (cda != std::string::npos) {
            size_t cdb = block.find("]]>", cda + 9);
            if (cdb != std::string::npos) {
                block = block.substr(cda + 9, cdb - cda - 9);
            }
        }
        css += block;
        css += '\n';
        pos = end + 8;  // strlen("</style>")
    }
    return css;
}

/* 解析 CSS 文本 → ".classname" 选择器 → properties 字符串. 不接受复杂选择器. */
void ParseCssRules(const std::string& css,
                   std::unordered_map<std::string, std::string>& rules) {
    size_t i = 0;
    while (i < css.size()) {
        SkipCssWs(css, i);
        if (i >= css.size()) break;

        // 读 selector list (到 '{')
        size_t sel_s = i;
        while (i < css.size() && css[i] != '{' && css[i] != '}') {
            // 在 selector 里也可能有 /* */ 注释, 简单跳过
            if (i + 1 < css.size() && css[i] == '/' && css[i + 1] == '*') {
                i += 2;
                while (i + 1 < css.size() && !(css[i] == '*' && css[i + 1] == '/')) i++;
                if (i + 1 < css.size()) i += 2;
            } else {
                i++;
            }
        }
        if (i >= css.size()) break;
        if (css[i] == '}') { i++; continue; }  // stray

        size_t sel_e = i;
        i++;  // skip '{'

        // 读 properties (到匹配的 '}'), 处理嵌套 (@media etc.)
        size_t prop_s = i;
        int depth = 1;
        while (i < css.size() && depth > 0) {
            char c = css[i];
            if (c == '{') depth++;
            else if (c == '}') {
                depth--;
                if (depth == 0) break;
            }
            i++;
        }
        if (depth != 0) break;
        size_t prop_e = i;
        i++;  // skip '}'

        // 跳 @ 开头的 at-rule (整段忽略)
        size_t s = sel_s;
        while (s < sel_e && IsWs((unsigned char)css[s])) s++;
        if (s < sel_e && css[s] == '@') continue;

        std::string props = Trim(css.substr(prop_s, prop_e - prop_s));
        if (props.empty()) continue;

        // 按逗号分割 selector
        size_t cs = sel_s;
        while (cs < sel_e) {
            while (cs < sel_e && IsWs((unsigned char)css[cs])) cs++;
            size_t ce = cs;
            while (ce < sel_e && css[ce] != ',') ce++;
            size_t tail = ce;
            while (tail > cs && IsWs((unsigned char)css[tail - 1])) tail--;

            // 只接受 ".classname" — 必须 '.' + 全部 IsClassNameChar
            if (tail > cs + 1 && css[cs] == '.') {
                bool ok = true;
                for (size_t k = cs + 1; k < tail; k++) {
                    if (!IsClassNameChar((unsigned char)css[k])) { ok = false; break; }
                }
                if (ok) {
                    std::string cls(css.data() + cs + 1, tail - cs - 1);
                    auto it = rules.find(cls);
                    if (it == rules.end()) {
                        rules.emplace(std::move(cls), props);
                    } else {
                        // 后定义追加 → CSS 语义: 后者覆盖前者 (在 inline style 里
                        // 同样是后者覆盖前者). 添加分隔符 ';' 避免上一条没收尾.
                        if (!it->second.empty() && it->second.back() != ';') {
                            it->second += ';';
                        }
                        it->second += props;
                    }
                }
            }
            if (ce < sel_e) cs = ce + 1; else break;
        }
    }
}

}  // namespace

std::string InlineSvgStyleClasses(const std::string& svg_xml) {
    // 短路: 大多数 icon SVG 没 <style> 块
    if (svg_xml.find("<style") == std::string::npos) {
        return svg_xml;
    }

    std::string css = ExtractStyleBlocks(svg_xml);
    if (css.empty()) return svg_xml;

    std::unordered_map<std::string, std::string> rules;
    ParseCssRules(css, rules);
    if (rules.empty()) return svg_xml;

    std::string out;
    out.reserve(svg_xml.size() + svg_xml.size() / 8);  // ~12% 增长预算

    size_t i = 0;
    const size_t N = svg_xml.size();
    while (i < N) {
        if (svg_xml[i] != '<') {
            out += svg_xml[i++];
            continue;
        }

        size_t tag_s = i;
        size_t tag_e = FindTagEnd(svg_xml, tag_s);
        if (tag_e == std::string::npos) {
            // 损坏 SVG: dump 剩下原样
            out.append(svg_xml, tag_s, std::string::npos);
            break;
        }

        // 跳过特殊 markup: <!-- comment -->, <![CDATA[..]]>, <?xml..?>
        // 这些里面的 '>' 已经被 FindTagEnd 错误识别? 不会 — FindTagEnd 只看
        // 引号. comment 里没引号但有 '>'? 是的, `<!-- > -->` 会让 FindTagEnd
        // 提前停在第一个 '>'. 但 comment 不可能含 class= 属性, 退一步说就算
        // 误识 tag, FindAttr 找 class 也找不到. 不影响正确性, 只是多扫一次.
        // 为简单先不特殊处理.

        size_t cls_vs, cls_ve;
        bool has_class = FindAttr(svg_xml, tag_s, tag_e, "class", cls_vs, cls_ve);
        if (!has_class) {
            out.append(svg_xml, tag_s, tag_e - tag_s + 1);
            i = tag_e + 1;
            continue;
        }

        // 累积该元素所有 class 引用的 props (空格分隔)
        std::string props_acc;
        size_t cn = cls_vs;
        while (cn < cls_ve) {
            while (cn < cls_ve && IsWs((unsigned char)svg_xml[cn])) cn++;
            size_t ce = cn;
            while (ce < cls_ve && !IsWs((unsigned char)svg_xml[ce])) ce++;
            if (ce > cn) {
                std::string one(svg_xml.data() + cn, ce - cn);
                auto it = rules.find(one);
                if (it != rules.end()) {
                    if (!props_acc.empty() && props_acc.back() != ';') {
                        props_acc += ';';
                    }
                    props_acc += it->second;
                }
            }
            cn = ce;
        }

        if (props_acc.empty()) {
            out.append(svg_xml, tag_s, tag_e - tag_s + 1);
            i = tag_e + 1;
            continue;
        }

        // 已有 style="..." 吗?
        size_t sty_vs, sty_ve;
        bool has_style = FindAttr(svg_xml, tag_s, tag_e, "style", sty_vs, sty_ve);

        if (has_style) {
            // 在 style 值开头插 stylesheet props + ';' (原 inline 在后, 优先级正确)
            out.append(svg_xml, tag_s, sty_vs - tag_s);  // <tag ... style="
            out += props_acc;
            if (props_acc.back() != ';') out += ';';
            out.append(svg_xml, sty_vs, tag_e - sty_vs + 1);  // 原值 + "..." + 后续 + >
        } else {
            // 没 style, 在 tag 闭合 (`/>` 或 `>`) 前插入 ` style="props"`
            // 找闭合前的实际位置: 跳过 trailing whitespace 和 '/'
            size_t insert_at = tag_e;  // 此处是 '>'
            while (insert_at > tag_s + 1
                   && IsWs((unsigned char)svg_xml[insert_at - 1])) insert_at--;
            if (insert_at > tag_s + 1 && svg_xml[insert_at - 1] == '/') {
                insert_at--;
                while (insert_at > tag_s + 1
                       && IsWs((unsigned char)svg_xml[insert_at - 1])) insert_at--;
            }
            out.append(svg_xml, tag_s, insert_at - tag_s);
            out += " style=\"";
            out += props_acc;
            out += '"';
            out.append(svg_xml, insert_at, tag_e - insert_at + 1);
        }
        i = tag_e + 1;
    }

    return out;
}

namespace {

/* 给定标签区间 [tag_s..tag_e] (s=='<', e=='>'), 返回插入新属性的位置:
 * 闭合 '>' 或 '/>' 之前 (跳过尾随空白和 '/'). 跟 InlineSvgStyleClasses 里
 * 没有 style 时的插入逻辑一致. */
size_t AttrInsertPos(const std::string& s, size_t tag_s, size_t tag_e) {
    size_t insert_at = tag_e;  // 此处是 '>'
    while (insert_at > tag_s + 1 && IsWs((unsigned char)s[insert_at - 1])) insert_at--;
    if (insert_at > tag_s + 1 && s[insert_at - 1] == '/') {
        insert_at--;
        while (insert_at > tag_s + 1 && IsWs((unsigned char)s[insert_at - 1])) insert_at--;
    }
    return insert_at;
}

/* 取标签元素名范围 [ns, ne). closing 置位表示是 `</name>` 闭合标签.
 * 注释 / CDATA / PI / DOCTYPE (`<!` / `<?` 开头) 返 false. */
bool TagElementName(const std::string& s, size_t tag_s, size_t tag_e,
                    size_t& ns, size_t& ne, bool& closing) {
    size_t i = tag_s + 1;
    closing = false;
    if (i < tag_e && s[i] == '/') { closing = true; i++; }
    if (i >= tag_e) return false;
    unsigned char c0 = (unsigned char)s[i];
    if (c0 == '!' || c0 == '?') return false;
    ns = i;
    while (i < tag_e && !IsWs((unsigned char)s[i]) && s[i] != '/' && s[i] != '>') i++;
    ne = i;
    return ne > ns;
}

bool NameEq(const std::string& s, size_t ns, size_t ne, const char* name) {
    size_t len = std::strlen(name);
    return (ne - ns == len) && std::memcmp(s.data() + ns, name, len) == 0;
}

/* 标签是否自闭合 (`.../>`), 容忍 '/' 前后空白. */
bool IsSelfClosing(const std::string& s, size_t ne, size_t tag_e) {
    size_t k = tag_e;  // '>'
    while (k > ne && IsWs((unsigned char)s[k - 1])) k--;
    return k > ne && s[k - 1] == '/';
}

}  // namespace

std::string NormalizeSvgRefsAndSymbols(const std::string& xml) {
    // 短路: 既无 href 又无 <symbol> 就没活干 (覆盖绝大多数 inline-fill 图标).
    const bool any_href   = xml.find("href") != std::string::npos;
    const bool any_symbol = xml.find("<symbol") != std::string::npos;
    if (!any_href && !any_symbol) return xml;

    static const char* kXlinkNs =
        " xmlns:xlink=\"http://www.w3.org/1999/xlink\"";

    std::string out;
    out.reserve(xml.size() + xml.size() / 16 + 64);

    int defs_depth = 0;                 // 当前嵌套 <defs> 深度 (含注入的)
    std::vector<bool> sym_wrapped;      // 每个未闭合 <symbol> 是否套了注入 <defs>

    size_t i = 0;
    const size_t N = xml.size();
    while (i < N) {
        if (xml[i] != '<') { out += xml[i++]; continue; }

        size_t tag_s = i;
        size_t tag_e = FindTagEnd(xml, tag_s);
        if (tag_e == std::string::npos) {
            out.append(xml, tag_s, std::string::npos);  // 损坏 SVG, 原样 dump
            break;
        }

        size_t ns, ne; bool closing;
        if (!TagElementName(xml, tag_s, tag_e, ns, ne, closing)) {
            out.append(xml, tag_s, tag_e - tag_s + 1);   // 注释 / CDATA / PI
            i = tag_e + 1;
            continue;
        }

        // ---- 闭合标签 ----
        if (closing) {
            if (NameEq(xml, ns, ne, "symbol")) {
                out += "</g>";
                if (!sym_wrapped.empty()) {
                    bool wrapped = sym_wrapped.back();
                    sym_wrapped.pop_back();
                    if (wrapped) { out += "</defs>"; if (defs_depth > 0) defs_depth--; }
                }
            } else {
                if (NameEq(xml, ns, ne, "defs") && defs_depth > 0) defs_depth--;
                out.append(xml, tag_s, tag_e - tag_s + 1);
            }
            i = tag_e + 1;
            continue;
        }

        // ---- 起始 / 自闭合标签 ----
        const bool is_symbol = NameEq(xml, ns, ne, "symbol");
        const bool is_svg    = NameEq(xml, ns, ne, "svg");
        const bool is_defs   = NameEq(xml, ns, ne, "defs");
        const bool self_close = IsSelfClosing(xml, ne, tag_e);

        // 顶层 (defs_depth==0) 的 symbol 要套一层注入 <defs> 防止单独绘制.
        const bool wrap_symbol = is_symbol && (defs_depth == 0);
        if (wrap_symbol) { out += "<defs>"; defs_depth++; }

        // 计算要插入的属性: xlink:href 补全 + root svg 的 xmlns:xlink.
        std::string attr_ins;
        if (!is_symbol) {
            size_t hvs, hve, xvs, xve;
            bool has_href  = FindAttr(xml, tag_s, tag_e, "href", hvs, hve);
            bool has_xlink = FindAttr(xml, tag_s, tag_e, "xlink:href", xvs, xve);
            if (has_href && !has_xlink) {
                attr_ins += " xlink:href=\"";
                attr_ins.append(xml, hvs, hve - hvs);
                attr_ins += '"';
            }
        }
        if (is_svg) {
            size_t d0, d1;
            if (!FindAttr(xml, tag_s, tag_e, "xmlns:xlink", d0, d1)) attr_ins += kXlinkNs;
        }

        size_t insert_at = AttrInsertPos(xml, tag_s, tag_e);

        out.append(xml, tag_s, ns - tag_s);          // "<"  (闭合在上面已分流)
        if (is_symbol) out += 'g'; else out.append(xml, ns, ne - ns);  // 名 (symbol→g)
        out.append(xml, ne, insert_at - ne);         // 原属性串
        out += attr_ins;                             // 注入属性
        out.append(xml, insert_at, tag_e - insert_at + 1);  // 尾部 (含 '/' 和 '>')

        if (is_symbol && self_close && wrap_symbol) { out += "</defs>"; if (defs_depth > 0) defs_depth--; }

        if (is_defs && !self_close) defs_depth++;
        if (is_symbol && !self_close) sym_wrapped.push_back(wrap_symbol);

        i = tag_e + 1;
    }

    return out;
}

std::string LoadSvgWithInlinedStyles(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (1LL << 30)) {
        CloseHandle(h);
        return {};
    }
    std::string raw(static_cast<size_t>(sz.QuadPart), '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(h, raw.data(), static_cast<DWORD>(sz.QuadPart), &read, nullptr);
    CloseHandle(h);
    if (!ok || read != static_cast<DWORD>(sz.QuadPart)) return {};
    /* 两道纯文本预处理, 顺序无关:
     *   ① InlineSvgStyleClasses — <style>.class{} → inline style (L48)
     *   ② NormalizeSvgRefsAndSymbols — href→xlink:href + <symbol>→<defs><g> (L86)
     * 处理后的 xml 同时喂 D2D 原生路径和 ParseSvgIcon fallback. */
    return NormalizeSvgRefsAndSymbols(InlineSvgStyleClasses(raw));
}

}  // namespace ui
