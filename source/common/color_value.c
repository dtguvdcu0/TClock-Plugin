#include "color_value.h"

#include <stddef.h>

#define TCV_NAMED(name, hex) { name, 0x##hex }

typedef struct TCV_NAMED_COLOR {
    const char* name;
    DWORD rgb;
} TCV_NAMED_COLOR;

static const TCV_NAMED_COLOR g_tcvNamedColors[] = {
    TCV_NAMED("aliceblue", f0f8ff), TCV_NAMED("antiquewhite", faebd7),
    TCV_NAMED("aqua", 00ffff), TCV_NAMED("aquamarine", 7fffd4),
    TCV_NAMED("azure", f0ffff), TCV_NAMED("beige", f5f5dc),
    TCV_NAMED("bisque", ffe4c4), TCV_NAMED("black", 000000),
    TCV_NAMED("blanchedalmond", ffebcd), TCV_NAMED("blue", 0000ff),
    TCV_NAMED("blueviolet", 8a2be2), TCV_NAMED("brown", a52a2a),
    TCV_NAMED("burlywood", deb887), TCV_NAMED("cadetblue", 5f9ea0),
    TCV_NAMED("chartreuse", 7fff00), TCV_NAMED("chocolate", d2691e),
    TCV_NAMED("coral", ff7f50), TCV_NAMED("cornflowerblue", 6495ed),
    TCV_NAMED("cornsilk", fff8dc), TCV_NAMED("crimson", dc143c),
    TCV_NAMED("cyan", 00ffff), TCV_NAMED("darkblue", 00008b),
    TCV_NAMED("darkcyan", 008b8b), TCV_NAMED("darkgoldenrod", b8860b),
    TCV_NAMED("darkgray", a9a9a9), TCV_NAMED("darkgreen", 006400),
    TCV_NAMED("darkgrey", a9a9a9), TCV_NAMED("darkkhaki", bdb76b),
    TCV_NAMED("darkmagenta", 8b008b), TCV_NAMED("darkolivegreen", 556b2f),
    TCV_NAMED("darkorange", ff8c00), TCV_NAMED("darkorchid", 9932cc),
    TCV_NAMED("darkred", 8b0000), TCV_NAMED("darksalmon", e9967a),
    TCV_NAMED("darkseagreen", 8fbc8f), TCV_NAMED("darkslateblue", 483d8b),
    TCV_NAMED("darkslategray", 2f4f4f), TCV_NAMED("darkslategrey", 2f4f4f),
    TCV_NAMED("darkturquoise", 00ced1), TCV_NAMED("darkviolet", 9400d3),
    TCV_NAMED("deeppink", ff1493), TCV_NAMED("deepskyblue", 00bfff),
    TCV_NAMED("dimgray", 696969), TCV_NAMED("dimgrey", 696969),
    TCV_NAMED("dodgerblue", 1e90ff), TCV_NAMED("firebrick", b22222),
    TCV_NAMED("floralwhite", fffaf0), TCV_NAMED("forestgreen", 228b22),
    TCV_NAMED("fuchsia", ff00ff), TCV_NAMED("gainsboro", dcdcdc),
    TCV_NAMED("ghostwhite", f8f8ff), TCV_NAMED("gold", ffd700),
    TCV_NAMED("goldenrod", daa520), TCV_NAMED("gray", 808080),
    TCV_NAMED("green", 008000), TCV_NAMED("greenyellow", adff2f),
    TCV_NAMED("grey", 808080), TCV_NAMED("honeydew", f0fff0),
    TCV_NAMED("hotpink", ff69b4), TCV_NAMED("indianred", cd5c5c),
    TCV_NAMED("indigo", 4b0082), TCV_NAMED("ivory", fffff0),
    TCV_NAMED("khaki", f0e68c), TCV_NAMED("lavender", e6e6fa),
    TCV_NAMED("lavenderblush", fff0f5), TCV_NAMED("lawngreen", 7cfc00),
    TCV_NAMED("lemonchiffon", fffacd), TCV_NAMED("lightblue", add8e6),
    TCV_NAMED("lightcoral", f08080), TCV_NAMED("lightcyan", e0ffff),
    TCV_NAMED("lightgoldenrodyellow", fafad2), TCV_NAMED("lightgray", d3d3d3),
    TCV_NAMED("lightgreen", 90ee90), TCV_NAMED("lightgrey", d3d3d3),
    TCV_NAMED("lightpink", ffb6c1), TCV_NAMED("lightsalmon", ffa07a),
    TCV_NAMED("lightseagreen", 20b2aa), TCV_NAMED("lightskyblue", 87cefa),
    TCV_NAMED("lightslategray", 778899), TCV_NAMED("lightslategrey", 778899),
    TCV_NAMED("lightsteelblue", b0c4de), TCV_NAMED("lightyellow", ffffe0),
    TCV_NAMED("lime", 00ff00), TCV_NAMED("limegreen", 32cd32),
    TCV_NAMED("linen", faf0e6), TCV_NAMED("magenta", ff00ff),
    TCV_NAMED("maroon", 800000), TCV_NAMED("mediumaquamarine", 66cdaa),
    TCV_NAMED("mediumblue", 0000cd), TCV_NAMED("mediumorchid", ba55d3),
    TCV_NAMED("mediumpurple", 9370db), TCV_NAMED("mediumseagreen", 3cb371),
    TCV_NAMED("mediumslateblue", 7b68ee), TCV_NAMED("mediumspringgreen", 00fa9a),
    TCV_NAMED("mediumturquoise", 48d1cc), TCV_NAMED("mediumvioletred", c71585),
    TCV_NAMED("midnightblue", 191970), TCV_NAMED("mintcream", f5fffa),
    TCV_NAMED("mistyrose", ffe4e1), TCV_NAMED("moccasin", ffe4b5),
    TCV_NAMED("navajowhite", ffdead), TCV_NAMED("navy", 000080),
    TCV_NAMED("oldlace", fdf5e6), TCV_NAMED("olive", 808000),
    TCV_NAMED("olivedrab", 6b8e23), TCV_NAMED("orange", ffa500),
    TCV_NAMED("orangered", ff4500), TCV_NAMED("orchid", da70d6),
    TCV_NAMED("palegoldenrod", eee8aa), TCV_NAMED("palegreen", 98fb98),
    TCV_NAMED("paleturquoise", afeeee), TCV_NAMED("palevioletred", db7093),
    TCV_NAMED("papayawhip", ffefd5), TCV_NAMED("peachpuff", ffdab9),
    TCV_NAMED("peru", cd853f), TCV_NAMED("pink", ffc0cb),
    TCV_NAMED("plum", dda0dd), TCV_NAMED("powderblue", b0e0e6),
    TCV_NAMED("purple", 800080), TCV_NAMED("rebeccapurple", 663399),
    TCV_NAMED("red", ff0000), TCV_NAMED("rosybrown", bc8f8f),
    TCV_NAMED("royalblue", 4169e1), TCV_NAMED("saddlebrown", 8b4513),
    TCV_NAMED("salmon", fa8072), TCV_NAMED("sandybrown", f4a460),
    TCV_NAMED("seagreen", 2e8b57), TCV_NAMED("seashell", fff5ee),
    TCV_NAMED("sienna", a0522d), TCV_NAMED("silver", c0c0c0),
    TCV_NAMED("skyblue", 87ceeb), TCV_NAMED("slateblue", 6a5acd),
    TCV_NAMED("slategray", 708090), TCV_NAMED("slategrey", 708090),
    TCV_NAMED("snow", fffafa), TCV_NAMED("springgreen", 00ff7f),
    TCV_NAMED("steelblue", 4682b4), TCV_NAMED("tan", d2b48c),
    TCV_NAMED("teal", 008080), TCV_NAMED("thistle", d8bfd8),
    TCV_NAMED("tomato", ff6347), TCV_NAMED("turquoise", 40e0d0),
    TCV_NAMED("violet", ee82ee), TCV_NAMED("wheat", f5deb3),
    TCV_NAMED("white", ffffff), TCV_NAMED("whitesmoke", f5f5f5),
    TCV_NAMED("yellow", ffff00), TCV_NAMED("yellowgreen", 9acd32)
};

