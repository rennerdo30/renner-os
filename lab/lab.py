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

  python lab/lab.py iso                   # fetch + verify the newest Arch ISO
  python lab/lab.py up test1 --iso latest # boot the newest ISO in isos/
  python lab/lab.py cache up|down|status  # local pacman cache (also started by `up`)
  python lab/lab.py use-cache test1       # point a booted Arch live system at it

Lab data (QEMU, ISOs, disks, logs, package cache) lives outside the repo, in
RENNER_LAB_DIR (default E:/AgentData/renneros-lab).
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import re
import secrets
import shlex
import socket
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

LAB = Path(os.environ.get("RENNER_LAB_DIR", "E:/AgentData/renneros-lab"))
QEMU_DIR = LAB / "qemu"
# Our QEMU build (rennerdo30/winq-emu-qemu: Venus on Windows, no Windows power
# throttling), the lab default; --stock-qemu uses QEMU_DIR instead.
OUR_QEMU = Path(os.environ.get("RENNER_QEMU", "E:/AgentData/winq-build/bin"))
VMS = LAB / "vms"
ISOS = LAB / "isos"
CACHE_STATE = LAB / "pkgcache.json"
CACHE_PORT = 8079
# Two independent mirrors must agree on the ISO checksum.
ISO_MIRRORS = ("https://geo.mirror.pkgbuild.com", "https://mirror.rackspace.com/archlinux")
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


# --- window placement (owner, 2026-11-08: "open the qemu windows on the second screen") ---


def _windows_of(pid: int) -> list[int]:
    """Visible top-level windows of a process."""
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    found: list[int] = []
    proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def callback(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(proc(callback), 0)
    return found


def _secondary_monitor():
    """Work area (left, top, right, bottom) of the first non-primary monitor, or None."""
    from ctypes import wintypes
    user32 = ctypes.windll.user32

    class MONITORINFO(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT),
                    ("rcWork", wintypes.RECT), ("dwFlags", wintypes.DWORD)]

    areas = []
    proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HMONITOR, wintypes.HDC,
                              ctypes.POINTER(wintypes.RECT), wintypes.LPARAM)

    def callback(hmon, _hdc, _rect, _):
        info = MONITORINFO()
        info.cbSize = ctypes.sizeof(MONITORINFO)
        user32.GetMonitorInfoW(hmon, ctypes.byref(info))
        if not info.dwFlags & 1:  # MONITORINFOF_PRIMARY
            r = info.rcWork
            areas.append((r.left, r.top, r.right, r.bottom))
        return True

    user32.EnumDisplayMonitors(0, 0, proc(callback), 0)
    return areas[0] if areas else None


