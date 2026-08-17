// rtsp_sender.cpp — RTSP-варіант відправника для Sine.video Beam.
//
// Той самий перевірений конвеєр, що і в rv1106_sender керівника
// (V4L2 YUYV -> RGA NV12+апскейл -> RK_MPI_VENC H.265 CBR), але вихід не сирий
// TCP, а RTSP-сервер (Rockchip librtsp/rtsp_demo). Beam підключається як
// RTSP-клієнт і тягне потік: rtsp://<IP-плати>:554/live/0
//
// Camera та Pipeline узяті один-в-один з коду керівника (включно з фіксами:
// без +u32Offset і з RK_MPI_SYS_MmzFlushCache перед читанням виходу VEPU).
// Змінено ЛИШЕ приймач кадрів: TcpServer -> RtspOutput.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "im2d.hpp"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rtsp_demo.h"  // Rockchip librtsp: extern "C" всередині

namespace {

volatile sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

uint64_t monotonic_ms() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000ULL +
           static_cast<uint64_t>(now.tv_nsec) / 1000000ULL;
}

struct Config {
    std::string device = "/dev/video0";
    unsigned input_width = 640;
    unsigned input_height = 512;
    unsigned output_width = 1280;
    unsigned output_height = 1024;
    unsigned fps = 60;
    unsigned bitrate_kbps = 8000;
    unsigned gop = 60;
    unsigned port = 554;              // RTSP-порт (Beam тягне з нього)
    std::string path = "/live/0";     // RTSP-шлях
    unsigned camera_buffers = 4;
};

struct CameraBuffer {
    void *address = MAP_FAILED;
    size_t length = 0;
};

int xioctl(int fd, unsigned long request, void *arg) {
    int rc;
    do {
        rc = ioctl(fd, request, arg);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

unsigned parse_u32(const char *name, const char *value, unsigned min_value) {
    char *end = nullptr;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno || !end || *end != '\0' || parsed < min_value || parsed > 0xffffffffUL) {
        std::fprintf(stderr, "Invalid value for %s: %s\n", name, value);
        std::exit(EXIT_FAILURE);
    }
    return static_cast<unsigned>(parsed);
}

void usage(const char *program) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  --device PATH       V4L2 device (default /dev/video0)\n"
        "  --input-width N     Camera width (default 640)\n"
        "  --input-height N    Camera height (default 512)\n"
        "  --output-width N    Encoded width (default 1280)\n"
        "  --output-height N   Encoded height (default 1024)\n"
        "  --fps N             Frame rate (default 60)\n"
        "  --bitrate N         H.265 CBR bitrate in kbit/s (default 8000)\n"
        "  --gop N             GOP length (default 60)\n"
        "  --port N            RTSP port (default 554)\n"
        "  --path STR          RTSP path (default /live/0)\n",
        program);
}

Config parse_args(int argc, char **argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--help")) {
            usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        }
        if (i + 1 >= argc) {
            usage(argv[0]);
            std::exit(EXIT_FAILURE);
        }
        const char *value = argv[++i];
        if (!std::strcmp(argv[i - 1], "--device")) cfg.device = value;
        else if (!std::strcmp(argv[i - 1], "--input-width")) cfg.input_width = parse_u32("input-width", value, 2);
        else if (!std::strcmp(argv[i - 1], "--input-height")) cfg.input_height = parse_u32("input-height", value, 2);
        else if (!std::strcmp(argv[i - 1], "--output-width")) cfg.output_width = parse_u32("output-width", value, 2);
        else if (!std::strcmp(argv[i - 1], "--output-height")) cfg.output_height = parse_u32("output-height", value, 2);
        else if (!std::strcmp(argv[i - 1], "--fps")) cfg.fps = parse_u32("fps", value, 1);
        else if (!std::strcmp(argv[i - 1], "--bitrate")) cfg.bitrate_kbps = parse_u32("bitrate", value, 1);
        else if (!std::strcmp(argv[i - 1], "--gop")) cfg.gop = parse_u32("gop", value, 1);
        else if (!std::strcmp(argv[i - 1], "--port")) cfg.port = parse_u32("port", value, 1);
        else if (!std::strcmp(argv[i - 1], "--path")) cfg.path = value;
        else {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i - 1]);
            usage(argv[0]);
            std::exit(EXIT_FAILURE);
        }
    }
    if ((cfg.input_width | cfg.input_height | cfg.output_width | cfg.output_height) & 1U) {
        std::fprintf(stderr, "YUV dimensions must be even\n");
        std::exit(EXIT_FAILURE);
    }
    if (cfg.port > 65535) {
        std::fprintf(stderr, "RTSP port must be <= 65535\n");
        std::exit(EXIT_FAILURE);
    }
    return cfg;
}

