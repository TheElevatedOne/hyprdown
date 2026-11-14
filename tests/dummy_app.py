#!/usr/bin/env python3
"""A pretend app for the tests: records SIGTERM, optionally ignores it, and can
start a child app of its own. Usage: dummy_app.py NAME MODE OUTDIR"""
import ctypes
import os
import signal
import subprocess
import sys
import time

name, mode, out = sys.argv[1:4]
ctypes.CDLL(None).prctl(15, name.encode(), 0, 0, 0)


def on_term(*_):
    with open(os.path.join(out, "events"), "a") as f:
        f.write(f"{time.monotonic()} term {name}\n")
    if mode != "ignore-term":
        os._exit(0)


signal.signal(signal.SIGTERM, on_term)
if mode.startswith("parent:"):
    subprocess.Popen([sys.executable, "-I", os.path.abspath(__file__), mode[7:], "normal", out])
with open(os.path.join(out, f"{name}.pid"), "w") as f:
    f.write(str(os.getpid()))
open(os.path.join(out, f"{name}.ready"), "w").close()
while True:
    time.sleep(60)
