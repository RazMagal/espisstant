#!/usr/bin/env python3
"""Byte-for-byte verification of components/switcher/switcher_proto.c.

Compiles the C packet builder for the host and diffs every packet type and
parser against a Python reference implementation, whose templates and
signing algorithm are copied verbatim from the aioswitcher project
(https://github.com/TomerFi/aioswitcher, Apache-2.0).

Run from anywhere:  python tools/test_switcher_proto.py
"""

import subprocess
import sys
from binascii import crc_hqx, hexlify, unhexlify
from pathlib import Path
from struct import pack

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "tools" / "test_bin" / "proto_test"

# --- reference implementation (aioswitcher, Apache-2.0) -----------------

P_SESSION = "00000000"
NO_TIMER = "00000000"
REQ1 = "{}340001000000000000000000{}00000000000000000000f0fe"
REQ2 = "{}390001000000000000000000{}00000000000000000000f0fe"
REQB = "{}000001000000000000000000{}00000000000000000000f0fe"
PAD72 = "0" * 72

LOGIN_PACKET_TYPE1 = "fef052000232a100" + P_SESSION + REQ1[2:] + "{}" + PAD72 + "00"
LOGIN_PACKET_TYPE2 = (
    "fef030000305a600" + P_SESSION + "ff0301000000" + "0000"
    + "00000000{}00000000000000000000f0fe{}00"
)
GET_STATE_PACKET_TYPE1 = "fef0300002320103" + REQ1 + "{}00"
GET_STATE_PACKET2_TYPE2 = "fef0300003050103" + REQ2 + "{}00"
SEND_CONTROL_PACKET = "fef05d0002320102" + REQ1 + "{}" + PAD72 + "000106000{}00{}"
BREEZE_COMMAND_PACKET = "fef0000003050102" + REQB + "{}" + PAD72 + "3701" + "{}{}"


def ts_hex(ts: int) -> str:
    return hexlify(pack("<I", ts)).decode()


def minutes_hex(minutes: int) -> str:
    return hexlify(pack("<I", minutes * 60)).decode()


def set_message_length(message: str) -> str:
    length = "{:x}".format(len(unhexlify(message + "00000000"))).ljust(4, "0")
    return "fef0" + str(length) + message[8:]


def sign(hex_packet: str) -> str:
    crc = hexlify(pack(">I", crc_hqx(unhexlify(hex_packet), 0x1021))).decode()
    crc_sliced = crc[6:8] + crc[4:6]
    key = unhexlify(crc_sliced + "30" * 32)
    kcrc = hexlify(pack(">I", crc_hqx(key, 0x1021))).decode()
    return hex_packet + crc_sliced + kcrc[6:8] + kcrc[4:6]


def finalize(packet: str) -> str:
    return sign(set_message_length(packet))


def breeze_cmd_len(command: str) -> str:
    return "{:x}".format(int(len(command) / 2)).ljust(4, "0")


# --- harness ------------------------------------------------------------


def build_c_binary() -> None:
    BIN.parent.mkdir(exist_ok=True)
    subprocess.run(
        [
            "gcc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-O2",
            "-I", str(ROOT / "components/switcher/include"),
            str(ROOT / "components/switcher/switcher_proto.c"),
            str(ROOT / "tools/proto_test_main.c"),
            "-o", str(BIN),
        ],
        check=True,
    )


def c_run(*args: str) -> str:
    res = subprocess.run(
        [str(BIN), *(str(a) for a in args)], capture_output=True, text=True
    )
    if res.returncode != 0:
        raise AssertionError(f"C driver failed for {args}: {res.stderr}")
    return res.stdout.strip()


failures = 0


def check(name: str, got: str, want: str) -> None:
    global failures
    if got == want:
        print(f"  ok   {name}")
    else:
        failures += 1
        print(f"  FAIL {name}\n       C:  {got}\n       py: {want}")


