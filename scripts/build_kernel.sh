#!/bin/bash
K=/Users/lertian/Agentspace/edl-huawei-mrr-w29/work/kernel/linux-sm8250
cd "$K" || { echo "NO_SRC"; exit 1; }
swapon --show 2>/dev/null | grep -q swap || { sudo fallocate -l 4G /swap.img 2>/dev/null; sudo mkswap /swap.img >/dev/null 2>&1; sudo swapon /swap.img 2>/dev/null; }
echo "[$(date +%H:%M:%S)] 开始编译（挂载目录 -j3 ✓ 不会缺文件 ✓）"
make ARCH=arm64 -j3 Image dtbs > /tmp/kb9.log 2>&1
rc=$?
echo "[$(date +%H:%M:%S)] RC=$rc"
if [ $rc -eq 0 ]; then
  cp arch/arm64/boot/Image /Users/lertian/Agentspace/edl-huawei-mrr-w29/work/kernel/Image-mrr-w29
  cp arch/arm64/boot/dts/qcom/sm8250-huawei-mrr-w29.dtb /Users/lertian/Agentspace/edl-huawei-mrr-w29/work/kernel/
  ls -la /Users/lertian/Agentspace/edl-huawei-mrr-w29/work/kernel/Image-mrr-w29 \
         /Users/lertian/Agentspace/edl-huawei-mrr-w29/work/kernel/sm8250-huawei-mrr-w29.dtb
  echo "DONE_OK 产物已回传宿主"
else
  echo "FAILED"; tail -6 /tmp/kb9.log
fi
