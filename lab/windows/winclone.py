#!/usr/bin/env python3
"""Throwaway clones of the Windows lab VM, for other projects' end-to-end tests.

Every start boots a fresh overlay on a frozen copy of the "win" VM's disk;
stop discards it. Commands run in tom's logged-on desktop session, not elevated
by default (an interactive scheduled task), so they see the desktop and the GPU.

  python lab/windows/winclone.py start [--minutes 60] [--mem 6G] [--no-gpu]
  python lab/windows/winclone.py push LOCAL [GUEST_PATH]      (default C:\\remora\\<name>)
  python lab/windows/winclone.py exec [--timeout 120] [--elevated] [--system] COMMAND
  python lab/windows/winclone.py exec-background [--elevated] COMMAND   (prints the log path)
  python lab/windows/winclone.py log ID                        (output of a background command)
  python lab/windows/winclone.py addr                          (host address of guest TCP 3390)
  python lab/windows/winclone.py stop                          (discards the clone's changes)
  python lab/windows/winclone.py status

COMMAND is one cmd.exe command line; "-" reads it from stdin (no quoting problems).
Base: BASE_DIR/base.qcow2 + base-efivars.qcow2, made once with
`qemu-img convert -O qcow2 vms/win/disk.qcow2 base.qcow2` while "win" is stopped.
"""

from __future__ import annotations

import argparse
import base64
import json
import re
import secrets
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import lab as L  # noqa: E402

BASE_DIR = L.LAB / "winclone"
USER = "tom"
FORWARDS = {3390: 13390}  # guest port -> host port (127.0.0.1)
GUEST_DIR = r"C:\Users\Public\winclone"  # writable by tom's limited token


def die(msg: str, code: int = 1):
    print(f"winclone: {msg}", file=sys.stderr)
    sys.exit(code)


# --- guest agent -------------------------------------------------------------


class Agent:
    """QEMU guest agent connection (commands run as SYSTEM in session 0)."""

    def __init__(self, name: str, timeout: float = 30):
        state = L.load_state(name)
        self.s = socket.create_connection(("127.0.0.1", L.qga_port(state)), timeout=timeout)
        self.buf = b""
        token = secrets.randbelow(1 << 30)
        for _ in range(20):  # drop stale replies queued by an earlier client
            try:
                if self.call("guest-sync-delimited", id=token) == token:
                    break
            except RuntimeError:
                continue

    def call(self, execute: str, **arguments):
        self.s.sendall(json.dumps({"execute": execute, "arguments": arguments}).encode() + b"\n")
        while True:
            while b"\n" not in self.buf:
                chunk = self.s.recv(65536)
                if not chunk:
                    raise ConnectionError("guest agent closed the connection")
                self.buf += chunk
            line, self.buf = self.buf.split(b"\n", 1)
            line = line.lstrip(b"\xff").strip()
            if not line:
                continue
            msg = json.loads(line)
            if "error" in msg:
                raise RuntimeError(f"guest agent {execute}: {msg['error'].get('desc')}")
            if "return" in msg:
                return msg["return"]

    def run(self, argv: list[str], timeout: float) -> tuple[int, str]:
        pid = self.call("guest-exec", path=argv[0], arg=argv[1:], **{"capture-output": True})["pid"]
        end = time.time() + timeout
        while time.time() < end:
            st = self.call("guest-exec-status", pid=pid)
            if st.get("exited"):
                out = base64.b64decode(st.get("out-data", "")) + base64.b64decode(st.get("err-data", ""))
                return st.get("exitcode", -1), out.decode(errors="replace")
            time.sleep(0.5)
        raise TimeoutError(f"guest command did not finish in {timeout:.0f} s")

    def ps(self, script: str, timeout: float = 60) -> tuple[int, str]:
        script = "$ProgressPreference = 'SilentlyContinue'\n" + script
        enc = base64.b64encode(script.encode("utf-16-le")).decode()
        return self.run(["powershell", "-NoProfile", "-NonInteractive", "-EncodedCommand", enc], timeout)

    def close(self):
        self.s.close()


def agent(name: str) -> Agent:
    if not L.running(L.load_state(name)):
        die(f"the clone '{name}' is not running (winclone.py start)")
    return Agent(name)


# --- commands ----------------------------------------------------------------


def ensure_vm(name: str) -> None:
    d = L.vm_dir(name)
    if (d / "vm.json").exists():
        return
    d.mkdir(parents=True, exist_ok=True)
    (d / "winclone").write_text("disk.qcow2 is a throwaway overlay\n")
    taken = {json.loads((p / "vm.json").read_text())["qmp_port"]
             for p in L.VMS.iterdir() if (p / "vm.json").exists()}
    index = next(i for i in range(100) if L.QMP_PORT_BASE + i not in taken)
    L.save_state(name, {"name": name, "qmp_port": L.QMP_PORT_BASE + index,
                        "vnc_display": L.VNC_DISPLAY_BASE + index, "pid": None})


