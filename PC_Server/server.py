"""Async TCP server, device registry, and local operator console."""

import argparse
import asyncio
from dataclasses import dataclass, field
import json
import logging
from logging.handlers import RotatingFileHandler
from pathlib import Path
import secrets
import sys
import threading

from . import protocol
from .storage import CsvStorage, utc_now

LOG = logging.getLogger("edge")


@dataclass(eq=False)
class Connection:
    writer: asyncio.StreamWriter
    pending: dict = field(default_factory=dict)

    async def send(self, message):
        self.writer.write(protocol.encode(message))
        await asyncio.wait_for(self.writer.drain(), timeout=2)

    def close(self):
        for _, future in self.pending.values():
            if not future.done():
                future.set_result(None)
        self.writer.close()


@dataclass
class Device:
    device_id: str
    simulated: bool
    connection: Connection | None = None
    last_seen: str = ""
    samples: int = 0
    errors: int = 0
    latest: dict | None = None
    status: dict | None = None
    link: dict | None = None

    def view(self):
        return dict(device_id=self.device_id, online=self.connection is not None,
                    simulated=self.simulated, last_seen=self.last_seen, samples=self.samples,
                    bad_messages=self.errors, latest=self.latest, status=self.status, link=self.link)


class Monitor:
    def __init__(self, storage, idle_timeout=12):
        self.storage = storage
        self.idle_timeout = idle_timeout
        self.devices = {}
        self.connections = set()
        self.rid = secrets.randbelow(2**32 - 1)

    async def _read(self, reader, timeout):
        try:
            data = await asyncio.wait_for(reader.readline(), timeout)
        except ValueError as exc:
            raise ConnectionError("line exceeded buffer limit") from exc
        if not data:
            raise EOFError
        return protocol.decode(data)

    def _register(self, hello, connection):
        device_id = hello["device_id"]
        device = self.devices.get(device_id)
        if device is None:
            if len(self.devices) >= 64:
                raise ConnectionError("device registry full")
            device = self.devices[device_id] = Device(device_id, hello["simulated"])
        elif device.connection is not None:
            LOG.info("Replacing existing connection for %s", device_id)
            device.connection.close()
        if device.simulated != hello["simulated"]:
            device.latest = None
            device.status = None
        device.simulated = hello["simulated"]
        device.connection = connection
        device.last_seen = utc_now()
        device.link = None
        return device

    async def client(self, reader, writer):
        connection = Connection(writer)
        if len(self.connections) >= 32:
            connection.close()
            return
        self.connections.add(connection)
        device = None
        try:
            hello = await self._read(reader, 5)
            if hello["type"] != "hello":
                raise protocol.ProtocolError("hello expected first")
            device = self._register(hello, connection)
            LOG.info("%s online (simulation=%s)", device.device_id, device.simulated)
            loop = asyncio.get_running_loop()
            deadline = loop.time() + self.idle_timeout
            while device.connection is connection:
                remaining = deadline - loop.time()
                if remaining <= 0:
                    raise TimeoutError
                try:
                    message = await self._read(reader, remaining)
                    if device.connection is not connection:
                        break
                    if message["device_id"] != device.device_id or message["type"] == "hello":
                        raise protocol.ProtocolError("connection identity changed")
                    await self._handle(device, connection, message)
                except protocol.ProtocolError as exc:
                    device.errors += 1
                    if device.errors <= 5 or device.errors % 100 == 0:
                        LOG.warning("%s rejected message: %s", device.device_id, exc)
                    continue
                device.last_seen = utc_now()
                deadline = loop.time() + self.idle_timeout
        except TimeoutError:
            LOG.info("%s receive timeout", device.device_id if device else "Unregistered peer")
        except (EOFError, OSError, protocol.ProtocolError) as exc:
            if str(exc):
                LOG.info("Connection closed: %s", exc)
        finally:
            if device is not None and device.connection is connection:
                device.connection = None
                LOG.info("%s offline", device.device_id)
            self.connections.discard(connection)
            connection.close()
            try:
                await asyncio.wait_for(writer.wait_closed(), 2)
            except (OSError, TimeoutError):
                pass

    async def _handle(self, device, connection, message):
        kind = message["type"]
        if kind == "ping":
            device.link = message
            await connection.send({"type": "pong"})
        elif kind == "sensor":
            try:
                device.latest = self.storage.append(message, device.simulated)
            except OSError:
                device.errors += 1
                LOG.exception("%s CSV write failed", device.device_id)
                return
            device.samples += 1
            LOG.info("%s%s T=%s C H=%s %% D=%s cm flags=0x%02X", device.device_id,
                     " [sim]" if device.simulated else "", message["temperature"], message["humidity"],
                     message["distance_cm"], message["flags"])
        elif kind == "status":
            device.status = message
        elif kind == "error":
            if message["code"]:
                LOG.warning("%s device error 0x%X at %d ms", device.device_id,
                            message["code"], message["uptime_ms"])
            else:
                LOG.info("%s sensor fault cleared at %d ms", device.device_id, message["uptime_ms"])
        elif kind == "response":
            pending = connection.pending.get(message["rid"])
            if pending is not None:
                operation, future = pending
                if operation == message["op"] and not future.done():
                    future.set_result(message)

    async def command(self, device_id, operation, period_ms=None):
        if operation not in protocol.COMMANDS:
            raise ValueError("unknown command")
        if operation == "set_period" and not protocol.integer(period_ms, protocol.PERIOD_MIN, protocol.PERIOD_MAX):
            raise ValueError("采样周期须为 2000–60000 ms 的整数")
        device = self.devices.get(device_id)
        if device is None or device.connection is None:
            raise ConnectionError("设备离线")
        connection = device.connection
        if connection.pending:
            raise ConnectionError("该设备有一条命令等待响应")
        self.rid = self.rid % (2**32 - 1) + 1
        request_id = self.rid
        future = asyncio.get_running_loop().create_future()
        connection.pending[request_id] = (operation, future)
        request = {"type": "command", "rid": request_id, "op": operation}
        if operation == "set_period":
            request["period_ms"] = period_ms
        try:
            await connection.send(request)
            response = await asyncio.wait_for(future, timeout=5)
            if response is None:
                raise ConnectionError("等待响应时设备断开")
            return {**response, "result_text": protocol.RESULTS[response["result"]]}
        finally:
            connection.pending.pop(request_id, None)
            future.cancel()

    def close(self):
        for connection in self.connections:
            connection.close()


