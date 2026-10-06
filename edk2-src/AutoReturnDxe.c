/** @file
  AutoReturnDxe —— mrr-w29（Huawei MatePad Pro 10.8 2021 / sm8250）自动收尾驱动。

  目标：让 UEFI payload 在无人值守测试里总能自己回到 Android（0 按键 / 0 EDL）。

  ★ 主路径 = ReadyToBoot 通知（gEfiEventReadyToBootGuid，EVT_NOTIFY_SIGNAL / TPL_CALLBACK）：
    SimpleInit 真正 StartImage 内核之前会显式 EfiSignalEventReadyToBoot()
    （GPLDrivers/Library/SimpleInit/src/linux-boot/uefi.c:71）；BDS 的 BmBoot 在启动每个
    boot option 前也会 signal 一次（UefiBootManagerLib/BmBoot.c:1888）。此时**启动服务完全可用**，
    依次做：
      ① 截屏 dump 到 patch 偏移 128MB（4096B 头 + 像素，布局见下）；
      ② 把 patch+8 起 24MB 原厂 boot 镜像写回 boot 分区（若之前已回写则跳过）；
      ③ 取消 180s 定时器；
      ④ 不调用 ResetSystem —— 之后由内核接管，这里回写只为"之后任何复位都回 Android"。

    ★ 绝不在 ExitBootServices 通知里做存储 I/O —— 本驱动**不注册** ExitBootServices：
    真机实测（照片）：内核 EFI stub 报 "Exit boot services failed" ⇒ 内核启动中止。
    根因就是 EBS 通知里的 40MB 存储 I/O：即使回调自身不分配内存，UFS/SCSI 栈内部也会
    分配/改变内存映射，让内核刚拿到的 MapKey 失效。ReadyToBoot 早于 StartImage，
    此时做 I/O 不会碰内核的 EBS 窗口 ⇒ 内核 EBS 恢复正常。

  ★ 次路径 = 180 秒定时器（如果平台的 UEFI timer event 能派发的话）：
    到期 → 截屏 dump → 回写原厂 boot → ResetSystem(EfiResetWarm)。
    注意：本平台定时器事件仍可能不触发，所以它只是兜底，不是主路径。

  另外在驱动入口、以及每个新 BlockIO 句柄出现时，都尽早把原厂 boot 回写掉
  （这样即使后面挂死/手动断电，任何复位也都会引导回 Android）。

  截屏 dump 落盘布局（保持既有布局）：patch 偏移 128MB（0x8000000）：
    +0x0000 起 4096 字节：头 = "FBDUMP01" + width/height/stride/size（小端）
    +0x1000 起 16,384,000 字节：像素（BGRA，1600x2560，stride 6400）

  ★ 诊断块（patch 偏移 0x8FC0000 处 4KB，紧邻 dump 区之后、永不重叠）：
    magic "AUTORTN1" + u32 版本 + u32 记录序号 + u32 记录条数 + u32 最近阶段 +
    u32 最近时间戳 + u32 首时间戳 + u32 最近失败状态码 + u32 该失败对应阶段 +
    u32 各阶段计数[16] + {u32 阶段, u32 时间戳} 时间线 ×499（见 AUTO_RETURN_DIAG_BLOCK）。
    ⇒ 即使控制台看不到，事后从 EDL 读这一块（patch+0x8FC0000）就知道驱动有没有被派发、
       跑到哪一步。阶段编号表见 AUTO_RETURN_DIAG_STAGE_* 宏；记录里 Stage 带 0x80000000
       位 = 该阶段失败，错误码在 LastStatus。时间戳 = CNTVCT_EL0 低 32 位
       （19.2MHz ⇒ 约 224 秒回绕，够覆盖 180 秒测试窗口）。
       ★ 只能用虚拟计数器 CNTVCT_EL0：平台 DSC 选的是 ArmGenericTimerVirtCounterLib
       （sm8250.dsc: USE_PHYSICAL_TIMER=FALSE），整个 FV 里 190+ 个模块没有第二个读
       CNTPCT_EL0 的；而真机失败的 v2 正是唯一读它的版本（同版其余 83 个 FV 文件与能启动的
       v1 逐字节相同）——见 docs/uefi-port-notes.md。
    诊断块在驱动入口首次记录时整体初始化（覆盖上一次启动留下的内容）；
    如果事后读到的内容与上一次完全相同，说明本次启动驱动根本没能写盘（多半没被派发，
    或者从未看到磁盘）。诊断写盘最早发生在"驱动入口或第一次 BlockIO 通知时能定位 patch"之后；
    在那之前所有阶段记录都缓冲在 RAM 里，一旦 patch 可见就整批落盘。

  分区定位（AutoReturnLocatePartition）：
    主方案：枚举 BlockIO 句柄 → 读各 LUN 的 GPT，按 UTF-16 表项名匹配 "patch" / "boot"，
            并要求 GPT 推出的绝对字节偏移与设备实测值一致（候选 LBA 单位 4096 / 512 / BlockSize）；
    回退：在实测绝对字节偏移处读一个块，校验内容 magic（patch="STOCKBOOT"、boot="ANDROID!"）。
    两条路都必须通过介质/分区边界检查；写 boot 之前还会重新校验 patch 里的原厂镜像 magic。
    定位结果（BlockIO 句柄 + 绝对偏移）会缓存；事件通知里优先用缓存，避免重复枚举 GPT。

  调度位置：本驱动被放进平台 apriori 列表（BdsDxe 之前）——因为 SimpleInit 是 BDS 注册的
  FV boot option，SimpleInit 一旦启动就不再回到 DXE 调度器，所以事件注册与回写必须早于 BdsDxe。
  BlockIO 句柄可能到 BDS 的 EfiBootManagerConnectAll() 才出现，因此入口只做一次"尽力尝试"，
  之后靠 BlockIO 协议通知（每个新句柄都会触发）继续尝试，直到回写成功。

  Copyright (c) 2026, mrr-w29 porting project. SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Guid/EventGroup.h>
#include <Uefi/UefiGpt.h>

#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/BlockIo.h>

//
// 设备实测事实（docs/uefi-port-notes.md 第十二/十三节，"Offset" 是 LUN 起始的绝对字节偏移）：
//   patch (LUN0, 256MB)：0x2E200000 = 4K 扇区 188928
//   boot  (LUN4,  96MB)：0x10706000 = 4K 扇区 67334
// 这些值同时是边界检查与 magic 校验的锚点：只有"名字 + 绝对偏移 + magic"都对上才会写盘。
//
#define AUTO_RETURN_PATCH_BYTE_OFFSET   0x000000002E200000ULL
#define AUTO_RETURN_BOOT_BYTE_OFFSET    0x0000000010706000ULL

//
// patch 前 8 字节是原厂镜像的标记，真实镜像（Android boot image）从 +8 开始。
// 原厂 boot 真实内容 21.7MB，写回 24MB 足够覆盖（boot 分区 96MB，无虞）。
//
#define AUTO_RETURN_STOCK_MARKER        "STOCKBOOT"
#define AUTO_RETURN_STOCK_MARKER_BYTES  9ULL   // "STOCKBOOT" 是 9 个字符！原为 8 导致 ANDROID! 校验读错位
#define AUTO_RETURN_STOCK_IMAGE_BYTES   0x0000000001800000ULL
// mrr-w29：ESP 现位于 patch 分区 0..32MB，原厂镜像 blob 挪到 patch+32MB
#define AUTO_RETURN_STOCK_OFFSET        0x0000000002000000ULL

#define AUTO_RETURN_BOOT_MAGIC          "ANDROID!"

//
// 截屏 dump 落盘布局：patch + 128MB，头占一个 4K 块，像素紧随其后
// （像素理论大小 0xFA0000 = 1600×2560×4；实际大小按 PCD 几何算）。
//
#define AUTO_RETURN_FB_DUMP_OFFSET      0x0000000008000000ULL
#define AUTO_RETURN_FB_HEADER_BYTES     0x1000ULL
#define AUTO_RETURN_FB_MAGIC            "FBDUMP01"

//
// 诊断块：patch + 0x8FC0000（在 dump 区 0x8000000+0x1000+0xFA0000 = 0x8FA1000 之后）。
//
#define AUTO_RETURN_DIAG_OFFSET         0x0000000008FC0000ULL
#define AUTO_RETURN_DIAG_BYTES          0x1000ULL
#define AUTO_RETURN_DIAG_MAGIC          "AUTORTN1"
#define AUTO_RETURN_DIAG_VERSION        1U
#define AUTO_RETURN_DIAG_STAGE_COUNT    16U    // StageCounts[] 下标范围（阶段编号 1..15）
#define AUTO_RETURN_DIAG_FAIL_BIT       0x80000000U

//
// 诊断块里的阶段编号（报告/事后解读用；记录里带 FAIL_BIT 表示该阶段失败）：
//   1  驱动入口（诊断块首次初始化）
//   2  patch 分区定位成功
//   3  boot 分区定位成功
//   4  原厂 boot 回写完成且回读校验一致
//   5  截屏 dump 完成
//   6  ReadyToBoot 通知进入
//   7  ReadyToBoot 通知处理完成
//   8  180s 定时器触发
//   9  定时器路径调用 ResetSystem
//   10 定时器事件创建 + SetTimer 成功
//   11 ReadyToBoot 事件注册成功
//   12 首次收到 BlockIO 协议通知
//
#define AUTO_RETURN_DIAG_STAGE_ENTRY        1U
#define AUTO_RETURN_DIAG_STAGE_PATCH_FOUND  2U
#define AUTO_RETURN_DIAG_STAGE_BOOT_FOUND   3U
#define AUTO_RETURN_DIAG_STAGE_RESTORE_DONE 4U
#define AUTO_RETURN_DIAG_STAGE_DUMP_DONE    5U
#define AUTO_RETURN_DIAG_STAGE_RTB_ENTER    6U
#define AUTO_RETURN_DIAG_STAGE_RTB_DONE     7U
#define AUTO_RETURN_DIAG_STAGE_TIMER_FIRED  8U
#define AUTO_RETURN_DIAG_STAGE_RESET_CALLED 9U
#define AUTO_RETURN_DIAG_STAGE_TIMER_ARMED  10U
#define AUTO_RETURN_DIAG_STAGE_RTB_EVENT_OK 11U
#define AUTO_RETURN_DIAG_STAGE_BLOCKIO_SEEN 12U

//
// 本平台所有用到的偏移/长度都是 4K 对齐；分块搬运的 bounce buffer 大小。
//
#define AUTO_RETURN_BLOCK_BYTES         0x1000U
#define AUTO_RETURN_CHUNK_BYTES         0x40000U

#define AUTO_RETURN_TIMEOUT_SECONDS     180
#define AUTO_RETURN_TIMEOUT_UNITS       (180ULL * 10000000ULL)  // SetTimer 的 100ns 单位

#define AUTO_RETURN_LOG_CHARS           320

//
// patch 分区至少要能放下：原厂镜像回写源（+8 起 24MB，再多读一个块）、
// 截屏 dump（128MB 处 4096 字节头 + 16,384,000 字节像素）以及诊断块（0x8FC0000 处 4KB）。
// 诊断块偏移最大 ⇒ 以它为准（dump 结束于 0x8FA1000，诊断块结束于 0x8FC1000）。
//
#define AUTO_RETURN_PATCH_REQUIRED_BYTES  (AUTO_RETURN_DIAG_OFFSET + AUTO_RETURN_DIAG_BYTES)

#pragma pack(1)
typedef struct {
  CHAR8     Magic[8];
  UINT32    Width;
  UINT32    Height;
  UINT32    Stride;
  UINT32    Size;
} AUTO_RETURN_FB_HEADER;

typedef struct {
  UINT32    Stage;
  UINT32    Tick;
} AUTO_RETURN_DIAG_RECORD;

//
// 4KB 诊断块：40 字节元数据 + 16×u32 阶段计数 + 499×8 字节时间线 = 4096 字节整。
//
typedef struct {
  CHAR8                    Magic[8];        // "AUTORTN1"
  UINT32                   Version;         // AUTO_RETURN_DIAG_VERSION
  UINT32                   Sequence;        // 累计记录条数（每次记录 +1，超出容量仍增）
  UINT32                   RecordCount;     // Records[] 已用条数
  UINT32                   LatestStage;     // 最近记录的阶段编号（含 FAIL_BIT）
  UINT32                   LatestTick;      // 最近记录的时间戳
  UINT32                   FirstTick;       // 第一条记录的时间戳
  UINT32                   LastStatus;      // 最近一次失败的 EFI_STATUS
  UINT32                   LastStatusStage; // 该失败对应的阶段编号（不含 FAIL_BIT）
  UINT32                   StageCounts[AUTO_RETURN_DIAG_STAGE_COUNT];
  AUTO_RETURN_DIAG_RECORD  Records[499];
} AUTO_RETURN_DIAG_BLOCK;
#pragma pack()

STATIC_ASSERT (
  sizeof (AUTO_RETURN_DIAG_BLOCK) == AUTO_RETURN_DIAG_BYTES,
  "诊断块必须是 4096 字节（patch+0x8FC0000 处一个块）"
  );

typedef struct {
  CONST CHAR16           *Name;           // L"patch" / L"boot"
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;        // 命中的 LUN 句柄
  UINT64                  ByteOffset;     // LUN 起始的绝对字节偏移（= 实测值）
  UINT64                  SizeBytes;      // GPT 给的分区大小；回退方案给"介质剩余容量"
  UINT64                  StartLba;       // GPT 表项（LBA 单位 = LbaUnit）
  UINT64                  EndLba;
  UINTN                   LbaUnit;
  BOOLEAN                 FromGpt;        // TRUE = GPT 名 + 偏移匹配；FALSE = 内容 magic 回退
} AUTO_RETURN_PARTITION;

//
// 分块搬运的两个 bounce buffer：
//   A —— 块对齐的读缓冲（源偏移不按块对齐时，从对齐位置多读一个块）
//   B —— 平移/校验后的写缓冲（始终块对齐）
// 都是静态的：事件通知里不再分配内存（ReadyToBoot 虽允许分配，但静态缓冲最可控）。
//
STATIC UINT8  mChunkA[AUTO_RETURN_CHUNK_BYTES + AUTO_RETURN_BLOCK_BYTES] __attribute__ ((aligned (4096)));
STATIC UINT8  mChunkB[AUTO_RETURN_CHUNK_BYTES + AUTO_RETURN_BLOCK_BYTES] __attribute__ ((aligned (4096)));

//
// 诊断块在 RAM 里的镜像：每次记录先改这里，再整块写回 patch。
// mDiagInitialized=FALSE 时，第一次记录会把它整体清零重建（覆盖上一次启动的内容）。
//
STATIC AUTO_RETURN_DIAG_BLOCK  mDiag __attribute__ ((aligned (4096)));
STATIC BOOLEAN                 mDiagInitialized = FALSE;

STATIC BOOLEAN               mPatchValid = FALSE;
STATIC AUTO_RETURN_PARTITION mPatch;
STATIC BOOLEAN               mBootValid = FALSE;
STATIC AUTO_RETURN_PARTITION mBoot;
STATIC BOOLEAN               mStockBootRestored = FALSE;
STATIC BOOLEAN               mStockBootRestoreRunning = FALSE;
STATIC BOOLEAN               mReadyToBoot = FALSE;
STATIC BOOLEAN               mTimerFired = FALSE;
STATIC BOOLEAN               mBlockIoSeen = FALSE;
STATIC EFI_EVENT             mTimeoutEvent = NULL;
STATIC EFI_EVENT             mReadyToBootEvent = NULL;
STATIC EFI_EVENT             mBlockIoNotifyEvent = NULL;
STATIC VOID                 *mBlockIoRegistration = NULL;

STATIC
EFI_STATUS
AutoReturnGetPatch (
  IN  BOOLEAN                  AllowLocate,
  OUT AUTO_RETURN_PARTITION  **Out
  );

STATIC
VOID
AutoReturnLog (
  IN CONST CHAR16  *Format,
  ...
  )
{
  VA_LIST  Marker;
  CHAR16   Buffer[AUTO_RETURN_LOG_CHARS];

  VA_START (Marker, Format);
  UnicodeVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);

  //
  // 控制台（FrameBufferSerialPortLib / 图形控制台）—— "屏幕就是日志"。
  //
  Print (L"[AutoReturn] %s", Buffer);
}

/**
  进入"可做长 IO"的 TPL：事件通知在 TPL_CALLBACK 运行，而 BlockIO 同步读写依赖
  事件派发（存储驱动内部等待完成事件），所以显式降到 TPL_APPLICATION；
  返回进入时的 TPL，由 AutoReturnEndIo() 恢复。嵌套调用是安全的（先抬到 HIGH 再降回来）。

  @return 进入本函数时的 TPL
**/
STATIC
EFI_TPL
AutoReturnBeginIo (
  VOID
  )
{
  EFI_TPL  EntryTpl;

  EntryTpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  gBS->RestoreTPL (TPL_APPLICATION);
  return EntryTpl;
}

