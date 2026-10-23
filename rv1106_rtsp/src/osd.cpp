// osd.cpp — див. osd.h
#include "osd.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "font8x16.h"

namespace {
const uint8_t kBlackY = 16;
}

void Osd::px(int x, int y, Col c) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    uint8_t *p = f_ + (static_cast<size_t>(y) * w_ + (x & ~1)) * 2;      // пара пікселів [Y0 U Y1 V]
    p[(x & 1) ? 2 : 0] = c.y;
    p[1] = c.u;
    p[3] = c.v;
}

void Osd::glyph_pass(int x, int y, char ch, Col c, int dx, int dy) {
    int idx = (ch >= 32 && ch <= 126) ? ch - 32 : '?' - 32;
    const uint8_t *g = FONT8X16[idx];
    for (int r = 0; r < 16; r++) {
        uint8_t bits = g[r];
        if (!bits) continue;
        for (int b = 0; b < 8; b++) {
            if (!(bits & (0x80 >> b))) continue;
            for (int sy = 0; sy < sc_; sy++)
                for (int sx = 0; sx < sc_; sx++) px(x + (b * sc_ + sx) + dx, y + (r * sc_ + sy) + dy, c);
        }
    }
}

int Osd::text_width(const char *s) const { return static_cast<int>(strlen(s)) * 8 * sc_; }

