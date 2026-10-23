// osd.h — HUD поверх кадру: малюється просто в YUYV-буфер камери ДО RGA-апскейлу (без додаткових кеш-операцій над кадром 1280x1024).
#pragma once
#include <stdint.h>
#include "mav_state.h"

class Osd {
public:
    // w x h — розмір YUYV-кадру (у пікселях); scale — множник шрифту (1 для 640x512: після ×2 апскейлу це 16x32)
    void draw(uint8_t *yuyv, int w, int h, const Telemetry &t, double now, int scale = 1);
    double link_timeout = 2.0;               // секунд без кадрів MAVLink → показуємо "NO FC LINK" замість застарілих даних
    bool horizon = true;
    bool extra = false;                      // GPS, газ, вертикальна швидкість, координати, дім, струм (за замовчуванням вимкнено: оператор обрав мінімальний HUD)
private:
    struct Col { uint8_t y, u, v; };
    uint8_t *f_ = nullptr;
    int w_ = 0, h_ = 0, sc_ = 1;
    void px(int x, int y, Col c);
    void glyph_pass(int x, int y, char ch, Col c, int dx, int dy);
    int text_width(const char *s) const;
    void text(int x, int y, const char *s, Col c);                 // x<0: від правого краю (x = -відступ), з обведенням
    void text_center(int cx, int y, const char *s, Col c);
    void line(int x0, int y0, int x1, int y1, Col c, int r);
    void thick_line(int x0, int y0, int x1, int y1, Col c);        // біла лінія з чорним контуром
    void draw_horizon(const Telemetry &t);
};
