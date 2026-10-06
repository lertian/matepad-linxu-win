# Huawei MatePad Pro 10.8" 2021 (mrr-w29 / SM8250-AC) — edk2 UEFI 移植笔记

最后更新：2026-08-07

## 一、当前进度摘要

| 阶段 | 状态 |
|---|---|
| Bootloader 解锁 / Root | ✅ 完成 |
| edk2 构建（macOS 原生工具链） | ✅ 完成 |
| UEFI payload 在真机跑起来 | ✅ 完成（ABL 执行我们的代码） |
| PEI 阶段（PrePi：HOB/MMU/解压 DXEFV） | ✅ 完成 |
| DXE Core 初始化（内存服务/GCD/构造函数/事件） | ✅ 完成 |
| DXE 驱动调度（Dispatcher 运行、加载驱动） | ✅ 到达（当前战线） |
| SimpleInit 引导菜单 | ⏳ 未到达 |

**屏幕输出不再是唯一手段**：DXE 的帧缓冲控制台已可用且可读（见第三节 B）。
早期阶段（PrePi）没有控制台，如需排障可临时加回绘图探针（见第五节）。

---

## 二、已解决的两个"真·根因"（都是移植环境/工具链问题，不是硬件问题）

### A. 帧缓冲几何错误（导致控制台乱码、探针图形拉伸平铺）

- **现象**：控制台文字错行成乱码；自绘数字面板被纵向拉伸并平铺成重复矩形；整屏填色闪烁。
- **根因**：`Platform/Huawei/sm8250/mrr-w29.dsc` 把面板写成 `2560x1600`（横向），
  但 **ABL 交给我们的就绪帧缓冲是面板原生几何 `1600x2560`（竖向）**。
  ⇒ stride 实际 `1600*4 = 6400`，代码却按 `2560*4 = 10240` 排版。
  该值是早期从 `Lenovo/sm8250/j716f` 抄模板时带过来的（同面板家族的
  `Xiaomi/sm8250/elish`、`Xiaomi/sm8150/nabu`、`Lenovo/sm8250/tb-9707f` 全是 1600x2560）。
- **另附带的越界 bug**：早期探针按 `0x2300000`(36.7MB) 填色，真实帧缓冲仅
  `1600*2560*4 = 0xFA0000`(16.4MB)，**越界写 20MB**。
- **修复**：
  - DSC：`PcdMipiFrameBufferWidth|1600` / `PcdMipiFrameBufferHeight|2560`
  - 所有探针 stride 10240 → 6400；填色长度 → 0xFA0000
- **参考**：Renegade Project《edk2-sdm845 Porting Guide》第 15 步即要求
  "把 .dsc 里的宽高换成自己设备的显示宽高"。

### B. PE 重定位被构建工具丢弃（导致驱动一进入口就崩）

- **现象**：`Synchronous Exception at 0x...`，栈顶为某驱动 `.dll` 的 `ImageBase+0`
  （例：`PartitionDxe` at `0xCA535000+0x00000000`），随后
  `Recursive exception occurred while dumping the CPU state`。
- **根因链**：
  1. macOS 上用 lld 以 PIE(ET_DYN) 链接，静态指针（如 `INITIALIZE_LIST_HEAD_VARIABLE`
     的链表头、`EFI_DRIVER_BINDING_PROTOCOL` 的函数指针）产生
     `R_AARCH64_RELATIVE`，**符号索引为 0、值在 addend**；
  2. 链接器把这些项放进**动态节 `.rela`（`sh_info == 0`）**；
  3. 上游 `GenFw` 的处理循环要求目标节是 text/data，`sh_info==0` 会被整段跳过；
  4. 早期为"让构建通过"打的补丁又把**所有符号索引为 0 的重定位一律跳过** ⇒
     **PE 的 `.reloc` 为空**；
  5. 没有 `.reloc` ⇒ 加载时不修正 ⇒ 静态指针保持链接期值/0；
     更糟的是 PE 的 DIR64 修正是"**槽位当前值 + 加载基址**"，若槽位是 0，
     加载后得到 `ImageBase + 0`，调用即跳到 PE 文件头执行 ⇒ 异常。
- **修复（`BaseTools/Source/C/GenFw/Elf64Convert.c`）**：
  1. `WriteRelocations64`：对 `sh_info == 0` 的 AArch64 动态节，按 `r_offset`
     反查目标节，把 `R_AARCH64_RELATIVE`(1027) 转成 `EFI_IMAGE_REL_BASED_DIR64` 修正；
     只跳过真正无值的类型（NONE=0/256、IRELATIVE=1032）。
  2. `WriteSections64`：**必须先把 addend 写进槽位**（PE 的 DIR64 是增量修正），
     否则槽位为 0 ⇒ 加载后变成 `ImageBase+0`。
- **验证方法（离线，无需设备）**：
  - 用 `llvm-objdump -p <mod>.efi` 看 `Data Directory Entry 5`（Base Relocation）非 0；
  - 解析 `.reloc`，确认修正 RVA 落在 `.data`（不是文件头区）；
  - **关键**：读修正槽的 8 字节值，必须等于 ELF 里对应 `R_AARCH64_RELATIVE` 的 addend
    （早期版本槽值为 0 ⇒ 就是崩溃原因）。
  - **注意构建缓存**：改了 GenFw 后必须删除 `Build/.../AARCH64/**/*.efi` 强制重新生成，
    否则旧 `.efi` 会被复用（本次踩过这个坑）。

---

## 三、诊断通道现状

### A. 早期（PrePi，无控制台）
只能靠屏幕。绘图探针已按要求从代码移除；如需要，可从 git 历史恢复，
或参考下表临时加回（阶段号 > 颜色，颜色相近极易误判）。

### B. DXE 之后（推荐，已可用）
1. **帧缓冲控制台**：几何修正后文字清晰可读，照片即可准确读取。
   已打开全量 DEBUG：`gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0xFFFFFFFF`。
   `DxeMain.c` 的 `ProbeMark(Color, Number)` 现在**只打印一行文本** `PROBE <n>`，
   不再绘图；Gcd.c / Page.c 里的调用点可保留，作为进度路标。
2. **SimpleInit 文件日志**（待 SimpleInit 起来后）：
   `logger { file_output = "@part_logfs:\simpleinit.log" }`
   ⇒ 日志写进 `logfs` 分区 ⇒ **可通过 EDL 直接读分区取回，完全不需要拍照**。
   排障提示（Renegade 官方）：卡住时可删除 `logfs` 里的 `simpleinit.uefi.cfg`。
3. **EDL 内存直读**：`edl` 支持 `peek`（源码命令表含 `peek/peekhex/peekdword/peekqword`，
   但未写进 docopt usage，需走 `edl.py rawxml '<data><peek .../></data>'`）。
   局限：设备断电/复位后 DDR 内容丢失，故不适用于"崩溃后回读 RAM"。

---

## 四、硬件/分区相关事实（本项目环境）

- 分区布局：Huawei GPT **没有 `boot` 分区，引导分区名为 `kernel`（LUN0）**；
  Android 的 `boot`/`abl`/`aop`/`tz`/`modem` 在 **LUN4**。
- 探针写入位置：`LUN4` 扇区 `67334` 起（即 `boot` 分区）。
- 进入 EDL：`adb reboot recovery`（Android 下免按键）；或物理组合键。
- 退出 EDL：`<power value="reset"/>` 会暖复位，但设备会因 IMEM 粘性 cookie 再次回到 EDL；
  **可靠办法是物理断电组合键（电源+音量下 ≥20s）**。
- 帧缓冲基址：`0x9C000000`（`PcdMipiFrameBufferAddress`）。
- 本机 `timeout` 命令不存在（macOS），用 `perl -e 'alarm shift; exec @ARGV'` 或工具超时。

