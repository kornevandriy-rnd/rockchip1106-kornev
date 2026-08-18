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

Потрібен `~/thermal_receiver.sh` (скрипт-приймач) + механізм автозапуску.

### Рекомендовано: systemd-служба (надійно, з рестартом)

Десктопний autostart (`.desktop`) не завжди спрацьовує; надійніше — systemd-служба,
яка стартує після графічної сесії, виводить на `:0` і сама перезапускається:
```sh
scp bpi/thermal_receiver.sh      armsom@192.168.0.162:~/thermal_receiver.sh
scp bpi/thermal-receiver.service armsom@192.168.0.162:/tmp/thermal-receiver.service
ssh armsom@192.168.0.162 'chmod +x ~/thermal_receiver.sh; \
  sudo mv /tmp/thermal-receiver.service /etc/systemd/system/; \
  sudo systemctl daemon-reload; sudo systemctl enable --now thermal-receiver.service'
```
Перевірка: `systemctl status thermal-receiver.service`, лог: `journalctl -u thermal-receiver -e`.

### Альтернатива: десктопний autostart

Якщо систему лишаєш як десктоп із автологіном і DE читає `~/.config/autostart`:
```sh
ssh armsom@192.168.0.162 'mkdir -p ~/.config/autostart'
scp bpi/thermal-receiver.desktop armsom@192.168.0.162:~/.config/autostart/thermal-receiver.desktop
```

Обидва файли можна також створити прямо на BPI через `cat > … <<'EOF'`.

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