def main() -> int:
    build_c_binary()

    ts = 1755100000
    dev_id = "a1b2c3"
    dev_key = "08"
    session = "12ab34cd"

    print("packet builders:")
    check(
        "login1",
        c_run("login1", ts, dev_key),
        finalize(LOGIN_PACKET_TYPE1.format(ts_hex(ts), dev_key)),
    )
    check(
        "login2",
        c_run("login2", ts, dev_id),
        finalize(LOGIN_PACKET_TYPE2.format(ts_hex(ts), dev_id)),
    )
    check(
        "state1",
        c_run("state1", session, ts, dev_id),
        finalize(GET_STATE_PACKET_TYPE1.format(session, ts_hex(ts), dev_id)),
    )
    check(
        "state2",
        c_run("state2", session, ts, dev_id),
        finalize(GET_STATE_PACKET2_TYPE2.format(session, ts_hex(ts), dev_id)),
    )
    check(
        "control1 on",
        c_run("control1", session, ts, dev_id, 1, 0),
        finalize(
            SEND_CONTROL_PACKET.format(session, ts_hex(ts), dev_id, 1, NO_TIMER)
        ),
    )
    check(
        "control1 off",
        c_run("control1", session, ts, dev_id, 0, 0),
        finalize(
            SEND_CONTROL_PACKET.format(session, ts_hex(ts), dev_id, 0, NO_TIMER)
        ),
    )
    check(
        "control1 on+30min timer",
        c_run("control1", session, ts, dev_id, 1, 30),
        finalize(
            SEND_CONTROL_PACKET.format(
                session, ts_hex(ts), dev_id, 1, minutes_hex(30)
            )
        ),
    )

    # Breeze command payload: "00000000" + hex("Para|HexCode"), as produced
    # by SwitcherBreezeRemote/br_build_command.
    ir = "00000000" + hexlify(b"3|100_36_1_1_0100100110010011").decode()
    check(
        "breeze command",
        c_run("breeze", session, ts, dev_id, ir),
        finalize(
            BREEZE_COMMAND_PACKET.format(
                session, ts_hex(ts), dev_id, breeze_cmd_len(ir), ir
            )
        ),
    )

    print("parsers:")
    # Login response: session id is bytes [8:12].
    login_resp = bytes(range(48)) + b"\x00" * 4
    check(
        "parse_session",
        c_run("parse_session", hexlify(login_resp).decode()),
        hexlify(login_resp)[16:24].decode(),
    )

    # Type-1 state response: state @hex[150:152], power @hex[154:162]
    # (int(hex[2:4] + hex[0:2])).
    state_resp = bytearray(100)
    state_resp[75] = 0x01
    state_resp[77] = 0x34  # power = 0x1234 = 4660 W (lo byte)
    state_resp[78] = 0x12
    check(
        "parse_state1",
        c_run("parse_state1", hexlify(bytes(state_resp)).decode()),
        "1 4660",
    )

    # Breeze state response, per aioswitcher StateMessageParser offsets.
    br = bytearray(100)
    br[76] = 0xE9  # temp 23.3 -> 233 = 0x00e9 (lo, hi)
    br[77] = 0x00
    br[78] = 0x01  # on
    br[79] = 0x04  # mode cool
    br[80] = 0x19  # target 25 (int of hex "19" = 25? no: int("19",16)=25)
    br[81] = 0x21  # fan '2', swing '1'
    br[84:92] = b"ELEC7001"
    check(
        "parse_breeze",
        c_run("parse_breeze", hexlify(bytes(br)).decode()),
        "1 4 23.3 25 2 1 ELEC7001",
    )

    # Discovery datagram, type 1 (165 bytes).
    dg = bytearray(165)
    dg[0:2] = b"\xfe\xf0"
    dg[18:21] = unhexlify(dev_id)
    dg[40] = 0x08
    name = b"My Heater"
    dg[42 : 42 + len(name)] = name
    dg[74:76] = unhexlify("0317")  # Switcher V4
    dg[76:80] = bytes([192, 168, 1, 77])
    dg[133] = 0x01
    dg[135] = 0xDC  # 2780 W = 0x0adc
    dg[136] = 0x0A
    check(
        "parse_broadcast type1",
        c_run("parse_broadcast", hexlify(bytes(dg)).decode()),
        f"{dev_id} 08 0317 192.168.1.77 My Heater 0 1 2780",
    )

    # Discovery datagram, type 2 (168 bytes, breeze).
    dg2 = bytearray(168)
    dg2[0:2] = b"\xfe\xf0"
    dg2[18:21] = unhexlify("0e0e0e")
    name2 = b"Breeze"
    dg2[42 : 42 + len(name2)] = name2
    dg2[74:76] = unhexlify("0e01")
    dg2[77:81] = bytes([10, 0, 0, 5])
    check(
        "parse_broadcast type2",
        c_run("parse_broadcast", hexlify(bytes(dg2)).decode()),
        "0e0e0e 00 0e01 10.0.0.5 Breeze 1 0 0",
    )

    print()
    if failures:
        print(f"{failures} FAILURE(S)")
        return 1
    print("all packets match the aioswitcher reference")
    return 0


if __name__ == "__main__":
    sys.exit(main())
