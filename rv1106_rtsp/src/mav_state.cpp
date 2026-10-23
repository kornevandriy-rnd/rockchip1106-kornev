// mav_state.cpp — див. mav_state.h
#include "mav_state.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

double mono_now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

namespace {

struct MsgInfo { uint32_t id; uint8_t crc_extra; uint8_t max_len; };
// crc_extra і повна довжина v2 (з розширеннями) звірені з pymavlink (dialect ardupilotmega).
const MsgInfo kMsgs[] = {
    {0, 50, 9},    // HEARTBEAT
    {1, 124, 43},  // SYS_STATUS
    {2, 137, 12},  // SYSTEM_TIME
    {24, 24, 52},  // GPS_RAW_INT
    {30, 39, 28},  // ATTITUDE
    {33, 104, 28}, // GLOBAL_POSITION_INT
    {74, 20, 20},  // VFR_HUD
    {242, 104, 60},// HOME_POSITION
};

const MsgInfo *find_msg(uint32_t id) {
    for (const MsgInfo &m : kMsgs) if (m.id == id) return &m;
    return nullptr;
}

uint16_t crc_step(uint16_t crc, uint8_t b) {
    uint8_t tmp = b ^ (crc & 0xFF);
    tmp ^= (tmp << 4);
    return (crc >> 8) ^ (static_cast<uint16_t>(tmp) << 8) ^ (static_cast<uint16_t>(tmp) << 3) ^ (tmp >> 4);
}

template <typename T> T rd(const uint8_t *p, int off) {
    T v;
    memcpy(&v, p + off, sizeof(T));          // MAVLink = little-endian, ARM/x86 теж
    return v;
}

double haversine(double la1, double lo1, double la2, double lo2) {
    const double R = 6371000.0, d2r = M_PI / 180.0;
    double dla = (la2 - la1) * d2r, dlo = (lo2 - lo1) * d2r;
    double a = sin(dla / 2) * sin(dla / 2) + cos(la1 * d2r) * cos(la2 * d2r) * sin(dlo / 2) * sin(dlo / 2);
    return 2 * R * asin(sqrt(a));
}

}  // namespace

void MavParser::push(const uint8_t *data, size_t n, const Handler &handler) {
    buf_.insert(buf_.end(), data, data + n);
    size_t i = 0;
    const size_t size = buf_.size();
    while (i < size) {
        uint8_t stx = buf_[i];
        if (stx != 0xFD && stx != 0xFE) { i++; continue; }
        bool v2 = stx == 0xFD;
        size_t hdr = v2 ? 10 : 6;
        if (size - i < hdr) break;                                          // потрібні ще байти
        const uint8_t *f = &buf_[i];
        size_t len = f[1];
        bool sign = v2 && (f[2] & 1);
        size_t total = hdr + len + 2 + (sign ? 13 : 0);
        if (size - i < total) break;
        uint32_t id = v2 ? (f[7] | (f[8] << 8) | (f[9] << 16)) : f[5];
        const MsgInfo *mi = find_msg(id);
        bool ok = false;
        if (mi) {
            uint16_t crc = 0xFFFF;
            for (size_t k = 1; k < hdr + len; k++) crc = crc_step(crc, f[k]);
            crc = crc_step(crc, mi->crc_extra);
            uint16_t rx = f[hdr + len] | (f[hdr + len + 1] << 8);
            ok = crc == rx;
            if (!ok) bad_crc++;
        }
        if (!ok) { i++; continue; }                                         // не кадр: шукаємо наступний початок з байта далі
        uint8_t full[64] = {0};
        memcpy(full, f + hdr, len < mi->max_len ? len : mi->max_len);       // v2 обрізає хвостові нулі — повертаємо їх
        uint8_t sys = v2 ? f[5] : f[3], comp = v2 ? f[6] : f[4];
        handler(id, sys, comp, full, mi->max_len);
        i += total;
    }
    buf_.erase(buf_.begin(), buf_.begin() + i);
    if (buf_.size() > 1024) buf_.erase(buf_.begin(), buf_.end() - 512);     // страховка від сміття без початку кадру
}