STATIC
VOID
AutoReturnEndIo (
  IN EFI_TPL  EntryTpl
  )
{
  gBS->RaiseTPL (EntryTpl);
}

/**
  按块大小算 LBA，并检查跨块对齐；避免任何越界/非对齐访问。

  @param  ByteOffset   LUN 起始的绝对字节偏移
  @retval EFI_INVALID_PARAMETER  偏移不是块大小整数倍
**/
STATIC
EFI_STATUS
AutoReturnByteOffsetToLba (
  IN  EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  IN  UINT64                  ByteOffset,
  IN  UINTN                   Length,
  OUT EFI_LBA                *Lba
  )
{
  UINT64  MediaBytes;

  if ((BlockIo == NULL) || (BlockIo->Media == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (((ByteOffset % BlockIo->Media->BlockSize) != 0) ||
      ((Length % BlockIo->Media->BlockSize) != 0))
  {
    AutoReturnLog (L"偏移/长度未按块对齐：offset=0x%lx len=0x%lx BlockSize=%u\n",
                   ByteOffset, (UINT64)Length, BlockIo->Media->BlockSize);
    return EFI_INVALID_PARAMETER;
  }

  MediaBytes = (BlockIo->Media->LastBlock + 1) * (UINT64)BlockIo->Media->BlockSize;
  if ((ByteOffset + Length) > MediaBytes) {
    AutoReturnLog (L"越界：offset=0x%lx len=0x%lx 超出介质 0x%lx 字节\n",
                   ByteOffset, (UINT64)Length, MediaBytes);
    return EFI_INVALID_PARAMETER;
  }

  *Lba = ByteOffset / BlockIo->Media->BlockSize;
  return EFI_SUCCESS;
}

/**
  读一段（块对齐）到指定缓冲。

  @param  Buffer  必须是 BufferBytes 大小、且（按块对齐要求）4K 对齐的静态缓冲
**/
STATIC
EFI_STATUS
AutoReturnReadBlocks (
  IN  EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  IN  UINT64                  ByteOffset,
  IN  UINTN                   Length,
  OUT VOID                   *Buffer
  )
{
  EFI_LBA     Lba;
  EFI_STATUS  Status;

  Status = AutoReturnByteOffsetToLba (BlockIo, ByteOffset, Length, &Lba);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, Lba, Length, Buffer);
}

STATIC
EFI_STATUS
AutoReturnWriteBlocks (
  IN  EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  IN  UINT64                  ByteOffset,
  IN  UINTN                   Length,
  IN  CONST VOID             *Buffer
  )
{
  EFI_LBA     Lba;
  EFI_STATUS  Status;

  if (BlockIo->Media->ReadOnly) {
    AutoReturnLog (L"目标介质只读，拒绝写入\n");
    return EFI_WRITE_PROTECTED;
  }

  Status = AutoReturnByteOffsetToLba (BlockIo, ByteOffset, Length, &Lba);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, Lba, Length, (VOID *)Buffer);
}