void Osd::text(int x, int y, const char *s, Col c) {
    if (x < 0) x = w_ + x - text_width(s);                              // від правого краю
    static const int ox[] = {-1, 0, 1, -1, 1, -1, 0, 1}, oy[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    Col black{kBlackY, 128, 128};
    for (int pass = 0; pass < 2; pass++) {                              // 1: чорний контур, 2: колір
        for (size_t i = 0; s[i]; i++) {
            int gx = x + static_cast<int>(i) * 8 * sc_;
            if (pass == 0) for (int k = 0; k < 8; k++) glyph_pass(gx, y, s[i], black, ox[k] * sc_, oy[k] * sc_);
            else glyph_pass(gx, y, s[i], c, 0, 0);
        }
    }
}

void Osd::text_center(int cx, int y, const char *s, Col c) { text(cx - text_width(s) / 2, y, s, c); }

void Osd::line(int x0, int y0, int x1, int y1, Col c, int r) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) px(x0 + i, y0 + j, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void Osd::thick_line(int x0, int y0, int x1, int y1, Col c) {
    Col black{kBlackY, 128, 128};
    line(x0, y0, x1, y1, black, 2);
    line(x0, y0, x1, y1, c, 1);
}

void Osd::draw_horizon(const Telemetry &t) {
    const Col white{235, 128, 128};
    int cx = w_ / 2, cy = h_ / 2;
    float roll = t.roll * static_cast<float>(M_PI) / 180.f;
    float off = t.pitch * 4.0f * sc_;                                    // 4 px на градус; пікс. вниз при кабруванні вгору
    float c = cosf(roll), s = sinf(roll);
    auto pt = [&](float along, float down, int &x, int &y) { x = cx + static_cast<int>(along * c - down * s); y = cy + static_cast<int>(along * s + down * c); };
    int x0, y0, x1, y1;
    const float inner = 28.f * sc_, outer = 110.f * sc_;
    for (int side = -1; side <= 1; side += 2) {
        pt(side * inner, off, x0, y0); pt(side * outer, off, x1, y1);
        thick_line(x0, y0, x1, y1, white);
    }
    // нерухомий прицільний маркер (літак/камера) по центру
    thick_line(cx - 14 * sc_, cy, cx - 5 * sc_, cy, white); thick_line(cx + 5 * sc_, cy, cx + 14 * sc_, cy, white);
    thick_line(cx, cy - 5 * sc_, cx, cy - 1, white);
}

void Osd::draw(uint8_t *yuyv, int w, int h, const Telemetry &t, double now, int scale) {
    f_ = yuyv; w_ = w; h_ = h; sc_ = scale < 1 ? 1 : scale;
    const Col white{235, 128, 128}, red{81, 90, 240}, yellow{210, 16, 146}, green{145, 54, 34};
    const int m = 8 * sc_, lh = 17 * sc_;
    char b[96];
    bool alive = t.frames > 0 && (now - t.t_any) < link_timeout;
    if (!alive) {
        text_center(w / 2, h / 2 - lh, t.frames ? "NO FC LINK" : "NO FC DATA", red);
        if (t.frames) { snprintf(b, sizeof b, "LAST %.0fs AGO", now - t.t_any); text_center(w / 2, h / 2, b, red); }
        return;                                                          // застарілі значення не показуємо
    }
    // ---- верх-ліворуч: режим / arm / GPS ----
    if (t.have_hb && (now - t.t_hb) < link_timeout) {
        char mn[32];
        snprintf(b, sizeof b, "%s %s", t.armed ? "ARMED" : "DISARMED", mode_name(t.type, t.autopilot, t.custom_mode, mn, sizeof mn));
        text(m, m, b, t.armed ? green : white);
    } else text(m, m, "NO HEARTBEAT", red);
    if (extra && t.have_gps) {
        const char *fx = t.fix >= 4 ? "RTK" : (t.fix == 3 ? "3D" : (t.fix == 2 ? "2D" : "NO FIX"));
        if (t.hdop >= 0 && t.fix >= 2) snprintf(b, sizeof b, "GPS %s %dSAT HDOP %.1f", fx, t.sats, t.hdop);
        else snprintf(b, sizeof b, "GPS %s %dSAT", fx, t.sats);
        text(m, m + lh, b, t.fix >= 3 ? white : red);
    }
    // ---- верх-праворуч: батарея / газ ----
    if (t.have_batt && t.batt_v > 0.1f) {
        if (t.batt_pct >= 0) snprintf(b, sizeof b, "BAT %.1fV %d%%", t.batt_v, t.batt_pct); else snprintf(b, sizeof b, "BAT %.1fV", t.batt_v);
        text(-m, m, b, (t.batt_pct >= 0 && t.batt_pct < 20) ? red : ((t.batt_pct >= 0 && t.batt_pct < 35) ? yellow : white));
        if (extra && t.batt_a >= 0) { snprintf(b, sizeof b, "%.1fA", t.batt_a); text(-m, m + lh, b, white); }
    }
    if (extra && t.have_vfr) { snprintf(b, sizeof b, "THR %d%%", t.throttle); text(-m, m + 2 * lh, b, white); }
    // ---- низ-ліворуч: висота, швидкість, вертикальна ----
    if (t.have_pos) {
        snprintf(b, sizeof b, "ALT %.1fm", t.alt_rel);
        text(m, h - m - 2 * lh, b, white);
    }
    if (t.have_vfr) {
        snprintf(b, sizeof b, "SPD %.1fm/s", t.groundspeed);
        text(m, h - m - lh, b, white);
        if (extra) { snprintf(b, sizeof b, "VS %+.1fm/s", t.climb); text(m, h - m - 3 * lh, b, white); }
    }
    // ---- низ-центр: курс і координати; низ-праворуч: дім ----
    if (t.have_pos || t.have_vfr) {
        snprintf(b, sizeof b, "HDG %03d", static_cast<int>(t.heading + 0.5f) % 360);
        text_center(w / 2, h - m - lh, b, white);
    }
    if (extra && t.have_pos && (t.lat != 0 || t.lon != 0)) {
        snprintf(b, sizeof b, "%.5f%c %.5f%c", fabs(t.lat), t.lat >= 0 ? 'N' : 'S', fabs(t.lon), t.lon >= 0 ? 'E' : 'W');
        text_center(w / 2, h - m - lh, b, white);
    }
    if (extra && t.have_home && t.home_dist >= 0) {
        if (t.home_dist >= 1000) snprintf(b, sizeof b, "HOME %.2fkm", t.home_dist / 1000.0); else snprintf(b, sizeof b, "HOME %.0fm", t.home_dist);
        text(-m, h - m - lh, b, white);
    }
    if (horizon && t.have_att) draw_horizon(t);
}