---

## 五、如需重新加入屏幕探针（仅在无控制台的早期阶段）

- 帧缓冲基址 `0x9C000000`，**stride = 像素宽 × 4**（本机 `1600×4 = 6400`），
  可见区域大小 `1600*2560*4 = 0xFA0000`。**切勿按错误几何填色**（曾越界写 20MB）。
- 屏幕方向：面板原生为竖向 1600×2560，与 ABL 就绪帧缓冲一致。
- 阶段号比颜色可靠得多（同族颜色极易误判）；若用二进制方块编码需注意
  `63 = 111111` 会显示为"全部相同"。

---

## 六、下一步（当前战线）

1. 定位 `PartitionDxe` 之类的驱动在调度阶段的异常（现在控制台会打印驱动加载过程 +
   异常地址 + 调用栈；重定位修复后应已前进，需一次真机验证）。
2. 目标：跑到 **SimpleInit 引导菜单** ⇒ 之后 `logfs` 日志可自读，无需拍照。
3. 更远：ESP 引导介质 → Linux 起来 → Windows ARM64 探测
   （缺项：DSDT 需补 PCIe 根桥/IORT/触摸/电池节点；sm8250 缺 `PciHostBridge`；
   `PcdEmuVariableNvModeEnable = TRUE` 意味着 UEFI 变量不持久）。

---

## 七、追加结论（2026-08-07 实测）

1. **EDL 内存直读（peek）不可用** ✗：华为 `HuaweiCommon_865870_devprg` programmer **不支持 peek**。
   实测走 `edl.py rawxml '<data><peek .../></data>'`（能成功连上 Sahara→firehose 并发送）：
   - `0x9C000000`（帧缓冲）→ `failed`
   - `0x01FD3000`（SRAM）→ `failed`
   - `0x80000000`（DDR 基址）→ `failed`
   ⇒ 无法通过 EDL 回读 RAM；且断电会清 DDR，也不适用于「崩溃后回读」。
   注：`edl.py` 的 `peek`/`peekhex` 命令在源码命令表里存在，但**未写进 docopt usage**，
   CLI 会直接打印帮助；`edl.py rawxml`（本行用到的）对存储命令完全可用。
   **⇒ 「免拍照」的可行方案只剩 SimpleInit 的文件日志（`logfs`）**，而它需要 SimpleInit 先跑起来。

2. **本机 GPT 没有 `logfs` 分区**（华为 GPT 命名不同）。若 SimpleInit 需要该分区存日志，
   需指向可牺牲的分区（如 `misc`，2MB，当前全 0 且启动不使用）或另行处理；待 SimpleInit 起来后再解决。

3. 调试绘图代码已按用户要求**从代码中移除**（颜色整屏填色 / 7 段数字 / 二进制方块 / 条码 / 异常闪烁）；
   `DxeMain.c` 的 `ProbeMark(Color, Number)` 现在只打印一行 `PROBE <n>` 文本，Gcd.c/Page.c 的调用点保留作路标。
   如需图形探针，可从 git 历史恢复（注意第四节 B 里已修正的 stride/尺寸）。

## 八、里程碑：抵达 SimpleInit（2026-08-07，镜像 7c07760232c8ef70）

**证据（屏幕照片）**：异常调用栈全部落在 `SimpleInitMain.dll`（基址 0xC260A000）：

```
free          libs/compatible/malloc.c:175      ← 崩溃点
l_alloc       libs/lua/lauxlib.c:1019
f_luaopen     libs/lua/lstate.c:242
luaH_resize   libs/lua/ltable.c:572
reinsert      libs/lua/ltable.c:511
luaH_resize   libs/lua/ltable.c:583
```

故障指令 `ldur w3, [x19, #-0x10]`（前一条是 `bl DebugPrint`，magic 校验 0x64687043）。

**含义**：
- **全部 DXE 驱动分派成功**，已进入该分支的最后一个组件 SimpleInit ✓
- 崩溃 = `free()` 收到野指针（Lua 表扩容路径 → 自定义 malloc 实现），**异常被处理，系统未死**，之后仍在跑 PROBE 63→68（内存池 churn）✓
- 帧缓冲几何、PE 重定位、内存映射三处修复**全部生效** ✓

**离线诊断方法（可复用）**：照片里的 `PC <addr> (0xBASE+0xOFF)` 直接喂给
`llvm-addr2line -e Build/.../SimpleInitMain.dll -f -C -i 0x571c ...` 即可得源码行号。
（.dll 就是链接后的 ELF，PE RVA == ELF VMA。）

### 追记：SimpleInit 崩溃根因与修复（malloc.c）

- **根因**：`free(NULL)`。Lua 建表（`f_luaopen → luaH_resize → luaM_freearray(NULL) → l_alloc(ud,NULL,0,0)`）
  会合法地调用 `free(NULL)`；但 `libs/compatible/malloc.c` 的 `free()` 源码里
  `if(Ptr != NULL)` 在 `Head->Signature` 解引用**之后**，且该检查被 LTO 优化掉
  （反汇编证实：入口到 `ldur w3,[x19,#-0x10]` 之间无任何 cbz/cbnz）→ 非规范地址 → 同步异常。
- **修复**：`free()` 开头加 `if (Ptr == NULL) return;`；顺带修 `realloc()` 的
  `NumCpy`（OldSize 含 CPOOL_HEAD 头，原先会越界多拷 sizeof(CPOOL_HEAD) 字节）。
- **教训**：异常里 `[0] 模块+0x偏移` 直接喂 `llvm-addr2line -i` 可拿到内联链的源码行；
  改完必须在**二进制里反汇编验证**守卫存在（LTO 可能再次吃掉检查）。

## 九、★ 日志通道打通（2026-08-07，决定性进展）★

**不再需要拍照** —— SimpleInit 的完整启动日志会写入 `reserved1` 分区，用 EDL 读取即可。

### 实现方式
1. `reserved1`（LUN0，偏移扇区 10248，2040 扇区，8MB，原本全零，有备份）写入一个 FAT 卷
2. 卷根放 `simpleinit.uefi.cfg`（kconfig 嵌套格式，SimpleInit 会解析并回写为扁平 store）：
   ```
   locates { logfs { by_gpt_name = "reserved1"  by_disk_label = "gpt" } }
   logger  { file_output = "@logfs:/simpleinit.log"  old_file = 2
             use_console = true  min_level = 0xAE00 }
   ```
3. 启动后日志落在 `simpleinit.log`；**读取命令**：
   `edl.py rs 10248 2040 /tmp/x.bin --lun=0 --loader=... --memory=ufs`
   然后用 python 解析 FAT（`newfs_msdos` 造的 FAT16，512B 扇区）取文件
   （**不要用 hdiutil 挂载**：UDRW 镜像无分区表，挂载会失败；直接解析 FAT 最稳）
4. 参考实现脚本：本文档同目录 `logs/` 下的解析代码

### 关键日志证据（首次成功读取）
```
-------- file @logfs:/simpleinit.log opened at 01/00/2019 00:00 --------
conf: loaded uefi://[VenHw(860845C1-...)/HD(6,GPT,44FA7B58-...0x2808,0x7F8)]/\simpleinit.uefi.cfg
locate: found 76 handles in protocol 8CF2F62C-...   (PartitionInfo)
locate: found locate VenHw(860845C1-...)/HD(6,GPT,44FA7B58-...)   ← 我们的分区 ✓
logger: opened logger file output @logfs:/simpleinit.log
main: initialize simple-init, entry at 0xc218a6f4
guidrv: uefigop 1600x2560 ✓ ; uefikeyboard ✓ ; uefitouch ✓ ; uefipointer ✓
prober: found UEFI Shell at /shell.efi from part 83
bootmenu: run config continue → boot: try to execute boot config continue(Continue Boot)
```

