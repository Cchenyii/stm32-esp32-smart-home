"""Authenticated LAN dashboard for the STM32 smart-home profile."""

import argparse
import base64
import hmac
import ipaddress
import json
import os
import re
import threading
import time
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import paho.mqtt.client as mqtt


STATE_TTL = 3.0
ACK_WAIT = 2.8
MAX_ID = 0xFFFFFFFF
STATE_FIELDS = {
    "session": (1, MAX_ID), "last_id": (0, MAX_ID), "uptime_ms": (0, MAX_ID),
    "temperature_tenths": (-400, 800), "humidity_tenths": (0, 1000),
    "light_raw": (0, 4095), "light_level": (0, 4095),
    "fan_on_tenths": (100, 600), "close_level": (600, 4000),
    "dht_flags": (0, 255), "flags": (0, 15), "curtain": (0, 2),
    "curtain_target": (0, 2), "source": (0, 2),
    "uart_errors": (0, MAX_ID), "uart_crc_errors": (0, MAX_ID),
}
OPERATIONS = {1: (0, 1, 1), 2: (0, 1, 1), 3: (1, 2, 1), 4: (100, 600, 10), 5: (600, 4000, 100)}


def in_range(value, lower, upper):
    return type(value) is int and lower <= value <= upper


def decode_object(data, limit):
    if len(data) > limit:
        return None
    try:
        value = json.loads(data)
    except (UnicodeError, ValueError, RecursionError):
        return None
    return value if isinstance(value, dict) else None


def valid_command(value):
    if not isinstance(value, dict) or set(value) != {"op", "value"} or not in_range(value["op"], 1, 5):
        return False
    lower, upper, step = OPERATIONS[value["op"]]
    return in_range(value["value"], lower, upper) and value["value"] % step == 0


@dataclass
class Pending:
    session: int
    id: int
    op: int
    done: threading.Event = field(default_factory=threading.Event)
    reply: dict | None = None


class HomeGateway:
    def __init__(self, device_id, username, password):
        self.topic = f"smarthome/{device_id}"
        self.lock = threading.Lock()
        self.connected = False
        self.state = None
        self.state_at = 0.0
        self.last_id = 0
        self.session = 0
        self.pending = None
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, protocol=mqtt.MQTTv311)
        self.client.username_pw_set(username, password)
        self.client.reconnect_delay_set(1, 8)
        self.client.max_queued_messages_set(4)
        self.client.on_connect = self.on_connect
        self.client.on_disconnect = self.on_disconnect
        self.client.on_message = self.on_message

    def on_connect(self, client, userdata, flags, reason, properties):
        if reason.is_failure:
            return
        client.subscribe([(f"{self.topic}/state", 0), (f"{self.topic}/ack", 1),
                          (f"{self.topic}/availability", 1)])
        with self.lock:
            self.connected = True

    def on_disconnect(self, client, userdata, flags, reason, properties):
        with self.lock:
            self.connected = False
            self.clear_state()

    def clear_state(self):
        self.state = None
        if self.pending is not None:
            self.pending.done.set()

    def on_message(self, client, userdata, message):
        if message.topic == f"{self.topic}/availability":
            if message.payload == b"offline":
                with self.lock:
                    self.clear_state()
            return
        if message.retain:
            return
        payload = decode_object(message.payload, 512)
        if payload is None:
            return
        with self.lock:
            if message.topic == f"{self.topic}/state":
                if not all(in_range(payload.get(key), *limits) for key, limits in STATE_FIELDS.items()):
                    return
                if self.session != payload["session"]:
                    self.clear_state()
                    self.session = payload["session"]
                    self.last_id = 0
                self.state = {key: payload[key] for key in STATE_FIELDS}
                self.state_at = time.monotonic()
            elif message.topic == f"{self.topic}/ack":
                pending = self.pending
                if pending is not None and not pending.done.is_set() and (
                    payload.get("session"), payload.get("id"), payload.get("op")
                ) == (pending.session, pending.id, pending.op) and (
                    all(type(payload[key]) is int for key in ("session", "id", "op")) and
                    in_range(payload.get("result"), 0, 9) and type(payload.get("confirmed")) is bool
                ):
                    pending.reply = {"result": payload["result"], "confirmed": payload["confirmed"]}
                    pending.done.set()

    def fresh(self):
        return self.connected and self.state is not None and time.monotonic() - self.state_at <= STATE_TTL

    def snapshot(self):
        with self.lock:
            fresh = self.fresh()
            return {"online": fresh, "state": self.state if fresh else None}

    def send(self, command):
        if not valid_command(command):
            return 400, {"error": "invalid"}
        with self.lock:
            if not self.fresh():
                return 503, {"error": "offline"}
            if self.pending is not None:
                return 409, {"error": "busy"}
            next_id = max(self.last_id, self.state["last_id"]) + 1
            if next_id > MAX_ID:
                return 409, {"error": "sequence_exhausted"}
            pending = Pending(self.session, next_id, command["op"])
            self.pending = pending
            self.last_id = next_id
            payload = {**command, "session": pending.session, "id": pending.id}
            try:
                info = self.client.publish(f"{self.topic}/command", json.dumps(payload), qos=0, retain=False)
            except (OSError, ValueError):
                self.pending = None
                return 503, {"error": "offline"}
            if info.rc != mqtt.MQTT_ERR_SUCCESS:
                self.pending = None
                return 503, {"error": "offline"}

        pending.done.wait(ACK_WAIT)
        with self.lock:
            self.pending = None
            if pending.reply is not None:
                return 200, pending.reply
            if pending.done.is_set():
                return 503, {"error": "interrupted"}
            return 504, {"error": "timeout"}