def logged_on(a: Agent) -> bool:
    code, out = a.ps(f"if (Get-Process explorer -IncludeUserName -ErrorAction SilentlyContinue | "
                     f"Where-Object UserName -like '*\\{USER}') {{ 'yes' }}", timeout=30)
    return code == 0 and "yes" in out


def cmd_start(a) -> None:
    base, efivars = BASE_DIR / "base.qcow2", BASE_DIR / "base-efivars.qcow2"
    if not base.exists() or not efivars.exists():
        die(f"no base image in {BASE_DIR}")
    ensure_vm(a.vm)
    own_clone(a.vm)
    if L.running(L.load_state(a.vm)):
        die(f"'{a.vm}' already runs; stop it first")
    t0 = time.time()
    d = L.vm_dir(a.vm)
    # A fresh overlay every start: whatever the last run changed is gone.
    for f in ("disk.qcow2", "efivars.qcow2"):
        (d / f).unlink(missing_ok=True)
    subprocess.run([str(L.OUR_QEMU / "qemu-img.exe"), "create", "-q", "-f", "qcow2", "-F", "qcow2",
                    "-b", str(base), str(d / "disk.qcow2")], check=True, timeout=60)
    shutil.copyfile(efivars, d / "efivars.qcow2")  # not copy: the base is read-only
    up = [sys.executable, str(Path(L.__file__).resolve()), "up", a.vm, "--window", "--nvme",
          "--no-enter", "--no-cache", "--minutes", str(a.minutes), "--mem", a.mem, "--cpus", str(a.cpus)]
    up += [f"--hostfwd=tcp:127.0.0.1:{h}-:{g}" for g, h in FORWARDS.items()]
    if a.no_gpu:
        up.append("--no-gl")
    if a.qemu_args:
        up.append(f"--qemu-args={a.qemu_args}")
    if a.qemu:  # another QEMU build (tests); Venus has to be asked for then
        up += ["--qemu", a.qemu] + ([] if a.no_gpu else ["--venus"])
    r = subprocess.run(up, timeout=300)
    if r.returncode:
        die("lab.py up failed")
    end = time.time() + a.timeout
    while time.time() < end:
        try:
            ag = Agent(a.vm, timeout=5)
            try:
                if logged_on(ag):
                    break
            finally:
                ag.close()
        except (OSError, ConnectionError, RuntimeError, TimeoutError):
            pass
        if not L.running(L.load_state(a.vm)):
            die("QEMU exited during the boot; see " + str(d / "qemu.log"))
        time.sleep(3)
    else:
        die(f"no logged-on desktop after {a.timeout} s (the clone keeps running; stop it)")
    ag = agent(a.vm)
    ag.ps(f"New-Item -ItemType Directory -Force {GUEST_DIR} | Out-Null; "
          f"icacls {GUEST_DIR} /grant '{USER}:(OI)(CI)M' | Out-Null")
    ag.close()
    print(f"start: '{a.vm}' up with {USER} logged on after {time.time() - t0:.0f} s; "
          f"limit {a.minutes} min; {addr_text()}")


def addr_text() -> str:
    return ", ".join(f"guest TCP {g} = 127.0.0.1:{h}" for g, h in FORWARDS.items())


def cmd_addr(a) -> None:
    print(f"127.0.0.1:{FORWARDS[3390]}")


def cmd_push(a) -> None:
    src = Path(a.local)
    if not src.is_file():
        die(f"{src} is not a file")
    dest = a.guest or rf"C:\remora\{src.name}"
    ag = agent(a.vm)
    parent = dest.rsplit("\\", 1)[0]
    ag.ps(f"New-Item -ItemType Directory -Force '{parent}' | Out-Null")
    h = ag.call("guest-file-open", path=dest, mode="wb")
    data = src.read_bytes()
    try:
        for i in range(0, len(data), 1 << 20):
            ag.call("guest-file-write", handle=h, **{"buf-b64": base64.b64encode(data[i:i + (1 << 20)]).decode()})
    finally:
        ag.call("guest-file-close", handle=h)
    import hashlib
    want = hashlib.sha256(data).hexdigest().upper()
    code, out = ag.ps(f"(Get-FileHash -Algorithm SHA256 '{dest}').Hash")
    ag.close()
    if want not in out:
        die(f"push: checksum mismatch for {dest}")
    print(f"push: {src} -> {dest} ({len(data)} bytes, sha256 ok)")