/**
  GPT 分区表项名匹配：只接受"名字一致 + GPT 推出的绝对字节偏移等于实测值"的表项。
  候选 LBA 单位 {BlockSize, 4096, 512}（用 4K 写表、但介质按 512 报块的情况都能覆盖）。

  @param  OutSizeBytes  命中分区大小（字节）
  @param  OutStartLba   命中分区的 StartLBA（OutLbaUnit 单位）
  @param  OutLbaUnit    命中的 LBA 单位（字节）
**/
STATIC
EFI_STATUS
AutoReturnGptFindPartition (
  IN  EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  IN  CONST CHAR16           *Name,
  IN  UINT64                  ExpectedByteOffset,
  OUT UINT64                 *OutSizeBytes,
  OUT UINT64                 *OutStartLba,
  OUT UINT64                 *OutEndLba,
  OUT UINTN                  *OutLbaUnit
  )
{
  EFI_PARTITION_TABLE_HEADER  Header;
  EFI_PARTITION_ENTRY         *Entry;
  EFI_BLOCK_IO_MEDIA          *Media;
  UINTN                       Candidates[3];
  UINTN                       CandidateIndex;
  UINTN                       PrevIndex;
  UINTN                       EntryIndex;
  UINTN                       CharIndex;
  UINTN                       Unit;
  UINTN                       ReadBytes;
  UINT64                      MediaBytes;
  UINT64                      EntryOffset;
  UINT64                      StartByte;
  EFI_STATUS                  Status;
  UINT8                       *Raw;
  BOOLEAN                     Duplicate;

  Media      = BlockIo->Media;
  MediaBytes = (Media->LastBlock + 1) * (UINT64)Media->BlockSize;

  Candidates[0] = Media->BlockSize;
  Candidates[1] = AUTO_RETURN_BLOCK_BYTES;
  Candidates[2] = 512;

  for (CandidateIndex = 0; CandidateIndex < 3; CandidateIndex++) {
    Unit = Candidates[CandidateIndex];
    if ((Unit == 0) || (Unit > AUTO_RETURN_BLOCK_BYTES) || ((Unit % Media->BlockSize) != 0)) {
      continue;
    }

    if (CandidateIndex > 0) {
      //
      // 去重：与前面出现过的候选相同就跳过
      //
      Duplicate = FALSE;
      for (PrevIndex = 0; PrevIndex < CandidateIndex; PrevIndex++) {
        if (Candidates[PrevIndex] == Unit) {
          Duplicate = TRUE;
          break;
        }
      }

      if (Duplicate) {
        continue;
      }
    }

    //
    // LBA1（GPT 主表头），读足 4K 一整个块
    //
    if ((Unit + AUTO_RETURN_BLOCK_BYTES) > MediaBytes) {
      continue;
    }

    Status = AutoReturnReadBlocks (BlockIo, Unit, AUTO_RETURN_BLOCK_BYTES, mChunkA);
    if (EFI_ERROR (Status)) {
      continue;
    }

    if (CompareMem (mChunkA, "EFI PART", 8) != 0) {
      continue;
    }

    CopyMem (&Header, mChunkA, sizeof (Header));
    if ((Header.MyLBA != 1) || (Header.Header.HeaderSize < sizeof (Header))) {
      continue;
    }

    if ((Header.SizeOfPartitionEntry != sizeof (EFI_PARTITION_ENTRY)) ||
        (Header.NumberOfPartitionEntries == 0) ||
        (Header.NumberOfPartitionEntries > 1024) ||
        (Header.PartitionEntryLBA != 2))
    {
      AutoReturnLog (L"GPT（单位 %u）非标准表项布局（EntryLBA=%lu N=%u 表项大小=%u），跳过\n",
                     (UINT64)Unit, Header.PartitionEntryLBA,
                     Header.NumberOfPartitionEntries, Header.SizeOfPartitionEntry);
      continue;
    }

    ReadBytes  = Header.NumberOfPartitionEntries * sizeof (EFI_PARTITION_ENTRY);
    ReadBytes  = ALIGN_VALUE (ReadBytes, (UINTN)Media->BlockSize);
    EntryOffset = Header.PartitionEntryLBA * (UINT64)Unit;

    if ((ReadBytes > sizeof (mChunkB)) || ((EntryOffset + ReadBytes) > MediaBytes)) {
      continue;
    }

    Status = AutoReturnReadBlocks (BlockIo, EntryOffset, ReadBytes, mChunkB);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Raw = mChunkB;
    for (EntryIndex = 0; EntryIndex < Header.NumberOfPartitionEntries; EntryIndex++) {
      Entry = (EFI_PARTITION_ENTRY *)(Raw + EntryIndex * sizeof (EFI_PARTITION_ENTRY));
      if (IsZeroGuid (&Entry->PartitionTypeGUID)) {
        continue;
      }

      for (CharIndex = 0; Name[CharIndex] != L'\0'; CharIndex++) {
        if (Entry->PartitionName[CharIndex] != Name[CharIndex]) {
          break;
        }
      }

      //
      // 名字必须完整匹配，且到此为止（GPT 名字不足 36 字符时补 0）
      //
      if ((Name[CharIndex] != L'\0') || (Entry->PartitionName[CharIndex] != L'\0')) {
        continue;
      }

      StartByte = Entry->StartingLBA * (UINT64)Unit;
      if (StartByte == ExpectedByteOffset) {
        *OutSizeBytes = (Entry->EndingLBA - Entry->StartingLBA + 1) * (UINT64)Unit;
        *OutStartLba  = Entry->StartingLBA;
        *OutEndLba    = Entry->EndingLBA;
        *OutLbaUnit   = Unit;
        return EFI_SUCCESS;
      }

      AutoReturnLog (L"名字 %s 命中但偏移不符：GPT 推算 0x%lx（单位 %u，LBA %lu..%lu），实测 0x%lx\n",
                     Name, StartByte, (UINT64)Unit, Entry->StartingLBA, Entry->EndingLBA,
                     ExpectedByteOffset);
    }
  }

  return EFI_NOT_FOUND;
}

