#!/usr/bin/env python3
"""
Cleans SpinDoctor raw capture logs before NanoEdge AI Studio import.

Each valid data row must be exactly CAPTURE_BUF_SIZE * 3 comma-separated
integers (x1,y1,z1,x2,y2,z2,...). Any line that doesn't match this exactly
(fused rows, truncated rows, stray printf/menu text mixed in) is dropped.
Prints a summary per file so you can see what was kept vs. removed.

Usage:
    python3 clean_captures.py /home/shrutik/SpinDoctor/data
"""

import sys
import os
import re

CAPTURE_BUF_SIZE = 64
AXES = 3
EXPECTED_FIELDS = CAPTURE_BUF_SIZE * AXES  # 192

# Matches a line that is ONLY comma-separated integers (optionally negative),
# nothing else before or after.
ROW_PATTERN = re.compile(r'^-?\d+(,-?\d+)*$')

FILES = [
    "speed1_healthy.log",
    "speed1_imbalance.log",
    "speed1_obstruction.log",
    "speed2_healthy.log",
    "speed2_imbalance.log",
    "speed2_obstruction.log",
    "speed3_healthy.log",
    "speed3_imbalance.log",
    "speed3_obstruction.log",
]


def clean_file(path):
    if not os.path.exists(path):
        print(f"  MISSING: {path}")
        return

    with open(path, "r", errors="replace") as f:
        lines = f.readlines()

    kept = []
    total_data_lines = 0
    dropped = 0

    for line in lines:
        stripped = line.strip()
        if not stripped:
            continue
        # Skip obvious non-data lines outright (markers, menu text, sensor
        # diagnostic prints, DHT11 lines, reset messages) without counting
        # them as "dropped data" since they were never meant to be rows.
        if stripped.startswith("===") or stripped.startswith("X:") or \
           stripped.startswith("DHT11") or stripped.startswith("RESET") or \
           stripped.startswith("!!!") or stripped.startswith("Speed") or \
           stripped.startswith("Class") or stripped.startswith("Enter") or \
           stripped.startswith("Send") or stripped.startswith("Selected") or \
           stripped.startswith("Invalid") or stripped.startswith("Cancelled"):
            continue

        # Candidate data row
        total_data_lines += 1
        fields = stripped.split(",")

        if ROW_PATTERN.match(stripped) and len(fields) == EXPECTED_FIELDS:
            kept.append(stripped)
        else:
            dropped += 1

    out_path = path.replace(".log", "_clean.csv")
    with open(out_path, "w") as f:
        for row in kept:
            f.write(row + "\n")

    print(f"  {os.path.basename(path):30s} candidate_rows={total_data_lines:4d}  kept={len(kept):4d}  dropped={dropped:4d}  -> {os.path.basename(out_path)}")


def main():
    if len(sys.argv) != 2:
        print("Usage: python3 clean_captures.py <data_directory>")
        sys.exit(1)

    data_dir = sys.argv[1]
    print(f"Cleaning capture logs in: {data_dir}\n")

    for fname in FILES:
        clean_file(os.path.join(data_dir, fname))

    print("\nDone. Each *_clean.csv contains only well-formed rows (exactly "
          f"{EXPECTED_FIELDS} comma-separated integers each), ready for "
          "NanoEdge AI Studio import.")


if __name__ == "__main__":
    main()