// ==== Camera: узято один-в-один з rv1106_sender/src/main.cpp керівника ====
class Camera {
public:
    ~Camera() { close_camera(); }

    bool open_camera(const Config &cfg) {
        fd_ = open(cfg.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) return fail("open V4L2 device");

        v4l2_capability caps{};
        if (xioctl(fd_, VIDIOC_QUERYCAP, &caps) < 0) return fail("VIDIOC_QUERYCAP");
        if (!(caps.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
            !(caps.capabilities & V4L2_CAP_STREAMING)) {
            std::fprintf(stderr, "%s is not a streaming capture device\n", cfg.device.c_str());
            return false;
        }

        v4l2_format format{};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = cfg.input_width;
        format.fmt.pix.height = cfg.input_height;
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        format.fmt.pix.field = V4L2_FIELD_ANY;
        if (xioctl(fd_, VIDIOC_S_FMT, &format) < 0) return fail("VIDIOC_S_FMT");
        if (format.fmt.pix.width != cfg.input_width || format.fmt.pix.height != cfg.input_height ||
            format.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
            std::fprintf(stderr,
                         "Camera rejected requested %ux%u YUYV format; got %ux%u fourcc %.4s\n",
                         cfg.input_width, cfg.input_height,
                         format.fmt.pix.width, format.fmt.pix.height,
                         reinterpret_cast<const char *>(&format.fmt.pix.pixelformat));
            return false;
        }
        bytes_per_frame_ = cfg.input_width * cfg.input_height * 2U;
        if (format.fmt.pix.sizeimage < bytes_per_frame_) {
            std::fprintf(stderr, "V4L2 sizeimage is smaller than one YUYV frame\n");
            return false;
        }

        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = cfg.fps;
        if (xioctl(fd_, VIDIOC_S_PARM, &parm) < 0) return fail("VIDIOC_S_PARM");

        v4l2_requestbuffers request{};
        request.count = cfg.camera_buffers;
        request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        request.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_REQBUFS, &request) < 0) return fail("VIDIOC_REQBUFS");
        if (request.count < 2) {
            std::fprintf(stderr, "Camera returned fewer than two buffers\n");
            return false;
        }

        buffers_.resize(request.count);
        for (unsigned i = 0; i < request.count; ++i) {
            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = i;
            if (xioctl(fd_, VIDIOC_QUERYBUF, &buffer) < 0) return fail("VIDIOC_QUERYBUF");
            buffers_[i].length = buffer.length;
            buffers_[i].address = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE,
                                       MAP_SHARED, fd_, buffer.m.offset);
            if (buffers_[i].address == MAP_FAILED) return fail("mmap V4L2 buffer");
            if (xioctl(fd_, VIDIOC_QBUF, &buffer) < 0) return fail("VIDIOC_QBUF");
        }

        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) return fail("VIDIOC_STREAMON");
        streaming_ = true;
        return true;
    }

    bool dequeue(v4l2_buffer &buffer, const void *&data) {
        pollfd item{fd_, POLLIN, 0};
        int rc;
        do rc = poll(&item, 1, 1000); while (rc < 0 && errno == EINTR && !g_stop);
        if (rc == 0) {
            std::fprintf(stderr, "V4L2 frame timeout\n");
            return false;
        }
        if (rc < 0) return fail("poll V4L2");
        std::memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN) return false;
            return fail("VIDIOC_DQBUF");
        }
        if (buffer.index >= buffers_.size() || buffer.bytesused < bytes_per_frame_) {
            std::fprintf(stderr, "Invalid or short V4L2 frame\n");
            requeue(buffer);
            return false;
        }
        data = buffers_[buffer.index].address;
        return true;
    }

    bool requeue(v4l2_buffer &buffer) { return xioctl(fd_, VIDIOC_QBUF, &buffer) == 0; }

