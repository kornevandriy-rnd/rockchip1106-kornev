# RV1106 thermal-camera stream

Two small programs implement this pipeline:

```text
USB V4L2 camera (640x512 YUYV, 60 fps)
  -> RGA2 (scale and convert)
  -> 1280x1024 NV12
  -> Rockit / VEPU H.265 CBR
  -> raw Annex-B HEVC over TCP
  -> Fedora receiver -> ffplay
```

The only CPU copy on the board is the 640x512 YUYV frame from the UVC MMAP
buffer to an MMZ/DMA buffer. RGA scaling/color conversion and H.265 encoding
are hardware accelerated. This fallback is intentional because many UVC
drivers do not support `VIDIOC_EXPBUF`.

## 1. Confirm the camera mode

On the board:

```sh
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --list-formats-ext
```

The camera must expose uncompressed `YUYV` at `640x512`, 60 fps. If its device
node differs, pass `--device` when starting the sender.

## 2. Build the RV1106 sender

Build the Luckfox SDK for `RV1106_Luckfox_Pico_Ultra` first. Then source its
32-bit uClibc toolchain environment and build:

```sh
cd /path/to/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf
source env_install_toolchain.sh

cd /path/to/this/project/rv1106_sender
make LUCKFOX_SDK=/path/to/luckfox-pico
```

Copy `rv1106_sender` to the board. The board image must contain `librockit.so`,
`librockchip_mpp.so`, and `librga.so` from the same SDK build.

Start it on the board:

```sh
./rv1106_sender --device /dev/video0 --port 5000
```

Defaults are `640x512@60`, `1280x1024`, H.265 CBR 8000 kbit/s, GOP 60. See all
options with:

```sh
./rv1106_sender --help
```

While streaming, the sender prints measured pipeline FPS and V4L2 sequence
drops every two seconds. This distinguishes a camera/USB FPS problem from an
RGA/VENC/network problem.

If the board firewall is enabled, allow TCP port 5000. Find the Ethernet IP
with `ip addr`.

## 3. Build and run the Fedora receiver

Install FFmpeg and compile the receiver:

```sh
sudo dnf install ffmpeg gcc make
cd /path/to/this/project/fedora_receiver
make
```

Run it with the board's IP address:

```sh
./fedora_receiver 192.168.1.50 5000
```

The receiver replaces itself with `ffplay`, which connects directly to the
board. Avoiding an intermediate pipe removes one buffering/backpressure point.
Stop with Ctrl-C.

## Diagnostics

Test the sender without the receiver program:

```sh
ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 \
  -f hevc tcp://192.168.1.50:5000
```

If the camera cannot actually sustain 60 fps, test the exact capture mode:

```sh
v4l2-ctl -d /dev/video0 \
  --set-fmt-video=width=640,height=512,pixelformat=YUYV \
  --set-parm=60 --stream-mmap=4 --stream-count=600 --stream-to=/dev/null
```

At 60 fps, USB capture alone transfers about 39.3 MB/s. This is above USB 2.0
full-speed capacity and requires a USB high-speed camera/link.
