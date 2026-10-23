"""Host test: pymavlink encodes, the C++ parser (src/mav_state.cpp) decodes. Run: ~/seek-ground-station/.venv/bin/python -m pytest test"""
import json
import os
import subprocess

import pytest

pytest.importorskip("pymavlink")
from pymavlink import mavutil  # noqa: E402
from pymavlink.dialects.v20 import ardupilotmega as m2  # noqa: E402
from pymavlink.dialects.v10 import ardupilotmega as m1  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = "/tmp/mav_decode_test"


@pytest.fixture(scope="session", autouse=True)
def build():
    subprocess.check_call(["g++", "-std=gnu++11", "-O1", "-Wall", "-Wextra", "-o", BIN, HERE + "/mav_decode.cpp", HERE + "/../src/mav_state.cpp", "-lpthread"])


def decode(data):
    return json.loads(subprocess.run([BIN], input=data, capture_output=True, check=True).stdout)


def stream(dialect, extra_msgs=()):
    mav = dialect.MAVLink(None, srcSystem=1, srcComponent=1)
    mav.robust_parsing = True
    msgs = [
        dialect.MAVLink_heartbeat_message(2, 3, 0x80 | 0x01, 5, 4, 3),                 # quad, ArduPilot, armed, LOITER
        dialect.MAVLink_attitude_message(100, 0.1, -0.2, -1.0, 0, 0, 0),
        dialect.MAVLink_global_position_int_message(100, 504501000, 305234000, 200000, 120000, 10, 20, -30, 27300),
        dialect.MAVLink_vfr_hud_message(10.0, 14.2, 273, 55, 120.0, 1.5),
        dialect.MAVLink_gps_raw_int_message(0, 3, 504501000, 305234000, 200000, 120, 200, 0, 0, 12),
        dialect.MAVLink_sys_status_message(0, 0, 0, 0, 15800, 1250, 74, 0, 0, 0, 0, 0, 0),
    ]
    msgs += list(extra_msgs)
    return b"".join(m.pack(mav) for m in msgs)


@pytest.mark.parametrize("dialect", [m2, m1], ids=["v2", "v1"])
def test_fields(dialect):
    r = decode(stream(dialect))
    assert r["frames"] == 6 and r["bad_crc"] == 0
    assert r["armed"] == 1 and r["mode"] == "LOITER" and r["sysid"] == 1
    assert (r["roll"], r["pitch"]) == (5.73, -11.46) and r["yaw"] == pytest.approx(302.7, abs=0.1)
    assert r["lat"] == pytest.approx(50.4501, abs=1e-7) and r["lon"] == pytest.approx(30.5234, abs=1e-7)
    assert r["alt"] == 120.0 and r["hdg"] == 273.0 and r["gs"] == pytest.approx(14.2, abs=0.01) and r["climb"] == pytest.approx(1.5, abs=0.01)
    assert r["fix"] == 3 and r["sats"] == 12 and r["hdop"] == 1.2
    assert r["batt_v"] == 15.8 and r["batt_pct"] == 74


def test_garbage_and_bad_crc_are_dropped():
    good = stream(m2)
    corrupted = bytearray(good)
    corrupted[20] ^= 0xFF                                           # break one frame
    # a false start byte claims a 253-byte frame and waits for the bytes to arrive: on a live link they do, here we pad the tail
    r = decode(b"\x00\xfd\xfd\x01garbage" + bytes(corrupted) + b"\xff\xfe" + b"\x00" * 300)
    assert r["bad_crc"] >= 1 and r["frames"] == 5                       # only the frame with the broken CRC is lost


def test_truncated_v2_payload_is_zero_extended():
    # a v2 heartbeat with custom_mode == 0 loses its trailing zero bytes on the wire; the parser must still read type/autopilot/base_mode
    mav = m2.MAVLink(None, 1, 1)
    hb = m2.MAVLink_heartbeat_message(2, 3, 0x80, 0, 4, 3).pack(mav)
    r = decode(hb)
    assert r["frames"] == 1 and r["armed"] == 1 and r["mode"] == "STABILIZE"


def test_home_distance_uses_position_at_arm():
    mav = m2.MAVLink(None, 1, 1)
    pos = lambda lat: m2.MAVLink_global_position_int_message(1, int(lat * 1e7), 305234000, 0, 0, 0, 0, 0, 65535).pack(mav)  # noqa: E731
    disarmed = m2.MAVLink_heartbeat_message(2, 3, 0x01, 0, 4, 3).pack(mav)
    armed = m2.MAVLink_heartbeat_message(2, 3, 0x81, 0, 4, 3).pack(mav)
    r = decode(pos(50.4500) + disarmed + armed + pos(50.4510))
    assert r["home"] == 1 and r["home_dist"] == pytest.approx(111.2, abs=1.0)    # 0.001 deg of latitude


def test_system_time_only_when_autopilot_knows_it():
    mav = m2.MAVLink(None, 1, 1)
    ok = m2.MAVLink_system_time_message(1_800_000_000_000_000, 1000).pack(mav)
    boot = m2.MAVLink_system_time_message(5_000_000, 1000).pack(mav)
    assert decode(ok)["unix"] == 1_800_000_000 and decode(boot)["unix"] == 0


def test_gcs_heartbeat_is_not_the_flight_controller():
    mav = m2.MAVLink(None, 255, 190)
    gcs = m2.MAVLink_heartbeat_message(6, 8, 0, 0, 4, 3).pack(mav)
    r = decode(gcs)
    assert r["sysid"] == 0 and r["mode"].startswith("MODE")
