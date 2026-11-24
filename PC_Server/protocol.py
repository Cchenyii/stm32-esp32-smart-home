"""Bounded newline-delimited JSON messages for the gateway connection."""

import json
import re

MAX_LINE = 768
DEVICE_ID = re.compile(r"[A-Za-z0-9_-]{1,31}\Z")
PERIOD_MIN = 2_000
PERIOD_MAX = 60_000
UINT32_MAX = 2**32 - 1
COMMANDS = {"set_period", "get_status"}
RESULTS = {
    0: "ok", 1: "invalid", 2: "unsupported", 3: "busy",
    4: "uart_unavailable", 5: "mcu_timeout",
}


class ProtocolError(ValueError):
    pass


def _object(pairs):
    obj = {}
    for name, value in pairs:
        if name in obj:
            raise ProtocolError(f"duplicate field {name!r}")
        obj[name] = value
    return obj


def _reject_constant(value):
    raise ProtocolError(f"invalid numeric constant {value!r}")


def integer(value, minimum=0, maximum=UINT32_MAX):
    return type(value) is int and minimum <= value <= maximum


def _reading(value, minimum, maximum):
    return value is None or (type(value) in (float, int) and minimum <= value <= maximum)


def encode(message):
    data = json.dumps(
        {"v": 1, **message}, ensure_ascii=True, allow_nan=False, separators=(",", ":")
    ).encode("ascii") + b"\n"
    if len(data) > MAX_LINE:
        raise ProtocolError("outgoing message too long")
    return data


def decode(data):
    if not data.endswith(b"\n") or len(data) > MAX_LINE:
        raise ProtocolError("missing newline or message too long")
    try:
        obj = json.loads(data.decode("utf-8"), object_pairs_hook=_object, parse_constant=_reject_constant)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError) as exc:
        raise ProtocolError("invalid JSON") from exc
    if not isinstance(obj, dict) or not integer(obj.get("v"), 1, 1):
        raise ProtocolError("expected protocol v1 object")
    device_id = obj.get("device_id")
    if not isinstance(device_id, str) or DEVICE_ID.fullmatch(device_id) is None:
        raise ProtocolError("invalid device ID")
    validate(obj)
    return obj


def _require(message, fields):
    for field in fields:
        if not integer(message.get(field)):
            raise ProtocolError(f"invalid {field}")


def validate(message):
    kind = message.get("type")
    if kind == "hello":
        if type(message.get("simulated")) is not bool:
            raise ProtocolError("invalid simulated flag")
    elif kind == "ping":
        _require(message, ("uptime_ms", "bridge_uart_errors", "bridge_crc_errors"))
        if type(message.get("uart_online")) is not bool or not integer(message.get("rssi"), -128, 0):
            raise ProtocolError("invalid link status")
    elif kind == "sensor":
        _require(message, ("uptime_ms", "flags", "drops"))
        if not integer(message.get("seq"), 0, 65535):
            raise ProtocolError("invalid sequence")
        for name, low, high, flag in (
            ("temperature", -40, 80, 1), ("humidity", 0, 100, 1), ("distance_cm", 2, 400, 2)
        ):
            value = message.get(name)
            if (name not in message or not _reading(value, low, high) or
                    bool(message["flags"] & flag) != (value is not None)):
                raise ProtocolError(f"invalid {name}")
    elif kind == "status":
        _require(message, ("seq", "uptime_ms", "period_ms", "bad_crc", "bad_frames", "uart_errors",
                           "oled_errors", "drops", "reset_flags", "stack_words", "board_state", "watchdog"))
        if not integer(message["period_ms"], PERIOD_MIN, PERIOD_MAX):
            raise ProtocolError("invalid sampling period")
    elif kind == "error":
        _require(message, ("seq", "uptime_ms", "code"))
    elif kind == "response":
        if (not integer(message.get("rid"), 1) or not integer(message.get("result"), 0, max(RESULTS))
                or not isinstance(message.get("op"), str) or message["op"] not in COMMANDS
                or not integer(message.get("period_ms"), 0, PERIOD_MAX)
                or (message["result"] == 0 and message["period_ms"] < PERIOD_MIN)):
            raise ProtocolError("invalid command response")
    else:
        raise ProtocolError("unknown message type")