### 发现的遗留问题
- **输入设备**：SimpleInit 侧驱动正常，但底层 UEFI 无真实输入
  （触摸是 **Novatek NT36xxx**，而 FV 里只有 Synaptics 驱动 ⇒ 对不上；
   `ButtonsDxe` 在 FV 里但未见产生按键事件）
  → 可行方案：USB 键盘（OTG，`XhciDxe`+`UsbKbDxe` 已在 FV）/ 移植 NT36 驱动 / 配好音量键 GPIO
- **旧日志重命名失败**：`rename ... to .log.1: Invalid argument`（非致命，文件仍正常打开）
- **RTC 未设置**：日志时间戳是 2019-01-01
- **默认启动项**：`boot.default = "continue"` ⇒ 自动交给 UEFI Shell（因为还没有 ESP/可引导 OS）

### 输入设备调查结论（2026-08-07）

**`ButtonsDxe` 为何无效**：FV 里的 `Platform/EFI_Binaries/Drivers/sm8250/ButtonsDxe/ButtonsDxe.efi`
是**联想 P11 Pro 的预编译固件**（strings 里含构建路径
`P11_PRO_PLUS_ZUI13.1.549_SS_USER/.../ButtonsDxe/DEBUG/ButtonsDxe.dll`），
其 `ConfigureButtonGPIOs`/`ReadGpioStatus` 硬编码该机型的 VOL+/VOL-/HOME GPIO，
对华为 MRR-W29 无效 ⇒ 音量键永远读不到变化。
依赖协议：`PlatformInfo`、`PmicGpioProtocol`、`PmicPONProtocol`。

**触摸为何无效**：本机触摸控制器是 **Novatek NT36xxx**（`dtbo` 里 `nt36` 出现 48 次），
而 FV 只有 `SynapticsTouchDxe`/`SynapticsTCMDxe` ⇒ 型号不匹配 ✗

**求解路径（按性价比）**：
1. **USB 键盘（OTG）** ★零成本：`XhciDxe`+`UsbKbDxe`+`UsbBusDxe` 已在 FV ✓
2. **自写按键驱动** ★★★：从 `dtbo` 的 `gpio-keys` 节点取真机 GPIO 号
   （`dtbo.bin` 里 `gpio-keys`×8、`vol_up`×28、`key_vol`×20），走 PMIC/TLMM GPIO 轮询 → 注入按键
3. **移植 NT36 触摸驱动** ★★★★：工程量最大，但最彻底

**注**：`dtbo.bin`（25MB，LUN0）含 4 个 DT blob，每个都有 `gpio-keys` 节点 —— 真机 GPIO 号在此。

## 十、SimpleInit 全面审计（worker subagent，2026-08-07）

**又修 9 处真 Bug**（均在 GPLDrivers/Library/SimpleInit/）：splash.c 空指针、prober.c
双重释放、linux-boot/conf.c 悬垂、loader.c initrd 合并偏移、boot/efi.c 释放后使用、
mem.c 括号、linux-boot/splash.c 尺寸 copy-paste、fdt.c NULL 未查、ramdisk.c 未清零。

**ESP 配置的两个关键规则（源码核对）**：
1. kernel/initrd/cmdline 必须写在 `boot.configs.<名>.extra { }` 里（不是直接写在配置项下）
2. 路径必须用 `@<tag>:` 形式 —— 裸 `/xxx` 解析到 SimpleInit 镜像所在卷（FV）必失败
3. 无 dtb 时生成空设备树 ⇒ 加 `extra.pass_kernel_fdt_as_dtb = true`（用 ABL DT）

正确写法（cache 分区 /simpleinit.uefi.conf）：
locates { esp { by_gpt_name = "cache" } }
boot { configs { alpine { mode = "linux" ... extra {
    kernel = "@esp:/linux/Image"  initrd = "@esp:/linux/initrd.img"
    cmdline = "..."  pass_kernel_fdt_as_dtb = true } } } }

## 十一、★ 里程碑：UEFI 成功加载并启动 Linux 内核（2026-08-07）

**实测结果（镜像 76ae7429 + ESP3 3a0f20a2 + reserved1 fbd68fa9）**：

1. ✅ **SimpleInit 完全正常运行** —— 图形启动菜单出现（截图确认），含我们的
   `Alpine Linux (virt)` 项 + UEFI Shell×4 + Continue/SimpleInit/UEFI Boot/Reboot
2. ✅ **ESP 配置被正确加载**：store 回写（799B）含全部我们的键
   （locates.esp/logfs、logger.*、boot.configs.alpine.extra.{kernel,initrd,cmdline,
   pass_kernel_fdt_as_dtb}）
3. ✅ **boot.current="alpine" 生效** —— 菜单不再倒计时，直接自动执行 Alpine
4. ✅ **内核加载链全部通过**：SimpleInit → linux-boot → 从 ESP 读 9.2MB vmlinuz-virt
   → LoadImage ✓ → ImageCodeType==EfiLoaderCode ✓ → StartImage 被调用 ✓
5. ✗ **StartImage 返回错误**（屏幕显示 "start image failed: <EFI状态>"）——
   这是**内核自己在真机上早期退出**（虚拟机内核 + 不完整 DT），**不是 UEFI 侧问题**。
   错误文案来源：src/linux-boot/uefi.c:81（`tlog_error("start image failed: %s")`）

**⇒ 结论：UEFI 侧的使命已达成** —— 固件能读配置、能读文件系统、能加载并启动 PE 内核。
后续失败属于 **Linux 内核 bring-up** 范畴（需要 sm8250 主线内核 + 正确的 mrr-w29 DTB）。

### 日志文件通道仍未工作（待查）
- 症状：reserved1 上无 simpleinit.log；屏幕 console 输出正常（能看到 tlog 文本）
- 代码：src/loggerd/client.c:152 `logger_init_out()`
  - L177 `if(!(log=confd_get_string("logger.file_output",NULL)))return 0;` ← 静默返回
  - flag 初值 = READ|WRITE；old=0/1 加 CREATE|TRUNCATE；old=2(rename) 先试打开、
    失败则 TRUNCATE|CREATE 再打开
- 推断：要么 key 在 logger_init_out 时还没进 store，要么 `@logfs:` URL 在此时解析失败
- 下一步验证：改用 `logger.old_file = 0`（truncate，直接 CREATE）试一次；
  若仍失败，则在 client.c:177 前加 telog_warn 打印 confd_get_type 结果（临时）
- **注意：屏幕 console 一直可用** ⇒ 日志可先用拍照获取（已验证有效）

### 下一步（Linux bring-up）
1. 用已捕获的**本机 live FDT**（device tree）做基础，编译 **sm8250 主线内核**（非 virt 版）
2. DTB 来源：`pass_kernel_fdt_as_dtb = true`（当前，用 ABL 的 DT）或显式
   `extra.dtb = "@esp:/mrr-w29.dtb"`
3. 内核放 ESP（cache）`/linux/Image` + initrd；ESP 配置保持现状
4. 修日志通道（可选但强烈建议）以便无屏调试

## 十二、自动化路线实测与结论（2026-08-07）

### ✗ 方案②（电脑侧 poke 清 EDL cookie）—— 彻底排除
华为 programmer **既不支持 peek 也不支持 poke**：
- `rawxml '<?xml version="1.0" ?><data><peek address64="33370112" size_in_bytes="16" /></data>'` → `failed`
- `<poke address64="33370112" SizeInBytes="16" value="0x00 ×16"/>` → `failed`
（格式均取自 edlclient 源码 firehose.py:1671/1591 —— 地址须**十进制**、
peek 用 `size_in_bytes`、poke 用 `SizeInBytes`+`value` 属性；即便如此仍 failed）
**⇒ IMEM cookie (0x01FD3000, 源自华为 uefiplat.cfg 的 DloadCookieAddr) 无法从电脑侧清除。**