HELP = "devices | status DEVICE | period DEVICE MS | history DEVICE [N] | help | quit"


async def console(monitor):
    loop = asyncio.get_running_loop()
    commands = asyncio.Queue()

    def read_stdin():
        for line in sys.stdin:
            if loop.is_closed():
                return
            loop.call_soon_threadsafe(commands.put_nowait, line.strip())

    threading.Thread(target=read_stdin, daemon=True).start()
    print(HELP, flush=True)
    while True:
        parts = (await commands.get()).split()
        if not parts:
            continue
        name, *args = parts
        try:
            if name == "quit" and not args:
                return
            if name == "help" and not args:
                print(HELP, flush=True)
                continue
            if name == "devices" and not args:
                result = [device.view() for device in monitor.devices.values()]
            elif name == "status" and len(args) == 1:
                result = await monitor.command(args[0], "get_status")
            elif name == "period" and len(args) == 2:
                result = await monitor.command(args[0], "set_period", int(args[1]))
            elif name == "history" and 1 <= len(args) <= 2:
                limit = int(args[1]) if len(args) == 2 else 10
                if not 1 <= limit <= 200:
                    raise ValueError("N must be 1–200")
                result = await asyncio.to_thread(monitor.storage.history, args[0], limit)
            else:
                raise ValueError(HELP)
            print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)
        except (ValueError, OSError) as exc:
            print(str(exc) or "设备响应超时", flush=True)


async def run(args):
    storage = CsvStorage(args.data_dir)
    handler = RotatingFileHandler(storage.directory / "monitor.log", maxBytes=1_000_000,
                                  backupCount=3, encoding="utf-8")
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s",
                        handlers=[logging.StreamHandler(), handler])
    monitor = Monitor(storage)
    server = await asyncio.start_server(monitor.client, args.host, args.port, limit=protocol.MAX_LINE)
    async with server:
        LOG.info("Listening on %s, CSV %s", server.sockets[0].getsockname(), storage.path)
        try:
            if args.no_console:
                await server.serve_forever()
            else:
                await console(monitor)
        finally:
            monitor.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=9000, type=int)
    parser.add_argument("--data-dir", default=Path(__file__).parent / "data", type=Path)
    parser.add_argument("--no-console", action="store_true")
    try:
        asyncio.run(run(parser.parse_args()))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