private:
    bool fail(const char *operation) {
        std::fprintf(stderr, "%s: %s\n", operation, std::strerror(errno));
        return false;
    }

    void close_camera() {
        if (fd_ >= 0 && streaming_) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            xioctl(fd_, VIDIOC_STREAMOFF, &type);
        }
        for (const CameraBuffer &buffer : buffers_) {
            if (buffer.address != MAP_FAILED) munmap(buffer.address, buffer.length);
        }
        if (fd_ >= 0) close(fd_);
    }

    int fd_ = -1;
    bool streaming_ = false;
    size_t bytes_per_frame_ = 0;
    std::vector<CameraBuffer> buffers_;
};

// ==== RtspOutput: заміна TcpServer — віддає H.265-кадри як RTSP ====
class RtspOutput {
public:
    ~RtspOutput() {
        if (session_) rtsp_del_session(session_);
        if (demo_) rtsp_del_demo(demo_);
    }

    bool start(unsigned port, const std::string &path) {
        demo_ = create_rtsp_demo(static_cast<int>(port));
        if (!demo_) {
            std::fprintf(stderr, "create_rtsp_demo(%u) failed (порт зайнятий? прибий rkipc)\n", port);
            return false;
        }
        session_ = rtsp_new_session(demo_, path.c_str());
        if (!session_) {
            std::fprintf(stderr, "rtsp_new_session(%s) failed\n", path.c_str());
            return false;
        }
        port_ = port;
        path_ = path;
        return true;
    }

    void poll() { if (demo_) rtsp_do_event(demo_); }

    // Згодувати один H.265 access unit (Annex-B). RTSP терпить відсутність клієнта.
    bool send(const uint8_t *au, size_t len) {
        if (!video_set_) {
            uint8_t hdr[1024];
            int hlen = extract_param_sets(au, len, hdr, static_cast<int>(sizeof hdr));
            if (hlen > 0) {
                rtsp_set_video(session_, RTSP_CODEC_ID_VIDEO_H265, hdr, hlen);
                rtsp_sync_video_ts(session_, rtsp_get_reltime(), rtsp_get_ntptime());
                video_set_ = true;
                std::printf("RTSP: H265 VPS/SPS/PPS зчитано (%d Б); стрім на rtsp://<IP-плати>:%u%s\n",
                            hlen, port_, path_.c_str());
            }
        }
        rtsp_do_event(demo_);
        rtsp_tx_video(session_, au, static_cast<int>(len), rtsp_get_reltime());
        return true;
    }

private:
    // Витягти VPS(32)/SPS(33)/PPS(34) NAL-и з першого access unit (кожен зі
    // старт-кодом 00 00 00 01) — для SDP через rtsp_set_video.
    static int extract_param_sets(const uint8_t *buf, size_t len, uint8_t *out, int out_cap) {
        int out_len = 0;
        size_t pos = 0;
        while (pos + 3 < len) {
            size_t sc = 0;
            if (buf[pos] == 0 && buf[pos + 1] == 0 && buf[pos + 2] == 0 && buf[pos + 3] == 1) sc = 4;
            else if (buf[pos] == 0 && buf[pos + 1] == 0 && buf[pos + 2] == 1) sc = 3;
            else { ++pos; continue; }
            size_t nal_start = pos + sc;
            if (nal_start >= len) break;
            size_t next = nal_start;
            while (next + 2 < len &&
                   !(buf[next] == 0 && buf[next + 1] == 0 && buf[next + 2] == 1) &&
                   !(next + 3 < len && buf[next] == 0 && buf[next + 1] == 0 &&
                     buf[next + 2] == 0 && buf[next + 3] == 1))
                ++next;
            size_t nal_end = (next + 2 < len) ? next : len;
            uint8_t type = (buf[nal_start] >> 1) & 0x3F;
            if (type == 32 || type == 33 || type == 34) {  // VPS / SPS / PPS
                int need = 4 + static_cast<int>(nal_end - nal_start);
                if (out_len + need <= out_cap) {
                    out[out_len++] = 0; out[out_len++] = 0; out[out_len++] = 0; out[out_len++] = 1;
                    std::memcpy(out + out_len, buf + nal_start, nal_end - nal_start);
                    out_len += static_cast<int>(nal_end - nal_start);
                }
            } else if (type <= 31) {
                break;  // перший VCL-слайс — параметр-сети скінчились
            }
            pos = nal_end;
        }
        return out_len;
    }