/**
  定位一个分区：先用 GPT 表项名 + 实测绝对偏移（主方案），再用实测偏移处的内容 magic（回退），
  最后用介质容量做边界检查。

  @param  ProbeMagic  回退方案与内容校验用的 8 字节 magic（"STOCKBOOT" / "ANDROID!"）
  @param  RequiredBytes  该分区必须容纳的字节数（从 ByteOffset 起算）
**/
STATIC
EFI_STATUS
AutoReturnLocatePartition (
  IN  CONST CHAR16           *Name,
  IN  UINT64                  ExpectedByteOffset,
  IN  UINT64                  RequiredBytes,
  IN  CONST CHAR8            *ProbeMagic,
  OUT AUTO_RETURN_PARTITION  *Result
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       NoHandles;
  UINTN       Index;
  UINTN       LunCount;

  ZeroMem (Result, sizeof (*Result));
  Result->Name       = Name;
  Result->ByteOffset = ExpectedByteOffset;

  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &NoHandles, &Handles);
  if (EFI_ERROR (Status)) {
    AutoReturnLog (L"找不到任何 BlockIO 句柄：%r\n", Status);
    return Status;
  }

  LunCount = 0;

  //
  // 第一遍：LUN 级句柄列表 + GPT 表项名匹配
  //
  for (Index = 0; Index < NoHandles; Index++) {
    EFI_BLOCK_IO_PROTOCOL  *BlockIo;
    EFI_BLOCK_IO_MEDIA     *Media;
    UINT64                  MediaBytes;
    UINT64                  SizeBytes;
    UINT64                  StartLba;
    UINT64                  EndLba;
    UINTN                   LbaUnit;

    Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (EFI_ERROR (Status) || (BlockIo == NULL) || (BlockIo->Media == NULL)) {
      continue;
    }

    Media = BlockIo->Media;

    //
    // 只要 LUN 级句柄（PartitionDxe 生成的分区子句柄 LogicalPartition = TRUE）
    //
    if (Media->LogicalPartition || (Media->BlockSize == 0) ||
        ((Media->BlockSize & (Media->BlockSize - 1)) != 0))
    {
      continue;
    }

    //
    // 只考虑可以用 4K 对齐做读写的介质（写路径里所有偏移/长度都是 4K 的整数倍）
    //
    if ((AUTO_RETURN_BLOCK_BYTES % Media->BlockSize) != 0) {
      continue;
    }

    MediaBytes = (Media->LastBlock + 1) * (UINT64)Media->BlockSize;
    LunCount++;
    AutoReturnLog (L"BlockIO LUN #%u：BlockSize=%u LastBlock=0x%lx 介质=0x%lx 字节%s%s\n",
                   (UINT64)LunCount, Media->BlockSize, Media->LastBlock, MediaBytes,
                   Media->ReadOnly ? L"（只读）" : L"",
                   Media->MediaPresent ? L"" : L"（MediaPresent=FALSE）");

    if (MediaBytes < (ExpectedByteOffset + RequiredBytes)) {
      continue;
    }

    Status = AutoReturnGptFindPartition (BlockIo, Name, ExpectedByteOffset,
                                        &SizeBytes, &StartLba, &EndLba, &LbaUnit);
    if (EFI_ERROR (Status)) {
      continue;
    }

    if (SizeBytes < RequiredBytes) {
      AutoReturnLog (L"分区 %s 只有 0x%lx 字节（需要 0x%lx），拒绝使用\n",
                     Name, SizeBytes, RequiredBytes);
      continue;
    }

    Result->BlockIo  = BlockIo;
    Result->SizeBytes = SizeBytes;
    Result->StartLba = StartLba;
    Result->EndLba   = EndLba;
    Result->LbaUnit  = LbaUnit;
    Result->FromGpt  = TRUE;

    AutoReturnLog (L"分区 %s：GPT 表项名匹配（LBA %lu..%lu × %u 字节 = 0x%lx 起，0x%lx 字节；"
                   L"BlockSize=%u LastBlock=0x%lx）\n",
                   Name, StartLba, EndLba, (UINT64)LbaUnit, ExpectedByteOffset, SizeBytes,
                   Media->BlockSize, Media->LastBlock);
    FreePool (Handles);
    return EFI_SUCCESS;
  }

  //
  // 第二遍：退化为实测绝对偏移处的内容 magic（仍要求介质容量足够）
  //
  AutoReturnLog (L"分区 %s：GPT 名匹配失败，尝试内容 magic 回退\n", Name);

  for (Index = 0; Index < NoHandles; Index++) {
    EFI_BLOCK_IO_PROTOCOL  *BlockIo;
    EFI_BLOCK_IO_MEDIA     *Media;
    UINT64                  MediaBytes;

    Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (EFI_ERROR (Status) || (BlockIo == NULL) || (BlockIo->Media == NULL)) {
      continue;
    }

    Media = BlockIo->Media;
    if (Media->LogicalPartition ||
        ((Media->BlockSize == 0) || ((AUTO_RETURN_BLOCK_BYTES % Media->BlockSize) != 0)))
    {
      continue;
    }

    MediaBytes = (Media->LastBlock + 1) * (UINT64)Media->BlockSize;
    if (MediaBytes < (ExpectedByteOffset + RequiredBytes)) {
      continue;
    }

    Status = AutoReturnReadBlocks (BlockIo, ExpectedByteOffset, AUTO_RETURN_BLOCK_BYTES, mChunkA);
    if (EFI_ERROR (Status)) {
      continue;
    }

    if (CompareMem (mChunkA, ProbeMagic, 8) != 0) {
      continue;
    }

    Result->BlockIo   = BlockIo;
    Result->SizeBytes = MediaBytes - ExpectedByteOffset;
    Result->StartLba  = ExpectedByteOffset / Media->BlockSize;
    Result->EndLba    = Media->LastBlock;
    Result->LbaUnit   = Media->BlockSize;
    Result->FromGpt   = FALSE;

    AutoReturnLog (L"分区 %s：内容 magic 回退命中（0x%lx 处 = \"%a\"；BlockSize=%u LastBlock=0x%lx；"
                   L"仅按介质容量做边界 0x%lx 字节）\n",
                   Name, ExpectedByteOffset, ProbeMagic, Media->BlockSize, Media->LastBlock,
                   Result->SizeBytes);
    FreePool (Handles);
    return EFI_SUCCESS;
  }

  FreePool (Handles);
  AutoReturnLog (L"分区 %s：两种方案都没定位到（期望偏移 0x%lx）\n", Name, ExpectedByteOffset);
  return EFI_NOT_FOUND;
}