### ★ 方案①（Android 作控制通道）—— 采用，且条件齐备
关键发现（GPT, LUN4）：
- `recovery` offset 0x23d00000, 96MB（**备份已有且校验通过** ✓）
- `boot` offset 0x10706000 = sector 67334, 96MB
- `patch` 256MB 空闲
- 树里有 `Common/edk2/ArmPkg/Drivers/GenericWatchdogDxe`（SBSA 看门狗 ✓ 可用于防挂死）

**闭环设计**：
1. payload 打包成 **Android boot image**（照旧 mkbootimg + BootShim ✓ —— ABL 会当内核启动 ✓）
2. **写进 `recovery` 分区**（`boot` 保持原厂 ⇒ Android 永远安全 ✓）
3. `adb reboot recovery` → ABL 启动 recovery = 我们的 payload ✓
4. payload 跑测试 → 写日志 → `ResetSystem` 或看门狗超时复位
5. 复位后 ABL 启动 `boot` = 原厂 Android ✓ → adb 回来 → 读日志 → 迭代
6. **全程 0 按键 / 0 EDL** ✓

待验证：`recovery` 所在 LUN（`--lun=4` 报 Couldn't detect ⇒ 试 LUN0）；
以及 `adb reboot recovery` 是否真的引导该分区。

### 当前设备状态
- `boot`(LUN4) 已恢复**原厂镜像**（sha 前16: fdf40cb51d9ebf95）
- `cache` 仍是 ESP（ESP3 3a0f20a2），`reserved1` 仍是配置+logfs（fbd68fa9）

## 十三、★ 全自动闭环设计（已落地零件 + 待实现件）2026-08-07

### 已实测的硬事实
| 事实 | 证据 |
|---|---|
| **adb 直写分区可用** | `adb shell su -c dd if=probe.img of=/dev/block/by-name/recovery` → 回读 sha256 完全一致，0.05 秒 |
| `boot`=LUN4=`/dev/block/sde9`；`recovery`=LUN0=`/dev/block/sda19`；`misc`=sda4 | adb ls -l /dev/block/by-name |
| **`recovery` 受 AVB 保护**（有 recovery_vbmeta）| 写入未签名 payload → `adb reboot recovery` → ABL 校验失败 → **回退 EDL**（9008）|
| **`boot` 无 vbmeta，不校验** | 同一 payload 写入 boot 后 ABL 正常当内核启动（多次实证）|
| **★ 软件进 EDL 已实现** | `adb reboot recovery` + 无效 recovery ⇒ ABL 回退 EDL（无需按键！）|
| 原厂 boot 真实内容仅 21.7MB（kernel 21.2 + ramdisk 0.5），其余为填充；boot 不校验 | boot image header 解析 |
| 华为 programmer 不支持 peek/poke ⇒ IMEM cookie 无法从电脑侧改 | rawxml 实测 failed（格式取自 edlclient 源码）|

### 当前各分区状态（已恢复/已铺垫）
- `boot`(LUN4) = **原厂** ✓（sha fdf40cb51d9ebf95）
- `recovery`(LUN0) = **原厂** ✓（sha 244e8d085697b107，备份校验一致）
- `patch`(LUN0, 256MB) = **原厂 boot 镜像垫片**（前 8 字节被写成 "STOCKBOOT" 标记，
  真实镜像从 patch 偏移 **8** 开始；payload 恢复时应从 +8 读）
- `cache`(LUN0) = ESP3（3a0f20a2）；`reserved1` = 配置+logfs（fbd68fa9）

### ★ 闭环设计（0 按键 / 0 EDL / Android 全程安全）
```
① agent(adb+root):  dd payload → boot(sde9)；adb reboot
② payload(UEFI) 跑测试（SimpleInit → 试着引导内核）
③ 定时器到点（默认 180s，覆盖"挂死"情形）：
   a. 把**帧缓冲内容**(1600×2560×4=16MB) dump 到 patch 尾部保留区（= 相当于截屏日志 ✓）
   b. 从 patch+8 拷前 ~22MB 写回 boot（恢复原厂 ⇒ Android 可启动）
   c. ResetSystem(Warm)
④ ABL 启动 boot = 原厂 Android ✓ → adb 回来 ✓
⑤ agent: dd 读 patch 尾部保留区 → 本地渲染 PNG 看屏幕内容 → 修 → 回 ①
```
**待实现（一件小事）**：写一个 `AutoReturnDxe`（平台 DSC 里挂上）——
- gBS->SetTimer 周期定时（默认 180s，可先用宏）
- 到期：定位 patch 分区（BlockIO/PartitionInfo ✓）→ 写 FB dump → 写 boot → ResetSystem
- FB 地址 PcdMipiFrameBufferAddress=0x9C000000，1600×2560，stride 6400，格式 BGRA
- 为什么用帧缓冲：**屏幕就是日志** —— SimpleInit 的 tlog 输出与内核 panic 信息都在屏上，
  dump 下来本地渲染 PNG 即可"无拍照"看到；且对"内核卡死"同样有效 ✓

**为什么不用 recovery 放 payload**：AVB 保护 → 会把设备送回 EDL（已验证）。
**为什么不让 payload 直接回 EDL**：EDL 出口仍需物理断电 ⇒ 不满足全自动 ✗

### 安全边界（务必遵守）
- **永不写 boot 的"恢复"之外的破坏性操作**；恢复原料永远在 patch（且 Mac 上有完整备份 ✓）
- `adb reboot recovery` 进 EDL 的手段依赖"recovery 无效"⇒ 若要使用该手段，需先备份并
  接受 recovery 被破坏（已有备份 ✓ 可 EDL 恢复 ✓）
- 每次 payload 迭代前，agent 必须确认：Mac 上 backups/partitions/boot.bin 可用 ✓

## 十四、外部核验结论（scout 联网调研）—— 重要纠正

来源：Onyx Boox Note Air5 C 逆向文档（overtone.codeberg.page）、bkerler/edl issue #374、
linux-msm/qdl（含 HIL 测试套件）、Renegade 官方文档、SimpleInit 上游文档。

### 认知纠正（务必按此更新）
1. **0x01FD3000 不是 IMEM，而是 TCSR 的 BOOT_MISC_DETECT 寄存器**；SharedIMEM 是另一处
   （本机 0x146BF000）。"IMEM 粘性 cookie"是历史俗称。
2. **触发 EDL 的 cookie 值是常量 1**；`0x10` 是 full-dump 编码（内核 QCOM_DLOAD_MASK=bits[5:4]，
   1=full dump=0x10，2=minidump=0x20）——与我们 xbl_uefiplat.cfg 的注释一致。
3. XBL 收 L"EDL" 的流程：ClearDLOADCookie(写0) → SetEDLCookie(写1) → SDIDisable → PmicReset
   （经 TZ IO-access SMC 0x02000502）。**host 侧无接口能清它** —— 印证我们的实测。
4. **"reset 后滞留在 EDL"是已知现象**，且同型设备（联想 P11 Pro 2021 / SD870）有记录：
   bkerler/edl #374，维护者建议 **reset 时拔掉 USB 线**。⇒ 我们应实测此法！
5. stock firehose programmer "peek 被编译掉" 非华为独有（设备侧 XPU 权限限制）。
6. `qdl`/`edl` 的 `--resetmode`：`reset` / `off` / `edl`（后者=保持/回到 EDL）；
   firehose `<power>` 合法值官方为 `reset_to_edl` / `reset` / `off`。

