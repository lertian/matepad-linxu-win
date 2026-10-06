# MatePad Pro 10.8" 2021 (MRR-W29) — UEFI + Linux → Windows ARM64

> 本项目把华为 MatePad Pro 10.8" 2021（MRR-W29 / HWMRR-Q / 骁龙 870 / SM8250-AC）
> 改造成可以跑自定义 UEFI、Linux，最终目标是 **Windows ARM64**。
> **Android 与原 61GB 数据、Magisk root 始终完好。**

## 当前状态（2026-10-06）

| 里程碑 | 状态 |
|---|---|
| EDL 访问 + 解锁 + Magisk root | ✅ |
| **edk2 UEFI 移植**（显示 / 按键 / 菜单 / 日志） | ✅ 真机验证 |
| **ExitBootServices 修复**（内存映射 64KB 对齐） | ✅ 真机验证 |
| **Linux 启动到 shell**（Alpine virt 内核 + initrd） | ✅ 真机验证 |
| **半自动迭代闭环**（adb 刷入 / 自动写回 / 读日志） | ✅ 真机验证 |
| 主线内核（pmOS sm8250 fork + 自制 DTS） | 🔄 正在编译 |

## 从新电脑继续（最短路径）

```bash
# 0. 工具
brew install python3 gh              # macOS；Linux 用 apt
gh auth login
# Android 平台工具 + adb

# 1. 内核源码（postmarketOS 维护的 SM8250 主线 fork，v7.2.0）
git clone --depth=1 --single-branch https://gitlab.postmarketos.org/soc/qualcomm-sm8250/linux.git
# ⚠️ 克隆**绝不能打断** ✗——被打断会导致工作区 94924 个文件没检出
#    修复：rm -f .git/index.lock && git reset --hard HEAD

# 2. 放入我们的设备树
cp dts/sm8250-huawei-mrr-w29*.dts* linux/arch/arm64/boot/dts/qcom/
echo 'dtb-$(CONFIG_ARCH_QCOM) += sm8250-huawei-mrr-w29.dtb' >> linux/arch/arm64/boot/dts/qcom/Makefile

# 3. 编译（Linux/aarch64 原生最省事；macOS 走虚拟机）
./scripts/config --disable CONFIG_KVM       # 本平台 arch/arm64/kvm/hyp 编不过，且不需要
make ARCH=arm64 olddefconfig
make ARCH=arm64 -j$(nproc) Image dtbs
# 产物：arch/arm64/boot/Image + arch/arm64/boot/dts/qcom/sm8250-huawei-mrr-w29.dtb
```

## 设备事实（必读）

- **EDL**：USB VID/PID `05c6:9008`，`Auth=None`，programmer `HuaweiCommon_865870_devprg`
- **分区（华为 GPT 无 `boot`，是 `kernel`；Android boot 在 LUN4）**
  | 分区 | 位置 | 我们放了什么 |
  |---|---|---|
  | `patch`（256MB, LBA 0x2E200）| LUN0 | **ESP(FAT, 0-32MB)** + **STOCKBOOT blob(+32MB, 24MB)** + fb dump(+128MB) + 诊断块(+0x8FC0000) |
  | `reserved1`（8MB, sector 10248）| LUN0 | `simpleinit.uefi.cfg` + `simpleinit.log` |
  | `cache` | LUN0 | ESP 副本（**Android 每次启动会擦掉** ✗，不用了）|
  | `boot`（sector 67334）| **LUN4** | 平时=原厂；测试时=我们的 payload |
- **boot 分区尺寸限制**：`kernel_size ≤ 6,110,000`（ABL 才接受）
- **ESP 配置**：SimpleInit 必须用**扁平点号键**（`a.b.c = value` 每行一个 ✓）
  块语法与一行多键**会被静默忽略** ✗
- **`/sdcard` 在 Magisk root 上下文不可见** ✗ ⇒ `su -c dd` 的源/目标必须用 `/data/local/tmp/` ✓
- **读日志全自动**（不需要 EDL ✗）：
  `adb shell su -c 'dd if=/dev/block/by-name/reserved1 of=/data/local/tmp/r.bin bs=4096 count=2040'` + `adb pull`

## 自动化闭环（已验证 ✓）

```
adb push payload → su dd of=/dev/block/by-name/boot      # 0.03s
adb reboot
  → payload 启动 → SimpleInit → AlterLinux 内核
  → AutoReturnDxe 在入口点把【原厂 boot】写回 boot 分区
长按电源 10-20 秒 → 重启 → ★ Android ★
adb + su 读 reserved1 日志 → 分析 → 下一轮
```

## 文件说明

- `docs/uefi-port-notes.md` — **全部笔记**（§1-21）。包含每个坑与根因，**接手必读**
- `dts/` — 为 MRR-W29 新建的设备树（派生自小米 Pad 6 `pipa`，同为 SD870 平板 ✓）
  其 `simple-framebuffer` 地址 `0x9c000000` 与本机内核实测地址一致 ✓
- `edk2-src/` — 关键源码：
  - `AutoReturnDxe.c/.inf` — 自动回退驱动（**踩坑：`AUTO_RETURN_STOCK_MARKER_BYTES` 必须=9**，`"STOCKBOOT"` 是 9 字符，写成 8 会让 blob 校验永远失败 ✗）
  - `MrrButtonsDxe.c` — 按键驱动（vol_up/down + 电源键映射为 SUSPEND + 回车 ✓）
  - `PlatformMemoryMapLib.c` — **EBS 根因修复**：runtime 条目必须 64KB 对齐
- `scripts/build_kernel.sh` — 目标机上的编译脚本
- `scripts/gen_memmap.py` — 生成 `PlatformMemoryMapLib.c`（⚠️ 手改会丢失，需同步）

## 下一步

1. 内核编好 → **ESP 扩到 64MB**（主线 Image 约 25-35MB ✗ 装不下 32MB ✗）
   并重排 patch 分区 + 同步改 `AutoReturnDxe` 的 `AUTO_RETURN_STOCK_OFFSET`
2. 首版**只用 `simple-framebuffer`**（不碰面板驱动）⇒ 屏幕应直接亮 ✓
3. 目标 **M1**：内核打印 sm8250 早期信息 → **M2**：**UFS 起来**
   （⇒ 内核自己写日志 ⇒ 迭代彻底零人工）
4. 之后再转 **Windows ARM64**：ACPI/DSDT（可照抄 `Platform/Lenovo/sm8250/AcpiTables/j716f/`）
   —— 见笔记 §21