    rtsp_demo_handle demo_ = nullptr;
    rtsp_session_handle session_ = nullptr;
    bool video_set_ = false;
    unsigned port_ = 554;
    std::string path_ = "/live/0";
};

// ==== Pipeline: RGA+VENC один-в-один з коду керівника, вихід -> RtspOutput ====
class Pipeline {
public:
    ~Pipeline() { shutdown(); }

    bool init(const Config &cfg) {
        cfg_ = cfg;
        input_size_ = cfg.input_width * cfg.input_height * 2U;
        output_size_ = cfg.output_width * cfg.output_height * 3U / 2U;

        if (RK_MPI_SYS_Init() != RK_SUCCESS) {
            std::fprintf(stderr, "RK_MPI_SYS_Init failed\n");
            return false;
        }
        mpi_initialized_ = true;

        if (RK_MPI_SYS_MmzAlloc(&input_mb_, nullptr, nullptr, input_size_) != RK_SUCCESS ||
            RK_MPI_SYS_MmzAlloc(&output_mb_, nullptr, nullptr, output_size_) != RK_SUCCESS) {
            std::fprintf(stderr, "RK_MPI_SYS_MmzAlloc failed\n");
            return false;
        }
        input_address_ = RK_MPI_MB_Handle2VirAddr(input_mb_);
        int input_fd = RK_MPI_MB_Handle2Fd(input_mb_);
        int output_fd = RK_MPI_MB_Handle2Fd(output_mb_);
        input_rga_ = importbuffer_fd(input_fd, input_size_);
        output_rga_ = importbuffer_fd(output_fd, output_size_);
        if (!input_address_ || !input_rga_ || !output_rga_) {
            std::fprintf(stderr, "Failed to import MMZ buffers into RGA\n");
            return false;
        }
        input_image_ = wrapbuffer_handle(input_rga_, cfg.input_width, cfg.input_height,
                                         RK_FORMAT_YUYV_422);
        output_image_ = wrapbuffer_handle(output_rga_, cfg.output_width, cfg.output_height,
                                          RK_FORMAT_YCbCr_420_SP);
        IM_STATUS check = imcheck(input_image_, output_image_, {}, {}, 0);
        if (check != IM_STATUS_NOERROR) {
            std::fprintf(stderr, "RGA configuration rejected: %s\n", imStrError(check));
            return false;
        }

        VENC_CHN_ATTR_S attr{};
        attr.stVencAttr.enType = RK_VIDEO_ID_HEVC;
        attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
        attr.stVencAttr.u32MaxPicWidth = cfg.output_width;
        attr.stVencAttr.u32MaxPicHeight = cfg.output_height;
        attr.stVencAttr.u32PicWidth = cfg.output_width;
        attr.stVencAttr.u32PicHeight = cfg.output_height;
        attr.stVencAttr.u32VirWidth = cfg.output_width;
        attr.stVencAttr.u32VirHeight = cfg.output_height;
        attr.stVencAttr.u32StreamBufCnt = 4;
        attr.stVencAttr.u32BufSize = output_size_;
        attr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
        attr.stRcAttr.stH265Cbr.u32Gop = cfg.gop;
        attr.stRcAttr.stH265Cbr.u32BitRate = cfg.bitrate_kbps;
        attr.stRcAttr.stH265Cbr.fr32DstFrameRateNum = cfg.fps;
        attr.stRcAttr.stH265Cbr.fr32DstFrameRateDen = 1;
        attr.stRcAttr.stH265Cbr.u32SrcFrameRateNum = cfg.fps;
        attr.stRcAttr.stH265Cbr.u32SrcFrameRateDen = 1;
        if (RK_MPI_VENC_CreateChn(0, &attr) != RK_SUCCESS) {
            std::fprintf(stderr, "RK_MPI_VENC_CreateChn failed\n");
            return false;
        }
        venc_created_ = true;

        VENC_RECV_PIC_PARAM_S receive{};
        receive.s32RecvPicNum = -1;
        if (RK_MPI_VENC_StartRecvFrame(0, &receive) != RK_SUCCESS) {
            std::fprintf(stderr, "RK_MPI_VENC_StartRecvFrame failed\n");
            return false;
        }
        venc_started_ = true;
        stream_.pstPack = &pack_;
        return true;
    }

