# Цифровий радіоканал (Sine.engineering): інтеграція в наш стрім

Документ під наступний етап: **замінити кабель/Wi-Fi між RV1106 і наземною
станцією цифровим радіолінком** Sine.engineering (video — **Sine.video Beam**,
телеметрія — **Sine.link**). Тут: цільова схема, що нам треба з'ясувати у
виробника, і готовий (англ.) блок питань для їхнього support.

## Цільова схема

```
БОРТ (дрон):
  Seek 640 (USB-UVC) → RV1106: V4L2 → RGA (NV12, 1280x1024) → VEPU H.265 (CBR)
      → [ІНТЕРФЕЙС?] → Sine.video Beam (Tx)  ))) РАДІО )))

ЗЕМЛЯ:
  Sine.video Beam (Rx) → [ІНТЕРФЕЙС?] → BPI-M7 / RK3588:
      gstreamer mppvideodec (HW-декод) → HDMI / ноут
  + Sine.link: телеметрія/керування (двосторонньо)
```

## Відповідь Sine.engineering (2026-08)

- Транспорт — **по Ethernet**.
- **Beam сам кодує і декодує** відео (ключі надає Sine.link) — тобто Beam
  розрахований бути енкодером/декодером, а не прозорою «трубою» для IP.
- Наші **додаткові кодек-плати** (H.265-енкодер на RV1106 + декодер на RK3588)
  **додають затримку і псують якість** — фактично подвійне кодування.

### Два режими інтеграції

- **Режим 1 — Beam як IP-міст (тестуємо першим).** Лишаємо наш робочий пайплайн,
  Beam возить наш H.265-потік по Ethernet «як є»:
  `RV1106 sender → Beam Tx (Ethernet) ))) Beam Rx → RK3588 decode → екран`.
  Плюс: нічого не переробляти. Ризик: якщо Beam не прозорий, а перекодовує —
  подвійний енкод (те, про що попередив Sine).
- **Режим 2 — Beam-native (fallback).** Віддаємо Beam «сире»/легко стиснуте відео,
  кодує та декодує сам Beam; RV1106 **не** кодує, RK3588 **не** декодує → без
  подвійного енкоду й зайвої затримки. Відкрите питання: формат входу Beam
  (raw-over-IP / HDMI / MIPI-CSI) і смуга — сире 1280×1024@60 ≈ 1.3 Гбіт/с, що
  забагато для 100 Мбіт/с Ethernet RV1106, тож імовірно потрібна нижча
  роздільність/фпс або легка компресія на вході.

### Рішення (2026-08)

Тестуємо **Режим 1** з наявним залізом. Якщо затримка/якість погані або Beam
відмовляється возити наш потік — переходимо до **Режиму 2** і досліджуємо, який
вхід очікує Beam.

### Результат тесту Режиму 1 (2026-08-17): НЕ працює

Beam вставлено в розрив Ethernet (RV1106 eth0 → Beam Tx ))) Beam Rx → BPI end0).
Перевірено:
- локальний лінк BPI↔Beam Rx піднятий (`end0` = `LOWER_UP`);
- радіо-передача самого Beam працює (його власне відео йде);
- **але наш IP через Beam не ходить**: ARP `192.168.50.2 FAILED`, DHCP без
  жодного offer, `ssh: connect ... No route to host` — і це при піднятому
  радіо-лінку.

**Висновок:** Beam **не прозорий IP-міст** — він не тунелює наш H.265-потік
RV1106→RK3588. Підтверджує слова Sine (Beam сам кодує/декодує відео). Режим 1
закритий, переходимо в **Режим 2**.

**Відкрите (потрібно від Sine / з їхньої утиліти конфігурації):**
- який вхід очікує **Beam Tx** — IP-потік (RTSP/UDP/RTP) на його IP? HDMI? MIPI-CSI?
- який **конфіг/management-IP** у модулів і як їх налаштовувати;
- що саме видає **Beam Rx** на землі (HDMI? IP-потік?).

### Відповідь Sine (2026-08-17): Beam приймає RTP/RTSP

- **Beam приймає відеопотік з підключеної камери по RTP та RTSP.** Тобто вхід —
  IP-відео у форматі RTP/RTSP (Beam виступає як приймач/клієнт RTSP-камери).
- Порада для польових тестів: на обох модемах виставити **максимальну потужність**
  і увімкнути функцію **Dynamic power**. Для настільного тесту дефолтне меню ок.

**Що це означає для нас (Режим 2):** RV1106 має віддавати H.265 не сирим TCP
(як зараз у `rv1106_sender`), а **як RTSP-сервер або RTP-потік**, щоб виглядати
для Beam як IP-камера. Треба:
- або підняти на платі **RTSP-сервер** H.265 (Beam тягне `rtsp://<rv1106>:554/...`),
- або **пушити RTP/H.265** на IP:порт Beam.
Що саме — залежить від того, як Beam очікує джерело в своєму меню (RTSP-URL, який
він тягне, чи прийом пушнутого RTP). Уточнити поле «камера/джерело» в меню Beam.

**Наслідок для землі:** якщо Beam Rx сам декодує і віддає відео (HDMI), то
RK3588 для декоду **не потрібен** — Beam закриває і кодування, і декодування.

## Що вже готово в нас

