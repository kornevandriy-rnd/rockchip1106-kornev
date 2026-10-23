// Хост-тест: читає сирий MAVLink зі stdin, друкує підсумковий стан як JSON-рядок.
#include <stdio.h>
#include "../src/mav_state.h"

int main() {
    MavParser p;
    Telemetry t;
    uint8_t chunk[7];                                                     // малі шматки: перевіряємо склеювання кадрів на межах читань
    double now = 1.0;
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, stdin)) > 0)
        p.push(chunk, n, [&](uint32_t id, uint8_t sys, uint8_t, const uint8_t *pl, int len) { telemetry_apply(t, id, sys, pl, len, now += 0.01); });
    char m[32];
    printf("{\"frames\":%u,\"bad_crc\":%u,\"armed\":%d,\"mode\":\"%s\",\"roll\":%.2f,\"pitch\":%.2f,\"yaw\":%.2f,\"lat\":%.7f,\"lon\":%.7f,"
           "\"alt\":%.2f,\"hdg\":%.1f,\"gs\":%.2f,\"climb\":%.2f,\"fix\":%d,\"sats\":%d,\"hdop\":%.2f,\"batt_v\":%.3f,\"batt_pct\":%d,\"unix\":%.0f,"
           "\"home\":%d,\"home_dist\":%.1f,\"sysid\":%d}\n",
           t.frames, p.bad_crc, t.armed, mode_name(t.type, t.autopilot, t.custom_mode, m, sizeof m), t.roll, t.pitch, t.yaw, t.lat, t.lon,
           t.alt_rel, t.heading, t.groundspeed, t.climb, t.fix, t.sats, t.hdop, t.batt_v, t.batt_pct, t.unix_time, t.have_home, t.home_dist, t.sysid);
    return 0;
}