    void request_idr() { RK_MPI_VENC_RequestIDR(0, RK_TRUE); }

    bool encode(const void *yuyv, uint64_t sequence, RtspOutput &rtsp) {
        std::memcpy(input_address_, yuyv, input_size_);
        RK_MPI_SYS_MmzFlushCache(input_mb_, RK_FALSE);

        IM_STATUS rga_status = imresize(input_image_, output_image_);
        if (rga_status != IM_STATUS_SUCCESS) {
            std::fprintf(stderr, "RGA resize/convert failed: %s\n", imStrError(rga_status));
            return false;
        }

        VIDEO_FRAME_INFO_S frame{};
        frame.stVFrame.pMbBlk = output_mb_;
        frame.stVFrame.u32Width = cfg_.output_width;
        frame.stVFrame.u32Height = cfg_.output_height;
        frame.stVFrame.u32VirWidth = cfg_.output_width;
        frame.stVFrame.u32VirHeight = cfg_.output_height;
        frame.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
        frame.stVFrame.enCompressMode = COMPRESS_MODE_NONE;
        frame.stVFrame.u64PTS = sequence * 1000000ULL / cfg_.fps;
        if (RK_MPI_VENC_SendFrame(0, &frame, 1000) != RK_SUCCESS) {
            std::fprintf(stderr, "RK_MPI_VENC_SendFrame failed\n");
            return false;
        }
        std::memset(&pack_, 0, sizeof(pack_));
        if (RK_MPI_VENC_GetStream(0, &stream_, 1000) != RK_SUCCESS) {
            std::fprintf(stderr, "RK_MPI_VENC_GetStream timeout/failure\n");
            return false;
        }
        // VEPU записав цей DMA-буфер. Інвалідуємо CPU-кеш перед читанням —
        // інакше Cortex-A7 віддає застарілі рядки кешу і псує HEVC NAL-и.
        if (RK_MPI_SYS_MmzFlushCache(pack_.pMbBlk, RK_TRUE) != RK_SUCCESS) {
            std::fprintf(stderr, "Encoded-buffer cache invalidation failed\n");
            RK_MPI_VENC_ReleaseStream(0, &stream_);
            return false;
        }
        // Rockit повертає MB-хендл, чий віртуальний адрес уже вказує на валідні
        // дані пака. НЕ додавати u32Offset — інакше HEVC NAL-и обрізаються.
        const uint8_t *encoded = static_cast<const uint8_t *>(RK_MPI_MB_Handle2VirAddr(pack_.pMbBlk));
        bool sent = encoded && rtsp.send(encoded, pack_.u32Len);
        RK_MPI_VENC_ReleaseStream(0, &stream_);
        return sent;
    }

private:
    void shutdown() {
        if (venc_started_) RK_MPI_VENC_StopRecvFrame(0);
        if (venc_created_) RK_MPI_VENC_DestroyChn(0);
        if (input_rga_) releasebuffer_handle(input_rga_);
        if (output_rga_) releasebuffer_handle(output_rga_);
        if (input_mb_) RK_MPI_MB_ReleaseMB(input_mb_);
        if (output_mb_) RK_MPI_MB_ReleaseMB(output_mb_);
        if (mpi_initialized_) RK_MPI_SYS_Exit();
    }

