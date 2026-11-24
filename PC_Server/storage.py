"""Store accepted readings in a single append-only CSV."""

import csv
from collections import deque
from datetime import datetime, timezone
from pathlib import Path

FIELDS = (
    "received_at", "device_id", "simulated", "seq", "uptime_ms",
    "temperature", "humidity", "distance_cm", "flags", "drops",
)


def utc_now():
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


class CsvStorage:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.path = self.directory / "readings.csv"

    def append(self, message, simulated):
        row = {name: message.get(name) for name in FIELDS}
        row.update(received_at=utc_now(), simulated=simulated)
        with self.path.open("a", newline="", encoding="utf-8") as file:
            writer = csv.DictWriter(file, fieldnames=FIELDS)
            if file.tell() == 0:
                writer.writeheader()
            writer.writerow(row)
        return row

    def history(self, device_id, limit):
        if not self.path.exists():
            return []
        with self.path.open("r", newline="", encoding="utf-8") as file:
            rows = (item for item in csv.DictReader(file)
                    if item.get("device_id") == device_id and None not in item.values())
            return list(deque(rows, maxlen=limit))