/**
  把当前 RAM 镜像里的诊断块整块写回 patch+0x8FC0000（patch 未定位到时静默跳过）。
  本函数不分配内存，可在事件通知里安全调用。
**/
STATIC
VOID
AutoReturnDiagFlush (
  VOID
  )
{
  EFI_TPL                EntryTpl;
  AUTO_RETURN_PARTITION  *Patch;

  if (!mDiagInitialized) {
    return;
  }

  if (EFI_ERROR (AutoReturnGetPatch (FALSE, &Patch))) {
    return;
  }

  EntryTpl = AutoReturnBeginIo ();
  AutoReturnWriteBlocks (Patch->BlockIo, Patch->ByteOffset + AUTO_RETURN_DIAG_OFFSET,
                         AUTO_RETURN_DIAG_BYTES, &mDiag);
  AutoReturnEndIo (EntryTpl);
}

/**
  记录一个阶段：更新 RAM 镜像里的诊断块，并尽力整块写回 patch+0x8FC0000。

  这是"事后可读"的唯一通道：即使控制台看不到任何东西，读这一块就知道驱动跑到哪一步。
  本函数不分配内存；patch 还没定位到时记录只留在 RAM 里（之后任何一次 Flush 会整批写盘）。

  @param  Stage   阶段编号（AUTO_RETURN_DIAG_STAGE_*）
  @param  Status  该阶段的 EFI_STATUS；非 EFI_SUCCESS 时记录带 FAIL_BIT 且写入 LastStatus
**/
STATIC
VOID
AutoReturnDiagRecord (
  IN UINT32      Stage,
  IN EFI_STATUS  Status
  )
{
  UINT32  Tick;
  UINT32  Stored;

  //
  // 时间戳：CNTVCT_EL0 低 32 位（19.2MHz ⇒ 约 224 秒回绕）
  //
  Tick = (UINT32)ArmReadCntvCt ();

  if (!mDiagInitialized) {
    ZeroMem (&mDiag, sizeof (mDiag));
    CopyMem (mDiag.Magic, AUTO_RETURN_DIAG_MAGIC, 8);
    mDiag.Version    = AUTO_RETURN_DIAG_VERSION;
    mDiag.FirstTick  = Tick;
    mDiagInitialized = TRUE;
  }

  Stored = Stage;
  if (EFI_ERROR (Status)) {
    Stored |= AUTO_RETURN_DIAG_FAIL_BIT;
    mDiag.LastStatus      = (UINT32)Status;
    mDiag.LastStatusStage = Stage;
  }

  mDiag.Sequence++;
  mDiag.LatestStage = Stored;
  mDiag.LatestTick  = Tick;

  if (Stage < AUTO_RETURN_DIAG_STAGE_COUNT) {
    mDiag.StageCounts[Stage]++;
  }

  if (mDiag.RecordCount < ARRAY_SIZE (mDiag.Records)) {
    mDiag.Records[mDiag.RecordCount].Stage = Stored;
    mDiag.Records[mDiag.RecordCount].Tick  = Tick;
    mDiag.RecordCount++;
  }

  AutoReturnDiagFlush ();
}

/**
  取 patch 分区（带缓存）。缓存很重要：定位要枚举 BlockIO 句柄并读各 LUN 的 GPT，
  代价大且每次都有分配；诊断块 flush 等高频路径只应从缓存取。

  @param  AllowLocate  FALSE 时只用缓存（不重新枚举句柄）
**/
STATIC
EFI_STATUS
AutoReturnGetPatch (
  IN  BOOLEAN                  AllowLocate,
  OUT AUTO_RETURN_PARTITION  **Out
  )
{
  EFI_STATUS  Status;

  if (mPatchValid) {
    *Out = &mPatch;
    return EFI_SUCCESS;
  }

  if (!AllowLocate) {
    return EFI_NOT_FOUND;
  }

  Status = AutoReturnLocatePartition (
             L"patch",
             AUTO_RETURN_PATCH_BYTE_OFFSET,
             AUTO_RETURN_PATCH_REQUIRED_BYTES,
             AUTO_RETURN_STOCK_MARKER,
             &mPatch
             );
  if (EFI_ERROR (Status)) {
    AutoReturnDiagRecord (AUTO_RETURN_DIAG_STAGE_PATCH_FOUND, Status);
    return Status;
  }

  //
  // 内容校验：patch+0 = "STOCKBOOT"，patch+8 = "ANDROID!"（原厂 boot 镜像）
  //
  if (AutoReturnReadBlocks (mPatch.BlockIo, AUTO_RETURN_PATCH_BYTE_OFFSET + AUTO_RETURN_STOCK_OFFSET,
                            AUTO_RETURN_BLOCK_BYTES, mChunkA) != EFI_SUCCESS)
  {
    AutoReturnLog (L"patch 首块读取失败，放弃缓存\n");
    return EFI_DEVICE_ERROR;
  }

  if ((CompareMem (mChunkA, AUTO_RETURN_STOCK_MARKER, 8) != 0) ||
      (CompareMem (mChunkA + AUTO_RETURN_STOCK_MARKER_BYTES, AUTO_RETURN_BOOT_MAGIC, 8) != 0))
  {
    AutoReturnLog (L"patch 内容校验失败（+0 应为 \"STOCKBOOT\"、+8 应为 \"ANDROID!\"），拒绝使用\n");
    return EFI_VOLUME_CORRUPTED;
  }

  mPatchValid = TRUE;
  *Out        = &mPatch;

  //
  // 缓存就绪 ⇒ 诊断块从这里开始能落盘（此前记录的阶段会整批写出）
  //
  AutoReturnDiagRecord (AUTO_RETURN_DIAG_STAGE_PATCH_FOUND, EFI_SUCCESS);

  return EFI_SUCCESS;
}