def read_command(a) -> str:
    cmd = sys.stdin.read().strip() if a.command == ["-"] else " ".join(a.command)
    if not cmd:
        die("no command")
    return cmd


def start_task(ag: Agent, cmd: str, elevated: bool) -> str:
    """Runs cmd in tom's desktop session through a scheduled task; returns its id."""
    rid = time.strftime("%H%M%S") + "-" + secrets.token_hex(2)
    # The command sits in its own batch file, run with `call`: the redirect then
    # covers a whole `a & b` chain, and `exit /b N` still reaches the rc line.
    run = f"@echo off\r\n{cmd}\r\n"
    body = (f"@echo off\r\ncd /d {GUEST_DIR}\r\ncall {GUEST_DIR}\\{rid}.run.cmd > {GUEST_DIR}\\{rid}.out 2>&1\r\n"
            f">{GUEST_DIR}\\{rid}.rc echo %ERRORLEVEL%\r\n")
    # Started through wscript with window style 0: no console window on tom's
    # desktop (it would show in screenshots and screen captures).
    hidden = f'CreateObject("WScript.Shell").Run "cmd.exe /c {GUEST_DIR}\\{rid}.cmd", 0, True\r\n'
    level = "Highest" if elevated else "Limited"
    code, out = ag.ps(f"""
$b = [Convert]::FromBase64String('{base64.b64encode(run.encode()).decode()}')
[IO.File]::WriteAllBytes('{GUEST_DIR}\\{rid}.run.cmd', $b)
$b = [Convert]::FromBase64String('{base64.b64encode(body.encode()).decode()}')
[IO.File]::WriteAllBytes('{GUEST_DIR}\\{rid}.cmd', $b)
$b = [Convert]::FromBase64String('{base64.b64encode(hidden.encode()).decode()}')
[IO.File]::WriteAllBytes('{GUEST_DIR}\\{rid}.vbs', $b)
$a = New-ScheduledTaskAction -Execute 'wscript.exe' -Argument '//B //Nologo {GUEST_DIR}\\{rid}.vbs'
$p = New-ScheduledTaskPrincipal -UserId {USER} -LogonType Interactive -RunLevel {level}
$s = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Hours 12) -AllowStartIfOnBatteries
Register-ScheduledTask -TaskName winclone-{rid} -Action $a -Principal $p -Settings $s -Force | Out-Null
Start-ScheduledTask -TaskName winclone-{rid}
""")
    if code:
        die(f"could not start the task: {out.strip()}")
    return rid


def cmd_exec(a) -> None:
    cmd = read_command(a)
    ag = agent(a.vm)
    if a.system:  # SYSTEM in session 0 (no desktop), straight through the guest agent
        # From a batch file: the guest agent escapes quotes in arguments
        # (\"), which cmd.exe does not understand.
        rid = time.strftime("%H%M%S") + "-" + secrets.token_hex(2)
        bat = rf"{GUEST_DIR}\{rid}.sys.cmd"
        ag.ps(f"New-Item -ItemType Directory -Force {GUEST_DIR} | Out-Null; [IO.File]::WriteAllBytes('{bat}', "
              f"[Convert]::FromBase64String('{base64.b64encode(f'@echo off{chr(13)}{chr(10)}{cmd}{chr(13)}{chr(10)}'.encode()).decode()}'))")
        code, out = ag.run(["cmd.exe", "/c", bat], a.timeout)
        sys.stdout.write(out)
        sys.exit(code)
    rid = start_task(ag, cmd, a.elevated)
    end = time.time() + a.timeout
    rc = None
    while time.time() < end:
        code, out = ag.ps(f"Get-Content -Raw '{GUEST_DIR}\\{rid}.rc' -ErrorAction SilentlyContinue")
        if out.strip():
            rc = int(out.strip())
            break
        time.sleep(1)
    if rc is None:
        ag.ps(f"Stop-ScheduledTask -TaskName winclone-{rid}")
    _, out = ag.ps(f"Get-Content -Raw '{GUEST_DIR}\\{rid}.out' -ErrorAction SilentlyContinue; "
                   f"Unregister-ScheduledTask -TaskName winclone-{rid} -Confirm:$false")
    ag.close()
    sys.stdout.reconfigure(errors="replace")
    sys.stdout.write(out)
    if rc is None:
        die(f"timed out after {a.timeout:.0f} s (task stopped; child processes may survive)", 124)
    sys.exit(rc)


