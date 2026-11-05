#!/usr/bin/env python3
"""Stand-in for the Hyprland IPC, used by run_tests.py. Speaks just enough of
socket1 and owns dummy apps that record how they were asked to stop."""
import ctypes
import json
import os
import re
import socket
import subprocess
import sys
import time

DUMMY = r"""
import ctypes, os, signal, sys, time
name, mode, out = sys.argv[1:4]
ctypes.CDLL(None).prctl(15, name.encode(), 0, 0, 0)
def on_term(*_):
    with open(os.path.join(out, "events"), "a") as f:
        f.write(f"{time.monotonic()} term {name}\n")
    if mode != "ignore-term":
        os._exit(0)
signal.signal(signal.SIGTERM, on_term)
open(os.path.join(out, f"{name}.ready"), "w").close()
while True:
    time.sleep(60)
"""

base, out = sys.argv[1], sys.argv[2]
scenario = json.loads(sys.argv[3])
ctypes.CDLL(None).prctl(15, b"Hyprland", 0, 0, 0)


def event(text):
    with open(os.path.join(out, "events"), "a") as f:
        f.write(f"{time.monotonic()} {text}\n")


def spawn(name, mode="normal"):
    proc = subprocess.Popen([sys.executable, "-I", "-c", DUMMY, name, mode, out])
    while not os.path.exists(os.path.join(out, f"{name}.ready")):
        time.sleep(0.01)
    with open(os.path.join(out, f"{name}.pid"), "w") as f:
        f.write(str(proc.pid))
    return proc


windows = {}
next_address = [0x1000]


def add_window(name, kind, proc, title=None):
    address = hex(next_address[0])
    next_address[0] += 0x10
    windows[address] = {"name": name, "kind": kind, "proc": proc, "title": title or name, "closes": 0}


keep = []
layers = []
for app in scenario.get("windows", []):
    add_window(app["name"], app["kind"], spawn(app["name"], app.get("mode", "normal")), app.get("title"))
for name in scenario.get("layers", []):
    proc = spawn(name)
    keep.append(proc)
    layers.append(proc)
for app in scenario.get("children", []):
    keep.append(spawn(app["name"], app.get("mode", "normal")))

directory = os.path.join(base, "hypr", "test")
os.makedirs(directory)
server = socket.socket(socket.AF_UNIX)
server.bind(os.path.join(directory, ".socket.sock"))
server.listen(64)
with open(os.path.join(directory, "hyprland.lock"), "w") as f:
    f.write(f"{os.getpid()}\nwayland-1\n")
open(os.path.join(out, "fake.ready"), "w").close()


def prune():
    for address in [a for a, w in windows.items() if w["proc"].poll() is not None]:
        del windows[address]


def close_window(address):
    window = windows.get(address)
    if not window:
        return "ok"
    window["closes"] += 1
    event(f"close {window['name']}")
    kind = window["kind"]
    if kind == "normal":
        window["proc"].kill()
        window["proc"].wait()
        del windows[address]
    elif kind == "tray":
        del windows[address]
        keep.append(window["proc"])
    elif kind == "respawn":
        window["proc"].kill()
        window["proc"].wait()
        del windows[address]
        name = window["name"] + "-new"
        add_window(name, "normal", spawn(name))
    elif kind == "reject":
        return "error: rejected by test"
    return "ok"


def handle(request):
    event(f"request {request}")
    prune()
    if request == "j/status":
        return json.dumps({"configProvider": "lua", "backend": "headless"})
    if request == "j/clients":
        return json.dumps([{"address": a, "class": w["name"], "title": w["title"], "pid": w["proc"].pid}
                           for a, w in windows.items()])
    if request == "j/layers":
        levels = {"0": [{"address": hex(0x9000 + i), "namespace": "layer", "pid": p.pid} for i, p in enumerate(layers)]}
        return json.dumps({"HEADLESS-1": {"levels": levels}})
    match = re.fullmatch(r"/dispatch hl\.dsp\.window\.close\(\{ window = 'address:(0x[0-9a-f]+)' \}\)", request)
    if match:
        return close_window(match.group(1))
    if request == "/dispatch hl.dsp.exit()":
        event("exit")
        return "ok"
    if request.startswith("/notify "):
        event(request)
        return "ok"
    return "unknown request"


while True:
    client, _ = server.accept()
    with client:
        reply = handle(client.recv(65536).decode())
        client.sendall(reply.encode())