/**
  取 boot 分区（同样带缓存；AllowLocate=FALSE 时不重新枚举句柄）。

  @param  AllowLocate  FALSE 时只用缓存
**/
STATIC
EFI_STATUS
AutoReturnGetBoot (
  IN  BOOLEAN                  AllowLocate,
  OUT AUTO_RETURN_PARTITION  **Out
  )
{
  EFI_STATUS  Status;

  if (mBootValid) {
    *Out = &mBoot;
    return EFI_SUCCESS;
  }

  if (!AllowLocate) {
    return EFI_NOT_FOUND;
  }

  Status = AutoReturnLocatePartition (L"boot", AUTO_RETURN_BOOT_BYTE_OFFSET,
                                      AUTO_RETURN_STOCK_IMAGE_BYTES,
                                      AUTO_RETURN_BOOT_MAGIC, &mBoot);
  if (EFI_ERROR (Status)) {
    AutoReturnDiagRecord (AUTO_RETURN_DIAG_STAGE_BOOT_FOUND, Status);
    return Status;
  }

  mBootValid = TRUE;
  *Out       = &mBoot;

  AutoReturnDiagRecord (AUTO_RETURN_DIAG_STAGE_BOOT_FOUND, EFI_SUCCESS);

  return EFI_SUCCESS;
}

/**
  回写原厂 boot：patch+8 起 24MB → boot+0。
  源偏移（+8）不按块对齐，因此每块先从对齐位置读到 mChunkA，再平移 8 字节到 mChunkB 写出。

  @param  AllowLocate  FALSE = 只用已缓存的 patch/boot，不重新枚举句柄
**/
STATIC
EFI_STATUS
AutoReturnRestoreStockBoot (
  IN CONST CHAR16  *Reason,
  IN BOOLEAN        AllowLocate
  )
{
  AUTO_RETURN_PARTITION  *Boot;
  AUTO_RETURN_PARTITION  *Patch;
  EFI_STATUS             Status;
  UINT64                 SourceStart;
  UINT64                 SourcePad;
  UINT64                 Offset;
  UINTN                  ChunkLen;
  UINTN                  ReadLen;
  UINT64                 SumSrc;
  UINT64                 SumDst;
  UINTN                  Index;

  if (mStockBootRestored) {
    return EFI_SUCCESS;
  }

  if (mStockBootRestoreRunning) {
    return EFI_ALREADY_STARTED;
  }

  mStockBootRestoreRunning = TRUE;
  SumSrc                   = 0;
  SumDst                   = 0;
  Status                   = EFI_SUCCESS;

  AutoReturnLog (L"=== 回写原厂 boot（触发：%s）===\n", Reason);

  Status = AutoReturnGetPatch (AllowLocate, &Patch);
  if (EFI_ERROR (Status)) {
    AutoReturnLog (L"patch 定位失败：%r（稍后由 BlockIO 通知重试）\n", Status);
    goto Done;
  }

  Status = AutoReturnGetBoot (AllowLocate, &Boot);
  if (EFI_ERROR (Status)) {
    AutoReturnLog (L"boot 定位失败：%r\n", Status);
    goto Done;
  }

  SourceStart = Patch->ByteOffset + AUTO_RETURN_STOCK_OFFSET + AUTO_RETURN_STOCK_MARKER_BYTES;   // patch+8
  SourcePad   = SourceStart % Patch->BlockIo->Media->BlockSize;

  AutoReturnLog (L"源 patch+8 → 目标 boot+0，长度 0x%lx（分 %u 块）\n",
                 AUTO_RETURN_STOCK_IMAGE_BYTES,
                 (UINT64)((AUTO_RETURN_STOCK_IMAGE_BYTES + AUTO_RETURN_CHUNK_BYTES - 1) / AUTO_RETURN_CHUNK_BYTES));

  for (Offset = 0; Offset < AUTO_RETURN_STOCK_IMAGE_BYTES; Offset += AUTO_RETURN_CHUNK_BYTES) {
    ChunkLen = (UINTN)MIN ((UINT64)AUTO_RETURN_CHUNK_BYTES,
                           AUTO_RETURN_STOCK_IMAGE_BYTES - Offset);
    ReadLen  = (UINTN)ALIGN_VALUE ((UINT64)ChunkLen + SourcePad, Patch->BlockIo->Media->BlockSize);

    Status = AutoReturnReadBlocks (Patch->BlockIo, SourceStart + Offset - SourcePad,
                                   ReadLen, mChunkA);
    if (EFI_ERROR (Status)) {
      AutoReturnLog (L"读 patch 失败（offset 0x%lx）：%r\n", SourceStart + Offset, Status);
      goto Done;
    }

    CopyMem (mChunkB, mChunkA + SourcePad, ChunkLen);

    Status = AutoReturnWriteBlocks (Boot->BlockIo, Boot->ByteOffset + Offset, ChunkLen, mChunkB);
    if (EFI_ERROR (Status)) {
      AutoReturnLog (L"写 boot 失败（offset 0x%lx）：%r\n", Boot->ByteOffset + Offset, Status);
      goto Done;
    }

    for (Index = 0; Index < ChunkLen; Index++) {
      SumSrc += mChunkB[Index];
    }

    if (((Offset / AUTO_RETURN_CHUNK_BYTES) % 16) == 0) {
      AutoReturnLog (L"…写回进度 0x%lx / 0x%lx\n", Offset, AUTO_RETURN_STOCK_IMAGE_BYTES);
    }
  }

  //
  // 回读校验：boot 前 24MB 与写入数据逐块校验和比对
  //
  for (Offset = 0; Offset < AUTO_RETURN_STOCK_IMAGE_BYTES; Offset += AUTO_RETURN_CHUNK_BYTES) {
    ChunkLen = (UINTN)MIN ((UINT64)AUTO_RETURN_CHUNK_BYTES,
                           AUTO_RETURN_STOCK_IMAGE_BYTES - Offset);

    Status = AutoReturnReadBlocks (Boot->BlockIo, Boot->ByteOffset + Offset, ChunkLen, mChunkA);
    if (EFI_ERROR (Status)) {
      AutoReturnLog (L"回读 boot 失败（offset 0x%lx）：%r\n", Boot->ByteOffset + Offset, Status);
      goto Done;
    }

    for (Index = 0; Index < ChunkLen; Index++) {
      SumDst += mChunkA[Index];
    }
  }

  if (SumSrc != SumDst) {
    AutoReturnLog (L"回读校验不一致：src=0x%lx dst=0x%lx（boot 内容可疑）\n", SumSrc, SumDst);
    Status = EFI_VOLUME_CORRUPTED;
    goto Done;
  }

  mStockBootRestored = TRUE;
  AutoReturnLog (L"回写完成并回读校验一致（校验和 0x%lx）：boot = 原厂镜像，复位即回 Android\n",
                 SumDst);
  AutoReturnDiagRecord (AUTO_RETURN_DIAG_STAGE_RESTORE_DONE, EFI_SUCCESS);
  Status = EFI_SUCCESS;

Done:
  mStockBootRestoreRunning = FALSE;
  if (EFI_ERROR (Status) && (Status != EFI_ALREADY_STARTED)) {
    AutoReturnDiagRecord (AUTO_RETURN_DIAG_STAGE_RESTORE_DONE, Status);
    AutoReturnLog (L"回写未完成（%r），会在 BlockIO 通知/180s 定时器里重试\n", Status);
  }

  return Status;
}