void telemetry_apply(Telemetry &t, uint32_t id, uint8_t sys, const uint8_t *p, int, double now) {
    t.frames++;
    t.t_any = now;
    switch (id) {
    case 0: {                                        // HEARTBEAT
        uint8_t type = p[4], ap = p[5], base = p[6];
        if (type == 6 || type == 18 || type == 27 || ap == 8) return;     // GCS / onboard controller / ADSB / invalid — не польотник
        t.have_hb = true; t.t_hb = now; t.sysid = sys;
        bool armed = (base & 0x80) != 0;
        if (armed && !t.armed && t.have_pos && !t.have_home) {            // нема HOME_POSITION: перша позиція при arm
            t.home_lat = t.lat; t.home_lon = t.lon; t.have_home = true;
        }
        t.armed = armed; t.type = type; t.autopilot = ap; t.custom_mode = rd<uint32_t>(p, 0);
        break;
    }
    case 1: {                                        // SYS_STATUS
        uint16_t mv = rd<uint16_t>(p, 14); int16_t ca = rd<int16_t>(p, 16); int8_t pct = static_cast<int8_t>(p[30]);
        t.have_batt = true;
        t.batt_v = mv == 0xFFFF ? 0.f : mv / 1000.f;
        t.batt_a = ca < 0 ? -1.f : ca / 100.f;
        t.batt_pct = pct;
        break;
    }
    case 2: {                                        // SYSTEM_TIME
        uint64_t us = rd<uint64_t>(p, 0);
        t.unix_time = us > 1700000000000000ULL ? us / 1e6 : 0.0;
        break;
    }
    case 24: {                                       // GPS_RAW_INT
        t.have_gps = true;
        t.fix = p[28]; t.sats = p[29] == 255 ? 0 : p[29];
        uint16_t eph = rd<uint16_t>(p, 20);
        t.hdop = eph == 0xFFFF ? -1.f : eph / 100.f;
        break;
    }
    case 30: {                                       // ATTITUDE
        t.have_att = true;
        const float k = 57.29578f;
        t.roll = rd<float>(p, 4) * k; t.pitch = rd<float>(p, 8) * k;
        float y = rd<float>(p, 12) * k; if (y < 0) y += 360.f;
        t.yaw = y;
        break;
    }
    case 33: {                                       // GLOBAL_POSITION_INT
        t.have_pos = true;
        t.lat = rd<int32_t>(p, 4) / 1e7; t.lon = rd<int32_t>(p, 8) / 1e7;
        t.alt_msl = rd<int32_t>(p, 12) / 1000.f; t.alt_rel = rd<int32_t>(p, 16) / 1000.f;
        uint16_t hdg = rd<uint16_t>(p, 26);
        if (hdg != 65535) t.heading = hdg / 100.f;
        if (t.have_home) t.home_dist = static_cast<float>(haversine(t.lat, t.lon, t.home_lat, t.home_lon));
        break;
    }
    case 74: {                                       // VFR_HUD
        t.have_vfr = true;
        t.airspeed = rd<float>(p, 0); t.groundspeed = rd<float>(p, 4); t.climb = rd<float>(p, 12);
        int16_t h = rd<int16_t>(p, 16); if (h >= 0) t.heading = h;
        t.throttle = rd<uint16_t>(p, 18);
        break;
    }
    case 242: {                                      // HOME_POSITION
        t.home_lat = rd<int32_t>(p, 0) / 1e7; t.home_lon = rd<int32_t>(p, 4) / 1e7; t.have_home = true;
        break;
    }
    default: break;
    }
}

