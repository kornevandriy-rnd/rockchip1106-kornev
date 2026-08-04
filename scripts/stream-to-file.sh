#!/bin/sh
# stream-to-file.sh — ЕКСПЕРИМЕНТАЛЬНО. Альтернатива stream-mjpeg.sh, стійкіша до
# зривів: пише КОЖЕН кадр у /tmp/live.jpg (атомарно), а ноут показує його з
# автооновленням. Немає потокових демуксерів -> старий кадр висить, поки не
# приїде новий. Запускати НА платі (рівно одну копію).
#
# На Fedora (через SSH ControlMaster, пароль один раз):
#   ssh -M -S /tmp/lf.sock -fN root@192.168.50.2
#   ssh -S /tmp/lf.sock root@192.168.50.2 'setsid sh /tmp/stream-to-file.sh >/tmp/s.log 2>&1 </dev/null &'
#   ( while true; do scp -o ControlPath=/tmp/lf.sock -q root@192.168.50.2:/tmp/live.jpg ~/live.jpg 2>/dev/null; sleep 1; done ) &
#   sleep 5; feh --reload 1 ~/live.jpg           # (dnf install feh; або eog ~/live.jpg)
#
# Зупинити:  ssh -S /tmp/lf.sock root@192.168.50.2 'pkill -f stream-to-file.sh'
#            kill %1 ; ssh -S /tmp/lf.sock -O exit root@192.168.50.2
#
# ⚠️ Те саме обмеження, що й у stream-mjpeg.sh: потрібен КОРОТКИЙ ЕКРАНОВАНИЙ
# кабель плата<->хаб, інакше цикл заклинює камеру. Див. README, розділ "Живий перегляд".

W=640
H=512

while true; do
    CAM=""
    for v in /dev/video*; do
        v4l2-ctl -d "$v" --info 2>/dev/null | grep -qi uvc || continue
        v4l2-ctl -d "$v" --list-formats 2>/dev/null | grep -q YUYV || continue
        CAM="$v"; break
    done
    [ -z "$CAM" ] && { sleep 0.5; continue; }
    rm -f /tmp/f.yuyv
    v4l2-ctl -d "$CAM" --set-fmt-video=width=$W,height=$H,pixelformat=YUYV \
        --stream-mmap --stream-count=1 --stream-to=/tmp/f.yuyv >/dev/null 2>&1 &
    p=$!; n=0
    while kill -0 "$p" 2>/dev/null; do
        n=$((n + 1)); [ "$n" -ge 10 ] && { kill -9 "$p" 2>/dev/null; break; }
        sleep 0.5
    done
    wait "$p" 2>/dev/null
    if [ -s /tmp/f.yuyv ]; then
        ffmpeg -y -f rawvideo -pix_fmt yuyv422 -s ${W}x${H} -i /tmp/f.yuyv \
            -vf normalize /tmp/live_tmp.jpg >/dev/null 2>&1 && mv /tmp/live_tmp.jpg /tmp/live.jpg
    fi
    sleep 0.1
done
