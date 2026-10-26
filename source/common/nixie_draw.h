#pragma once
// Native geometry follows the approved SVG mock; glyph paths are drawing data, not file paths.
struct NIX_STYLE { Color glass, mesh, base, glow, wire, core; };
static bool nix_make_wire(GraphicsPath& path, int digit)
{
    const WCHAR* wires[] = {
        L"M36 41C15 41 16 68 16 80S15 118 36 118S56 94 56 80S57 41 36 41Z",
        L"M22 58L37 42V118M23 118H51",
        L"M17 60C17 34 57 34 56 59C56 78 16 94 16 117H56",
        L"M18 47C42 32 62 49 49 68L34 78C65 77 63 119 34 119C25 119 19 114 16 109",
        L"M49 118V41L15 94H59",
        L"M54 42H21L18 77C46 66 60 84 55 103C52 122 26 124 16 111",
        L"M52 46C19 24 7 121 36 119C67 117 59 72 35 75C25 75 19 80 16 87",
        L"M15 43H56L29 118",
        L"M36 78C5 69 15 38 36 41C66 38 62 70 36 78C3 88 14 122 36 119C66 122 68 87 36 78Z",
        L"M56 74C49 90 18 89 16 65C13 35 55 29 56 62C63 116 35 129 21 111",
    };
    const WCHAR* input = wires[digit];
    WCHAR command = 0, previous = 0;
    PointF point(0, 0), control(0, 0), start(0, 0);
    while (*input) {
        if (*input == L' ' || *input == L',') { ++input; continue; }
        if (*input >= L'A' && *input <= L'Z') command = *input++;
        if (command == L'Z') { path.CloseFigure(); point = start; command = 0; previous = L'Z'; continue; }
        int count = command == L'C' ? 6 : command == L'S' ? 4 :
            (command == L'M' || command == L'L') ? 2 : (command == L'H' || command == L'V') ? 1 : 0;
        if (!count) return false;
        REAL values[6];
        for (int i = 0; i < count; ++i) {
            while (*input == L' ' || *input == L',') ++input;
            WCHAR* end;
            double value = wcstod(input, &end);
            if (end == input || !std::isfinite(value)) return false;
            values[i] = (REAL)value; input = end;
        }
        PointF next = point;
        if (command == L'M') { point = start = PointF(values[0], values[1]); path.StartFigure(); previous = L'M'; command = L'L'; continue; }
        if (command == L'C' || command == L'S') {
            PointF first = command == L'C' ? PointF(values[0], values[1]) :
                (previous == L'C' || previous == L'S') ? PointF(2*point.X-control.X, 2*point.Y-control.Y) : point;
            int offset = command == L'C' ? 2 : 0;
            control = PointF(values[offset], values[offset+1]);
            next = PointF(values[offset+2], values[offset+3]);
            if (path.AddBezier(point, first, control, next) != Ok) return false;
        } else {
            if (command == L'H') next.X = values[0];
            else if (command == L'V') next.Y = values[0];
            else next = PointF(values[0], values[1]);
            if (path.AddLine(point, next) != Ok) return false;
        }
        point = next; previous = command;
    }
    return path.GetPointCount() > 0;
}

static Color nix_set_alpha(Color color, BYTE alpha)
{
    return Color(alpha, color.GetR(), color.GetG(), color.GetB());
}

static void nix_paint_lamp(Graphics& graphics, const NIX_STYLE& style, REAL x, REAL y)
{
    SolidBrush outer(nix_set_alpha(style.glow, 28)), halo(nix_set_alpha(style.glow, 85));
    SolidBrush wire(style.wire), core(style.core);
    graphics.FillEllipse(&outer, x-9, y-9, 18.0f, 18.0f);
    graphics.FillEllipse(&halo, x-6, y-6, 12.0f, 12.0f);
    graphics.FillEllipse(&wire, x-4, y-4, 8.0f, 8.0f);
    graphics.FillEllipse(&core, x-2, y-2, 4.0f, 4.0f);
}

