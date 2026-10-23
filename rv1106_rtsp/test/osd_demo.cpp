// Хост-демо: малює HUD на синтетичному кадрі 640x512 YUYV і пише його у файл. Сценарії: ok | nolink | nofix | lowbatt
#include <stdio.h>
#include <string.h>
#include <vector>
#include "../src/osd.h"

int main(int argc, char **argv) {
    const char *scen = argc > 1 ? argv[1] : "ok";
    const char *out = argc > 2 ? argv[2] : "/tmp/osd.yuyv";
    const int W = 640, H = 512;
    std::vector<uint8_t> f(W * H * 2);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) { uint8_t Y = 60 + ((x / 40 + y / 40) % 2) * 50 + y / 8; f[(y * W + x) * 2] = Y; f[(y * W + (x & ~1)) * 2 + 1] = 128; f[(y * W + (x & ~1)) * 2 + 3] = 128; }
    Telemetry t;
    double now = 100.0;
    t.frames = 50; t.t_any = now - 0.1; t.t_hb = now - 0.2;
    t.have_hb = t.have_att = t.have_pos = t.have_vfr = t.have_gps = t.have_batt = t.have_home = true;
    t.type = 2; t.autopilot = 3; t.custom_mode = 5; t.armed = true;
    t.roll = -12; t.pitch = 6; t.lat = 50.45012; t.lon = 30.52345; t.alt_rel = 120.4f; t.heading = 273;
    t.groundspeed = 14.2f; t.climb = 1.5f; t.throttle = 55; t.fix = 3; t.sats = 14; t.hdop = 0.9f; t.batt_v = 15.8f; t.batt_a = 12.5f; t.batt_pct = 74;
    t.home_dist = 450;
    if (!strcmp(scen, "nolink")) t.t_any = now - 7;
    if (!strcmp(scen, "nofix")) { t.fix = 1; t.sats = 3; t.hdop = -1; }
    if (!strcmp(scen, "lowbatt")) { t.batt_pct = 12; t.batt_v = 13.9f; t.armed = false; }
    Osd osd;
    osd.draw(f.data(), W, H, t, now);
    FILE *fp = fopen(out, "wb"); fwrite(f.data(), 1, f.size(), fp); fclose(fp);
    return 0;
}
