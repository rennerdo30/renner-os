# Test lab (QEMU on this PC)

Owner direction (2026-11-07): test VMs run with **QEMU directly on this
Windows PC**. They never run inside the owner's WSL distros or other VMs.

- **QEMU 11.1.0** (official Windows build, `SoftwareFreedomConservancy.QEMU`,
  installer SHA256 checked against the winget manifest). It is unpacked, not
  installed, into `E:/AgentData/renneros-lab/qemu`, so there's no admin
  install and no PATH change.
- **Acceleration:** WHPX (Windows Hypervisor Platform).
- **Firmware:** UEFI from QEMU's edk2 build. The variable store is per VM, in
  qcow2 so snapshots work.
- **Lab data** lives outside the repo, in `E:/AgentData/renneros-lab/`
  (`qemu/`, `isos/`, `vms/<name>/` with disk, logs and screenshots). Override
  it with `RENNER_LAB_DIR`.

## Rules the tool enforces

- Headless (`-display none`); VNC only on `127.0.0.1`. A window (`--window`)
  only when the owner asks.
- Every VM has a time limit (`--minutes`, default 30). A detached watchdog
  quits it when the limit is reached.
- The PIDs of QEMU and the watchdog are recorded in `vms/<name>/vm.json`.
  `down` and the watchdog only stop that PID, and only after checking that it
  is still a `qemu-system` process.
- Defaults: 4 vCPUs, 8 GB RAM. That's light load next to everything else
  running on the PC.

## Usage

```
python lab/lab.py new test1 --size 64G
python lab/lab.py up test1 --iso E:/AgentData/renneros-lab/isos/<iso> --minutes 30
python lab/lab.py shot test1                 # screenshot (PNG), prints its path
python lab/lab.py key test1 ret ctrl+alt+f2  # key presses (QEMU qcodes)
python lab/lab.py type test1 "text" --enter  # type text (US layout)
python lab/lab.py snap test1 save clean      # disk snapshot: save / load / delete / list
                                             # (VM must be stopped; WHPX can't save live state)
python lab/lab.py status
python lab/lab.py down test1
```

To watch along: connect any VNC viewer to the address `status` prints.

## Limits

- No real GPU: a VM tests boot, A/B updates, rollback, KDE and the exe
  integration, but not NVIDIA/AMD/Intel drivers. Those need real hardware.
- No TPM yet (swtpm has no Windows build); Secure Boot comes later with the
  `edk2-x86_64-secure-code.fd` firmware.
