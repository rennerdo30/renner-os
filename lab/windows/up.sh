#!/usr/bin/env bash
# Windows lab VM "win" with Vulkan through Venus (our QEMU); Venera setup + virtio-win attached.
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/../.."
W=E:/AgentData/renneros-lab/win
python lab/lab.py up win --window --nvme --no-enter --no-cache --minutes ${MIN:-150} \
  --qemu-args "-drive if=none,id=cd1,media=cdrom,format=raw,readonly=on,file=$W/venera-setup-0.2.3.177.b7d6982.iso -device ide-cd,bus=ide.1,drive=cd1 -drive if=none,id=cd2,media=cdrom,format=raw,readonly=on,file=$W/virtio-win.iso -device ide-cd,bus=ide.2,drive=cd2 ${EXTRA:-}" "$@"