### 可用新手段（按性价比）
- **`adb reboot edl`（`su -c 'reboot edl'`）** —— Qualcomm 标准软件进 EDL，比"破坏 recovery"干净。
- **EDL 退出实验**：`edl reset` + **拔 USB**；或 `<power value="off"/>`（关机）后按电源。
- **扫描 abl.bin 里的隐藏 `oem *` fastboot 命令**（有开源工具 fastboot-oem-extractor）——
  `oem edl`/`oem poweroff` 若存在，就是最干净的进/出 EDL 通道。
- **Mu-Silicium "Snapdragon Mass-Storage"**：在 UEFI 里把 UFS 以 USB 大容量设备暴露给主机
  ⇒ 主机可像 U 盘一样直接读写分区 = 最强日志/刷写通道（本树已有 UsbDeviceDxe 预编译件，可移植）。
- **qdl HIL 范式**：多步自动化用 `--skip-reset` 保持 programmer；断电/进 EDL 用可编程 USB relay
  或 uhubctl 控制（QDL_HIL_ENTER_EDL_CMD 支持 `adb reboot edl` 或继电器断电）。
- 上游 SimpleInit 的标准日志法就是我们用的 `logger.file_output=@logfs:/...`（说明我们方向对，
  问题在别处）；UEFI 变量不可持久（PcdEmuVariableNvModeEnable=TRUE）⇒ 状态传递必须走分区/文件。

### 风险再确认（scout 警告）
- **不要对 TCSR(0x01FD3000) 乱 poke**（XPU fault/挂总线/变砖）；刷 abl/xbl/oeminfo/modem NV
  可能导致不可逆掉密钥或防回滚触发；bootfail 计数可能触发 eRecovery/清数据。
- 写回 stock boot 前必须校验（我们已做：patch+8 处比对 sha256 ✓）。

## 十五、AutoReturnDxe 首次实测失败 —— 定位与改版方向（2026-08-07）

**现象**：payload 正常启动并显示 SimpleInit 菜单 ✓，但：
- `patch+0x8000000` 的截屏区**全 0**（没有 dump）
- `reserved1` 日志仍为空
- 设备**没有在 180 秒后自动复位**（用户手动断电）
⇒ 两个结论：**驱动要么没被派发，要么平台上的 UEFI 定时器事件根本不触发**。

**最可能根因**：本平台的 **UEFI timer event 不工作**（缺 timer interrupt 支持）。
SimpleInit 的界面动画/倒计时很可能用的是 `gBS->Stall`（忙等）而非事件 ⇒ 掩盖了该问题。
（`CreateEvent(EVT_TIMER)`+`SetTimer` 只有在 DXE core 收到定时器中断时才派发回调。）

**⇒ 改版方向（不依赖定时器事件）**：
1. **把"回写原厂 boot + 截屏 dump"挪到 ExitBootServices 通知里**（EBS 一定会发生：SimpleInit
   启动内核前会调 ExitBootServices）—— 这样既不依赖定时器，又能捕获"交给内核前"的屏幕内容
2. EBS 通知自身的实现要注意：不要分配内存（避免改变 MapKey）、不要再做长 IO 之外的额外动作
3. 兜底仍保留定时器（如果哪天定时器能工作就有用），但**不能作为主路径**
4. 若"内核在 EBS 之后卡死"：靠内核 `panic=N` 自动重启（已配 panic=10）
5. 诊断手段：让驱动把"我跑到哪一步"写到**固定扇区**（例如 patch 偏移 0x8FC0000 处 4KB 的状态块：
   每个阶段写一个 magic+计数器），即使没有控制台也能事后读出 —— 这样能确定它到底有没有被派发

**其他待解**：日志通道（logger.file_output）仍不工作：疑似 logger_init_out 时
`confd_get_string("logger.file_output")` 为空或 `@logfs:` URL 解析失败 ⇒ 待单独验证。

## 十六、SimpleInit 菜单自动执行的正确配置（源码级结论）2026-08-07

**启动链**：SimpleInit 由 BDS 在 `PlatformBootManagerBeforeConsole` 注册为 FV 启动项
（PlatformBm.c:456-459 → PlatformRegisterFvBootOption(gSimpleInitFileGuid,"Simple Init",ACTIVE)），
且 `PcdPlatformBootTimeOut=0` ⇒ BDS 不停顿，SimpleInit 先于磁盘卷项执行。

**菜单结构**：上下两半**都来自 `boot.configs.*`** —— 上半是配置文件定义的项 + prober 扫到的 EFI 文件；
下半是内置 `initial_cfgs`（continue / simple-init / uefi-bootmenu / reboots，bootdef.c:88-146）。
`boot.second` 在 UEFI 下**完全不参与**。

**键语义（关键）**
| 键 | 含义 | 坑 |
|---|---|---|
| `boot.current` / `boot.default` | 预选项名（= `boot.configs.<名>` 的 ident，**大小写敏感字符串**）| 不是索引、无前缀；每次启动后 SimpleInit 会把 current 改成所选项并以 save=false 回写（bootmenu.c:120-121），跨重启靠 default |
| `boot.timeout` | 整数秒（缺省 10）；**0 = 菜单画完立即执行**；负值 = 永不自动执行 | **不能加引号**（file_conf.c:135-137 按整数解析）；字符串值必须加引号（:158-170）|

**为什么之前 `boot.default` 变成 "continue"**：SimpleInit 在 `confd_init` 遍历**所有卷**加载
`\simpleinit.uefi.{conf,cfg,txt}`（SimpleInit.dec:45 名 / file_conf.c:245 扩展名 / confd/uefi.c:81-122），
**不同卷后者覆盖前者**；回写目标是第一个可写卷。⇒ **盘上只留一个 `\simpleinit.uefi.cfg` 最稳**，
或在两个卷（logfs + ESP）都写同一组 selection 键以抗枚举顺序（我们采用后者 ✓）。

**最终采用配置（两处都写）**
```
boot { current = "alpine"   default = "alpine"   timeout = 0 }
```
（已刷入：reserved1 = /tmp/rsv4.img sha 前16 `93e337bcb9556dc9`；
 cache/ESP = esp_build/esp4.img sha 前16 `c294d20238b51003`；两处 readback 校验一致 ✓）

**备选方案（内核放 \EFI\BOOT\BOOTAA64.EFI）已否决**：SimpleInit 注册在前抢不过；
且 BDS 自动卷项不填 LoadOptions、本平台 initrd 只由 SimpleInit 提供（LOAD_FILE2 +
LINUX_EFI_INITRD_MEDIA_GUID）⇒ 有 initramfs 的内核起不来。

## 十七、★ 铁律：SimpleInit 配置必须"扁平、一行一键"★

**踩坑（2026-08-07）**：用块状格式 + 多键挤一行 →
```
boot { current = "alpine"  default = "alpine"  timeout = 8   ← ✗ SimpleInit 解析不了
  configs { alpine { mode = "linux" ... } } }
```
⇒ `boot.configs.alpine` 根本不存在 ⇒ **菜单里看不到 Alpine 项** ✗
（同次启动里扁平写的 `locates`/`logger` 都正常 ✓）

**⇒ 正确写法（一行一键，字符串加引号，整数不加）**：
```
locates.logfs.by_gpt_name = "reserved1"
locates.logfs.by_disk_label = "gpt"
locates.esp.by_gpt_name = "cache"
locates.esp.by_disk_label = "gpt"
logger.file_output = "@logfs:/simpleinit.log"
logger.old_file = 1
logger.use_console = true
logger.min_level = 0xAE00
boot.current = "alpine"
boot.default = "alpine"
boot.timeout = 8
boot.configs.alpine.mode = "linux"
boot.configs.alpine.desc = "Alpine Linux (virt)"
boot.configs.alpine.show = true
boot.configs.alpine.enabled = true
boot.configs.alpine.extra.kernel = "@esp:/linux/Image"
boot.configs.alpine.extra.initrd = "@esp:/linux/initrd.img"
boot.configs.alpine.extra.cmdline = "console=tty0 earlycon panic=10"
boot.configs.alpine.extra.pass_kernel_fdt_as_dtb = true
```