/**
  截屏 dump：帧缓冲 → patch + 128MB（头 1 个块 + 像素）。
**/
STATIC
EFI_STATUS
AutoReturnDumpFramebuffer (
  IN CONST CHAR16  *Reason,
  IN BOOLEAN        AllowLocate
  )
{
  AUTO_RETURN_PARTITION   *Patch;
  AUTO_RETURN_FB_HEADER   Header;
  EFI_STATUS              Status;
  UINT32                  Width;
  UINT32                  Height;
  UINT32                  Stride;
  UINT32                  Pixels;
  UINT64                  Base;
  UINT64                  PixelOffset;
  UINT64                  Offset;
  UINTN                   ChunkLen;

  Width  = PcdGet32 (PcdMipiFrameBufferWidth);
  Height = PcdGet32 (PcdMipiFrameBufferHeight);
  Base   = PcdGet32 (PcdMipiFrameBufferAddress);

  if ((Width == 0) || (Height == 0) || (Base == 0)) {
    AutoReturnLog (L"帧缓冲 PCD 不完整（base=0x%lx %ux%u），跳过 dump\n",
                   Base, Width, Height);
    AutoReturnDiagRecord (AUTO_RETURN_DIAG_STAGE_DUMP_DONE, EFI_NOT_FOUND);
    return EFI_NOT_FOUND;
  }

  Stride = Width * 4;                       // BGRA
  Pixels = Stride * Height;                 // 16,384,000

  AutoReturnLog (L"=== 截屏 dump（触发：%s）===\n", Reason);
  AutoReturnLog (L"帧缓冲 0x%lx %ux%u stride=%u 像素字节=%u\n", Base, Width, Height, Stride, Pixels);

  Status = AutoReturnGetPatch (AllowLocate, &Patch);
  if (EFI_ERROR (Status)) {
    AutoReturnLog (L"patch 定位失败：%r，dump 放弃\n", Status);
    AutoReturnDiagRecord (AUTO_RETURN_DIAG_STAGE_DUMP_DONE, Status);
    return Status;
  }

  //
  // 头：4096 字节块，前 24 字节是 FBDUMP01 + width/height/stride/size（小端），其余补 0
  //
  ZeroMem (&Header, sizeof (Header));
  CopyMem (Header.Magic, AUTO_RETURN_FB_MAGIC, 8);
  Header.Width  = Width;
  Header.Height = Height;
  Header.Stride = Stride;
  Header.Size   = Pixels;

  ZeroMem (mChunkB, AUTO_RETURN_BLOCK_BYTES);
  CopyMem (mChunkB, &Header, sizeof (Header));

  Status = AutoReturnWriteBlocks (Patch->BlockIo, Patch->ByteOffset + AUTO_RETURN_FB_DUMP_OFFSET,
                                  AUTO_RETURN_BLOCK_BYTES, mChunkB);
  if (EFI_ERROR (Status)) {
    AutoReturnLog (L"写 dump 头失败：%r\n", Status);
    AutoReturnDiagRecord (AUTO_RETURN_DIAG_STAGE_DUMP_DONE, Status);
    return Status;
  }

  PixelOffset = Patch->ByteOffset + AUTO_RETURN_FB_DUMP_OFFSET + AUTO_RETURN_FB_HEADER_BYTES;

  for (Offset = 0; Offset < Pixels; Offset += AUTO_RETURN_CHUNK_BYTES) {
    ChunkLen = (UINTN)MIN ((UINT64)AUTO_RETURN_CHUNK_BYTES, (UINT64)Pixels - Offset);

    //
    // 经 bounce buffer 写：不直接把帧缓冲交给存储驱动做 DMA/缓存维护
    //
    CopyMem (mChunkB, (VOID *)(UINTN)(Base + Offset), ChunkLen);

    Status = AutoReturnWriteBlocks (Patch->BlockIo, PixelOffset + Offset, ChunkLen, mChunkB);
    if (EFI_ERROR (Status)) {
      AutoReturnLog (L"写 dump 像素失败（offset 0x%lx）：%r\n", Offset, Status);
      AutoReturnDiagRecord (AUTO_RETURN_DIAG_STAGE_DUMP_DONE, Status);
      return Status;
    }

    if (((Offset / AUTO_RETURN_CHUNK_BYTES) % 16) == 0) {
      AutoReturnLog (L"…dump 进度 0x%lx / 0x%lx\n", Offset, (UINT64)Pixels);
    }
  }

  AutoReturnLog (L"dump 完成：patch+0x%lx 头（\"%a\" %ux%u stride=%u size=%u），"
                 L"patch+0x%lx 起像素 0x%lx 字节\n",
                 AUTO_RETURN_FB_DUMP_OFFSET, AUTO_RETURN_FB_MAGIC, Width, Height, Stride, Pixels,
                 AUTO_RETURN_FB_DUMP_OFFSET + AUTO_RETURN_FB_HEADER_BYTES, (UINT64)Pixels);
  AutoReturnDiagRecord (AUTO_RETURN_DIAG_STAGE_DUMP_DONE, EFI_SUCCESS);
  return EFI_SUCCESS;
}

/**
  180 秒定时器（兜底）：覆盖"SimpleInit / 内核早期卡死"——dump 截屏 → 补回写 → 复位。
  本平台定时器事件可能不派发，因此这不是主路径（主路径见 ReadyToBoot 通知）。
  注意：ReadyToBoot 通知里会主动取消本定时器 —— 之后（内核已 StartImage）任何定时器
  回调都不允许再做存储 I/O，否则又会落进内核的 ExitBootServices 窗口。
**/
STATIC
VOID
EFIAPI
AutoReturnTimeoutNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_TPL  EntryTpl;

  gBS->SetTimer (mTimeoutEvent, TimerCancel, 0);

  if (mTimerFired || mReadyToBoot) {
    AutoReturnLog (L"定时器重复触发/已过 ReadyToBoot，忽略\n");
    return;
  }

  mTimerFired = TRUE;
  AutoReturnLog (L"*** 180 秒定时器到期（%u 秒）：截屏 dump → 回写 → 复位 ***\n",
                 AUTO_RETURN_TIMEOUT_SECONDS);
  AutoReturnDiagRecord (AUTO_RETURN_DIAG_STAGE_TIMER_FIRED, EFI_SUCCESS);

  EntryTpl = AutoReturnBeginIo ();
  AutoReturnDumpFramebuffer (L"180s timeout", TRUE);
  if (!mStockBootRestored) {
    AutoReturnRestoreStockBoot (L"180s timeout 兜底", TRUE);
  }

  AutoReturnEndIo (EntryTpl);

  AutoReturnDiagRecord (AUTO_RETURN_DIAG_STAGE_RESET_CALLED, EFI_SUCCESS);
  AutoReturnLog (L"ResetSystem(EfiResetWarm)\n");
  gRT->ResetSystem (EfiResetWarm, EFI_SUCCESS, 0, NULL);

  AutoReturnLog (L"ResetSystem 意外返回（未复位）\n");
}