    Config cfg_;
    size_t input_size_ = 0;
    size_t output_size_ = 0;
    bool mpi_initialized_ = false;
    bool venc_created_ = false;
    bool venc_started_ = false;
    MB_BLK input_mb_ = nullptr;
    MB_BLK output_mb_ = nullptr;
    void *input_address_ = nullptr;
    rga_buffer_handle_t input_rga_ = 0;
    rga_buffer_handle_t output_rga_ = 0;
    rga_buffer_t input_image_{};
    rga_buffer_t output_image_{};
    VENC_PACK_S pack_{};
    VENC_STREAM_S stream_{};
};

}  // namespace

int main(int argc, char **argv) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    Config cfg = parse_args(argc, argv);
    Camera camera;
    Pipeline pipeline;
    RtspOutput rtsp;
    if (!camera.open_camera(cfg) || !pipeline.init(cfg) || !rtsp.start(cfg.port, cfg.path))
        return EXIT_FAILURE;

    std::printf("Camera %ux%u YUYV @ %u fps; RGA -> %ux%u NV12; HEVC %u kbit/s\n",
                cfg.input_width, cfg.input_height, cfg.fps,
                cfg.output_width, cfg.output_height, cfg.bitrate_kbps);
    std::printf("RTSP: rtsp://<IP-плати>:%u%s  (у меню Beam вписати цей URL)\n",
                cfg.port, cfg.path.c_str());

    // На відміну від TCP-версії, RTSP-сервер стрімить БЕЗПЕРЕРВНО (як IP-камера);
    // клієнти (Beam / ffplay / VLC) під'єднуються будь-коли.
    pipeline.request_idr();
    uint64_t sequence = 0;
    uint64_t stats_started_ms = monotonic_ms();
    uint64_t stats_frames = 0;
    uint64_t camera_drops = 0;
    uint32_t previous_camera_sequence = 0;
    bool have_camera_sequence = false;
    while (!g_stop) {
        rtsp.poll();
        v4l2_buffer buffer{};
        const void *data = nullptr;
        if (!camera.dequeue(buffer, data)) continue;
        if (have_camera_sequence && buffer.sequence > previous_camera_sequence + 1)
            camera_drops += buffer.sequence - previous_camera_sequence - 1;
        previous_camera_sequence = buffer.sequence;
        have_camera_sequence = true;
        bool ok = pipeline.encode(data, sequence++, rtsp);
        if (!camera.requeue(buffer)) {
            std::fprintf(stderr, "VIDIOC_QBUF failed: %s\n", std::strerror(errno));
            break;
        }
        if (!ok) break;
        ++stats_frames;
        uint64_t now_ms = monotonic_ms();
        uint64_t elapsed_ms = now_ms - stats_started_ms;
        if (elapsed_ms >= 2000) {
            double measured_fps = stats_frames * 1000.0 / elapsed_ms;
            std::printf("Pipeline: %.1f fps, V4L2 dropped frames: %llu\n",
                        measured_fps, static_cast<unsigned long long>(camera_drops));
            stats_started_ms = now_ms;
            stats_frames = 0;
            camera_drops = 0;
        }
    }
    return EXIT_SUCCESS;
}