def place_window(pid: int, wait: float = 15.0) -> str:
    """Moves a VM window to the second screen (keeps its size), centred."""
    user32 = ctypes.windll.user32
    from ctypes import wintypes
    area = _secondary_monitor()
    if not area:
        return "only one screen; window left where it is"
    end = time.time() + wait
    while time.time() < end:
        wins = _windows_of(pid)
        if wins:
            hwnd = wins[0]
            rect = wintypes.RECT()
            user32.GetWindowRect(hwnd, ctypes.byref(rect))
            w, h = rect.right - rect.left, rect.bottom - rect.top
            left, top, right, bottom = area
            x = left + max(0, (right - left - w) // 2)
            y = top + max(0, (bottom - top - h) // 2)
            # ASYNCWINDOWPOS: never wait for a busy QEMU window to answer.
            SWP_NOSIZE, SWP_NOZORDER, SWP_ASYNCWINDOWPOS = 0x0001, 0x0004, 0x4000
            user32.SetWindowPos(hwnd, 0, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_ASYNCWINDOWPOS)
            return f"window on the second screen at {x},{y}"
        time.sleep(0.5)
    return "no window found"


def cmd_place(a) -> None:
    state = load_state(a.name)
    if not running(state):
        sys.exit(f"lab: VM '{a.name}' is not running")
    print(f"{a.name}: {place_window(state['pid'])}")


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
    # Default: our QEMU, and with a window Vulkan through Venus (the installed
    # system; the installer ISO keeps plain graphics, the lab drives it by
    # screenshots, which a GL display does not give).
    if not a.qemu and not a.stock_qemu and (OUR_QEMU / "qemu-system-x86_64.exe").exists():
        a.qemu = str(OUR_QEMU)
        if a.window and not a.iso and not a.no_gl:
            a.venus = True
    if a.venus:
        a.gl = True
    args = [
        str(Path(a.qemu) / "qemu-system-x86_64.exe") if a.qemu else qemu_bin("qemu-system-x86_64"),
        "-name", f"renneros-{a.name}",
        "-machine", "q35", "-accel", "whpx", "-cpu", "host",
        "-smp", str(a.cpus), "-m", a.mem,
        "-drive", f"if=none,id=hd0,format=qcow2,file={d / 'disk.qcow2'}",
        # --nvme: Windows guests (inbox driver; virtio-blk needs viostor during setup)
        "-device", ("nvme,drive=hd0,serial=renner-hd0,bootindex=1" if a.nvme
                    else "virtio-blk-pci,drive=hd0,bootindex=1"),
        # --gl: virgl passes the guest's OpenGL to the host GPU through the SDL
        # window's GL context; plain virtio-vga renders in software (llvmpipe).
        # --venus: Vulkan through virtio-gpu (needs blob resources and host
        # memory); upstream QEMU has it only on Linux hosts, so it takes a
        # QEMU build with Venus on Windows (--qemu).
        "-device", ("virtio-vga-gl,blob=on,hostmem=4G,venus=on" if a.venus
                    else "virtio-vga-gl" if a.gl else "virtio-vga"),
        "-device", "qemu-xhci", "-device", "usb-tablet",
        "-nic", "user,model=virtio-net-pci",
        "-audiodev", "none,id=snd0",
        # A guest-triggered warm reset hangs under WHPX, so a guest reboot ends
        # QEMU instead; `lab.py up` then does a cold boot.
        "-no-reboot",
        "-qmp", f"tcp:127.0.0.1:{state['qmp_port']},server=on,wait=off",
        # Second serial port (ttyS1): the installer's lab control channel.
        "-chardev", f"socket,id=ctl,host=127.0.0.1,port={ctl_port(state)},server=on,wait=off",
        "-device", "isa-serial,chardev=ctl",
        # QEMU guest agent (installed systems): lab.py exec runs commands inside.
        "-device", "virtio-serial-pci",
        "-chardev", f"socket,id=qga,host=127.0.0.1,port={qga_port(state)},server=on,wait=off",
        "-device", "virtserialport,chardev=qga,name=org.qemu.guest_agent.0",
        "-serial", f"file:{d / 'serial.log'}",
        "-display", ("sdl,gl=on" if a.gl else "sdl") if a.window else "none",
    ]
    if not a.gl:  # QEMU refuses VNC next to a GL display
        args += ["-vnc", f"127.0.0.1:{state['vnc_display']}"]
    args += shlex.split(a.qemu_args)
    if a.gl and not a.window:
        sys.exit("lab: --gl needs --window (virgl renders through the SDL window's GL context)")
    if not a.bios:  # UEFI (OVMF); without these drives QEMU boots SeaBIOS
        args += ["-drive", f"if=pflash,format=raw,readonly=on,file={QEMU_DIR / 'share/edk2-x86_64-code.fd'}",
                 "-drive", f"if=pflash,format=qcow2,file={d / 'efivars.qcow2'}"]
    if a.iso:
        # bootindex, not -boot once=d: OVMF follows QEMU's boot order, but keeps
        # booting an installed disk first when only -boot is given.
        args += ["-drive", f"if=none,id=cd0,media=cdrom,format=raw,readonly=on,file={resolve_iso(a.iso)}",
                 "-device", "ide-cd,bus=ide.0,drive=cd0,bootindex=0"]
    if not a.no_cache:  # keep the package cache alive at least as long as the VM
        s = cache_up(a.minutes + 10)
        if s["deadline"] < time.time() + a.minutes * 60:
            print("note: the running cache stops before this VM; restart it with a longer --minutes")
    log = d / "qemu.log"
    log.write_text(" ".join(args) + "\n")
    # The ports can be taken: by a VM that was just stopped, and by WSL, which
    # blocks these local ports while it runs and for about a minute after its
    # last distro stopped. QEMU then fails ("Failed to find an available
    # port") or freezes in the boot (2026-11-08).
    ports = [state["qmp_port"], ctl_port(state), qga_port(state)]
    end = time.time() + 180
    told = False
    while time.time() < end:
        busy = []
        for port in ports:
            with socket.socket() as probe:
                try:
                    probe.bind(("127.0.0.1", port))
                except OSError:
                    busy.append(port)
        if not busy:
            break
        if not told:
            print(f"lab: waiting for ports {busy} (WSL or a stopping VM holds them)")
            told = True
        time.sleep(2)
    else:
        sys.exit(f"lab: ports still in use: {busy}")
    pid = spawn_detached(args, log)
    if a.window:
        place_window(pid)
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
    if not a.no_enter:
        # systemd-boot's countdown does not run under WHPX (ISO and installed
        # system alike), so pick the default entry ourselves. The menu shows up
        # after a varying delay (slower with a visible window), so press Enter
        # every 3 s for 30 s. Stop as soon as the guest agent has opened its
        # port: the system is up, and a later Enter would be an empty password
        # at the login screen (three lock the account; owner, 2026-11-14).
        for _ in range(10):
            time.sleep(3)
            q = Qmp(state["qmp_port"])
            if any(c["label"] == "qga" and c.get("frontend-open") for c in q.cmd("query-chardev")):
                q.close()
                break
            send_keys(q, ["ret"])
            q.close()
    print(f"up: {a.name} pid={pid} limit={a.minutes} min "
          + ("gl=virgl " if a.gl else f"vnc=127.0.0.1:{5900 + state['vnc_display']} ")
          + f"qmp={state['qmp_port']}")


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


def ctl_port(state: dict) -> int:
    return state["qmp_port"] + 100


def qga_port(state: dict) -> int:
    return state["qmp_port"] + 200


def guest_exec(name: str, argv: list[str], timeout: float = 60.0) -> tuple[int, str, str]:
    """Run argv inside the guest (as root) through the QEMU guest agent."""
    import base64
    state = load_state(name)
    with socket.create_connection(("127.0.0.1", qga_port(state)), timeout=10) as s:
        buf = b""

        def call(execute, **arguments):
            nonlocal buf
            s.sendall(json.dumps({"execute": execute, "arguments": arguments}).encode() + b"\n")
            while True:
                while b"\n" not in buf:
                    chunk = s.recv(65536)
                    if not chunk:
                        raise ConnectionError("guest agent closed the connection")
                    buf += chunk
                line, buf = buf.split(b"\n", 1)
                # guest-sync-delimited answers with a 0xFF marker first.
                line = line.lstrip(b"\xff").strip()
                if not line:
                    continue
                msg = json.loads(line)
                if "error" in msg:
                    raise RuntimeError(f"guest agent {execute}: {msg['error'].get('desc')}")
                if "return" in msg:
                    return msg["return"]

        # Resynchronise: a stale reply from an earlier client could be queued;
        # replies until our token are dropped.
        token = secrets.randbelow(1 << 30)
        for _ in range(20):
            try:
                if call("guest-sync-delimited", id=token) == token:
                    break
            except RuntimeError:
                continue
        pid = call("guest-exec", path=argv[0], arg=argv[1:], **{"capture-output": True})["pid"]
        end = time.time() + timeout
        while time.time() < end:
            st = call("guest-exec-status", pid=pid)
            if st.get("exited"):
                out = base64.b64decode(st.get("out-data", "")).decode(errors="replace")
                err = base64.b64decode(st.get("err-data", "")).decode(errors="replace")
                return st.get("exitcode", -1), out, err
            time.sleep(0.5)
    raise TimeoutError(f"{argv[0]} did not finish in {timeout:.0f} s")


def cmd_exec(a) -> None:
    """lab.py exec VM [--timeout S] [--wait-agent S] -- CMD ARGS..."""
    # argparse's REMAINDER also swallows options after the VM name; take them here.
    argv = list(a.argv)
    while argv and argv[0] in ("--timeout", "--wait-agent") and len(argv) > 1:
        setattr(a, argv[0][2:].replace("-", "_"), float(argv[1]))
        argv = argv[2:]
    if argv and argv[0] == "--":
        argv = argv[1:]
    if not argv:
        sys.exit("lab: exec needs a command")
    end = time.time() + a.wait_agent
    while True:
        try:
            code, out, err = guest_exec(a.name, argv, a.timeout)
            break
        except (OSError, ConnectionError, RuntimeError) as e:
            if time.time() >= end:
                sys.exit(f"lab: guest agent not reachable: {e}")
            time.sleep(3)
    # Guest output is UTF-8 (pacman prints ✔); a Windows console code page
    # cannot encode all of it.
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(errors="replace")
    sys.stdout.write(out)
    sys.stderr.write(err)
    sys.exit(code)


def ui_command(name: str, line: str, timeout: float = 15.0) -> str:
    """One command to the installer's control channel; returns its reply."""
    state = load_state(name)
    with socket.create_connection(("127.0.0.1", ctl_port(state)), timeout=timeout) as s:
        s.sendall(line.encode() + b"\n")
        data = b""
        while not data.endswith(b"\n"):
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
    return data.decode(errors="replace").strip()


def cmd_ui(a) -> None:
    """Drive the installer: lab.py ui VM CMD...  |  lab.py ui VM --wait-page N [--timeout S]"""
    if a.wait_page is not None:
        end = time.time() + a.timeout
        reply = ""
        while time.time() < end:
            try:
                reply = ui_command(a.name, "state", timeout=5)
            except OSError:
                reply = "no reply"
            if reply.startswith(f"page={a.wait_page} ") or (a.fail_page and reply.startswith(f"page={a.fail_page} ")):
                print(reply)
                sys.exit(0 if reply.startswith(f"page={a.wait_page} ") else 1)
            time.sleep(2)
        sys.exit(f"timeout waiting for page {a.wait_page}; last: {reply}")
    reply = ui_command(a.name, " ".join(a.command))
    print(reply)
    if reply.startswith("err"):
        sys.exit(1)


def cmd_type(a) -> None:
    q = qmp_for(a.name)
    for ch in a.text:
        send_keys(q, char_keys(ch))
    if a.enter:
        send_keys(q, ["ret"])
    q.close()


def cmd_click(a) -> None:
    """Left click at screen pixel X,Y through the USB tablet (absolute axes)."""
    q = qmp_for(a.name)
    ax = round(a.x * 32767 / max(a.width - 1, 1))
    ay = round(a.y * 32767 / max(a.height - 1, 1))
    q.cmd("input-send-event", events=[
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ])
    time.sleep(0.05)
    for down in (True, False):
        q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": "left"}}])
        time.sleep(0.05)
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


# --- Arch ISO -----------------------------------------------------------------


def _fetch(url: str, timeout: int = 30) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "renneros-lab"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def cmd_iso(a) -> None:
    sums = {}
    for mirror in ISO_MIRRORS:
        text = _fetch(f"{mirror}/iso/latest/sha256sums.txt").decode()
        m = re.search(r"^([0-9a-f]{64})\s+(archlinux-\d{4}\.\d{2}\.\d{2}-x86_64\.iso)$", text, re.M)
        if not m:
            sys.exit(f"lab: no ISO checksum in {mirror}")
        sums[mirror] = (m.group(2), m.group(1))
    if len(set(sums.values())) != 1:
        sys.exit(f"lab: mirrors disagree on the latest ISO: {sums}")
    name, digest = next(iter(sums.values()))
    ISOS.mkdir(parents=True, exist_ok=True)
    target = ISOS / name
    if target.exists():
        print(f"latest: {target} (already here)")
        return
    version = name.split("-")[1]
    url = f"{ISO_MIRRORS[0]}/iso/{version}/{name}"
    part = target.with_suffix(".iso.part")
    h = hashlib.sha256()
    print(f"downloading {url}")
    req = urllib.request.Request(url, headers={"User-Agent": "renneros-lab"})
    with urllib.request.urlopen(req, timeout=60) as resp, open(part, "wb") as out:
        while chunk := resp.read(1 << 20):
            out.write(chunk)
            h.update(chunk)
    if h.hexdigest() != digest:
        part.unlink()
        sys.exit(f"lab: checksum mismatch for {name}; download discarded")
    part.replace(target)
    print(f"latest: {target} (sha256 verified)")


def resolve_iso(value: str) -> Path:
    if value != "latest":
        return Path(value).resolve()
    isos = sorted(ISOS.glob("archlinux-*-x86_64.iso"))
    if not isos:
        sys.exit("lab: no ISO yet; run: lab.py iso")
    return isos[-1]


# --- package cache --------------------------------------------------------------


def cache_state() -> dict | None:
    if not CACHE_STATE.exists():
        return None
    s = json.loads(CACHE_STATE.read_text())
    alive = s.get("pid") and pid_alive(s["pid"]) and pid_image(s["pid"]).startswith("python")
    return s if alive else None


def cache_up(minutes: int) -> dict:
    s = cache_state()
    if s:
        return s
    token = secrets.token_hex(16)
    env_token = dict(os.environ, RENNER_PKGCACHE_TOKEN=token)
    log = LAB / "pkgcache.log"
    flags = 0x00000008 | 0x00000200 | 0x08000000 if os.name == "nt" else 0
    with open(log, "ab") as fh:
        proc = subprocess.Popen(
            [sys.executable, str(Path(__file__).with_name("pkgcache.py")),
             "--root", str(LAB / "pkgcache"), "--port", str(CACHE_PORT), "--minutes", str(minutes),
             "--files", str(Path(__file__).resolve().parents[1])],
            stdin=subprocess.DEVNULL, stdout=fh, stderr=subprocess.STDOUT,
            creationflags=flags, env=env_token, close_fds=True,
        )
    s = {"pid": proc.pid, "port": CACHE_PORT, "token": token, "deadline": time.time() + minutes * 60}
    CACHE_STATE.write_text(json.dumps(s, indent=2))
    for _ in range(50):
        try:
            _fetch(f"http://127.0.0.1:{CACHE_PORT}/_lab/stats", timeout=2)
            break
        except OSError:
            if not pid_alive(proc.pid):
                sys.exit(f"lab: package cache failed to start; see {log}")
            time.sleep(0.2)
    print(f"cache: up pid={proc.pid} http://127.0.0.1:{CACHE_PORT} (guests: http://10.0.2.2:{CACHE_PORT}) "
          f"limit={minutes} min")
    return s


def cache_down() -> None:
    s = cache_state()
    if not s:
        print("cache: not running")
        return
    try:
        _fetch(f"http://127.0.0.1:{s['port']}/_lab/quit?token={s['token']}", timeout=5)
    except OSError:
        pass
    for _ in range(50):
        if not pid_alive(s["pid"]):
            break
        time.sleep(0.2)
    else:
        if pid_image(s["pid"]).startswith("python"):  # only our recorded PID
            subprocess.run(["taskkill", "/PID", str(s["pid"]), "/F"], capture_output=True, timeout=15)
    print("cache: down")


def cmd_cache(a) -> None:
    if a.action == "up":
        cache_up(a.minutes)
    elif a.action == "down":
        cache_down()
    else:
        s = cache_state()
        if not s:
            print("cache: not running")
            return
        stats = _fetch(f"http://127.0.0.1:{s['port']}/_lab/stats", timeout=5).decode().strip()
        left = max(0, int((s["deadline"] - time.time()) / 60))
        size = sum(f.stat().st_size for f in (LAB / "pkgcache").rglob("*.pkg.tar.*"))
        print(f"cache: running pid={s['pid']} {left} min left, {size / 1e9:.2f} GB cached, {stats}")


def cmd_use_cache(a) -> None:
    """Type the cache mirror into a booted Arch live system (root shell)."""
    q = qmp_for(a.name)
    line = (f"systemctl stop reflector.service; "
            f"echo 'Server = http://10.0.2.2:{CACHE_PORT}/$repo/os/$arch' > /etc/pacman.d/mirrorlist")
    for ch in line:
        send_keys(q, char_keys(ch))
    send_keys(q, ["ret"])
    q.close()
    print(f"{a.name}: pacman now uses http://10.0.2.2:{CACHE_PORT}")


def main() -> None:
    p = argparse.ArgumentParser(description="renner.dev OS QEMU test lab")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("new"); s.add_argument("name"); s.add_argument("--size", default="64G"); s.set_defaults(f=cmd_new)
    s = sub.add_parser("up"); s.add_argument("name"); s.add_argument("--iso")
    s.add_argument("--minutes", type=int, default=30); s.add_argument("--cpus", type=int, default=4)
    s.add_argument("--mem", default="8G")
    s.add_argument("--window", action="store_true", help="visible window; only when the owner asks")
    s.add_argument("--no-cache", action="store_true", help="don't start the package cache")
    s.add_argument("--no-enter", action="store_true", help="don't press Enter at the boot menu")
    s.add_argument("--bios", action="store_true", help="legacy BIOS (SeaBIOS) instead of UEFI")
    s.add_argument("--nvme", action="store_true", help="NVMe disk instead of virtio-blk (Windows guests)")
    s.add_argument("--gl", action="store_true",
                   help="3D acceleration (virgl: guest OpenGL on the host GPU; needs --window). "
                        "No Vulkan; automated tests stay on software rendering")
    s.add_argument("--venus", action="store_true",
                   help="Vulkan through Venus (implies --gl); needs a QEMU with Venus on Windows (--qemu)")
    s.add_argument("--qemu", metavar="DIR", help="other QEMU build (folder with qemu-system-x86_64.exe); "
                   "default: our build in RENNER_QEMU (E:/AgentData/winq-build/bin) with Venus")
    s.add_argument("--stock-qemu", action="store_true", help=f"the stock QEMU in {QEMU_DIR}, not our build")
    s.add_argument("--no-gl", action="store_true", help="our QEMU without Venus/virgl (plain virtio-vga)")
    s.add_argument("--qemu-args", default="", metavar="ARGS",
                   help="extra QEMU arguments, e.g. '-d guest_errors' (debugging)")
    s.set_defaults(f=cmd_up)
    s = sub.add_parser("down"); s.add_argument("name"); s.set_defaults(f=cmd_down)
    s = sub.add_parser("status"); s.set_defaults(f=cmd_status)
    s = sub.add_parser("shot"); s.add_argument("name"); s.add_argument("out", nargs="?"); s.set_defaults(f=cmd_shot)
    s = sub.add_parser("key"); s.add_argument("name"); s.add_argument("keys", nargs="+", help="qcodes, e.g. ret ctrl+alt+f2")
    s.set_defaults(f=cmd_key)
    s = sub.add_parser("click", help="left click at a screen pixel (screenshot coordinates)")
    s.add_argument("name"); s.add_argument("x", type=int); s.add_argument("y", type=int)
    s.add_argument("--width", type=int, default=1280); s.add_argument("--height", type=int, default=800)
    s.set_defaults(f=cmd_click)
    s = sub.add_parser("type"); s.add_argument("name"); s.add_argument("text"); s.add_argument("--enter", action="store_true")
    s.set_defaults(f=cmd_type)
    s = sub.add_parser("exec", help="run a command in the installed system (QEMU guest agent)")
    s.add_argument("name"); s.add_argument("argv", nargs=argparse.REMAINDER)
    s.add_argument("--timeout", type=float, default=60); s.add_argument("--wait-agent", type=float, default=0)
    s.set_defaults(f=cmd_exec)
    s = sub.add_parser("place", help="move a VM window to the second screen"); s.add_argument("name"); s.set_defaults(f=cmd_place)
    s = sub.add_parser("ui", help="drive the installer over its control channel (renner.test=1)")
    s.add_argument("name"); s.add_argument("command", nargs="*")
    s.add_argument("--wait-page", type=int); s.add_argument("--fail-page", type=int)
    s.add_argument("--timeout", type=float, default=120); s.set_defaults(f=cmd_ui)
    s = sub.add_parser("snap", help="disk snapshots of a stopped VM")
    s.add_argument("name"); s.add_argument("action", choices=("save", "load", "delete", "list"))
    s.add_argument("tag", nargs="?"); s.set_defaults(f=cmd_snap)
    s = sub.add_parser("iso", help="fetch and verify the newest Arch ISO"); s.set_defaults(f=cmd_iso)
    s = sub.add_parser("cache", help="local pacman package cache")
    s.add_argument("action", choices=("up", "down", "status"))
    s.add_argument("--minutes", type=int, default=120); s.set_defaults(f=cmd_cache)
    s = sub.add_parser("use-cache", help="point a booted Arch live system at the cache")
    s.add_argument("name"); s.set_defaults(f=cmd_use_cache)
    s = sub.add_parser("_watchdog"); s.add_argument("name"); s.set_defaults(f=cmd_watchdog)

    a = p.parse_args()
    a.f(a)


if __name__ == "__main__":
    main()