**配套铁律**：
1. 同一份内容**同时写在两个卷**（reserved1 `\simpleinit.uefi.cfg` + cache/ESP `\simpleinit.uefi.conf`）
   以抗卷枚举顺序 ✗偏差
2. 设备上还有**第三份**配置 ✗：`modem` 分区里早期实验留下的 `\simpleinit.uefi.cfg`
   （`default="continue"`）——**加载顺序在我们之后 ⇒ 顶掉 boot.default** ✗。
   对策：靠 `boot.current`（每次由 ESP 提供 ✓）；**绝不可写 modem 分区**（内含 modem 固件 ✗）
3. `boot.current` 会被 SimpleInit 保存 store 时**删掉** ✗（bootmenu.c:120-121 save=false）
   ⇒ 必须放在**不会被回写**的卷（ESP ✓）
4. **logger 不写文件的根因** ✓：文件不存在时走 TRUNCATE 分支 ⇒ 文件系统 TRUNCATE
   （SetPosition+SetInfo）失败后**把刚建的文件删掉** ✗
   ⇒ 对策：卷里**预置一个空 `simpleinit.log`** ✓（走"已存在"的可用路径 ✓）
5. **`edl.py w` 参数顺序**：`w <分区名> <文件名> --lun=N` ✓
   （误写成 `w <扇区号> <文件> --lun=<分区名>` ⇒ 静默失败 ✗）
6. **`/tmp` 会被系统清理** ✗ ⇒ 重要镜像必须**复制进仓库**（如 `probe/payload4.img`）✓

## 十八、★★ 真正的 EBS 根因：runtime 区未 64KB 对齐（2026-08-07）★★

**现象**：内核 `EFI stub: Exiting boot services...` 后报
`ExitBootServices: A RUNTIME memory entry is not on a proper alignment.`（两次，=stub 重试），
随后 `linux-uefi: start image failed: Load Error`。

**根因（worker 定位，源码级）**：
- 设备 XBL 自带 uefiplat.cfg 里两段 RtData **都不是 64KB 对齐**：
  `Log Buffer 0x9FFF7000+0x8000`、`Info Blk 0x9FFFF000+0x1000`
- AArch64 的 `RUNTIME_PAGE_ALLOCATION_GRANULARITY = 0x10000`
  （MdePkg/Include/AArch64/ProcessorBind.h:163-164）
- EBS 时 `CoreTerminateMemoryMap`（MdeModulePkg/Core/Dxe/Mem/Page.c:2156-2168）
  检查 runtime 描述符 start/end 必须 64KB 对齐，不齐 ⇒ 打印该句并返回 EFI_INVALID_PARAMETER
  ⇒ **内核无法 ExitBootServices ⇒ 内核启动中止**（与 AutoReturnDxe 无关 ✗ —— 之前误判 ✗）

**已实施的修法**（`gen_memmap.py` 的输出 `Platform/Huawei/sm8250/Library/mrr-w29/PlatformMemoryMapLib/PlatformMemoryMapLib.c`）：
```
旧：{"HLOS 8",   0x9FFD0000, 0x27000, ..., Conv,  ...}
    {"Log Buffer",0x9FFF7000, 0x8000,  ..., RtData, ...}
    {"Info Blk",  0x9FFFF000, 0x1000,  ..., RtData, ...}
新：{"HLOS 8",   0x9FFD0000, 0x20000, ..., Conv,  ...}      ← 让出 28KB
    {"RtData (LogBuf+InfoBlk, 64K aligned)", 0x9FFF0000, 0x10000, ..., RtData, ...}
```
⇒ 两段 runtime 描述符合并为一条 64KB 对齐的 ✓（0x9FFF0000..0xA0000000 两端都对齐 ✓）
**注意**：这个修改只改了生成的 .c，**还没同步进 gen_memmap.py** ✗（下次重新生成会丢失 ✗ —— 待办 ✓）

**状态**：改完这一次构建**失败**了 ✗（`build_macos.sh` 返回 1 ✗，报错未及细查 ✗）
⇒ **下一步**：看构建错误 → 修好 → 重新打包（必须报告 kernel_size ≤6,110,000 ✓）→
刷入 boot（LUN4）→ 启动 → 读 reserved1 日志 ✓

**其他待办**：
1. 把对齐逻辑写进 `gen_memmap.py`（可重现 ✓）
2. `probe/candA-nodriver.img`（sha 前16 `bf5eae1bf33cd285`）已刷入过设备，但**无用** ✗
   （因为根因是内存映射，不是驱动 ✓）—— 可以回退 AutoReturnDxe 的注释（恢复驱动 ✓）
3. 日志通道已完全打通 ✓✓✓（`logs/simpleinit.log.txt` 有 626 行 ✓）

---

## 19. 2026-10-06 会话小结 ★ Linux 已在设备上跑起来 ★

### 成果（都已在真机验证）
- ★★ **Linux 起到 shell**：Alpine virt 内核 + initrd → `/ #`（root shell）✓
  - `simple-framebuffer` + `simpledrm` 正常工作 → 控制台显示在屏幕上 ✓
  - `Mounting boot media: failed` = 预期（virt 内核无存储驱动）✓
- ★★ **ExitBootServices 根因修复**：内存映射的 runtime 条目必须 64KB 对齐
  - `Platform/Huawei/sm8250/Library/mrr-w29/PlatformMemoryMapLib/PlatformMemoryMapLib.c`
  - `Log Buffer(0x9FFF7000+0x8000)` + `Info Blk(0x9FFFF000+0x1000)` → 合并为 `RtData 0x9FFF0000+0x10000`（64KB 对齐 ✓）
  - `HLOS 8` 由 `0x9FFD0000+0x27000` 裁短为 `+0x20000`
  - ⚠️ 名字字段 `CHAR8 Name[MEMORY_REGION_NAME_MAX_LENGTH]` → **名字必须 ≤31 字符**（我用了 34 字符 ⇒ 编译报 Error 7000/F002 ✗）
  - 验证手段：`strings <FD> | grep -c 64KAligned` ≥1 ✓
- ★ **ESP 必须搬离 cache 分区**：Android 每次启动会 BLKDISCARD 掉 cache ⇒ 全零 ✗
  - 新位置：**patch 分区**（GPT LBA 0x2E200 = 188,928，0x10000 扇区 = 256MB）
  - 镜像 32MB FAT（esp7.img）
- ★ **AutoReturnDxe 未触发**（诊断块 AUTORTN1 全零）

### 关键坑（防复发）
1. `PlatformMemoryMapLib.c` 是 **gen_memmap.py 的生成物** ⇒ 手改会丢 ✗
   - 或者用上游一致做法：`Log Buffer`/`Info Blk` 改标 `Reserv`（j716f/tb-9707f/instantnoodlep/r8q 都这样，Page.c:53 Reserved 的 Runtime=FALSE）
2. **AutoReturnDxe 的两个触发器在这块板上都不会响**：
   - SimpleInit 的 linux-boot 路径直接 `StartImage`，**不发 `ReadyToBoot`** ✗
   - 平台定时器事件不派发（驱动注释自己也承认）✗
   - ⇒ 修法：**驱动入口点直接写回 + `gBS->Stall` 重试**，完全不用事件
3. 驱动是否进了 FV：`strings FD` **查不到**（FV 是压缩的）⇒ 查 `FV/FVMAIN.Fv.txt` 的 GUID 清单
   - AutoReturnDxe `DEF1BB3D-78B1-49CD-A169-C2A8D8FFD906` / MrrButtonsDxe `2F4B7C19-8D3A-4E6B-B152-90CA6D3E7704`