static void nix_paint_separator(Graphics& graphics, const NIX_STYLE& style, bool shortTube)
{
    GraphicsState saved = graphics.Save();
    if (shortTube) graphics.TranslateTransform(21, 0);
    int bottom = shortTube ? 44 : 124, shoulder = shortTube ? 20 : 28, top = shortTube ? 9 : 17;
    GraphicsPath outline;
    outline.AddLine(5, bottom, 5, shoulder);
    outline.AddBezier(5, shoulder, 5, top+5, 10, top, 16, top);
    outline.AddBezier(16, top, 22, top, 27, top+5, 27, shoulder);
    outline.AddLine(27, shoulder, 27, bottom); outline.CloseFigure();
    LinearGradientBrush glass(PointF(5, 0), PointF(27, 0), nix_set_alpha(style.glass, 70), nix_set_alpha(style.glass, 82));
    Color colors[] = {nix_set_alpha(style.glass, 70), nix_set_alpha(style.glass, 10), nix_set_alpha(style.glass, 5), nix_set_alpha(style.glass, 82)};
    REAL stops[] = {0, .22f, .65f, 1}; glass.SetInterpolationColors(colors, stops, 4);
    Pen edge(nix_set_alpha(style.glass, 128), .8f), reflection(nix_set_alpha(style.glass, 140), 1.8f);
    graphics.FillPath(&glass, &outline); graphics.DrawPath(&edge, &outline);
    reflection.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    graphics.DrawBezier(&reflection, 8, shoulder, 8, top+6, 11, top+3, 15, top+3);
    Pen side(nix_set_alpha(style.glass, 38), 1), support(nix_set_alpha(style.mesh, 70), .7f);
    graphics.DrawLine(&side, 24, shoulder, 24, bottom-6);
    graphics.DrawLine(&support, 9, shoulder+3, 9, bottom-2);
    graphics.DrawLine(&support, 23, shoulder+3, 23, bottom-2);
    for (int i = 0; i < (shortTube ? 1 : 2); ++i) {
        int y = shortTube ? 27 : i ? 94 : 64;
        Pen wire(nix_set_alpha(style.mesh, 64), .6f); graphics.DrawLine(&wire, 12, y+5, 12, bottom);
        nix_paint_lamp(graphics, style, 16, (REAL)y);
    }
    graphics.Restore(saved);
}

static void nix_paint_base(Graphics& graphics, const NIX_STYLE& style, REAL x, REAL y, REAL width, REAL scale, bool shortRow)
{
    REAL top = y+(shortRow ? 45 : 127)*scale, height = 10*scale;
    LinearGradientBrush base(PointF(x, top), PointF(x, top+height), Color(255, 80, 88, 98), style.base);
    Pen edge(nix_set_alpha(style.glass, 100), std::max(.5f, .6f*scale));
    REAL left = x+.3f*scale, right = x+width-.3f*scale;
    REAL radius = std::min(3*scale, std::max(0.0f, (right-left)/2));
    if (radius <= 0) return;
    REAL arc = radius*2;
    GraphicsPath outline;
    outline.AddArc(left, top, arc, arc, 180, 90);
    outline.AddArc(right-arc, top, arc, arc, 270, 90);
    outline.AddArc(right-arc, top+height-arc, arc, arc, 0, 90);
    outline.AddArc(left, top+height-arc, arc, arc, 90, 90);
    outline.CloseFigure();
    graphics.FillPath(&base, &outline); graphics.DrawPath(&edge, &outline);
}