- Борт видає **чистий H.265 Annex-B**, CBR ~8 Мбіт/с, 1280×1024, GOP 60.
- Зараз транспорт — **TCP** (`rv1106_sender` слухає `:5000`, приймач під'єднується).
  Для радіо TCP погано (ретрансмісії, head-of-line blocking, congestion control
  б'ється з радіо). Найпевніше перейдемо на **UDP + RTP/H.265** або **MPEG-TS/SRT** —
  залежить від того, що очікує Beam.
- Земля вже декодує апаратно (BPI-M7, `mppvideodec`) — див. `../reproduce/BPI_M7_GROUND.md`.

## Головні невідомі (від них залежить, що міняти в коді)

1. **Як відео заходить у Beam Tx і виходить з Rx** — Ethernet(UDP)? HDMI? USB? MIPI-CSI?
   Це визначає, чи ми віддаємо ES по мережі, чи потрібен HDMI-вихід з борту.
2. **Який формат/контейнер** очікує Beam — сирий H.265 ES, RTP, MPEG-TS, SRT, RTSP?
   Beam сам щось перекодовує чи возить наші пакети «як є»?
3. **Реальна пропускна здатність** на робочій дальності — щоб виставити бітрейт енкодера.
4. **Втрати пакетів / FEC / ARQ** — наш H.265 без захисту при втраті = розсипана
   картинка. Чи є у Beam вбудований FEC/повтори, чи це на нас (RTP+FEC, IDR-інтервал)?
5. **Затримка** «скло-в-скло», яку додає радіо (для UAV критично).
6. **MTU / фрагментація** — щоб не рвати NAL-и на радіо-MTU.

---

## Готовий блок питань для Sine support (EN)

> Скопіюй і надішли у Sine.engineering support. Контекст: airborne RV1106 board
> outputs hardware-encoded **H.265/HEVC, 1280×1024, CBR ~8 Mbps, GOP 60**; ground
> side is an **RK3588** doing hardware HEVC decode.

**Video ingest/egress**
1. How does video enter the **Beam transmitter** and leave the **receiver** —
   Ethernet/IP (UDP), HDMI, USB, or MIPI-CSI? If IP: what does the Tx expect
   (raw UDP unicast/multicast, RTP, MPEG-TS, SRT, RTSP)?
2. Can the link carry a **pre-encoded H.265/HEVC elementary stream as-is**, or does
   the Beam re-encode internally? If it re-encodes, what codec/latency/bitrate?
3. If IP-based: what **IP config / port / stream URL** does the Rx present on the
   ground side (so we can point `gst-launch … mppvideodec` at it)?

**Throughput & video params**
4. Usable **net throughput** for video at typical UAV ranges (e.g. 1/3/5/10 km),
   and how it degrades with distance/interference?
5. Recommended **max video bitrate** to stay stable, and do you prefer **CBR**?
6. Any constraints on **resolution / framerate / GOP / IDR interval** for best
   recovery after packet loss?

**Reliability (loss handling)**
7. Does the link provide built-in **FEC and/or ARQ (retransmission)**, or should we
   add error resilience on our side (RTP + FEC, frequent IDR, intra-refresh)?
8. Typical **packet-loss / BER** in good vs marginal conditions?
9. **MTU** of the link and how oversized frames are handled — do we need to keep
   NAL/packet size under a specific MTU to avoid fragmentation?

**Latency**
10. End-to-end **added latency** (encoder-out → decoder-in) at nominal bitrate, and
    any buffering/jitter-buffer we should expect or can tune?

**Telemetry / control (Sine.link)**
11. Bandwidth, interface (UART/Ethernet/USB) and latency of the **telemetry/control**
    channel? Is it a separate radio or shared with video?
12. Is the control channel **bidirectional** (ground → air), and its round-trip latency?

**Physical / integration (UAV)**
13. **Power** (voltage/current draw) of the airborne Tx, **weight**, size, connectors,
    operating temperature?
14. **Frequency bands**, output power, antenna options, and regulatory notes for our region?
15. **Pairing/config workflow** — how do we bind Tx↔Rx and set video/IP params (CLI,
    web UI, config tool)?

**Dev/integration**
16. Any **SDK / API / example pipelines** (especially gstreamer or ffmpeg) for feeding
    an H.265 stream in and pulling it out on the ground?
17. Reference designs or customers using **Rockchip (RV1106 / RK3588)** with your link?

---

## Що зробимо в коді за результатами відповідей

Sine підтвердив: **Beam приймає RTP/RTSP** → зроблено RTSP-режим.

- ✅ **RTSP-камера з RV1106 — реалізовано:** `../rv1106_rtsp/` (той самий
  Seek→RGA→H.265-конвеєр, що й у керівника, але вихід — RTSP-сервер на
  `librtsp`/`rtsp_demo`). URL: `rtsp://192.168.50.2:554/live/0`. Файл керівника
  не змінювався — це окрема збірка. Інструкція: `../rv1106_rtsp/README.md`.
- Якщо натомість Beam хоче **RTP-push** (а не тягне RTSP) — додамо режим
  UDP/RTP-H.265 на IP:порт Beam (той самий конвеєр, інший вихід).
- Менший **GOP** (частіші IDR) для швидшого відновлення після втрат на радіо;
  бітрейт енкодера — під заміряну смугу Beam з запасом.
- Наземний бік: якщо Beam Rx сам віддає відео (HDMI) — RK3588-декод не потрібен.
