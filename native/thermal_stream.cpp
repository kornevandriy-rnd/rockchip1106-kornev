// thermal_stream.cpp
// ─────────────────────────────────────────────────────────────────────────────
// Апаратний відеострім тепловізора Seek 640 на Luckfox Pico Ultra (Rockchip RV1106).
//
// Ланцюг (ВЕСЬ на залізі, БЕЗ ffmpeg — як вимагає керівник):
//
//     /dev/video21 (USB-UVC, YUYV 640x512)                    ← V4L2 capture (mmap)
//         │
//         ▼  RGA (librga, im2d)  — конвертація кольору + опційний масштаб
//     NV12 (у DMA-буфері MB)
//         │
//         ▼  RK_MPI_VENC (librockit, VEPU)  — апаратне H.264/H.265
//     Annex-B бітпотік
//         │
//         ▼  TCP :8080  → плеєр на ноуті (ffplay/mpv/VLC із H.264/HEVC-декодером)
//
// Чому саме так (підтверджено експериментами, див. README репозиторію):
//   * камера віддає ЛИШЕ YUYV422 — VEPU його напряму не їсть, тому RGA→NV12;
//   * на RV1106 НЕМА V4L2-M2M енкодера → ffmpeg h264_v4l2m2m/hevc_v4l2m2m не працюють;
//   * апаратний кодек доступний тільки через MPP/rockit (RK_MPI_VENC) або mpi_enc_test.
//
// Збірка: крос-компіляція тулчейном Luckfox SDK (див. Makefile / README).
// На платі НЕ компілюється (gcc там немає) — тільки готовий бінар через scp.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <csignal>
#include <string>

#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <linux/videodev2.h>

// ── Rockchip RGA (2D-прискорювач: колірна конверсія + масштаб) ────────────────
#include "im2d.h"
#include "rga.h"

// ── Rockchip MPP/rockit (апаратний енкодер VEPU) ──────────────────────────────
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_mb.h"
#include "rk_comm_venc.h"
#include "rk_comm_video.h"
#include "rk_comm_sys.h"

// ─────────────────────────────────────────────────────────────────────────────
// Конфіг за замовчуванням (усе перевизначається аргументами CLI)
// ─────────────────────────────────────────────────────────────────────────────
struct Config {
    std::string dev   = "/dev/video21"; // нода UVC-камери (перевір: v4l2-ctl --list-formats)
    int  capW         = 640;            // роздільність камери
    int  capH         = 512;
    int  outW         = 640;            // роздільність кодування (RGA масштабує cap→out)
    int  outH         = 512;
    int  fps          = 30;
    int  port         = 8080;
    int  bitrateKbps  = 4000;           // цільовий бітрейт CBR
    int  gop          = 30;             // інтервал IDR (кадрів)
    bool hevc         = true;           // true=H.265 (керівник), false=H.264
    int  vencChn      = 0;
};

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int) { g_stop = 1; }

#define LOGI(...) do { fprintf(stderr, "[i] " __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#define LOGE(...) do { fprintf(stderr, "[E] " __VA_ARGS__); fprintf(stderr, "\n"); } while (0)

// ─────────────────────────────────────────────────────────────────────────────
// V4L2: захоплення YUYV із USB-UVC камери (mmap, кільце буферів)
// ─────────────────────────────────────────────────────────────────────────────
struct V4l2Buf { void *start; size_t length; };

class V4l2Capture {
public:
    bool open(const Config &c) {
        cfg_ = c;
        n_ = 0;
        fd_ = ::open(cfg_.dev.c_str(), O_RDWR | O_NONBLOCK, 0);
        if (fd_ < 0) { LOGE("open %s: %s", cfg_.dev.c_str(), strerror(errno)); return false; }

        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width       = cfg_.capW;
        fmt.fmt.pix.height      = cfg_.capH;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field       = V4L2_FIELD_NONE;
        if (xioctl(VIDIOC_S_FMT, &fmt) < 0) { LOGE("S_FMT: %s", strerror(errno)); return false; }
        if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
            LOGE("камера не дала YUYV (got 0x%x)", fmt.fmt.pix.pixelformat); return false;
        }
        // Реальні width/height могли скоригуватись драйвером — беремо як є.
        cfg_.capW = fmt.fmt.pix.width;
        cfg_.capH = fmt.fmt.pix.height;
        LOGI("V4L2: %s %dx%d YUYV, bytesperline=%d",
             cfg_.dev.c_str(), cfg_.capW, cfg_.capH, fmt.fmt.pix.bytesperline);

