#!/usr/bin/env python3
"""SQLite demo persists an increment across separate processes."""

import subprocess
import sys
import tempfile
from pathlib import Path


def main(executable):
    with tempfile.TemporaryDirectory() as directory:
        database = Path(directory) / "metrics.db"
        first = subprocess.run(
            [executable, str(database)],
            capture_output=True, text=True, check=True,
            timeout=3,
        )
        second = subprocess.run(
            [executable, str(database)],
            capture_output=True, text=True, check=True,
            timeout=3,
        )
        if first.stdout.strip() != "requests=1":
            raise RuntimeError(first.stdout)
        if second.stdout.strip() != "requests=2":
            raise RuntimeError(second.stdout)


if __name__ == "__main__":
    main(sys.argv[1])