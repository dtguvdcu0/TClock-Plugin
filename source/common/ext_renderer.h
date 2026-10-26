#pragma once
#include "ext_detach.h"
#include "flip_clock.h"
#include <algorithm>
#include <vector>

// Bound both dimensions when restoring a width-based pose in another orientation.
inline SIZE ext_scale_size(int width, SIZE aspect) {
    int lower = std::max(4L, (4 * aspect.cx + aspect.cy - 1) / aspect.cy);
    int upper = std::min(4096, MulDiv(4096, aspect.cx, aspect.cy));
    int target = std::max(lower, std::min(upper, width));
    return SIZE{target, std::max(4, MulDiv(target, aspect.cy, aspect.cx))};
}

// One renderer owner per home; movement and persistence do not depend on display kind.
struct EXT_RENDERER {
    ACS_CONTEXT* analog = nullptr;
    FLP_CONTEXT* flip = nullptr;
    LED_CONTEXT* led = nullptr;
    int mode = -1, face = -1;
    std::vector<BYTE> pixels;
    EXT_RENDERER() = default;
    EXT_RENDERER(const EXT_RENDERER&) = delete;
    EXT_RENDERER& operator=(const EXT_RENDERER&) = delete;
    ~EXT_RENDERER() { acs_destroy(analog); flp_destroy(flip); led_destroy(led); }

    bool configure(HINSTANCE module, const EXT_DETACH_PACKET& packet) {
        const auto& options = packet.options;
        if (mode != options.mode || face != options.face) {
            ACS_CONTEXT* nextAnalog = nullptr;
            FLP_CONTEXT* nextFlip = nullptr;
            LED_CONTEXT* nextLed = nullptr;
            if (options.mode == EXT_MODE_LED) nextLed = led_create();
            else if (options.mode == EXT_MODE_FLIP) nextFlip = flp_create(module, options.face);
            else if (options.mode == EXT_MODE_NIXIE) nextFlip = flp_create_nixie(module);
            else nextAnalog = acs_create(module, options.mode, options.face);
            if (!nextAnalog && !nextFlip && !nextLed) return false;
            acs_destroy(analog); flp_destroy(flip); led_destroy(led);
            analog = nextAnalog; flip = nextFlip; led = nextLed;
            mode = options.mode; face = options.face;
        }
        if (led) led_apply_snapshot(led, &packet.text);
        return true;
    }

    bool render(SIZE size, const SYSTEMTIME& time, const ACS_OPTIONS& options,
        bool stacked, bool animate, ULONGLONG tick) {
        if (size.cx < 4 || size.cy < 4 || size.cx > 4096 || size.cy > 4096) return false;
        SIZE natural = size;
        const BYTE* source = nullptr;
        if (analog) {
            int diameter = std::min(1024L, std::min(size.cx, size.cy));
            if (!acs_render(analog, diameter, &time, options.seconds)) return false;
            natural = {diameter, diameter}; source = acs_get_pixels(analog);
        } else if (flip) {
            int height = stacked ? size.cx : size.cy;
            // A stacked clock's width is wider than one cell. Measure the exact layout.
            SIZE unit;
            if (!flp_get_mode_size(mode, 100, stacked, options.seconds, options.colon, &unit)) return false;
            if (stacked) height = std::max(1, MulDiv(size.cx, 100, unit.cx));
            height = std::min(1024, height);
            flp_set_base(flip, options.nixieBase);
            if (!flp_render(flip, height, stacked, options.seconds, options.colon,
                &time, tick, animate, options.flipDuration)) return false;
            source = flp_get_pixels(flip, &natural);
        } else if (led) {
            if (natural.cy > 2048) { natural.cx = std::max(1, MulDiv(natural.cx, 2048, natural.cy)); natural.cy = 2048; }
            if (!led_render_snapshot(led, natural, tick, animate)) return false;
            source = led_get_pixels(led);
        }
        if (!source || natural.cx <= 0 || natural.cy <= 0) return false;
        pixels.resize(static_cast<size_t>(size.cx) * size.cy * 4);
        if (natural.cx == size.cx && natural.cy == size.cy)
            memcpy(pixels.data(), source, pixels.size());
        else for (int y = 0; y < size.cy; ++y) for (int x = 0; x < size.cx; ++x) {
            int sx = static_cast<int>(static_cast<LONGLONG>(x) * natural.cx / size.cx);
            int sy = static_cast<int>(static_cast<LONGLONG>(y) * natural.cy / size.cy);
            memcpy(pixels.data() + (static_cast<size_t>(y) * size.cx + x) * 4,
                source + (static_cast<size_t>(sy) * natural.cx + sx) * 4, 4);
        }
        return true;
    }
};
