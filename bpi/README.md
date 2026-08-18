# Наземна плата BPI-M7 — автоприймач (hands-free)

Щоб уся наземна станція була «увімкнув → працює»: RV1106 сам віддає стрім
(`board/etc/init.d/S99thermal`), а BPI-M7 при завантаженні сам приймає й показує.

## Що ставимо

- `thermal_receiver.sh` → у `~/` на BPI. Самовідновний цикл: TCP-приймач
  `mppvideodec` (HW) → HDMI (`rkximagesink`), перепідключається після будь-якого
  зриву.
- `thermal-receiver.desktop` → у `~/.config/autostart/` на BPI. Запускає скрипт
  при вході в графічну сесію.

## Встановлення (на BPI)

Скопіювати файли з ноута (BPI = звичайний Debian, scp працює):
```sh
scp bpi/thermal_receiver.sh   armsom@192.168.0.162:~/thermal_receiver.sh
ssh armsom@192.168.0.162 'chmod +x ~/thermal_receiver.sh; mkdir -p ~/.config/autostart'
scp bpi/thermal-receiver.desktop armsom@192.168.0.162:~/.config/autostart/thermal-receiver.desktop
```
або створити прямо на BPI (див. блок у чаті/нижче).

## Передумови

- Статичний IP на дротовому інтерфейсі BPI має бути постійним (nmcli-профіль):
  `end0` = `192.168.50.10/24`, або міст `br0` = `192.168.50.10/24` (якщо задіяні
  обидва порти BPI під Beam — див. `../reproduce/BPI_M7_GROUND.md`).
- RV1106 доступний на `192.168.50.2:5000` (його автозапуск `S99thermal`).
- Графічна сесія на BPI піднімається сама (autologin). Якщо ні — після входу в
  десктоп приймач стартує автоматично.

## Робочий процес

1. Увімкнути RV1106 (камера через живлений хаб + спільна земля).
2. Увімкнути BPI-M7 (з під'єднаним HDMI-монітором).
3. Відео зʼявляється на екрані само — жодних команд.

## Керування вручну (за потреби)

```sh
~/thermal_receiver.sh          # запустити вручну
pkill -f thermal_receiver      # зупинити цикл
pkill -f gst-launch            # зупинити лише поточний gst
```
