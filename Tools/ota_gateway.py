"""Authorize and verify a smart-home ESP32 upgrade on a trusted local network."""

import argparse
import hashlib
import http.client
import ipaddress
import json
from pathlib import Path
import time

import serial


PROJECT = "edge_gateway"
IMAGE_HEADER_BYTES = 304


def console_request(port, command, prefix, timeout=4):
    port.reset_input_buffer()
    port.write((command + "\n").encode("ascii"))
    port.flush()
    deadline = time.monotonic() + timeout
    pending = bytearray()
    while time.monotonic() < deadline:
        chunk = port.read_until(b"\n", size=1024)
        if not chunk:
            continue
        pending.extend(chunk)
        if not pending.endswith(b"\n"):
            if len(pending) > 2048:
                pending.clear()
            continue
        _, marker, raw = pending.partition(prefix)
        pending.clear()
        if marker:
            try:
                return json.loads(raw)
            except (ValueError, UnicodeError):
                continue
    raise TimeoutError(f"No reply to {command} from the gateway")


def status(port):
    return console_request(port, "OTAGET", b"OTA_STATUS ")


def require_gateway(state):
    if state.get("project") != PROJECT or not state.get("station_mac"):
        raise RuntimeError("The USB device is not a smart-home ESP32 gateway")


def authorize(port, state):
    if state.get("image_state") != "valid" or not state.get("healthy"):
        raise RuntimeError("Wait for the current image to be confirmed before arming OTA")
    result = console_request(port, "OTAARM", b"OTA_ARM ")
    if not result.get("key"):
        raise RuntimeError("Gateway refused OTA authorization: " + result.get("error", "unknown"))
    return result


def response_json(connection):
    reply = connection.getresponse()
    return reply.status, json.loads(reply.read(4096))


def check_lan_gateway(host, usb_state):
    connection = http.client.HTTPConnection(host, timeout=5)
    try:
        connection.request("GET", "/api/ota")
        code, remote = response_json(connection)
    finally:
        connection.close()
    if code != 200 or any(remote.get(key) != usb_state.get(key)
                          for key in ("project", "station_mac", "version", "partition")):
        raise RuntimeError("The LAN endpoint does not match the USB gateway")
    return remote


def send_image(host, data, key, digest, interrupt_at):
    headers = {"Content-Type": "application/octet-stream",
               "X-OTA-Key": key, "X-Firmware-SHA256": digest}
    connection = http.client.HTTPConnection(host, timeout=130)
    try:
        if interrupt_at is None:
            connection.request("POST", "/api/update", data, headers)
            return response_json(connection)
        connection.putrequest("POST", "/api/update")
        for name, value in headers.items():
            connection.putheader(name, value)
        connection.putheader("Content-Length", str(len(data)))
        connection.endheaders()
        connection.send(data[:interrupt_at])
        return None, {}
    finally:
        connection.close()


def wait_for_status(port, before, predicate, timeout=100):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        time.sleep(1.5)
        try:
            state = status(port)
        except TimeoutError:
            continue
        require_gateway(state)
        if state["station_mac"] != before["station_mac"]:
            raise RuntimeError("The USB gateway changed during validation")
        if predicate(state):
            return state
    raise TimeoutError("The expected OTA result was not observed over USB")


def upload(port, before, args):
    address = ipaddress.IPv4Address(before.get("ipv4", ""))
    if not address.is_private or address.is_loopback or address.is_unspecified:
        raise RuntimeError("Connect the gateway and computer to the same private LAN")
    host = str(address)
    remote = check_lan_gateway(host, before)
    data = args.file.read_bytes()
    if not IMAGE_HEADER_BYTES <= len(data) <= remote["slot_bytes"]:
        raise ValueError("Select the ESP32 gateway's application .bin within the OTA slot capacity")
    if args.abort_after is not None and not IMAGE_HEADER_BYTES <= args.abort_after < len(data):
        raise ValueError("--abort-after must be between the image header and the end of the application")
    digest = hashlib.sha256(data).hexdigest()
    if args.bad_digest:
        digest = ("0" if digest[0] != "0" else "1") + digest[1:]

    key = authorize(port, before)["key"]
    code, reply = send_image(host, data, key, digest, args.abort_after)
    if args.bad_digest:
        if code != 400 or reply.get("status") != "sha256_mismatch":
            raise RuntimeError(f"Unexpected SHA-256 test result: HTTP {code}: {reply.get('status')}")
        after = status(port)
        if after.get("version") != before["version"] or after.get("partition") != before["partition"]:
            raise RuntimeError("The running image changed after the failed checksum")
        return {"result": "checksum_rejected", "state": after}
    if args.abort_after is not None:
        after = wait_for_status(port, before, lambda s: s["version"] == before["version"] and
                                s["partition"] == before["partition"] and s.get("last_error") == "receive_timeout", 25)
        return {"result": "interruption_preserved_image", "state": after}
    if code != 200 or not reply.get("success") or not reply.get("version"):
        raise RuntimeError(f"Upload rejected: HTTP {code}: {reply.get('status')}")
    if args.expect_rollback:
        after = wait_for_status(port, before, lambda s: s["image_state"] == "valid" and s["healthy"] and
                                s["version"] == before["version"] and s["partition"] == before["partition"] and
                                s["last_invalid_version"] == reply["version"])
        return {"result": "rollback_confirmed", "state": after}
    after = wait_for_status(port, before, lambda s: s["image_state"] == "valid" and s["healthy"] and
                            s["version"] == reply["version"] and s["partition"] == reply["partition"])
    return {"result": "upgrade_confirmed", "state": after}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Gateway USB serial port")
    subcommands = parser.add_subparsers(dest="action", required=True)
    subcommands.add_parser("status", help="Show OTA status over USB")
    subcommands.add_parser("arm", help="Show a short-lived key for use in the local upgrade page")
    upload_command = subcommands.add_parser("upload", help="Verify the device, then upload over the LAN")
    upload_command.add_argument("file", type=Path)
    tests = upload_command.add_mutually_exclusive_group()
    tests.add_argument("--bad-digest", action="store_true")
    tests.add_argument("--abort-after", type=int)
    tests.add_argument("--expect-rollback", action="store_true")
    args = parser.parse_args()

    with serial.Serial(baudrate=115200, timeout=0.1) as device:
        device.dtr = False
        device.rts = False
        device.port = args.port
        device.open()
        before = status(device)
        require_gateway(before)
        if args.action == "status":
            result = before
        elif args.action == "arm":
            authorization = authorize(device, before)
            print(f'One-time key ({authorization["ttl_s"]} s): {authorization["key"]}')
            return
        else:
            result = upload(device, before, args)
        print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        raise SystemExit(str(error)) from None
