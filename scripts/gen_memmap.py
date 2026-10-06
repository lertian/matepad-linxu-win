#!/usr/bin/env python3
"""从华为 XBL 自带的 uefiplat.cfg 生成 edk2 的 PlatformMemoryMapLib.c（mrr-w29 专用）。

原理：xbl_uefiplat.cfg 的 [MemoryMap]/[RegisterMap] 列顺序与
ARM_MEMORY_REGION_DESCRIPTOR_EX 的字段顺序完全一致，可直接 1:1 转换：
    {MemBase, MemSize, MemLabel, BuildHob, ResourceType, ResourceAttribute,
     MemoryType, CacheAttributes}
    {name,    addr,    size,    HobOption, ResourceType, ResourceAttribute,
     MemoryType, ArmAttributes}

在 ABL 自有保留区之外，补上：
  * Hypervisor 预留（[Config] UnusableDDRMemory*，与 j716f 同名同属性）
  * 我们自己的 UEFI FD 落地区（FD_BASE/FD_SIZE，来自 configs/sm8250.conf）
  * 真实内存 bank 减去上述保留区后剩下的可可用 RAM（Conv / WRITE_BACK_XN）
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

CFG = Path("backups/analysis/xbl_uefiplat.cfg")
OUT = Path("work/edk2-msm/Platform/Huawei/sm8250/Library/mrr-w29/PlatformMemoryMapLib/PlatformMemoryMapLib.c")

# 设备真实内存 bank（research/device/mrr-w29-hardware.md，来自 /proc/iomem 实测）
BANKS = [
    (0x80000000, 0x3BB00000),   # 955 MiB
    (0xC0000000, 0xC0000000),   # 3072 MiB
    (0x180000000, 0x100000000),  # 4096 MiB
]

# 我们自己的 UEFI 载荷落地区（configs/sm8250.conf: FD_BASE/FD_SIZE）
FD_BASE, FD_SIZE = 0xCE000000, 0x00700000

LINE = re.compile(
    r"^\s*(0x[0-9A-Fa-f]+)\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*\"([^\"]*)\"\s*,\s*"
    r"(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*$"
)

PROTO = ("Name", "Address", "Length", "HobOption", "ResourceType",
         "ResourceAttribute", "MemoryType", "ArmAttributes")


def parse_cfg(path: Path):
    """返回 ([MemoryMap 条目], [RegisterMap 条目])，均为 8 元组。"""
    mem, reg = [], []
    section = None
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line.startswith("[") and line.endswith("]"):
            section = line.strip("[]")
            continue
        if not line or line.startswith("#"):
            continue
        m = LINE.match(line)
        if not m:
            continue
        addr, size, label = int(m.group(1), 16), int(m.group(2), 16), m.group(3)
        rest = m.group(4, 5, 6, 7, 8)
        entry = (label, addr, size, *rest)
        if section == "MemoryMap":
            mem.append(entry)
        elif section == "RegisterMap":
            reg.append(entry)
    return mem, reg


def subtract(ranges, cut):
    """在 ranges 中挖掉 cut（单个区间），返回剩余区间列表。"""
    out = []
    c_start, c_end = cut
    for start, size in ranges:
        end = start + size
        if c_end <= start or c_start >= end:      # 无交集
            out.append((start, size))
            continue
        if c_start > start:                       # 左残段
            out.append((start, c_start - start))
        if c_end < end:                           # 右残段
            out.append((c_end, end - c_end))
    return out


def main() -> int:
    if not CFG.exists():
        print(f"找不到 {CFG}", file=sys.stderr)
        return 1
    mem, reg = parse_cfg(CFG)
    print(f"[cfg] MemoryMap {len(mem)} 条, RegisterMap {len(reg)} 条")

    # ABL 自身环境的区域改名，避免与我们自己的 UEFI FD 混淆；
    # "Kernel" 是 ABL 装载 BootShim 的位置，BootShim 拷完 FD 即跳走 → 可转为可用内存
    RENAME = {
        "UEFI FD": "XBL UEFI FD",
        "DXE Heap": "XBL DXE Heap",
        "UEFI Stack": "XBL UEFI Stack",
        "Kernel": "HLOS (ex-Kernel)",
    }
    entries = []
    for e in mem:
        name, addr, size, hob, rt, ra, mt, ca = e
        if name == "Kernel":
            mt = "Conv"           # BootShim 拷贝完 FD 后该区即空闲
        entries.append((RENAME.get(name, name), addr, size, hob, rt, ra, mt, ca))

    # 1) Hypervisor 预留（cfg 里以 Config 参数描述，j716f 用同样属性显式列出）
    hyper = ("Hypervisor", 0x80000000, 0x00600000,
             "AddMem", "SYS_MEM", "SYS_MEM_CAP", "Reserv", "NS_DEVICE")
    entries.append(hyper)

    # 2) 我们自己的 UEFI FD 落地区
    entries.append(("UEFI FD (edk2)",
                    FD_BASE, FD_SIZE,
                    "AddMem", "SYS_MEM", "SYS_MEM_CAP", "BsData", "WRITE_BACK"))

    # 3) 可用 RAM = bank − 所有 DDR 保留区
    reserved = [(e[1], e[2]) for e in entries if e[1] < 0x100000000]
    usable = list(BANKS)
    for r in reserved:
        usable = subtract(usable, (r[0], r[0] + r[1]))
    usable = [u for u in usable if u[1] > 0]
    for i, (start, size) in enumerate(usable, 1):
        entries.append((f"HLOS {i}", start, size,
                        "AddMem", "SYS_MEM", "SYS_MEM_CAP", "Conv", "WRITE_BACK_XN"))
    print(f"[gen] 可用 RAM 段 {len(usable)} 个, 合计 {sum(s for _, s in usable) / 1024**2:.0f} MiB")

    # 4) 排序 + 重叠自检
    # 规范化：丢弃完全被其它条目包含的条目（源数据里 IPC SHM 就位于 Hypervisor 区内；
    # 上游 j716f 亦不列出该条），并保证结果无重叠——edk2 的 HOB 构造不接受重叠描述符
    def contained(a, b):
        if a is b:
            return False
        return b[1] <= a[1] and a[1] + a[2] <= b[1] + b[2]

    dropped = [e for e in entries if any(contained(e, o) for o in entries)]
    for e in dropped:
        print(f"[规范化] 丢弃被包含条目: {e[0]} @0x{e[1]:X}+0x{e[2]:X}")
    entries = [e for e in entries if e not in dropped]

    entries.sort(key=lambda e: e[1])
    prev_end, prev_name = None, None
    for e in entries:
        if prev_end is not None and e[1] < prev_end:
            raise SystemExit(
                f"[致命] {e[0]} @0x{e[1]:X} 与 {prev_name} 重叠（prev_end=0x{prev_end:X}）"
            )
        prev_end, prev_name = e[1] + e[2], e[0]

    # 5) 生成 C 代码
    lines = [
        "/** @file",
        "  PlatformMemoryMapLib for Huawei MRR-W29 (MatePad Pro 10.8 2021, SM8250-AC).",
        "",
        "  本文件由本项目 gen_memmap.py 从设备 XBL 自带 uefiplat.cfg 机械生成，请勿手改。",
        "  来源: backups/analysis/xbl_uefiplat.cfg（提取自 backups/partitions/xbl.bin @ 0xbf65c）",
        "  内存 bank: 0x80000000+955MiB / 0xC0000000+3072MiB / 0x180000000+4096MiB",
        "",
        "  Copyright (c) DuoWoA authors. All rights reserved.",
        "  Copyright (c) Renegade Project. All rights reserved.",
        "  SPDX-License-Identifier: BSD-2-Clause-Patent",
        "**/",
        "",
        "#include <Library/BaseLib.h>",
        "#include <Library/PlatformMemoryMapLib.h>",
        "",
        "static ARM_MEMORY_REGION_DESCRIPTOR_EX gDeviceMemoryDescriptorEx[] = {",
        "  /* DDR：华为 ABL 自报内存图（逐条取自 uefiplat.cfg） */",
    ]
    for name, addr, size, h, rt, ra, mt, ca in entries:
        lines.append(
            f'  {{"{name}", 0x{addr:X}, 0x{size:X}, {h}, {rt}, {ra}, {mt}, {ca}}},'
        )
    lines += [
        "",
        "  /* 寄存器/IO 区：取自 uefiplat.cfg [RegisterMap] */",
    ]
    for name, addr, size, hob, rt, ra, mt, ca in reg:
        lines.append(
            f'  {{"{name}", 0x{addr:X}, 0x{size:X}, {hob}, {rt}, {ra}, {mt}, {ca}}},'
        )
    lines += [
        "",
        '  {"Terminator", 0, 0, 0, 0, 0, 0, 0}',
        "};",
        "",
        "ARM_MEMORY_REGION_DESCRIPTOR_EX *",
        "GetPlatformMemoryMap ()",
        "{",
        "  return gDeviceMemoryDescriptorEx;",
        "}",
        "",
    ]
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"[out] {OUT} ({OUT.stat().st_size} B, {len(entries)} DDR + {len(reg)} REG)")

    # 6) 人可读摘要
    print("\n  DDR 映射（升序）:")
    for name, addr, size, _hob, _rt, _ra, mt, ca in entries:
        print(f"    0x{addr:09X} +0x{size:08X}  {name:<22} {mt:<7} {ca}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