const char *mode_name(uint8_t type, uint8_t autopilot, uint32_t cm, char *buf, size_t n) {
    static const char *copter[] = {"STABILIZE", "ACRO", "ALT_HOLD", "AUTO", "GUIDED", "LOITER", "RTL", "CIRCLE", "?", "LAND", "?", "DRIFT", "?", "SPORT",
                                   "FLIP", "AUTOTUNE", "POSHOLD", "BRAKE", "THROW", "AVOID_ADSB", "GUIDED_NOGPS", "SMART_RTL", "FLOWHOLD", "FOLLOW",
                                   "ZIGZAG", "SYSTEMID", "AUTOROTATE", "AUTO_RTL"};
    static const char *plane[] = {"MANUAL", "CIRCLE", "STABILIZE", "TRAINING", "ACRO", "FBWA", "FBWB", "CRUISE", "AUTOTUNE", "?", "AUTO", "RTL", "LOITER",
                                  "TAKEOFF", "AVOID_ADSB", "GUIDED", "?", "QSTABILIZE", "QHOVER", "QLOITER", "QLAND", "QRTL", "QAUTOTUNE", "QACRO", "THERMAL"};
    const char *s = nullptr;
    if (autopilot == 3) {                                                      // ArduPilot
        bool is_copter = type == 2 || type == 3 || type == 4 || type == 13 || type == 14 || type == 15 || type == 29;
        bool is_plane = type == 1 || (type >= 19 && type <= 25);
        if (is_copter && cm < sizeof(copter) / sizeof(*copter)) s = copter[cm];
        else if (is_plane && cm < sizeof(plane) / sizeof(*plane)) s = plane[cm];
    } else if (autopilot == 12) {                                              // PX4
        uint32_t main = (cm >> 16) & 0xFF, sub = (cm >> 24) & 0xFF;
        static const char *px4_main[] = {"?", "MANUAL", "ALTCTL", "POSCTL", "AUTO", "ACRO", "OFFBOARD", "STABILIZED"};
        static const char *px4_auto[] = {"?", "READY", "TAKEOFF", "LOITER", "MISSION", "RTL", "LAND"};
        if (main == 4 && sub < sizeof(px4_auto) / sizeof(*px4_auto)) s = px4_auto[sub];
        else if (main < sizeof(px4_main) / sizeof(*px4_main)) s = px4_main[main];
    }
    if (!s || s[0] == '?') { snprintf(buf, n, "MODE %u", static_cast<unsigned>(cm)); return buf; }
    snprintf(buf, n, "%s", s);
    return buf;
}

// ---------------------------------------------------------------- вихідні кадри
static int mav_finish_v2(uint8_t *out, uint8_t seq, uint8_t sys, uint8_t comp, uint32_t msgid, const uint8_t *pl, int plen, uint8_t extra) {
    while (plen > 1 && pl[plen - 1] == 0) plen--;                  // v2: хвостові нулі обрізаються
    int i = 0;
    out[i++] = 0xFD; out[i++] = static_cast<uint8_t>(plen); out[i++] = 0; out[i++] = 0; out[i++] = seq; out[i++] = sys; out[i++] = comp;
    out[i++] = msgid & 0xFF; out[i++] = (msgid >> 8) & 0xFF; out[i++] = (msgid >> 16) & 0xFF;
    memcpy(out + i, pl, plen); i += plen;
    uint16_t crc = 0xFFFF;
    for (int k = 1; k < i; k++) crc = crc_step(crc, out[k]);
    crc = crc_step(crc, extra);
    out[i++] = crc & 0xFF; out[i++] = crc >> 8;
    return i;
}

int mav_build_heartbeat(uint8_t *out, uint8_t seq) {
    uint8_t pl[9] = {0};
    pl[4] = 18;      // MAV_TYPE_ONBOARD_CONTROLLER
    pl[5] = 8;       // MAV_AUTOPILOT_INVALID
    pl[8] = 3;       // mavlink_version
    return mav_finish_v2(out, seq, 200, 191, 0, pl, 9, 50);          // sysid 200, MAV_COMP_ID_ONBOARD_COMPUTER
}

int mav_build_set_interval(uint8_t *out, uint8_t seq, uint8_t tsys, uint8_t tcomp, uint32_t msgid, float hz) {
    uint8_t pl[33] = {0};                                           // COMMAND_LONG
    float interval_us = hz <= 0 ? -1.f : 1e6f / hz;
    float p1 = static_cast<float>(msgid);
    memcpy(pl + 0, &p1, 4);
    memcpy(pl + 4, &interval_us, 4);
    uint16_t cmd = 511;                                             // MAV_CMD_SET_MESSAGE_INTERVAL
    memcpy(pl + 28, &cmd, 2);
    pl[30] = tsys; pl[31] = tcomp; pl[32] = 0;
    return mav_finish_v2(out, seq, 200, 191, 76, pl, 33, 152);
}