        v4l2_requestbuffers req{};
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (xioctl(VIDIOC_REQBUFS, &req) < 0) { LOGE("REQBUFS: %s", strerror(errno)); return false; }
        n_ = req.count;

        for (unsigned i = 0; i < n_; ++i) {
            v4l2_buffer b{};
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            b.index = i;
            if (xioctl(VIDIOC_QUERYBUF, &b) < 0) { LOGE("QUERYBUF: %s", strerror(errno)); return false; }
            bufs_[i].length = b.length;
            bufs_[i].start = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, b.m.offset);
            if (bufs_[i].start == MAP_FAILED) { LOGE("mmap: %s", strerror(errno)); return false; }
        }
        for (unsigned i = 0; i < n_; ++i) {
            v4l2_buffer b{};
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            b.index = i;
            if (xioctl(VIDIOC_QBUF, &b) < 0) { LOGE("QBUF: %s", strerror(errno)); return false; }
        }
        v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(VIDIOC_STREAMON, &t) < 0) { LOGE("STREAMON: %s", strerror(errno)); return false; }
        return true;
    }

    // Відкрити камеру, автоматично знайшовши ноду. Після USB-реенумерації номер
    // /dev/videoN «плаває», тож пробуємо задану ноду, далі скануємо /dev/video0..24
    // й беремо ту, що приймає YUYV у нашій роздільності.
    bool openAuto(Config &c) {
        if (open(c)) return true;
        close();
        for (int i = 0; i <= 24; ++i) {
            Config t = c;
            t.dev = "/dev/video" + std::to_string(i);
            if (t.dev == c.dev) continue;
            if (open(t)) { c.dev = t.dev; return true; }
            close();
        }
        return false;
    }

    // Блокуюче очікування кадру (select). Повертає індекс і вказівник на YUYV.
    // Після обробки обов'язково requeue().
    int grab(void **data, size_t *bytes, int timeoutMs = 2000) {
        for (;;) {
            fd_set fds; FD_ZERO(&fds); FD_SET(fd_, &fds);
            timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
            int r = select(fd_ + 1, &fds, nullptr, nullptr, &tv);
            if (r < 0) { if (errno == EINTR) continue; LOGE("select: %s", strerror(errno)); return -1; }
            if (r == 0) { LOGE("V4L2 timeout (камера відвалилась?)"); return -1; }

            cur_ = v4l2_buffer{};
            cur_.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            cur_.memory = V4L2_MEMORY_MMAP;
            if (xioctl(VIDIOC_DQBUF, &cur_) < 0) {
                if (errno == EAGAIN) continue;
                LOGE("DQBUF: %s", strerror(errno));
                return -1;
            }
            *data = bufs_[cur_.index].start;
            *bytes = cur_.bytesused;
            return (int)cur_.index;
        }
    }

    bool requeue() {
        return xioctl(VIDIOC_QBUF, &cur_) >= 0;
    }

    void close() {
        if (fd_ >= 0) {
            v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            xioctl(VIDIOC_STREAMOFF, &t);
            for (unsigned i = 0; i < n_; ++i)
                if (bufs_[i].start && bufs_[i].start != MAP_FAILED) munmap(bufs_[i].start, bufs_[i].length);
            ::close(fd_);
            fd_ = -1;
        }
    }

    ~V4l2Capture() { close(); }

private:
    int xioctl(unsigned long req, void *arg) {
        int r; do { r = ioctl(fd_, req, arg); } while (r < 0 && errno == EINTR);
        return r;
    }
    Config cfg_;
    int fd_ = -1;
    unsigned n_ = 0;
    V4l2Buf bufs_[8]{};
    v4l2_buffer cur_{};
};

// ─────────────────────────────────────────────────────────────────────────────
// TCP: слухаємо порт, чекаємо на одного клієнта (плеєр на ноуті)
// ─────────────────────────────────────────────────────────────────────────────
static int tcp_listen_accept(int port) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { LOGE("socket: %s", strerror(errno)); return -1; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(srv, (sockaddr *)&a, sizeof(a)) < 0) { LOGE("bind :%d: %s", port, strerror(errno)); ::close(srv); return -1; }
    if (listen(srv, 1) < 0) { LOGE("listen: %s", strerror(errno)); ::close(srv); return -1; }
    LOGI("Слухаю на :%d — під'єднуй плеєр (tcp://<IP плати>:%d)", port, port);
    sockaddr_in cli{}; socklen_t cl = sizeof(cli);
    int fd = accept(srv, (sockaddr *)&cli, &cl);
    ::close(srv);
    if (fd < 0) { LOGE("accept: %s", strerror(errno)); return -1; }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); // низька затримка
    LOGI("Клієнт під'єднався: %s", inet_ntoa(cli.sin_addr));
    return fd;
}

