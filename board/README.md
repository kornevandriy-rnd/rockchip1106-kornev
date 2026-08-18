# Файли для плати RV1106 (автозапуск)

## `etc/init.d/S99thermal` — автозапуск sender'а при завантаженні

Щоб RV1106 працювала як «камера»: увімкнув живлення → sender уже слухає на
`:5000`, і на землі (BPI) достатньо запустити лише приймач. Жодних команд на
плату слати не треба.

Скрипт (виконується BusyBox-ініт після штатного `S21appinit`):
1. зупиняє штатний камерний застосунок `rkipc` (через `RkLunch-stop.sh`) — звільняє
   камеру й апаратний енкодер;
2. чекає появи USB-камери (YUYV-нода, номер `/dev/videoN` «плаває»);
3. піднімає `/root/rv1106_sender --port 5000 --fps 60` у фоні (лог `/tmp/thermal.log`),
   з кількома повторами, якщо камера/VEPU ще зайняті.

### Встановлення (з ноута, через BPI як проксі)

```sh
cd ~/rockchip1106-kornev && git pull origin claude/luckfox-pico-linux-setup-wrz9u6
scp -o ProxyJump=armsom@192.168.0.162 board/etc/init.d/S99thermal root@192.168.50.2:/etc/init.d/S99thermal
ssh -J armsom@192.168.0.162 root@192.168.50.2 'chmod +x /etc/init.d/S99thermal && echo INSTALLED'
```

### Передумова

`/root/rv1106_sender` має бути на платі (той самий бінар, що ми деплоїли).
Перевірка: `ssh -J armsom@192.168.0.162 root@192.168.50.2 'ls -l /root/rv1106_sender'`.

### Ручне керування / діагностика

```sh
/etc/init.d/S99thermal start|stop|restart|status
cat /tmp/thermal.log
```

### Робочий процес після встановлення

1. Увімкнути живлення RV1106 (камера через живлений хаб + спільна земля).
2. На BPI запустити приймач: `reproduce/05_run_bpi_receiver.sh`
   (або команда gstreamer з `reproduce/BPI_M7_GROUND.md`).

> Примітка: це автозапуск при завантаженні (не респавн). Якщо колись треба, щоб
> sender сам перезапускався після падіння — додамо запис у `inittab` (respawn).