// ---------------------------------------------------------------- читач
bool MavReader::start(const std::string &spec, bool request_streams) {
    spec_ = spec; request_ = request_streams;
    if (spec == "off" || spec.empty()) { status_ = "вимкнено"; return false; }
    if (spec.compare(0, 4, "udp:") == 0) {
        is_udp_ = true; port_ = atoi(spec.c_str() + 4);
    } else if (spec.compare(0, 5, "uart:") == 0) {
        std::string r = spec.substr(5);
        size_t c = r.rfind(':');
        dev_ = c == std::string::npos ? r : r.substr(0, c);
        baud_ = c == std::string::npos ? 115200 : atoi(r.c_str() + c + 1);
    } else {
        status_ = "невідомий формат: " + spec;
        return false;
    }
    stop_ = false;
    th_ = std::thread(&MavReader::run, this);
    return true;
}

void MavReader::stop() {
    stop_ = true;
    if (th_.joinable()) th_.join();
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
}

Telemetry MavReader::snapshot() {
    std::lock_guard<std::mutex> l(mu_);
    return t_;
}

static speed_t baud_const(int b) {
    switch (b) {
    case 9600: return B9600; case 19200: return B19200; case 38400: return B38400; case 57600: return B57600;
    case 115200: return B115200; case 230400: return B230400; case 460800: return B460800; case 921600: return B921600;
    default: return B115200;
    }
}

bool MavReader::open_source() {
    if (is_udp_) {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) { status_ = "udp: socket"; return false; }
        int one = 1; setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(port_);
        if (bind(fd_, reinterpret_cast<sockaddr *>(&a), sizeof(a)) < 0) { status_ = std::string("udp bind: ") + strerror(errno); close(fd_); fd_ = -1; return false; }
        status_ = "udp:" + std::to_string(port_);
        return true;
    }
    fd_ = open(dev_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) { status_ = dev_ + ": " + strerror(errno); return false; }
    termios tio{};
    tcgetattr(fd_, &tio);
    cfmakeraw(&tio);
    cfsetispeed(&tio, baud_const(baud_)); cfsetospeed(&tio, baud_const(baud_));
    tio.c_cflag |= CLOCAL | CREAD; tio.c_cflag &= ~(CRTSCTS | PARENB | CSTOPB);
    tcsetattr(fd_, TCSANOW, &tio);
    status_ = dev_ + "@" + std::to_string(baud_);
    return true;
}

void MavReader::send_requests() {
    if (!request_ || is_udp_) return;                       // у UDP-режимі (симулятор) ми лише слухаємо
    double now = mono_now();
    uint8_t buf[64];
    if (now - t_hb_sent_ >= 1.0) {
        t_hb_sent_ = now;
        int n = mav_build_heartbeat(buf, tx_seq_++);
        if (write(fd_, buf, n) < 0) {}
    }
    if (now - t_req_sent_ >= 10.0) {
        t_req_sent_ = now;
        const struct { uint32_t id; float hz; } want[] = {{30, 10}, {33, 5}, {74, 5}, {24, 1}, {1, 1}, {2, 0.2f}};
        for (auto &w : want) {
            int n = mav_build_set_interval(buf, tx_seq_++, fc_sys_, fc_comp_, w.id, w.hz);
            if (write(fd_, buf, n) < 0) {}
        }
    }
}

void MavReader::run() {
    MavParser parser;
    uint8_t rx[512];
    while (!stop_) {
        if (fd_ < 0 && !open_source()) {                    // порт може з'явитись пізніше — пробуємо знову
            for (int i = 0; i < 20 && !stop_; i++) usleep(100000);
            continue;
        }
        pollfd pf{fd_, POLLIN, 0};
        int r = poll(&pf, 1, 200);
        send_requests();
        if (r <= 0) continue;
        ssize_t n = read(fd_, rx, sizeof(rx));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            close(fd_); fd_ = -1; status_ = "помилка читання, перевідкриваю"; continue;
        }
        double now = mono_now();
        parser.push(rx, static_cast<size_t>(n), [&](uint32_t id, uint8_t sys, uint8_t comp, const uint8_t *pl, int len) {
            std::lock_guard<std::mutex> l(mu_);
            if (t_.have_hb && sys != t_.sysid) return;           // на лінії може бути трафік інших систем (компаньйон, що пересилається польотником): беремо лише польотник
            telemetry_apply(t_, id, sys, pl, len, now);
            t_.bad_crc = parser.bad_crc;
            if (id == 0 && t_.have_hb && sys == t_.sysid) { fc_sys_ = sys; fc_comp_ = comp; }
        });
    }
}