/**
  ReadyToBoot：内核启动前最后一刻（主路径）。依次：① 截屏 dump ② 回写原厂 boot
  ③ 取消 180s 定时器 ④ 不复位（由内核接管；回写只为"之后任何复位都回 Android"）。

  ★ 本驱动不注册 ExitBootServices 事件：任何存储 I/O 都不能落在内核
  "GetMemoryMap → ExitBootServices" 的窗口里（真机实测：EBS 通知里的存储 I/O 会让
  内核的 ExitBootServices 失败、内核启动中止）。本函数返回后启动服务仍然完全可用，
  可安全使用 AllocatePool/LocateHandleBuffer（与旧 EBS 路径不同）。

  每次 signal 都会重做 dump（后者覆盖前者）：BDS 启动 SimpleInit 前一次，
  SimpleInit 真正 StartImage 内核前再一次 —— 最后一次 = 内核启动前的屏幕。
  回写/校验只做一次（mStockBootRestored 后跳过）。
**/
STATIC
VOID
EFIAPI
AutoReturnReadyToBootNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_TPL  EntryTpl;

  mReadyToBoot = TRUE;

  //
  // 先取消 180s 定时器：内核 StartImage 之后绝不能再有定时器回调做存储 I/O
  // （否则又会落进内核的 ExitBootServices 窗口）；同时也避免下面降 TPL 做长 IO 时被复位。
  //
  if (mTimeoutEvent != NULL) {
    gBS->SetTimer (mTimeoutEvent, TimerCancel, 0);
  }

  AutoReturnLog (L"*** ReadyToBoot：截屏 dump + 回写原厂 boot（不复位）***\n");
  AutoReturnLog (L"原厂 boot 回写状态：%s\n",
                 mStockBootRestored ? L"已完成" : L"未完成（本次在这里回写）");
  AutoReturnDiagRecord (AUTO_RETURN_DIAG_STAGE_RTB_ENTER, EFI_SUCCESS);

  EntryTpl = AutoReturnBeginIo ();
  AutoReturnDumpFramebuffer (L"ReadyToBoot", TRUE);
  if (!mStockBootRestored) {
    AutoReturnRestoreStockBoot (L"ReadyToBoot", TRUE);
  }

  AutoReturnEndIo (EntryTpl);

  AutoReturnDiagRecord (AUTO_RETURN_DIAG_STAGE_RTB_DONE, EFI_SUCCESS);
  AutoReturnLog (L"ReadyToBoot 处理完毕（不复位，放行给内核）\n");
}

/**
  每出现一个新的 BlockIO 句柄（LUN/分区）都会被通知一次：磁盘刚连上就把原厂 boot 回写掉，
  并把诊断块里积压的阶段记录落盘。
  （payload 被 mkbootimg 打成 Android boot image 写进 boot 分区，本次启动已不需要 boot 内容。）
**/
STATIC
VOID
EFIAPI
AutoReturnBlockIoNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_TPL  EntryTpl;

  if (!mBlockIoSeen) {
    mBlockIoSeen = TRUE;
    AutoReturnDiagRecord (AUTO_RETURN_DIAG_STAGE_BLOCKIO_SEEN, EFI_SUCCESS);
  }

  if (mStockBootRestored || mStockBootRestoreRunning || mReadyToBoot) {
    return;
  }

  EntryTpl = AutoReturnBeginIo ();
  AutoReturnRestoreStockBoot (L"BlockIO 协议通知", TRUE);
  AutoReturnEndIo (EntryTpl);
}

/**
  DXE 驱动入口：注册 ReadyToBoot 通知（主路径）+ 180s 定时器（兜底）+ BlockIO 通知，
  初始化诊断块并尽力先回写一次。不注册 ExitBootServices（EBS 里绝不做 I/O）。
**/
EFI_STATUS
EFIAPI
AutoReturnDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_TPL     EntryTpl;

  //
  // 显式初始化状态（不依赖 BSS 零初始化）
  //
  mPatchValid              = FALSE;
  mBootValid               = FALSE;
  mStockBootRestored       = FALSE;
  mStockBootRestoreRunning = FALSE;
  mReadyToBoot             = FALSE;
  mTimerFired              = FALSE;
  mBlockIoSeen             = FALSE;
  mDiagInitialized         = FALSE;
  mTimeoutEvent            = NULL;
  mReadyToBootEvent        = NULL;
  mBlockIoNotifyEvent      = NULL;
  mBlockIoRegistration     = NULL;

  AutoReturnLog (L"AutoReturnDxe 入口：主路径=ReadyToBoot（dump+回写 patch+8→boot，不复位）、"
                 L"兜底=%u 秒定时器、诊断块 patch+0x%lx\n",
                 AUTO_RETURN_TIMEOUT_SECONDS, AUTO_RETURN_DIAG_OFFSET);
  AutoReturnLog (L"patch 期望偏移 0x%lx（4K 扇区 %lu）、boot 期望偏移 0x%lx（4K 扇区 %lu）、"
                 L"dump 落盘 patch+0x%lx\n",
                 AUTO_RETURN_PATCH_BYTE_OFFSET, AUTO_RETURN_PATCH_BYTE_OFFSET / 4096,
                 AUTO_RETURN_BOOT_BYTE_OFFSET, AUTO_RETURN_BOOT_BYTE_OFFSET / 4096,
                 AUTO_RETURN_FB_DUMP_OFFSET);

  //
  // 诊断块：第一条记录（patch 还看不到时先留在 RAM，等第一次能定位就整批落盘）
  //
  AutoReturnDiagRecord (AUTO_RETURN_DIAG_STAGE_ENTRY, EFI_SUCCESS);

  //
  // ReadyToBoot 事件（主路径）：内核 StartImage 之前抓屏幕 + 回写原厂 boot。
  // ★ 不注册 ExitBootServices —— EBS 里做存储 I/O 会破坏内核的 ExitBootServices（真机实测）。
  //
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  AutoReturnReadyToBootNotify,
                  NULL,
                  &gEfiEventReadyToBootGuid,
                  &mReadyToBootEvent
                  );
  if (EFI_ERROR (Status)) {
    AutoReturnLog (L"注册 ReadyToBoot 事件失败：%r\n", Status);
    AutoReturnDiagRecord (AUTO_RETURN_DIAG_STAGE_RTB_EVENT_OK, Status);
  } else {
    AutoReturnDiagRecord (AUTO_RETURN_DIAG_STAGE_RTB_EVENT_OK, EFI_SUCCESS);
  }

  //
  // 180 秒定时器（兜底；本平台可能不派发）
  //
  Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, AutoReturnTimeoutNotify, NULL, &mTimeoutEvent);
  if (EFI_ERROR (Status)) {
    AutoReturnLog (L"创建定时器事件失败：%r\n", Status);
    AutoReturnDiagRecord (AUTO_RETURN_DIAG_STAGE_TIMER_ARMED, Status);
  } else {
    Status = gBS->SetTimer (mTimeoutEvent, TimerRelative, AUTO_RETURN_TIMEOUT_UNITS);
    if (EFI_ERROR (Status)) {
      AutoReturnLog (L"设置 %u 秒定时器失败：%r\n", AUTO_RETURN_TIMEOUT_SECONDS, Status);
      AutoReturnDiagRecord (AUTO_RETURN_DIAG_STAGE_TIMER_ARMED, Status);
    } else {
      AutoReturnDiagRecord (AUTO_RETURN_DIAG_STAGE_TIMER_ARMED, EFI_SUCCESS);
    }
  }

  //
  // BlockIO 协议通知：磁盘（UFS LUN）一出现就回写原厂 boot
  //
  mBlockIoNotifyEvent = EfiCreateProtocolNotifyEvent (
                          &gEfiBlockIoProtocolGuid,
                          TPL_CALLBACK,
                          AutoReturnBlockIoNotify,
                          NULL,
                          &mBlockIoRegistration
                          );
  if (mBlockIoNotifyEvent == NULL) {
    AutoReturnLog (L"注册 BlockIO 协议通知失败\n");
  }

  //
  // 立即尽力尝试一次（磁盘若已连上就不用等通知）
  //
  EntryTpl = AutoReturnBeginIo ();
  AutoReturnRestoreStockBoot (L"driver entry", TRUE);
  AutoReturnEndIo (EntryTpl);

  return EFI_SUCCESS;
}
