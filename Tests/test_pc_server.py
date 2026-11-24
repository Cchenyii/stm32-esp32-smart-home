import asyncio
from pathlib import Path
import tempfile
import unittest

from PC_Server import protocol
from PC_Server.server import Monitor
from PC_Server.storage import CsvStorage


def message(device, kind, **extra):
    return protocol.encode({"device_id": device, "type": kind, **extra})


def reading(device, seq, valid=True):
    return message(device, "sensor", seq=seq, temperature=24.1 if valid else None,
                   humidity=51.0 if valid else None, distance_cm=None,
                   flags=1 if valid else 4, uptime_ms=10000, drops=0)


class TcpIntegration(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.monitor = Monitor(CsvStorage(Path(self.temp.name)), idle_timeout=0.7)
        self.server = await asyncio.start_server(self.monitor.client, "127.0.0.1", 0,
                                                 limit=protocol.MAX_LINE)
        self.addAsyncCleanup(self._cleanup_server)

    async def _cleanup_server(self):
        self.monitor.close()
        self.server.close()
        await self.server.wait_closed()
        await self.await_state(lambda: not self.monitor.connections)

    async def await_state(self, predicate):
        async def poll():
            while not predicate():
                await asyncio.sleep(.005)
        await asyncio.wait_for(poll(), timeout=2)

    async def peer(self, device, simulated=False):
        reader, writer = await asyncio.open_connection("127.0.0.1",
                                                        self.server.sockets[0].getsockname()[1])
        self.addAsyncCleanup(self._close, writer)
        hello = message(device, "hello", simulated=simulated)
        writer.write(hello[:9])
        await writer.drain()
        writer.write(hello[9:] + message(device, "ping", uptime_ms=20, rssi=-50, uart_online=True,
                                         bridge_uart_errors=0, bridge_crc_errors=0))
        await writer.drain()
        self.assertEqual(await asyncio.wait_for(reader.readline(), 1), b'{"v":1,"type":"pong"}\n')
        return reader, writer

    async def _close(self, writer):
        writer.close()
        await writer.wait_closed()

    async def test_split_and_joined_messages_and_history(self):
        _, writer = await self.peer("node_a")
        data = reading("node_a", 5) + b'{"v":1,malformed}\n' + reading("node_a", 6, valid=False)
        for start in range(0, len(data), 11):
            writer.write(data[start:start+11])
            await writer.drain()
        device = self.monitor.devices["node_a"]
        await self.await_state(lambda: device.samples == 2)
        self.assertEqual(device.errors, 1)
        self.assertIsNone(device.latest["temperature"])
        rows = self.monitor.storage.history("node_a", 10)
        self.assertEqual([row["seq"] for row in rows], ["5", "6"])
        self.assertEqual(rows[1]["temperature"], "")

    async def test_two_nodes_and_command_correlation(self):
        reader_a, writer_a = await self.peer("node_a")
        _, writer_b = await self.peer("node_b", simulated=True)
        writer_a.write(reading("node_a", 1))
        writer_b.write(reading("node_b", 2))
        await writer_a.drain()
        await writer_b.drain()
        import json
        command = asyncio.create_task(self.monitor.command("node_a", "set_period", 5000))
        request = json.loads(await asyncio.wait_for(reader_a.readline(), 1))
        rid = request["rid"]
        writer_a.write(message("node_a", "response", rid=rid, op="get_status", result=0, period_ms=3000)
                       + message("node_a", "response", rid=rid, op="set_period", result=0, period_ms=5000))
        await writer_a.drain()
        reply = await asyncio.wait_for(command, 1)
        self.assertEqual(reply["period_ms"], 5000)
        self.assertTrue(self.monitor.devices["node_b"].simulated)
        await self.await_state(lambda: self.monitor.devices["node_b"].samples == 1)
        self.assertEqual(self.monitor.storage.history("node_b", 5)[0]["simulated"], "True")
        with self.assertRaises(ValueError):
            await self.monitor.command("node_a", "set_period", 1999)

    async def test_same_device_reconnect_and_timeout(self):
        first_reader, _ = await self.peer("node_a")
        original = self.monitor.devices["node_a"].connection
        second_reader, _ = await self.peer("node_a")
        self.assertIsNot(self.monitor.devices["node_a"].connection, original)
        self.assertEqual(await asyncio.wait_for(first_reader.read(), 1), b"")
        self.assertIsNotNone(self.monitor.devices["node_a"].connection)
        self.assertEqual(await asyncio.wait_for(second_reader.read(), 2), b"")
        await self.await_state(lambda: self.monitor.devices["node_a"].connection is None)
        with self.assertRaises(ConnectionError):
            await self.monitor.command("node_a", "get_status")

    async def test_oversized_input_does_not_affect_other_device(self):
        reader_a, writer_a = await self.peer("node_a")
        _, writer_b = await self.peer("node_b")
        writer_a.write(b"x" * (protocol.MAX_LINE + 50))
        await writer_a.drain()
        self.assertEqual(await asyncio.wait_for(reader_a.read(), 1), b"")
        writer_b.write(reading("node_b", 3))
        await writer_b.drain()
        await self.await_state(lambda: self.monitor.devices["node_b"].samples == 1)
