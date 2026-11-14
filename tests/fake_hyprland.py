#!/usr/bin/env python3
"""Stand-in for the Hyprland IPC, used by run_tests.py. Speaks just enough of
socket1 and owns dummy apps that record how they were asked to stop."""
import ctypes
import json
import os
import re
import signal
import socket
import subprocess
import sys
import time

base, out = sys.argv[1], sys.argv[2]
scenario = json.loads(sys.argv[3])
ctypes.CDLL(None).prctl(15, b"Hyprland", 0, 0, 0)
DUMMY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dummy_app.py")


def event(text):
    with open(os.path.join(out, "events"), "a") as f:
        f.write(f"{time.monotonic()} {text}\n")


def wait_ready(name):
    while not os.path.exists(os.path.join(out, f"{name}.ready")):
        time.sleep(0.01)
    with open(os.path.join(out, f"{name}.pid")) as f:
        return int(f.read())


def spawn(name, mode="normal"):
    subprocess.Popen([sys.executable, "-I", DUMMY, name, mode, out])
    return wait_ready(name)


def alive(pid):
    try:
        with open(f"/proc/{pid}/stat") as f:
            return f.read().rsplit(")", 1)[1].split()[0] not in "ZX"
    except FileNotFoundError:
        return False


def kill(pid):
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


windows = {}
next_address = [0x1000]


def add_window(name, kind, pid, title=None):
    address = hex(next_address[0])
    next_address[0] += 0x10
    windows[address] = {"name": name, "kind": kind, "pid": pid, "title": title or name}


layers = []
for app in scenario.get("windows", []):
    if "launcher" in app:
        # The window belongs to a child of the launcher, like Steam's webhelper.
        spawn(app["launcher"], "parent:" + app["name"])
        pid = wait_ready(app["name"])
    else:
        pid = spawn(app["name"], app.get("mode", "normal"))
    add_window(app["name"], app["kind"], pid, app.get("title"))
for name in scenario.get("layers", []):
    layers.append(spawn(name))
for app in scenario.get("children", []):
    spawn(app["name"], app.get("mode", "normal"))

directory = os.path.join(base, "hypr", "test")
os.makedirs(directory)
server = socket.socket(socket.AF_UNIX)
server.bind(os.path.join(directory, ".socket.sock"))
server.listen(64)
with open(os.path.join(directory, "hyprland.lock"), "w") as f:
    f.write(f"{os.getpid()}\nwayland-1\n")
open(os.path.join(out, "fake.ready"), "w").close()


def prune():
    for address, window in list(windows.items()):
        if window["kind"] == "closing" and time.monotonic() >= window["close_at"]:
            kill(window["pid"])
        if not alive(window["pid"]):
            del windows[address]


def close_window(address):
    window = windows.get(address)
    if not window:
        return "ok"
    event(f"close {window['name']}")
    kind = window["kind"]
    if kind == "normal":
        kill(window["pid"])
        del windows[address]
    elif kind == "tray":
        del windows[address]
    elif kind == "respawn":
        kill(window["pid"])
        del windows[address]
        name = window["name"] + "-new"
        add_window(name, "normal", spawn(name))
    elif kind == "slow":
        window["kind"] = "closing"
        window["close_at"] = time.monotonic() + 2
    elif kind == "reject":
        return "error: rejected by test"
    return "ok"


def handle(request):
    event(f"request {request}")
    prune()
    if request == "j/status":
        return json.dumps({"configProvider": "lua", "backend": "headless"})
    if request == "j/clients":
        return json.dumps([{"address": a, "class": w["name"], "title": w["title"], "pid": w["pid"]}
                           for a, w in windows.items()])
    if request == "j/layers":
        levels = {"0": [{"address": hex(0x9000 + i), "namespace": "layer", "pid": p} for i, p in enumerate(layers)]}
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