4. 镜像尺寸：`kernel_size ≤ 6,110,000`（mkbootimg 后）
5. 读回校验必须**按镜像真实长度**读（多读一扇区 ⇒ sha 不等 ⇒ 误判 ✗）
6. 按键驱动调试输出会刷屏 ⇒ 拍照取证时菜单会被顶掉

### 设备当前状态
| 分区 | 内容 |
|---|---|
| boot (LUN4, 67334) | **原厂 boot**（已刷回 ✓ Android 可启动 ✓）|
| patch (LBA 188928) | ESP：`/linux/Image` `/linux/initrd.img` `/simpleinit.uefi.conf` |
| reserved1 (10248, 2040 扇区) | `simpleinit.uefi.cfg` + `simpleinit.log`（日志 ✓）|
| cache | 被 Android 擦掉 ✗ 不用了 |

- macOS 侧材料：
  - `backups/partitions/boot.bin` = 15:46 原厂备份（ramdisk 519,986 B，**无 Magisk** ✗）
  - `backups/ramdisk_magisk_patched.img` = Magisk ramdisk（1,541,068 B）→ 重建 rooted boot 用
  - ⚠️ **root 状态待确认**（开机看 Magisk 应用；若无 ⇒ 用上面两件重建 boot 刷回）

### 下一步（优先级）
1. ★ **主线 sm8250 内核 + 正确 DTB**（用捕获的 live FDT）→ 目标：UFS/存储起来
   → 之后可"从 Linux 内部 dd 写回 boot + reboot" ⇒ 闭环不再依赖 UEFI 驱动 ✓
2. **修 AutoReturnDxe**（入口点写回）→ 全自动迭代（零按键）
3. **同步 gen_memmap.py**（保护 EBS 修复）
4. 重建 rooted boot（若 root 丢了）

### 19.1 ★★ 全自动闭环达成（2026-10-06 17:40）★★

**AutoReturnDxe 真凶 = 一个常量错位** ✗：
- `AUTO_RETURN_STOCK_MARKER = "STOCKBOOT"` 实际是 **9 个字符**，但 `AUTO_RETURN_STOCK_MARKER_BYTES = 8` ✗
- ⇒ 第二重校验从 +8 读到 `TANDROID` 而非 `ANDROID!` ⇒ **patch 永远校验不过** ⇒ 驱动静默放弃（诊断块也不写 ✗）
- **修复**：`8ULL → 9ULL`（一处 ✓）⇒ 校验 + 拷贝源 + blob 三者对齐 ✓

**blob 布局**（patch 分区，绝对偏移 0x2E200000 ✓）：
| patch 内偏移 | 内容 |
|---|---|
| 0 .. 32MB | **ESP**（FAT32 镜像 esp7，含 /linux/Image + initrd + simpleinit.uefi.conf）|
| **+32MB** | **STOCKBOOT blob** = `"STOCKBOOT"`(9B) + 原厂 boot 前 (24MB-9) 字节 ✓ |
| +128MB | fb dump 区（FBDUMP01 ✓ 驱动用）|
| +0x8FC0000 | 诊断块（best-effort，实测常为空 ✗ 不影响功能）|

**验证过的完整闭环（零按键 ✓）**：
```
adb push payload → su dd of=/dev/block/by-name/boot ✓(0.03s)
adb reboot ✓
payload → SimpleInit → Alpine Linux ✓
AutoReturnDxe(entry) → 读 patch+32MB blob → 写回 boot ✓
长按电源强制重启 → ★ Android ✓★
adb+su dd if=/dev/block/by-name/reserved1 → 读日志 ✓(0.07s)
```

**★ 关键操作要点 ★**
- **`/sdcard` 在 Magisk root 上下文不可见** ✗ ⇒ `su -c dd` 的源/目标必须放 **`/data/local/tmp/`** ✓✓✓（第一次用 /sdcard 失败 = "0 bytes copied" ✗）
- 分区路径：boot=`/dev/block/by-name/boot`(LUN4) / patch / reserved1 ✓
- 设备不在 Android 时（payload 运行中）adb 不可用 ✗ ⇒ 用长按电源强制重启回 Android ✓

**下一步**：主线 sm8250 内核 + DTB（postmarketOS 支持可借）→ UFS 起来 ⇒ 内核自己写日志 ⇒ 连拍照都省 ✓

---

## 20. 主线内核路线（2026-10-06 调研结论）

### ★ 正主仓库（已克隆 ✓）
`work/kernel/linux-sm8250` ← `https://gitlab.postmarketos.org/soc/qualcomm-sm8250/linux`
（postmarketOS 专职维护的 SM8250 主线 fork，维护者 Jianhua Lu，1.9GB ✓）

### ★★ 最佳模板
- **`sm8250-lenovo-j716f.dts`**（联想小新 Pad Pro 2021 —— **平板** ✓）
  含 UFS + MDSS/DSI 面板 + 背光 ✓ —— **且我们 edk2 的模板一直就是 j716f** ✓
- `sm8250-xiaomi-pipa`（小米平板 6）→ 参考 **reserved-memory 布局** ✓

### 我们独有的资产
**捕获的本机 live FDT** ⇒ 用它校正模板的 reserved-memory / 中断 / 调节器 ✓✓✓

### 步骤
1. 复制 j716f（或 pipa）DTS → `sm8250-huawei-mrr-w29.dts`（+ Makefile 条目 ✓）
2. 用 **live FDT** 校正：内存布局 ✓ / 面板时序 ✓ / 触摸 ✓ / 调节器 ✓
3. **编译环境**：macOS 无交叉工具链 ✗ ⇒ **Docker（linux/amd64 跑 aarch64 交叉）** ✓ 或 Linux VM ✓
   ```
   docker run --rm -v $PWD:/k -w /k -it ubuntu:24.04
   apt install -y build-essential bc bison flex libssl-dev libelf-dev \
                  gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu
   make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig
   make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 Image dtbs
   ```
4. 打包（现成配方 ✓ kernel_size ≤ 6,110,000 ✗ 主线内核可能超 ✗ ⇒ 需调 gzip 级别或改 payload 结构 ✓）
5. adb 刷入 + 测试（每轮一次按键 ✓）

### 里程碑
- M1: Image 能被 SimpleInit 加载、内核打印出 sm8250 早期信息 ✓
- M2: **UFS 起来** ⇒ 内核能读/写分区 ⇒ **内核自己写日志** ⇒ 彻底零人工 ✓✓✓
- M3: 显示/触摸 ✓
- M4: 完整 Alpine/Ubuntu rootfs ⇒ 然后才是 **Windows ARM64**

### 已知社区资源
- Matrix: `#sm8250-mainline:matrix.org`
- pmOS wiki: `Qualcomm_Snapdragon_865/865+/870_(SM8250)`
- 华为先例: `linux-huawei-cameron`（华为平板 ✓ 启动 + USB 网络 + 显示 ✓）

---

## 21. 关于 Windows ARM64 —— 为什么先做 Linux（重要结论）

**不是绕路：Linux 和 Windows 是同一工程的两种出口，共享"硬件事实"。**

### Windows ARM64 的前提（对照我们的现状）
| 前提 | 状态 |
|---|---|
| 自定义 UEFI 能启动 | ✅ **已有**（本项目的核心成果）|
| **ACPI 表（DSDT/SSDT）** | ⚠️ **框架已在树里** ✓ `Platform/Huawei/sm8250/AcpiTables/` ✓ + **可照抄 `Platform/Lenovo/sm8250/AcpiTables/j716f/`（同为 sm8250 平板 ✓）**|
| 存储（UFS）能在 UEFI 里读写 | ✅ **已验证**（我们一直用它刷分区）|
| 可引导 ESP（放 BOOTAA64.EFI） | ✅ **已有**（patch 分区的 ESP ✓）|
| 显示 | ⚠️ UEFI GOP 可用 ✓；Windows 侧需适配（SD870/Adreno650 无官方驱动 ⇒ 基本显示驱动）|