def handler_for(gateway, web_password):
    page = Path(__file__).with_name("home_dashboard.html").read_bytes()
    expected = "Basic " + base64.b64encode(f"home:{web_password}".encode()).decode()

    class Handler(BaseHTTPRequestHandler):
        def setup(self):
            super().setup()
            self.connection.settimeout(6)

        def send_data(self, status, content, kind):
            self.send_response(status)
            if status == 401:
                self.send_header("WWW-Authenticate", 'Basic realm="Smart Home", charset="UTF-8"')
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("X-Frame-Options", "DENY")
            self.end_headers()
            self.wfile.write(content)

        def send_json(self, status, payload):
            self.send_data(status, json.dumps(payload).encode(), "application/json")

        def authorize(self):
            if hmac.compare_digest(self.headers.get("Authorization", "").encode(), expected.encode()):
                return True
            self.send_json(401, {"error": "authentication"})
            return False

        def do_GET(self):
            if not self.authorize():
                return
            if self.path == "/":
                self.send_data(200, page, "text/html; charset=utf-8")
            elif self.path == "/api/state":
                self.send_json(200, gateway.snapshot())
            else:
                self.send_json(404, {"error": "not_found"})

        def do_POST(self):
            if not self.authorize():
                return
            if self.path != "/api/command":
                self.send_json(404, {"error": "not_found"})
                return
            if self.headers.get("X-Smart-Home") != "1" or self.headers.get_content_type() != "application/json":
                self.send_json(403, {"error": "invalid_request"})
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                length = 0
            if not 1 <= length <= 128:
                self.send_json(400, {"error": "invalid"})
                return
            status, payload = gateway.send(decode_object(self.rfile.read(length), 128))
            self.send_json(status, payload)

        def log_request(self, code="-", size="-"):
            # Keep periodic dashboard polls out of the terminal.
            if self.command != "GET" or self.path != "/api/state":
                super().log_request(code, size)

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--broker", default="127.0.0.1")
    parser.add_argument("--broker-port", type=int, default=1883)
    parser.add_argument("--device", default="edge-01")
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()
    try:
        bind_ip = ipaddress.IPv4Address(args.bind)
    except ValueError:
        parser.error("--bind must be a local private IPv4 address")
    if not bind_ip.is_private or bind_ip.is_unspecified:
        parser.error("--bind must be a local private IPv4 address")
    if not re.fullmatch("[a-zA-Z0-9_-]{1,31}", args.device):
        parser.error("Invalid device ID")
    username = os.environ.get("SMARTHOME_MQTT_USER")
    password = os.environ.get("SMARTHOME_MQTT_PASSWORD")
    web_password = os.environ.get("SMARTHOME_WEB_PASSWORD")
    if not username or not password or not web_password:
        parser.error("Set SMARTHOME_MQTT_USER, SMARTHOME_MQTT_PASSWORD and SMARTHOME_WEB_PASSWORD")
    gateway = HomeGateway(args.device, username, password)
    with ThreadingHTTPServer((args.bind, args.port), handler_for(gateway, web_password)) as server:
        gateway.client.connect_async(args.broker, args.broker_port, 10)
        gateway.client.loop_start()
        print(f"Smart home: http://{args.bind}:{args.port}/", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            gateway.client.disconnect()
            gateway.client.loop_stop()


if __name__ == "__main__":
    main()