static Bitmap* nix_make_tile(const NIX_STYLE& style, int digit)
{
    int width = digit == 10 ? 32 : 74, height = digit == 11 ? 54 : 142;
    std::unique_ptr<Bitmap> tile(new Bitmap(width, height, PixelFormat32bppPARGB));
    if (tile->GetLastStatus() != Ok) return nullptr;
    Graphics graphics(tile.get());
    graphics.Clear(Color(0, 0, 0, 0));
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    if (digit == 10 || digit == 11) nix_paint_separator(graphics, style, digit == 11);
    else if (digit < 0) {
        GraphicsPath outline;
        outline.AddLine(8, 124, 8, 40);
        outline.AddBezier(8, 40, 8, 24, 22, 18, 32, 17);
        outline.AddLine(32, 17, 32, 10);
        outline.AddBezier(32, 10, 32, 1, 42, 1, 42, 10);
        outline.AddLine(42, 10, 42, 17);
        outline.AddBezier(42, 17, 52, 18, 66, 24, 66, 40);
        outline.AddLine(66, 40, 66, 124); outline.CloseFigure();
        LinearGradientBrush glass(PointF(8, 0), PointF(66, 0), nix_set_alpha(style.glass, 65), nix_set_alpha(style.glass, 70));
        Color colors[] = {nix_set_alpha(style.glass, 65), nix_set_alpha(style.glass, 8), nix_set_alpha(style.glass, 3), nix_set_alpha(style.glass, 32), nix_set_alpha(style.glass, 70)};
        REAL stops[] = {0, .13f, .5f, .9f, 1}; glass.SetInterpolationColors(colors, stops, 5);
        Pen edge(nix_set_alpha(style.glass, 115), .8f);
        graphics.FillPath(&glass, &outline); graphics.DrawPath(&edge, &outline);
        Pen mesh(nix_set_alpha(style.mesh, 70), .4f), support(nix_set_alpha(style.glass, 85), 1.2f);
        for (int x = 14; x <= 60; x += 7) graphics.DrawLine(&mesh, x, 36, x, 126);
        for (int y = 36; y <= 126; y += 9) graphics.DrawLine(&mesh, 14, y, 60, y);
        graphics.DrawLine(&support, 14, 35, 14, 126); graphics.DrawLine(&support, 60, 35, 60, 126);
        graphics.DrawLine(&mesh, 12, 35, 62, 35); graphics.DrawLine(&mesh, 12, 126, 62, 126);
        Pen ghost(nix_set_alpha(style.mesh, 8), 1.1f);
        for (int i = 0; i < 10; ++i) { GraphicsPath wire; if (!nix_make_wire(wire, i)) return nullptr; graphics.DrawPath(&ghost, &wire); }
        Pen reflection(nix_set_alpha(style.glass, 180), 2.6f); reflection.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
        graphics.DrawBezier(&reflection, 14, 38, 14, 29, 21, 25, 29, 23);
        Pen side(nix_set_alpha(style.glass, 40), 2); graphics.DrawLine(&side, 60, 36, 60, 118);
        graphics.DrawLine(&edge, 35, 8, 35, 14);

    } else {
        GraphicsPath wire; if (!nix_make_wire(wire, digit)) return nullptr;
        const REAL widths[] = {10, 7, 5, 2.8f, .85f};
        const Color colors[] = {nix_set_alpha(style.glow, 12), nix_set_alpha(style.glow, 26), nix_set_alpha(style.glow, 65), style.wire, style.core};
        for (int i = 0; i < 5; ++i) {
            Pen pen(colors[i], widths[i]); pen.SetLineCap(LineCapRound, LineCapRound, DashCapRound); pen.SetLineJoin(LineJoinRound);
            if (graphics.DrawPath(&pen, &wire) != Ok) return nullptr;
        }
    }
    graphics.Flush(FlushIntentionSync);
    return tile.release();
}

static bool nix_draw_alpha(Graphics& graphics, Bitmap* image, REAL x, REAL y, int width, int height, REAL alpha)
{
    if (alpha <= 0) return true;
    ColorMatrix matrix = {{{1,0,0,0,0},{0,1,0,0,0},{0,0,1,0,0},{0,0,0,alpha,0},{0,0,0,0,1}}};
    ImageAttributes attributes; attributes.SetColorMatrix(&matrix);
    return graphics.DrawImage(image, RectF(x, y, (REAL)width, (REAL)height), 0, 0,
        (REAL)image->GetWidth(), (REAL)image->GetHeight(), UnitPixel, &attributes) == Ok;
}