def cmd_exec_background(a) -> None:
    cmd = read_command(a)
    ag = agent(a.vm)
    rid = start_task(ag, cmd, a.elevated)
    ag.close()
    print(rid)  # first line: the id alone, for scripts
    print(f"output: {GUEST_DIR}\\{rid}.out (winclone.py log {rid})")


def cmd_log(a) -> None:
    if not re.fullmatch(r"\d{6}-[0-9a-f]{4}", a.id):
        die(f"'{a.id}' is not a run id (exec-background prints it on its first line)")
    ag = agent(a.vm)
    code, _ = ag.ps(f"if (-not (Test-Path '{GUEST_DIR}\\{a.id}.cmd')) {{ exit 3 }}")
    if code == 3:
        die(f"no run {a.id} in this clone")
    _, out = ag.ps(f"Get-Content -Raw '{GUEST_DIR}\\{a.id}.out' -ErrorAction SilentlyContinue; "
                   f"$rc = Get-Content -Raw '{GUEST_DIR}\\{a.id}.rc' -ErrorAction SilentlyContinue; "
                   f"if ($rc) {{ \"[exited: $($rc.Trim())]\" }} else {{ '[running]' }}")
    ag.close()
    sys.stdout.reconfigure(errors="replace")
    sys.stdout.write(out)


def cmd_stop(a) -> None:
    ensure_vm(a.vm)
    own_clone(a.vm)
    state = L.load_state(a.vm)
    L.quit_vm(a.vm, state)
    if L.running(state):
        die("QEMU did not stop; overlay kept")
    state["pid"] = None
    L.save_state(a.vm, state)
    d = L.vm_dir(a.vm)
    # QEMU can hold the files for a moment after its process ended.
    end = time.time() + 30
    for f in ("disk.qcow2", "efivars.qcow2"):
        while True:
            try:
                (d / f).unlink(missing_ok=True)
                break
            except PermissionError:
                if time.time() >= end:
                    die(f"{d / f} still in use after 30 s; the next start replaces it")
                time.sleep(1)
    print(f"stop: '{a.vm}' stopped, changes discarded")


def own_clone(name: str) -> None:
    """start and stop delete the VM's disk: only for clones this tool made."""
    d = L.vm_dir(name)
    if not (d / "winclone").exists():
        if name != "remora":  # made before the marker existed
            die(f"'{name}' is not a winclone VM; refusing to replace its disk")
        (d / "winclone").write_text("disk.qcow2 is a throwaway overlay\n")


def cmd_status(a) -> None:
    ensure_vm(a.vm)
    s = L.load_state(a.vm)
    if L.running(s):
        print(f"{a.vm}: running pid={s['pid']} {max(0, int((s['deadline'] - time.time()) / 60))} min left; "
              + addr_text())
    else:
        print(f"{a.vm}: stopped")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--vm", default="remora", help="clone name (default remora)")
    sub = p.add_subparsers(required=True)
    s = sub.add_parser("start")
    s.add_argument("--minutes", type=int, default=60, help="hard time limit (default 60)")
    s.add_argument("--mem", default="6G"); s.add_argument("--cpus", type=int, default=4)
    s.add_argument("--no-gpu", action="store_true", help="plain virtio-vga instead of Vulkan/D3D through Venus")
    s.add_argument("--timeout", type=float, default=300, help="wait this long for the desktop")
    s.add_argument("--qemu-args", default="", help="extra QEMU arguments (debugging)")
    s.add_argument("--qemu", metavar="DIR", help="other QEMU build folder (test builds)")
    s.set_defaults(f=cmd_start)
    s = sub.add_parser("push"); s.add_argument("local"); s.add_argument("guest", nargs="?")
    s.set_defaults(f=cmd_push)
    for name, f in (("exec", cmd_exec), ("exec-background", cmd_exec_background)):
        s = sub.add_parser(name)
        s.add_argument("--elevated", action="store_true", help="elevated in tom's session (RunLevel Highest)")
        if name == "exec":
            s.add_argument("--system", action="store_true", help="as SYSTEM in session 0 (no desktop)")
            s.add_argument("--timeout", type=float, default=120)
        s.add_argument("command", nargs=argparse.REMAINDER)
        s.set_defaults(f=f)
    s = sub.add_parser("log"); s.add_argument("id"); s.set_defaults(f=cmd_log)
    for name, f in (("addr", cmd_addr), ("stop", cmd_stop), ("status", cmd_status)):
        sub.add_parser(name).set_defaults(f=f)
    a = p.parse_args()
    try:
        a.f(a)
    except (TimeoutError, ConnectionError) as e:
        die(f"the guest agent stopped answering ({e}); the guest may hang. "
            "Screenshot: lab.py shot <vm>; then stop", 125)


if __name__ == "__main__":
    main()
