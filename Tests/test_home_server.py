import asyncio
import base64
import json
import tempfile
import threading
import unittest
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

try:
    import paho.mqtt.client as mqtt
    from amqtt.broker import Broker
    from passlib.hash import sha256_crypt
    from PC_Server import home_server
except ModuleNotFoundError:
    Broker = None


@unittest.skipIf(Broker is None, "MQTT integration requires paho-mqtt and amqtt")
class HomeDashboardIntegration(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.files = tempfile.TemporaryDirectory()
        self.addCleanup(self.files.cleanup)
        password_file = Path(self.files.name) / "users"
        password_file.write_text("test:" + sha256_crypt.using(rounds=1000).hash("fixture") + "\n")
        self.broker_settings = {
            "listeners": {"default": {"type": "tcp", "bind": "127.0.0.1:0"}},
            "plugins": {"amqtt.plugins.authentication.FileAuthPlugin": {"password_file": str(password_file)}},
        }
        self.broker = Broker(self.broker_settings)
        await self.broker.start()
        self.addAsyncCleanup(self.stop_broker)
        self.broker_port = self.broker._servers["default"].instance.sockets[0].getsockname()[1]
        self.web = home_server.HomeGateway("example", "test", "fixture")
        self.server = home_server.ThreadingHTTPServer(("127.0.0.1", 0), home_server.handler_for(self.web, "fixture-web"))
        self.addAsyncCleanup(self.stop_http)
        self.url = f"http://127.0.0.1:{self.server.server_port}"
        self.http_thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.http_thread.start()
        self.subscribed = threading.Event()
        self.commands = asyncio.Queue()
        self.device = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        self.device.username_pw_set("test", "fixture")
        loop = asyncio.get_running_loop()
        self.device.on_connect = lambda client, *_: client.subscribe("smarthome/example/command", 1)
        self.device.on_subscribe = lambda *_: self.subscribed.set()
        self.device.on_message = lambda _, __, msg: loop.call_soon_threadsafe(
            self.commands.put_nowait, json.loads(msg.payload)
        )
        for client in (self.web.client, self.device):
            client.connect_async("127.0.0.1", self.broker_port, 10)
            client.loop_start()
            self.addAsyncCleanup(self.stop_client, client)
        self.assertTrue(await asyncio.to_thread(self.subscribed.wait, 3))

    async def stop_client(self, client):
        client.disconnect()
        await asyncio.to_thread(client.loop_stop)

    async def stop_broker(self):
        if self.broker is not None:
            await self.broker.shutdown()
            self.broker = None

    async def stop_http(self):
        await asyncio.to_thread(self.server.shutdown)
        self.server.server_close()

    def http(self, path, command=None, authorize=True, control_header=True):
        headers = {}
        if authorize:
            headers["Authorization"] = "Basic " + base64.b64encode(b"home:fixture-web").decode()
        if command is not None:
            headers["Content-Type"] = "application/json"
            if control_header:
                headers["X-Smart-Home"] = "1"
        data = json.dumps(command).encode() if command is not None else None
        try:
            response = urlopen(Request(self.url + path, data=data, headers=headers), timeout=5)
        except HTTPError as exc:
            response = exc
        with response:
            content = response.read()
            if response.headers.get_content_type() == "application/json":
                content = json.loads(content)
            return response.status, content

    async def wait_state(self, session):
        for _ in range(35):
            _, current = await asyncio.to_thread(self.http, "/api/state")
            if current["online"] and current["state"]["session"] == session:
                return
            await asyncio.sleep(0.04)
        self.fail("Dashboard did not receive a live state")

    async def publish_state(self, session, last_id=0, flags=3):
        for _ in range(12):
            self.device.publish("smarthome/example/state", json.dumps({
                "session": session, "last_id": last_id, "uptime_ms": 2000,
                "temperature_tenths": 250, "humidity_tenths": 520,
                "light_raw": 1000, "light_level": 1000, "flags": flags, "dht_flags": 1,
                "fan_on_tenths": 300, "close_level": 3000, "curtain": 1, "curtain_target": 1,
                "source": 1, "uart_errors": 0, "uart_crc_errors": 0,
            }))
            await asyncio.sleep(0.05)
            if self.web.snapshot()["online"] and self.web.snapshot()["state"]["session"] == session:
                break
        await self.wait_state(session)

    def publish_reply(self, command, result):
        ack = {key: command[key] for key in ("id", "op", "session")}
        self.device.publish("smarthome/example/ack", json.dumps({**ack, "result": result, "confirmed": True}), qos=1)

    async def test_state_validation_auth_and_ack_correlation(self):
        self.assertEqual((await asyncio.to_thread(self.http, "/", None, False))[0], 401)
        page_code, page = await asyncio.to_thread(self.http, "/")
        self.assertEqual(page_code, 200)
        self.assertIn("智能家居控制".encode(), page)
        self.assertEqual((await asyncio.to_thread(self.http, "/api/command", {"op": 2, "value": 1}))[0], 503)
        await self.publish_state(123, last_id=19)
        self.assertEqual((await asyncio.to_thread(self.http, "/api/command", {"op": 2, "value": 1}, True, False))[0], 403)
        self.assertEqual((await asyncio.to_thread(self.http, "/api/command", {"op": 2, "value": True}))[0], 400)
        pending = asyncio.create_task(asyncio.to_thread(self.http, "/api/command", {"op": 2, "value": 1}))
        command = await asyncio.wait_for(self.commands.get(), 2)
        self.assertEqual(command, {"op": 2, "value": 1, "session": 123, "id": 20})
        self.assertEqual((await asyncio.to_thread(self.http, "/api/command", {"op": 1, "value": 1}))[0], 409)
        self.publish_reply({**command, "session": 124}, 0)
        await asyncio.sleep(0.13)
        self.assertFalse(pending.done())
        self.publish_reply(command, 4)
        self.assertEqual(await pending, (200, {"result": 4, "confirmed": True}))
        self.assertTrue(self.commands.empty())

    async def test_offline_interrupt_and_new_session(self):
        await self.publish_state(800, last_id=99, flags=7)
        pending = asyncio.create_task(asyncio.to_thread(self.http, "/api/command", {"op": 3, "value": 2}))
        old = await asyncio.wait_for(self.commands.get(), 2)
        self.device.publish("smarthome/example/availability", "offline", qos=1, retain=True)
        self.assertEqual(await pending, (503, {"error": "interrupted"}))
        self.assertEqual(await asyncio.to_thread(self.http, "/api/state"), (200, {"online": False, "state": None}))
        await self.publish_state(801, flags=7)
        pending = asyncio.create_task(asyncio.to_thread(self.http, "/api/command", {"op": 3, "value": 2}))
        fresh = await asyncio.wait_for(self.commands.get(), 2)
        self.assertEqual((fresh["session"], fresh["id"]), (801, 1))
        self.publish_reply(old, 0)
        await asyncio.sleep(0.12)
        self.assertFalse(pending.done())
        self.publish_reply(fresh, 7)
        self.assertEqual(await pending, (200, {"result": 7, "confirmed": True}))

    async def test_broker_restart_requires_new_live_state(self):
        await self.publish_state(901)
        self.subscribed.clear()
        await self.stop_broker()
        for _ in range(50):
            await asyncio.sleep(0.04)
            if not self.web.snapshot()["online"]:
                break
        self.assertEqual(await asyncio.to_thread(self.http, "/api/state"), (200, {"online": False, "state": None}))
        self.broker_settings["listeners"]["default"]["bind"] = f"127.0.0.1:{self.broker_port}"
        self.broker = Broker(self.broker_settings)
        await self.broker.start()
        self.assertTrue(await asyncio.to_thread(self.subscribed.wait, 6))
        self.assertEqual((await asyncio.to_thread(self.http, "/api/command", {"op": 1, "value": 1}))[0], 503)
        await self.publish_state(902)
        self.assertTrue(self.commands.empty())