static bool send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return false; }
        p += n; len -= (size_t)n;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// RK_MPI_VENC: налаштування апаратного енкодера
// ─────────────────────────────────────────────────────────────────────────────
static bool venc_init(const Config &c) {
    VENC_CHN_ATTR_S attr;
    memset(&attr, 0, sizeof(attr));

    attr.stVencAttr.enType         = c.hevc ? RK_VIDEO_ID_HEVC : RK_VIDEO_ID_AVC;
    attr.stVencAttr.enPixelFormat  = RK_FMT_YUV420SP;          // NV12 на вході енкодера
    attr.stVencAttr.u32PicWidth    = c.outW;
    attr.stVencAttr.u32PicHeight   = c.outH;
    attr.stVencAttr.u32VirWidth    = c.outW;
    attr.stVencAttr.u32VirHeight   = c.outH;
    attr.stVencAttr.u32StreamBufCnt = 3;
    attr.stVencAttr.u32BufSize     = c.outW * c.outH * 3 / 2;
    attr.stVencAttr.enMirror       = MIRROR_NONE;

    // Керування бітрейтом — CBR (стабільний потік для мережі).
    if (c.hevc) {
        attr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
        attr.stRcAttr.stH265Cbr.u32BitRate           = c.bitrateKbps;
        attr.stRcAttr.stH265Cbr.u32Gop               = c.gop;
        attr.stRcAttr.stH265Cbr.u32SrcFrameRateNum   = c.fps;
        attr.stRcAttr.stH265Cbr.u32SrcFrameRateDen   = 1;
        attr.stRcAttr.stH265Cbr.fr32DstFrameRateNum  = c.fps;
        attr.stRcAttr.stH265Cbr.fr32DstFrameRateDen  = 1;
    } else {
        attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
        attr.stRcAttr.stH264Cbr.u32BitRate           = c.bitrateKbps;
        attr.stRcAttr.stH264Cbr.u32Gop               = c.gop;
        attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum   = c.fps;
        attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen   = 1;
        attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum  = c.fps;
        attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen  = 1;
    }

    int ret = RK_MPI_VENC_CreateChn(c.vencChn, &attr);
    if (ret != RK_SUCCESS) { LOGE("RK_MPI_VENC_CreateChn: 0x%x", ret); return false; }

    LOGI("VENC чан %d: %s %dx%d, CBR %d kbps, GOP %d, %d fps",
         c.vencChn, c.hevc ? "H.265" : "H.264", c.outW, c.outH, c.bitrateKbps, c.gop, c.fps);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Головний цикл
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char **argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](int &dst){ if (i + 1 < argc) dst = atoi(argv[++i]); };
        if      (a == "-d" && i + 1 < argc) c.dev = argv[++i];
        else if (a == "-p") next(c.port);
        else if (a == "-b") next(c.bitrateKbps);
        else if (a == "-g") next(c.gop);
        else if (a == "-f") next(c.fps);
        else if (a == "-W") next(c.outW);
        else if (a == "-H") next(c.outH);
        else if (a == "--h264") c.hevc = false;
        else if (a == "--h265") c.hevc = true;
        else if (a == "-h" || a == "--help") {
            printf("Використання: %s [опції]\n"
                   "  -d <dev>   нода камери (деф. /dev/video21)\n"
                   "  -p <port>  TCP-порт (деф. 8080)\n"
                   "  -b <kbps>  бітрейт CBR (деф. 4000)\n"
                   "  -g <gop>   інтервал IDR (деф. 30)\n"
                   "  -f <fps>   кадрів/с (деф. 30)\n"
                   "  -W <w> -H <h>  роздільність кодування; RGA масштабує (деф. 640x512)\n"
                   "  --h264 | --h265  кодек (деф. --h265)\n", argv[0]);
            return 0;
        }
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    // 1) rockit + апаратний енкодер — ІНІЦІАЛІЗУЄМО ПЕРШИМИ.
    //    Якщо камеру відкрити ДО RK_MPI_SYS_Init, ініціалізація rockit/медіа-підсистеми
    //    збиває вже активний V4L2-стрім USB-камери і перший DQBUF повертає ENODEV
    //    ("No such device"). Тому камеру відкриваємо/стрімимо ОСТАННЬОЮ.
    if (RK_MPI_SYS_Init() != RK_SUCCESS) { LOGE("RK_MPI_SYS_Init"); return 1; }
    if (!venc_init(c)) { RK_MPI_SYS_Exit(); return 1; }

    // 2) Пул DMA-буферів під NV12 (вихід RGA / вхід VENC)
    const int nv12Size = c.outW * c.outH * 3 / 2;
    MB_POOL_CONFIG_S poolCfg;
    memset(&poolCfg, 0, sizeof(poolCfg));
    poolCfg.u64MBSize    = nv12Size;
    poolCfg.u32MBCnt     = 4;
    poolCfg.enAllocType  = MB_ALLOC_TYPE_DMA;
    poolCfg.enRemapMode  = MB_REMAP_MODE_CACHED;
    MB_POOL pool = RK_MPI_MB_CreatePool(&poolCfg);
    if (pool == MB_INVALID_POOLID) { LOGE("MB_CreatePool"); RK_MPI_VENC_DestroyChn(c.vencChn); RK_MPI_SYS_Exit(); return 1; }

    // 3) Мережа — чекаємо плеєр (щоб не стрімити камеру «в нікуди»)
    int cli = tcp_listen_accept(c.port);
    if (cli < 0) { RK_MPI_MB_DestroyPool(pool); RK_MPI_VENC_DestroyChn(c.vencChn); RK_MPI_SYS_Exit(); return 1; }

    // 4) Камера — відкриваємо й вмикаємо стрім ОСТАННЬОЮ, коли rockit готовий і клієнт під'єднаний
    V4l2Capture cap;
    if (!cap.openAuto(c)) { ::close(cli); RK_MPI_MB_DestroyPool(pool); RK_MPI_VENC_DestroyChn(c.vencChn); RK_MPI_SYS_Exit(); return 1; }

    // 5) Пул DMA-буферів під ВХІДНИЙ YUYV — копія з V4L2-mmap, щоб RGA читала dma-fd
    const int yuyvSize = c.capW * c.capH * 2;
    MB_POOL_CONFIG_S srcPoolCfg;
    memset(&srcPoolCfg, 0, sizeof(srcPoolCfg));
    srcPoolCfg.u64MBSize   = yuyvSize;
    srcPoolCfg.u32MBCnt    = 3;
    srcPoolCfg.enAllocType = MB_ALLOC_TYPE_DMA;
    srcPoolCfg.enRemapMode = MB_REMAP_MODE_CACHED;
    MB_POOL srcPool = RK_MPI_MB_CreatePool(&srcPoolCfg);
    if (srcPool == MB_INVALID_POOLID) {
        LOGE("MB_CreatePool src");
        cap.close(); ::close(cli); RK_MPI_MB_DestroyPool(pool);
        RK_MPI_VENC_DestroyChn(c.vencChn); RK_MPI_SYS_Exit(); return 1;
    }

    LOGI("Старт стріму. Ctrl-C — стоп.");
    VENC_RECV_PIC_PARAM_S recv;
    memset(&recv, 0, sizeof(recv));
    recv.s32RecvPicNum = -1;                        // приймати кадри без обмеження
    RK_MPI_VENC_StartRecvFrame(c.vencChn, &recv);

    uint64_t frameIdx = 0;
    bool alive = true;
    while (!g_stop && alive) {
        // --- захопити YUYV ---
        void *yuyv = nullptr; size_t ybytes = 0;
        int idx = cap.grab(&yuyv, &ybytes);
        if (idx < 0) {
            // Камера відвалилась (USB-реенумерація: DQBUF=ENODEV). Стрім НЕ рвемо —
            // перевідкриваємо камеру (нода могла змінитись), VENC/мережу тримаємо.
            LOGE("Втрата камери — перевідкриваю (VENC/мережа лишаються)…");
            cap.close();
            bool ok = false;
            for (int t = 0; t < 30 && !g_stop && alive; ++t) {
                usleep(400 * 1000);
                if (cap.openAuto(c)) { ok = true; break; }
            }
            if (!ok) { LOGE("Камера не повернулась за ~12с — вихід."); break; }
            LOGI("Камера повернулась (%s), продовжую.", c.dev.c_str());
            continue;
        }

        // --- DMA-буфер під NV12 (вихід RGA / вхід VENC) ---
        MB_BLK blk = RK_MPI_MB_GetMB(pool, nv12Size, RK_TRUE);
        if (!blk) { LOGE("GetMB nv12"); cap.requeue(); continue; }
        int nfd = RK_MPI_MB_Handle2Fd(blk);

        // --- копія YUYV із V4L2-mmap у DMA-буфер ---
        // RGA не імпортує V4L2-mmap за віртуальною адресою (device-memory) → падає.
        // Кладемо кадр у DMA-MB і віддаємо RGA як dma-fd.
        MB_BLK sblk = RK_MPI_MB_GetMB(srcPool, (RK_U64)yuyvSize, RK_TRUE);
        if (!sblk) { LOGE("GetMB src"); RK_MPI_MB_ReleaseMB(blk); cap.requeue(); continue; }
        memcpy(RK_MPI_MB_Handle2VirAddr(sblk), yuyv,
               ybytes < (size_t)yuyvSize ? ybytes : (size_t)yuyvSize);
        int sfd = RK_MPI_MB_Handle2Fd(sblk);
        cap.requeue(); // V4L2-буфер повертаємо одразу після копії
        RK_MPI_SYS_MmzFlushCache(sblk, RK_FALSE);

        // --- RGA: YUYV → NV12 (+масштаб, якщо out != cap) ---
        rga_buffer_t src = wrapbuffer_fd(sfd, c.capW, c.capH, RK_FORMAT_YUYV_422);
        rga_buffer_t dst = wrapbuffer_fd(nfd, c.outW, c.outH, RK_FORMAT_YCbCr_420_SP);
        IM_STATUS st = imcvtcolor(src, dst, src.format, dst.format);
        RK_MPI_MB_ReleaseMB(sblk);
        if (st <= 0) { // 0=FAILED, <0=помилки; 1=SUCCESS, 2=NOERROR
            LOGE("RGA imcvtcolor: %d (%s)", st, imStrError(st));
            RK_MPI_MB_ReleaseMB(blk);
            continue;
        }
        RK_MPI_SYS_MmzFlushCache(blk, RK_FALSE); // синхронізувати кеш перед VEPU

        // --- згодувати кадр енкодеру ---
        VIDEO_FRAME_INFO_S frame;
        memset(&frame, 0, sizeof(frame));
        frame.stVFrame.pMbBlk        = blk;
        frame.stVFrame.u32Width      = c.outW;
        frame.stVFrame.u32Height     = c.outH;
        frame.stVFrame.u32VirWidth   = c.outW;
        frame.stVFrame.u32VirHeight  = c.outH;
        frame.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
        frame.stVFrame.u32TimeRef    = (RK_U32)frameIdx;
        frame.stVFrame.u64PTS        = frameIdx * 1000000ull / (c.fps ? c.fps : 30);
        int ret = RK_MPI_VENC_SendFrame(c.vencChn, &frame, 1000);
        RK_MPI_MB_ReleaseMB(blk); // VENC утримує власне посилання до завершення
        if (ret != RK_SUCCESS) { LOGE("SendFrame: 0x%x", ret); continue; }

        // --- забрати закодований пакет і віддати в мережу ---
        VENC_STREAM_S stream;
        VENC_PACK_S pack;
        memset(&stream, 0, sizeof(stream));
        memset(&pack, 0, sizeof(pack));
        stream.pstPack = &pack;
        stream.u32PackCount = 1;
        ret = RK_MPI_VENC_GetStream(c.vencChn, &stream, 1000);
        if (ret == RK_SUCCESS && stream.u32PackCount > 0) {
            void *p = RK_MPI_MB_Handle2VirAddr(pack.pMbBlk);
            RK_U32 off = pack.u32Offset;
            RK_U32 len = pack.u32Len;
            if (p && len > off) {
                if (!send_all(cli, (char *)p + off, len - off)) {
                    LOGI("Клієнт відключився — завершую.");
                    alive = false;
                }
            }
            RK_MPI_VENC_ReleaseStream(c.vencChn, &stream);
        } else if (ret != RK_SUCCESS) {
            // не критично — інколи пакет ще не готовий
        }

        if ((++frameIdx % (uint64_t)(c.fps ? c.fps : 30)) == 0)
            LOGI("… %llu кадрів", (unsigned long long)frameIdx);
    }

    LOGI("Зупинка. Всього кадрів: %llu", (unsigned long long)frameIdx);
    RK_MPI_VENC_StopRecvFrame(c.vencChn);
    RK_MPI_VENC_DestroyChn(c.vencChn);
    RK_MPI_MB_DestroyPool(pool);
    RK_MPI_MB_DestroyPool(srcPool);
    if (cli >= 0) ::close(cli);
    cap.close();
    RK_MPI_SYS_Exit();
    return 0;
}