static void tcv_skip_space(const WCHAR** cursor)
{
    while (**cursor == L' ' || **cursor == L'\t' || **cursor == L'\r' || **cursor == L'\n') (*cursor)++;
}

static WCHAR tcv_lower(WCHAR ch)
{
    if (ch >= L'A' && ch <= L'Z') return (WCHAR)(ch + (L'a' - L'A'));
    return ch;
}

static int tcv_hex_digit(WCHAR ch)
{
    if (ch >= L'0' && ch <= L'9') return ch - L'0';
    ch = tcv_lower(ch);
    if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
    return -1;
}

static BOOL tcv_equal(const WCHAR* text, const char* ascii)
{
    int i;
    for (i = 0; ascii[i]; i++) {
        if (!text[i] || tcv_lower(text[i]) != (WCHAR)ascii[i]) return FALSE;
    }
    return text[i] == L'\0';
}

static BOOL tcv_parse_name(const WCHAR* text, COLORREF* color)
{
    size_t i;
    for (i = 0; i < sizeof(g_tcvNamedColors) / sizeof(g_tcvNamedColors[0]); i++) {
        if (tcv_equal(text, g_tcvNamedColors[i].name)) {
            DWORD rgb = g_tcvNamedColors[i].rgb;
            *color = RGB((BYTE)(rgb >> 16), (BYTE)(rgb >> 8), (BYTE)rgb);
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL tcv_read_uint(const WCHAR** cursor, int maxValue, int* value)
{
    const WCHAR* p = *cursor;
    int parsed = 0;
    if (*p < L'0' || *p > L'9') return FALSE;
    do {
        if (parsed > maxValue / 10) return FALSE;
        parsed = parsed * 10 + (*p - L'0');
        if (parsed > maxValue) return FALSE;
        p++;
    } while (*p >= L'0' && *p <= L'9');
    *cursor = p;
    *value = parsed;
    return TRUE;
}

static BOOL tcv_parse_rgb(const WCHAR** cursor, COLORREF* color)
{
    const WCHAR* p = *cursor;
    int red;
    int green;
    int blue;
    if (*p != L'r' && *p != L'R') return FALSE;
    p++;
    if (*p != L'g' && *p != L'G') return FALSE;
    p++;
    if (*p != L'b' && *p != L'B') return FALSE;
    p++;
    tcv_skip_space(&p);
    if (*p != L'(') return FALSE;
    p++;
    tcv_skip_space(&p);
    if (!tcv_read_uint(&p, 255, &red)) return FALSE;
    tcv_skip_space(&p);
    if (*p != L',') return FALSE;
    p++;
    tcv_skip_space(&p);
    if (!tcv_read_uint(&p, 255, &green)) return FALSE;
    tcv_skip_space(&p);
    if (*p != L',') return FALSE;
    p++;
    tcv_skip_space(&p);
    if (!tcv_read_uint(&p, 255, &blue)) return FALSE;
    tcv_skip_space(&p);
    if (*p != L')') return FALSE;
    p++;
    *cursor = p;
    *color = RGB(red, green, blue);
    return TRUE;
}

static BOOL tcv_parse_hex(const WCHAR** cursor, COLORREF* color)
{
    const WCHAR* p = *cursor;
    unsigned int digits[6];
    int count = 0;
    int i;
    if (*p != L'#') return FALSE;
    p++;
    while (count < 6 && tcv_hex_digit(*p) >= 0) digits[count++] = (unsigned int)tcv_hex_digit(*p++);
    if (count != 3 && count != 6) return FALSE;
    if (count == 3) {
        *color = RGB(digits[0] * 17, digits[1] * 17, digits[2] * 17);
    }
    else {
        for (i = 0; i < 6; i += 2) digits[i / 2] = digits[i] * 16 + digits[i + 1];
        *color = RGB(digits[0], digits[1], digits[2]);
    }
    *cursor = p;
    return TRUE;
}

BOOL tcv_parse_next(const WCHAR** cursor, COLORREF* color, TCV_KIND* kind)
{
    const WCHAR* p;
    WCHAR name[48];
    int i;
    int value;
    if (!cursor || !*cursor || !color) return FALSE;
    p = *cursor;
    tcv_skip_space(&p);
    if (*p == L'#') {
        if (!tcv_parse_hex(&p, color)) return FALSE;
        if (kind) *kind = TCV_KIND_HEX;
    }
    else if ((p[0] == L'r' || p[0] == L'R') &&
        (p[1] == L'g' || p[1] == L'G') &&
        (p[2] == L'b' || p[2] == L'B') &&
        (p[3] == L'(' || p[3] == L' ' || p[3] == L'\t' || p[3] == L'\r' || p[3] == L'\n')) {
        if (!tcv_parse_rgb(&p, color)) return FALSE;
        if (kind) *kind = TCV_KIND_RGB;
    }
    else if (*p >= L'0' && *p <= L'9') {
        if (!tcv_read_uint(&p, 0x00ffffff, &value)) return FALSE;
        *color = (COLORREF)value;
        if (kind) *kind = TCV_KIND_DECIMAL;
    }
    else {
        i = 0;
        while (((p[i] >= L'A' && p[i] <= L'Z') || (p[i] >= L'a' && p[i] <= L'z')) && i < (int)(sizeof(name) / sizeof(name[0])) - 1) {
            name[i] = p[i];
            i++;
        }
        if (i == 0 || (((p[i] >= L'A' && p[i] <= L'Z') || (p[i] >= L'a' && p[i] <= L'z')))) return FALSE;
        name[i] = L'\0';
        if (!tcv_parse_name(name, color)) return FALSE;
        p += i;
        if (kind) *kind = TCV_KIND_NAME;
    }
    *cursor = p;
    return TRUE;
}

BOOL tcv_parse(const WCHAR* text, COLORREF* color, TCV_KIND* kind)
{
    const WCHAR* p = text;
    TCV_KIND parsedKind;
    if (!text || !color || !tcv_parse_next(&p, color, &parsedKind)) return FALSE;
    tcv_skip_space(&p);
    if (*p != L'\0') return FALSE;
    if (kind) *kind = parsedKind;
    return TRUE;
}

BOOL tcv_parse_ascii(const char* text, COLORREF* color, TCV_KIND* kind)
{
    WCHAR wide[128];
    int i;
    if (!text || !text[0]) return FALSE;
    for (i = 0; text[i] && i < (int)(sizeof(wide) / sizeof(wide[0])) - 1; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch > 0x7f) return FALSE;
        wide[i] = (WCHAR)ch;
    }
    if (text[i] != '\0') return FALSE;
    wide[i] = L'\0';
    return tcv_parse(wide, color, kind);
}