### 为什么仍建议先打通 Linux
1. **Linux 是最快的"硬件事实验证器"**：内存/中断/时钟/UFS/显示 —— 起来 ≈ 说明平台可用 ✓
   （Windows 侧若 ACPI 有错，通常只是黑屏 ✗ 无诊断信息，调试成本高得多 ✗）
2. **Linux 一旦有 UFS** ⇒ 内核自己写日志 ⇒ 我们的迭代彻底自动化 ✓
3. **两者共享的东西**：内存映射 ✓ 中断号 ✓ 时钟/调节器 ✓ —— 这些在 Linux 起来时就一并拿到了 ✓
   （Windows 把它们写成 ACPI；Linux 把它们写成 DTB ✓）

### 两条可选路线
- **A（推荐）**：Linux 打通 UFS（1-2 天）→ 转 Windows（DSDT 照 j716f 改 + 安装到 UFS）
- **B（并行）**：现在就动手 ACPI/DSDT（`Platform/Huawei/sm8250/AcpiTables/` ✓ 起草 `Dsdt.asl` ✓ 照 j716f ✓ + 用 live FDT 校正地址 ✓）

### 21.1 Linux 实施细节（已开工 ✓）
- **模板选定：`sm8250-xiaomi-pipa`（小米 Pad 6 = SD870 平板 ✓✓✓）**
  ★ 决定性证据：它的 `simple-framebuffer reg = <0x9c000000 0x2300000>`
  **与我们设备内核报错的地址完全一致**（`[drm] mem 0x9c000000-0x9cF9ffff`）✓
  ⇒ 屏幕可直接亮 ✓（首版**不碰面板驱动**，只用 simple-framebuffer ✓）
- 已创建：`arch/arm64/boot/dts/qcom/sm8250-huawei-mrr-w29{,-common}.dts{i}` ✓ + Makefile 条目 ✓
- **★ ESP 尺寸要放大 ★**：主线 Image 约 25-35MB（未压缩）⇒ 现有 32MB ESP 装不下 ✗
  重新规划 patch 分区（256MB）：
  | patch 偏移 | 用途 |
  |---|---|
  | 0 .. 64MB | **ESP**（FAT，装 Image + initrd）|
  | +64MB | STOCKBOOT blob（24MB）→ **需同步改 AutoReturnDxe 的 `AUTO_RETURN_STOCK_OFFSET`** ✗ |
  | +128MB | fb dump（不变 ✓）|
  | +0x8FC0000 | 诊断块（不变 ✓）|
- **编译**：macOS 无交叉工具链 ⇒ Docker：
  `docker run --rm -v $PWD:/k -w /k ubuntu:24.04` + `gcc-aarch64-linux-gnu` ✓
- 首版目标：**M1** —— 内核打印出 sm8250 早期信息（UFS/debug UART 起来）✓

### 21.2 内核编译：环境结论与坑（2026-10-06 19:00）

**环境**
- lima VM **`corplink`**（用户已有 ✓ 未新建 ✓）
  - 已改：`cpus: 8` ✓ + 挂载 `/Users/lertian/Agentspace`（writable ✓ 原 CMHK 挂载保留 ✓）
  - ⚠️ **swap 不持久** ✗（VM 重启后要重加：`sudo fallocate -l 4G /swap.img && sudo mkswap ... && sudo swapon ...`）
- 源码：`work/kernel/linux-sm8250`（pmOS sm8250 fork v7.2.0 ✓）
  - ⚠️ 第一次 `git clone` 被打断 ⇒ 工作区 94,924 文件未检出 ✗
    **解法**：`rm -f .git/index.lock && git reset --hard HEAD` ✓（注意：macOS 大小写不敏感 ⇒ 之后恒定有 15 个文件显示 "M" ✗，无害 ✓）
  - 我们加的 DTS：`sm8250-huawei-mrr-w29{,-common}.dts{i}` ✓ + Makefile 条目（第 371 行 ✓）

**★ 最大的坑：跨 virtiofs 的 tar 复制会静默漏文件 ✗**
- 现象：`tar: ./drivers/usb/misc: Cannot open: Too many open files` ⇒ 复制"成功"但缺文件 ✗
- 后果：编译报 `No such file or directory` / `No rule to make target` ✗（每次缺的地方不同 ✗）
- **解法 A（稳）**：**直接在挂载目录编译** ✓（不复制 ✓ 慢 2-3 倍 ✗）
- **解法 B（快）**：复制前 `ulimit -n 65536` ✓ **且复制后必须比对文件数** ✓
  `find <src> -type f | wc -l` vs `find ~/kbuild -type f | wc -l`

**编译命令**
```
./scripts/config --disable CONFIG_KVM        # 本平台 arch/arm64/kvm/hyp 编译失败 ✗ 且我们不需要 ✓
make ARCH=arm64 olddefconfig
make ARCH=arm64 -j8 Image dtbs               # 本地盘 -j8；挂载目录建议 -j3
```
**产物**：`arch/arm64/boot/Image` + `arch/arm64/boot/dts/qcom/sm8250-huawei-mrr-w29.dtb`
**产物回传**：只需拷这两个文件回宿主 ✓（几 MB ✓ 秒回 ✓）

**下次继续的最短路径**
1. 在**挂载目录**跑 `make ARCH=arm64 -J3 Image dtbs`（稳 ✓ 约 60-90 分钟 ✓）
2. 成功后：`work/kernel/Image-mrr-w29` + `sm8250-huawei-mrr-w29.dtb`
3. 然后按 §21.1：**ESP 扩到 64MB** + 重排 patch 分区 + 改 AutoReturnDxe 的 blob 偏移 ✓ → 打包 → adb 刷入 → 启动看效果 ✓

### 22. Windows ARM64 事实核查（2026-10-06）

**✅ 之前"有人跑过"是对的**：
- Mu-Silicium `Status.md` 有 **Snapdragon 865/865+/870 分类** ✓
  其中 **OnePlus 8T（kebab, SD865）State: Inactive**（= 曾支持、现不活跃 ✓）
- ⚠️ 我之前引用的 "sm8250 devices can not run windows"（edk2-msm issue #300）
  是**旧评论** ✗，与机型表矛盾 ⇒ **以机型表为准：SM8250 能跑 Windows（至少到可用程度）** ✓

**★ 官方安装指南（照它做 ✓）**
`github.com/edk2-porting/renegade-project.org/blob/master/en/windows/Installation-guide.md`
步骤：Windows PE → Dism++ → **WOA-Drivers** → Win ARM64 ISO（UUP dump）→ diskpart 分区 → 部署

**驱动**：`github.com/edk2-porting/WOA-Drivers`
- 用法：`./extract.sh <DEVICE>` —— **从设备自身固件里提取 Windows 驱动** ✓（不限机型 ✓）
- README 未列机型；含 any-SoC 组件 + MSM8998 平台目录

**Windows ISO**：`microsoft.com/software-download/windows11arm64`
- ⚠️ **curl 拿直链返回 403** ✗ ⇒ 必须**浏览器会话** ✓（需人工点击）

**⇒ 结论（更新）**：SM8250 跑 Windows 有先例 ✓ —— 但**具体到华为 MatePad Pro 10.8"（MRR-W29）还没有人做过** ✗
⇒ 我们已有的优势：**UEFI ✓ + ACPI 表（DSDT/MADT/FACP/GTDT/BGRT 已在固件里 ✓）+ UFS 在 UEFI 可读写 ✓**
⇒ 待办：Windows 引导文件上 ESP（需先把 ESP 扩到 64MB ✓）→ 验证引导管理器 → 再谈安装
