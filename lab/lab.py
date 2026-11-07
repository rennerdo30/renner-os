#!/usr/bin/env python3
"""renner.dev OS test lab: headless QEMU VMs on this Windows PC (WHPX).

Every VM runs hidden, gets a hard time limit (a detached watchdog quits it),
and records its own PID; `down` stops only that PID.

  python lab/lab.py new test1 --size 64G
  python lab/lab.py up test1 --iso E:/AgentData/renneros-lab/isos/archlinux.iso --minutes 30
  python lab/lab.py shot test1            # PNG screenshot, path printed
  python lab/lab.py type test1 "root"     # type text
  python lab/lab.py key test1 ret         # press keys (QEMU qcodes)
  python lab/lab.py snap test1 save clean # disk snapshot (VM stopped)
  python lab/lab.py status
  python lab/lab.py down test1

Lab data (QEMU, ISOs, disks, logs) lives outside the repo, in RENNER_LAB_DIR
(default E:/AgentData/renneros-lab).
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

LAB = Path(os.environ.get("RENNER_LAB_DIR", "E:/AgentData/renneros-lab"))
QEMU_DIR = LAB / "qemu"
VMS = LAB / "vms"
QMP_PORT_BASE = 44400
VNC_DISPLAY_BASE = 40  # VNC port 5900 + n, localhost only

# --- process helpers (Windows, no psutil) ---------------------------------

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
STILL_ACTIVE = 259


def pid_alive(pid: int) -> bool:
    if os.name != "nt":
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False
    k32 = ctypes.windll.kernel32
    handle = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return False
    try:
        code = ctypes.c_ulong()
        return bool(k32.GetExitCodeProcess(handle, ctypes.byref(code))) and code.value == STILL_ACTIVE
    finally:
        k32.CloseHandle(handle)


def pid_image(pid: int) -> str:
    """Executable name of a PID, so we never kill a recycled PID."""
    out = subprocess.run(
        ["tasklist", "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"],
        capture_output=True, text=True, timeout=15,
    ).stdout.strip()
    return out.split(",")[0].strip('"').lower() if out.startswith('"') else ""


def spawn_detached(args: list[str], log: Path) -> int:
    flags = 0
    if os.name == "nt":
        flags = 0x00000008 | 0x00000200 | 0x08000000  # DETACHED | NEW_GROUP | NO_WINDOW
    with open(log, "ab") as fh:
        proc = subprocess.Popen(
            args, stdin=subprocess.DEVNULL, stdout=fh, stderr=subprocess.STDOUT,
            creationflags=flags, close_fds=True,
        )
    return proc.pid


# --- VM state -------------------------------------------------------------


def vm_dir(name: str) -> Path:
    return VMS / name


def load_state(name: str) -> dict:
    path = vm_dir(name) / "vm.json"
    if not path.exists():
        sys.exit(f"lab: no VM named '{name}' (create it with: lab.py new {name})")
    return json.loads(path.read_text())


def save_state(name: str, state: dict) -> None:
    (vm_dir(name) / "vm.json").write_text(json.dumps(state, indent=2))


def running(state: dict) -> bool:
    pid = state.get("pid")
    return bool(pid) and pid_alive(pid) and pid_image(pid).startswith("qemu-system")


# --- QMP --------------------------------------------------------------------


class Qmp:
    def __init__(self, port: int, timeout: float = 10):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.buf = b""
        self._read()  # greeting
        self.cmd("qmp_capabilities")

    def _read(self) -> dict:
        while True:
            while b"\n" not in self.buf:
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise ConnectionError("QMP connection closed")
                self.buf += chunk
            line, self.buf = self.buf.split(b"\n", 1)
            msg = json.loads(line)
            if "event" not in msg:
                return msg

    def cmd(self, execute: str, **arguments):
        self.sock.sendall(json.dumps({"execute": execute, "arguments": arguments}).encode() + b"\n")
        reply = self._read()
        if "error" in reply:
            raise RuntimeError(f"QMP {execute}: {reply['error'].get('desc')}")
        return reply.get("return")

    def hmp(self, line: str) -> str:
        return self.cmd("human-monitor-command", **{"command-line": line}) or ""

    def close(self):
        self.sock.close()


def qmp_for(name: str) -> Qmp:
    state = load_state(name)
    if not running(state):
        sys.exit(f"lab: VM '{name}' is not running")
    return Qmp(state["qmp_port"])


# --- keyboard ---------------------------------------------------------------

_PLAIN = {" ": "spc", "-": "minus", "=": "equal", "[": "bracket_left", "]": "bracket_right",
          ";": "semicolon", "'": "apostrophe", "`": "grave_accent", "\\": "backslash",
          ",": "comma", ".": "dot", "/": "slash", "\n": "ret", "\t": "tab"}
_SHIFTED = {"_": "minus", "+": "equal", "{": "bracket_left", "}": "bracket_right",
            ":": "semicolon", '"': "apostrophe", "~": "grave_accent", "|": "backslash",
            "<": "comma", ">": "dot", "?": "slash", "!": "1", "@": "2", "#": "3",
            "$": "4", "%": "5", "^": "6", "&": "7", "*": "8", "(": "9", ")": "0"}


def char_keys(ch: str) -> list[str]:
    if ch.isascii() and (ch.islower() or ch.isdigit()):
        return [ch]
    if ch.isascii() and ch.isupper():
        return ["shift", ch.lower()]
    if ch in _PLAIN:
        return [_PLAIN[ch]]
    if ch in _SHIFTED:
        return ["shift", _SHIFTED[ch]]
    raise ValueError(f"cannot type {ch!r} (US layout only)")


def send_keys(qmp: Qmp, keys: list[str]) -> None:
    qmp.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in keys])
    time.sleep(0.03)


# --- commands ---------------------------------------------------------------


def qemu_bin(name: str) -> str:
    exe = QEMU_DIR / (name + (".exe" if os.name == "nt" else ""))
    if not exe.exists():
        sys.exit(f"lab: {exe} not found; QEMU belongs in {QEMU_DIR}")
    return str(exe)


def cmd_new(a) -> None:
    d = vm_dir(a.name)
    if (d / "vm.json").exists():
        sys.exit(f"lab: VM '{a.name}' already exists")
    d.mkdir(parents=True)
    subprocess.run([qemu_bin("qemu-img"), "create", "-q", "-f", "qcow2", str(d / "disk.qcow2"), a.size],
                   check=True, timeout=60)
    # qcow2, not raw: savevm needs every writable drive to support snapshots.
    subprocess.run([qemu_bin("qemu-img"), "convert", "-q", "-f", "raw", "-O", "qcow2",
                    str(QEMU_DIR / "share/edk2-i386-vars.fd"), str(d / "efivars.qcow2")],
                   check=True, timeout=60)
    index = len([p for p in VMS.iterdir() if (p / "vm.json").exists()])
    save_state(a.name, {"name": a.name, "qmp_port": QMP_PORT_BASE + index,
                        "vnc_display": VNC_DISPLAY_BASE + index, "pid": None})
    print(f"created {d}")


def cmd_up(a) -> None:
    state = load_state(a.name)
    if running(state):
        sys.exit(f"lab: VM '{a.name}' already runs (pid {state['pid']})")
    d = vm_dir(a.name)
    args = [
        qemu_bin("qemu-system-x86_64"),
        "-name", f"renneros-{a.name}",
        "-machine", "q35", "-accel", "whpx", "-cpu", "host",
        "-smp", str(a.cpus), "-m", a.mem,
        "-drive", f"if=pflash,format=raw,readonly=on,file={QEMU_DIR / 'share/edk2-x86_64-code.fd'}",
        "-drive", f"if=pflash,format=qcow2,file={d / 'efivars.qcow2'}",
        "-drive", f"if=virtio,format=qcow2,file={d / 'disk.qcow2'}",
        "-device", "virtio-vga",
        "-device", "qemu-xhci", "-device", "usb-tablet",
        "-nic", "user,model=virtio-net-pci",
        "-audiodev", "none,id=snd0",
        "-qmp", f"tcp:127.0.0.1:{state['qmp_port']},server=on,wait=off",
        "-serial", f"file:{d / 'serial.log'}",
        "-vnc", f"127.0.0.1:{state['vnc_display']}",
        "-display", "sdl" if a.window else "none",
    ]
    if a.iso:
        args += ["-drive", f"media=cdrom,format=raw,readonly=on,file={Path(a.iso).resolve()}",
                 "-boot", "once=d"]
    log = d / "qemu.log"
    log.write_text(" ".join(args) + "\n")
    pid = spawn_detached(args, log)
    deadline = time.time() + a.minutes * 60
    state.update(pid=pid, deadline=deadline, started=time.time())
    save_state(a.name, state)
    watchdog = spawn_detached([sys.executable, str(Path(__file__).resolve()), "_watchdog", a.name],
                              d / "watchdog.log")
    state["watchdog_pid"] = watchdog
    save_state(a.name, state)

    for _ in range(50):  # wait for QMP so we fail loudly on a bad start
        if not pid_alive(pid):
            sys.exit(f"lab: QEMU exited at start; see {log}")
        try:
            Qmp(state["qmp_port"], timeout=2).close()
            break
        except OSError:
            time.sleep(0.2)
    print(f"up: {a.name} pid={pid} limit={a.minutes} min "
          f"vnc=127.0.0.1:{5900 + state['vnc_display']} qmp={state['qmp_port']}")


def quit_vm(name: str, state: dict) -> None:
    pid = state.get("pid")
    if not running(state):
        return
    try:
        q = Qmp(state["qmp_port"], timeout=5)
        q.cmd("quit")
        q.close()
    except (OSError, RuntimeError, ConnectionError):
        pass
    for _ in range(50):
        if not pid_alive(pid):
            return
        time.sleep(0.2)
    # Still alive: kill only the PID we started, after checking it is QEMU.
    if pid_image(pid).startswith("qemu-system"):
        subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True, timeout=15)


def cmd_down(a) -> None:
    state = load_state(a.name)
    quit_vm(a.name, state)
    state["pid"] = None
    save_state(a.name, state)
    print(f"down: {a.name}")


def cmd_watchdog(a) -> None:
    while True:
        state = load_state(a.name)
        pid = state.get("pid")
        if not pid or not pid_alive(pid):
            return
        if time.time() >= state["deadline"]:
            print(f"{time.ctime()}: time limit reached, stopping {a.name}", flush=True)
            quit_vm(a.name, state)
            return
        time.sleep(5)


def cmd_status(a) -> None:
    if not VMS.exists():
        print("no VMs")
        return
    for d in sorted(VMS.iterdir()):
        if not (d / "vm.json").exists():
            continue
        s = json.loads((d / "vm.json").read_text())
        if running(s):
            left = max(0, int((s["deadline"] - time.time()) / 60))
            print(f"{s['name']}: running pid={s['pid']} {left} min left vnc=127.0.0.1:{5900 + s['vnc_display']}")
        else:
            print(f"{s['name']}: stopped")


def cmd_shot(a) -> None:
    q = qmp_for(a.name)
    shots = vm_dir(a.name) / "shots"
    shots.mkdir(exist_ok=True)
    out = Path(a.out) if a.out else shots / time.strftime("%Y%m%d-%H%M%S.png")
    q.cmd("screendump", filename=str(out.resolve()), format="png")
    q.close()
    print(out.resolve())


def cmd_key(a) -> None:
    q = qmp_for(a.name)
    for combo in a.keys:
        send_keys(q, combo.split("+"))
    q.close()


def cmd_type(a) -> None:
    q = qmp_for(a.name)
    for ch in a.text:
        send_keys(q, char_keys(ch))
    if a.enter:
        send_keys(q, ["ret"])
    q.close()


def cmd_snap(a) -> None:
    # Disk snapshots of a stopped VM. WHPX cannot save a running VM's state
    # ("missing dirty memory tracking support"), so live savevm is not offered.
    state = load_state(a.name)
    if running(state):
        sys.exit(f"lab: stop '{a.name}' first (lab.py down {a.name}); snapshots are disk-only")
    if a.action == "list":
        subprocess.run([qemu_bin("qemu-img"), "snapshot", "-l", str(vm_dir(a.name) / "disk.qcow2")],
                       check=True, timeout=60)
        return
    if not a.tag:
        sys.exit("lab: snapshot tag required")
    flag = {"save": "-c", "load": "-a", "delete": "-d"}[a.action]
    for image in ("disk.qcow2", "efivars.qcow2"):  # keep UEFI boot entries in step with the disk
        subprocess.run([qemu_bin("qemu-img"), "snapshot", flag, a.tag, str(vm_dir(a.name) / image)],
                       check=True, timeout=300)
    print(f"{a.action} {a.tag}: ok")


def main() -> None:
    p = argparse.ArgumentParser(description="renner.dev OS QEMU test lab")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("new"); s.add_argument("name"); s.add_argument("--size", default="64G"); s.set_defaults(f=cmd_new)
    s = sub.add_parser("up"); s.add_argument("name"); s.add_argument("--iso")
    s.add_argument("--minutes", type=int, default=30); s.add_argument("--cpus", type=int, default=4)
    s.add_argument("--mem", default="8G")
    s.add_argument("--window", action="store_true", help="visible window; only when the owner asks")
    s.set_defaults(f=cmd_up)
    s = sub.add_parser("down"); s.add_argument("name"); s.set_defaults(f=cmd_down)
    s = sub.add_parser("status"); s.set_defaults(f=cmd_status)
    s = sub.add_parser("shot"); s.add_argument("name"); s.add_argument("out", nargs="?"); s.set_defaults(f=cmd_shot)
    s = sub.add_parser("key"); s.add_argument("name"); s.add_argument("keys", nargs="+", help="qcodes, e.g. ret ctrl+alt+f2")
    s.set_defaults(f=cmd_key)
    s = sub.add_parser("type"); s.add_argument("name"); s.add_argument("text"); s.add_argument("--enter", action="store_true")
    s.set_defaults(f=cmd_type)
    s = sub.add_parser("snap", help="disk snapshots of a stopped VM")
    s.add_argument("name"); s.add_argument("action", choices=("save", "load", "delete", "list"))
    s.add_argument("tag", nargs="?"); s.set_defaults(f=cmd_snap)
    s = sub.add_parser("_watchdog"); s.add_argument("name"); s.set_defaults(f=cmd_watchdog)

    a = p.parse_args()
    a.f(a)


if __name__ == "__main__":
    main()
