#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "im2d.hpp"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"

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
    unsigned port = 5000;
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
        "  --port N            TCP listen port (default 5000)\n",
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
        std::fprintf(stderr, "TCP port must be <= 65535\n");
        std::exit(EXIT_FAILURE);
    }
    return cfg;
}

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
        if (parm.parm.capture.timeperframe.numerator &&
            parm.parm.capture.timeperframe.denominator / parm.parm.capture.timeperframe.numerator != cfg.fps) {
            std::fprintf(stderr, "Warning: camera selected %u/%u seconds per frame\n",
                         parm.parm.capture.timeperframe.numerator,
                         parm.parm.capture.timeperframe.denominator);
        }

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

class TcpServer {
public:
    ~TcpServer() {
        disconnect_client();
        if (listen_fd_ >= 0) close(listen_fd_);
    }

    bool start(unsigned port) {
        listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listen_fd_ < 0) return fail("socket");
        int one = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(static_cast<uint16_t>(port));
        if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
            return fail("bind");
        if (listen(listen_fd_, 1) < 0) return fail("listen");
        std::printf("Listening on TCP port %u\n", port);
        return true;
    }

    bool wait_for_client() {
        disconnect_client();
        while (!g_stop) {
            pollfd item{listen_fd_, POLLIN, 0};
            int rc = poll(&item, 1, 500);
            if (rc < 0 && errno == EINTR) continue;
            if (rc < 0) return fail("poll listen socket");
            if (rc == 0) continue;
            client_fd_ = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (client_fd_ < 0 && errno == EINTR) continue;
            if (client_fd_ < 0) return fail("accept");
            int one = 1;
            setsockopt(client_fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            timeval timeout{2, 0};
            setsockopt(client_fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            std::printf("Receiver connected\n");
            return true;
        }
        return false;
    }

    bool send_all(const void *data, size_t length) {
        const unsigned char *cursor = static_cast<const unsigned char *>(data);
        while (length && !g_stop) {
            ssize_t sent = send(client_fd_, cursor, length, MSG_NOSIGNAL);
            if (sent < 0 && errno == EINTR) continue;
            if (sent <= 0) {
                std::fprintf(stderr, "Receiver disconnected: %s\n", std::strerror(errno));
                disconnect_client();
                return false;
            }
            cursor += sent;
            length -= static_cast<size_t>(sent);
        }
        return length == 0;
    }

private:
    bool fail(const char *operation) {
        std::fprintf(stderr, "%s: %s\n", operation, std::strerror(errno));
        return false;
    }
    void disconnect_client() {
        if (client_fd_ >= 0) close(client_fd_);
        client_fd_ = -1;
    }
    int listen_fd_ = -1;
    int client_fd_ = -1;
};

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

    bool encode(const void *yuyv, uint64_t sequence, TcpServer &network) {
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
        // Rockit returns an MB handle whose virtual address already points to
        // the valid pack data. Do not add u32Offset here: the official RV1106
        // samples send Handle2VirAddr(pMbBlk), u32Len directly. Adding the
        // offset a second time truncates HEVC NAL units and breaks references.
        void *encoded = RK_MPI_MB_Handle2VirAddr(pack_.pMbBlk);
        bool sent = encoded && network.send_all(encoded, pack_.u32Len);
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
    TcpServer network;
    if (!camera.open_camera(cfg) || !pipeline.init(cfg) || !network.start(cfg.port))
        return EXIT_FAILURE;

    std::printf("Camera %ux%u YUYV @ %u fps; RGA -> %ux%u NV12; HEVC %u kbit/s\n",
                cfg.input_width, cfg.input_height, cfg.fps,
                cfg.output_width, cfg.output_height, cfg.bitrate_kbps);

    uint64_t sequence = 0;
    uint64_t stats_started_ms = monotonic_ms();
    uint64_t stats_frames = 0;
    uint64_t camera_drops = 0;
    uint32_t previous_camera_sequence = 0;
    bool have_camera_sequence = false;
    while (!g_stop) {
        if (!network.wait_for_client()) break;
        pipeline.request_idr();
        stats_started_ms = monotonic_ms();
        stats_frames = 0;
        camera_drops = 0;
        have_camera_sequence = false;
        while (!g_stop) {
            v4l2_buffer buffer{};
            const void *data = nullptr;
            if (!camera.dequeue(buffer, data)) continue;
            if (have_camera_sequence && buffer.sequence > previous_camera_sequence + 1)
                camera_drops += buffer.sequence - previous_camera_sequence - 1;
            previous_camera_sequence = buffer.sequence;
            have_camera_sequence = true;
            bool ok = pipeline.encode(data, sequence++, network);
            if (!camera.requeue(buffer)) {
                std::fprintf(stderr, "VIDIOC_QBUF failed: %s\n", std::strerror(errno));
                g_stop = 1;
                break;
            }
            if (!ok) break;
            ++stats_frames;
            uint64_t now_ms = monotonic_ms();
            uint64_t elapsed_ms = now_ms - stats_started_ms;
            if (elapsed_ms >= 2000) {
                double measured_fps = stats_frames * 1000.0 / elapsed_ms;
                std::printf("Pipeline: %.1f fps, V4L2 dropped frames: %llu\n",
                            measured_fps,
                            static_cast<unsigned long long>(camera_drops));
                stats_started_ms = now_ms;
                stats_frames = 0;
                camera_drops = 0;
            }
        }
    }
    return EXIT_SUCCESS;
}
