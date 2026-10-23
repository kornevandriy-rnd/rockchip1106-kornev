// mav_state.h — мінімальний MAVLink (v1/v2) приймач польотних даних для OSD на RV1106.
// Без зовнішніх залежностей (uClibc/C++11): розбір кадрів, CRC, лише ті повідомлення, що потрібні HUD.
#pragma once
#include <stdint.h>
#include <atomic>
#include <functional>
#include <vector>
#include <mutex>
#include <string>
#include <thread>

struct Telemetry {
    // ---- HEARTBEAT ----
    bool have_hb = false;
    bool armed = false;
    uint8_t type = 0, autopilot = 0, sysid = 0;
    uint32_t custom_mode = 0;
    // ---- ATTITUDE (градуси) ----
    bool have_att = false;
    float roll = 0, pitch = 0, yaw = 0;
    // ---- GLOBAL_POSITION_INT / VFR_HUD ----
    bool have_pos = false;
    double lat = 0, lon = 0;               // градуси
    float alt_rel = 0, alt_msl = 0;        // метри
    float heading = 0;                     // градуси 0..360
    bool have_vfr = false;
    float groundspeed = 0, airspeed = 0, climb = 0;
    int throttle = 0;
    // ---- GPS_RAW_INT ----
    bool have_gps = false;
    int fix = 0, sats = 0;
    float hdop = -1;
    // ---- SYS_STATUS ----
    bool have_batt = false;
    float batt_v = 0, batt_a = -1;
    int batt_pct = -1;
    // ---- SYSTEM_TIME ----
    double unix_time = 0;                  // секунди; 0 якщо автопілот не знає часу
    // ---- домашня точка: HOME_POSITION або позиція першого arm ----
    bool have_home = false;
    double home_lat = 0, home_lon = 0;
    float home_dist = -1;                  // метри
    // ---- службове ----
    double t_any = 0;                      // монотонний час останнього валідного кадру
    double t_hb = 0;                       // і останнього HEARTBEAT
    uint32_t frames = 0, bad_crc = 0;
};

const char *mode_name(uint8_t type, uint8_t autopilot, uint32_t custom_mode, char *buf, size_t n);
double mono_now();

// Розбирач потоку з пересинхронізацією: хибний «початок кадру» не з'їдає наступні справжні кадри.
class MavParser {
public:
    typedef std::function<void(uint32_t msgid, uint8_t sys, uint8_t comp, const uint8_t *payload, int len)> Handler;
    // Додає байти й викликає handler для кожного валідного кадру (payload розширений нулями до повної довжини).
    void push(const uint8_t *data, size_t n, const Handler &handler);
    uint32_t bad_crc = 0;
private:
    std::vector<uint8_t> buf_;
};

// Застосувати розібране повідомлення до стану (поза потоками; блокування робить MavReader).
void telemetry_apply(Telemetry &t, uint32_t msgid, uint8_t sys, const uint8_t *p, int len, double now);

// Побудова вихідних кадрів (v2) — щоб попросити в польотника потрібні потоки.
int mav_build_heartbeat(uint8_t *out, uint8_t seq);
int mav_build_set_interval(uint8_t *out, uint8_t seq, uint8_t target_sys, uint8_t target_comp, uint32_t msgid, float hz);

// Читач: "uart:/dev/ttyS3:115200" | "udp:14550" | "off". Поток, що оновлює Telemetry.
class MavReader {
public:
    ~MavReader() { stop(); }
    bool start(const std::string &spec, bool request_streams = true);
    void stop();
    Telemetry snapshot();
    std::string status() const { return status_; }
private:
    void run();
    bool open_source();
    void send_requests();
    int fd_ = -1;
    bool is_udp_ = false, request_ = true;
    std::string spec_, dev_, status_;
    int baud_ = 115200, port_ = 14550;
    std::atomic<bool> stop_{false};
    std::thread th_;
    std::mutex mu_;
    Telemetry t_;
    uint8_t tx_seq_ = 0, fc_sys_ = 1, fc_comp_ = 1;
    double t_hb_sent_ = 0, t_req_sent_ = 0;
};
