/** @file
  MrrButtonsDxe —— Huawei MatePad Pro 10.8" 2021（MRR-W29 / SM8250）按键输入驱动。

  替代原来的预编译 ButtonsDxe（联想 P11 Pro 的 GPIO 硬编码固件）。本驱动的按键定义
  来自本机采集的设备树（备份 dtbo.bin / 合并后 live FDT），并且只使用平台 FV 内
  预编译 PmicDxe 通过 Qualcomm 标准 PMIC 协议暴露的接口，不直接操作 PMIC 寄存器。

  ── 本机按键（证据：research/dtb/mrr-w29_live.dtb 合并后 FDT） ──────────────────
    vol_up    /soc/gpio_keys/vol_up:  gpios = <&pm8150_gpios 0x06 0x01>（flag 1 = 低有效）
              → PM8150（SPMI USID0=主 PMIC）GPIO6，内部上拉，按下接地
    vol_down  /soc/spmi@c440000/qcom,pm8150@0/qcom,power-on@800/qcom,pon_2:
              qcom,pon-type = <1>（RESIN）linux,code = <0x72> = KEY_VOLUMEDOWN
    power     qcom,pon_1: qcom,pon-type = <0>（KPDPWR）linux,code = <0x74> = KEY_POWER

  ── 读取方式（复用 Qualcomm SM8250 参考实现，同 SoC 同 PMIC） ──────────────────
    QcomPkg/Drivers/ButtonsDxe/ButtonsDxe.c 与
    QcomPkg/SocPkg/8250/Library/ButtonsLib/ButtonsLib.c（boot_images BOOT.XF.3.3）：
      vol_up   = PmicGpio.IrqStatus(0, EFI_PM_GPIO_6, EFI_PM_IRQ_STATUS_RT) 取反（上拉，低有效）
      vol_down = PmicPwrOn.GetPonRtStatus(0, EFI_PM_PON_IRQ_RESIN_ON)
      power    = PmicPwrOn.GetPonRtStatus(0, EFI_PM_PON_IRQ_KPDPWR_ON)
    两个协议由 FV 内的预编译 PmicDxe 提供（Apriori 列表里位于本驱动之前），GUID 见下。

  ── EFI 键值选择依据（SimpleInit GUI 消费的是 EFI_SIMPLE_TEXT_INPUT_PROTOCOL） ──
    GPLDrivers/Library/SimpleInit/src/gui/drivers/uefi_keyboard.c：
      L80-84  SCAN_UP      → LV_KEY_PREV  （菜单上移）
      L85-89  SCAN_DOWN    → LV_KEY_NEXT  （菜单下移）
      L74/90  SCAN_SUSPEND → LV_KEY_ENTER （确认；SCAN_SUSPEND=0x0102 为本树
      Key.UnicodeChar = CHAR_CARRIAGE_RETURN;   // 让标准 UEFI 菜单也能"确认"
               MdePkg/Include/Protocol/SimpleTextInEx.h:140 的 Renegade 扩展码）
      L75/91  SCAN_ESC     → 返回（guiact_do_back），用作 vol_up+vol_down 组合键
    ⇒ 音量上=SCAN_UP，音量下=SCAN_DOWN，电源键=SCAN_SUSPEND（=确认），与参考实现
    Key.UnicodeChar = CHAR_CARRIAGE_RETURN;   // 让标准 UEFI 菜单也能"确认"
      的单键映射一致（ButtonsLib.c ConvertEfiKeyCode）。

  Copyright (c) 2026, mrr-w29 porting project. SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>

// 平铺复制的 Qualcomm boot_images 协议头（来源见各文件顶部注释）。
#include "EFIPmicGpio.h"
#include "EFIPmicPwrOn.h"

/* ── 协议 GUID ──────────────────────────────────────────────────────────
   gQcomPmicGpioProtocolGuid：EFIPmicGpio.h 中 EFI_PMIC_GPIO_PROTOCOL_GUID
   gQcomPmicPwrOnProtocolGuid：boot_images QcomPkg.dec 定义（PwrOn 头文件只 extern）。
   二者与平台内预编译 PmicDxe.efi 内嵌 GUID 完全一致（见仓库笔记）。 */
EFI_GUID  gQcomPmicGpioProtocolGuid  = EFI_PMIC_GPIO_PROTOCOL_GUID;
EFI_GUID  gQcomPmicPwrOnProtocolGuid =
  { 0x9ba45b66, 0xefa4, 0x441c, { 0xa3, 0xe4, 0xed, 0x22, 0x24, 0x78, 0x6b, 0xe2 } };

/* 本驱动的设备路径 GUID（仅用于把 SimpleTextIn 暴露给 console 聚合器）。 */
#define MRR_KEYPAD_DEVICE_GUID \
  { 0x7d9a1c4e, 0x3b52, 0x4f0d, { 0x9a, 0x61, 0x2c, 0x8e, 0x44, 0x17, 0xa3, 0xd8 } }

#define MRR_PMIC_DEVICE_PRIMARY  0               /* PM8150：SPMI USID0（DT: reg = <0 0>） */
#define MRR_VOL_UP_GPIO          EFI_PM_GPIO_6   /* DT: gpios = <&pm8150_gpios 6 ACTIVE_LOW> */
#define MRR_BUTTON_TIMER_US      50000           /* 50ms 轮询，与 Qualcomm 参考一致 */
#define MRR_KEY_BUFFER_SIZE      16

STATIC EFI_QCOM_PMIC_GPIO_PROTOCOL   *mPmicGpio;
STATIC EFI_QCOM_PMIC_PWRON_PROTOCOL  *mPmicPwrOn;

STATIC EFI_INPUT_KEY  mKeyBuffer[MRR_KEY_BUFFER_SIZE];
STATIC UINTN          mKeyRead;
STATIC UINTN          mKeyWrite;

/* 一次物理按压只产生一个 EFI 按键，全部松开后才重新触发（参考 ButtonsLib.c 的行为）。 */
STATIC BOOLEAN  mPressSessionActive;

STATIC EFI_EVENT  mWaitKeyEvt;
STATIC EFI_EVENT  mPollTimer;

typedef struct {
  EFI_HANDLE                NotifyHandle;
  EFI_KEY_DATA              KeyData;
  EFI_KEY_NOTIFY_FUNCTION   KeyNotificationFn;
  LIST_ENTRY                Link;
} MRR_KEY_NOTIFY;

STATIC LIST_ENTRY  mNotifyList;
STATIC BOOLEAN     mNotifyInited;

/* ─────────────────────────────── 按键读取 ─────────────────────────────── */

/**
  读 vol_up：PM8150 GPIO6 的实时（RT）状态。
  上拉 + 低有效：RT=1 表示松开，RT=0 表示按下（参考 ButtonsLib.c ReadGpioStatus 的取反）。
**/
STATIC
BOOLEAN
MrrVolUpPressed (
  VOID
  )
{
  BOOLEAN  RtStatus;

  if (mPmicGpio == NULL) {
    return FALSE;
  }
  RtStatus = FALSE;
  if (EFI_ERROR (mPmicGpio->IrqStatus (
                   MRR_PMIC_DEVICE_PRIMARY,
                   MRR_VOL_UP_GPIO,
                   EFI_PM_IRQ_STATUS_RT,
                   &RtStatus
                   )))
  {
    return FALSE;
  }
  return (BOOLEAN)!RtStatus;
}

/**
  读 PON 键（vol_down=RESIN / power=KPDPWR）的实时状态；按下为 TRUE。
**/
STATIC
BOOLEAN
MrrPonKeyPressed (
  IN EFI_PM_PON_IRQ_TYPE  IrqType
  )
{
  BOOLEAN  RtStatus;

  if (mPmicPwrOn == NULL) {
    return FALSE;
  }
  RtStatus = FALSE;
  if (EFI_ERROR (mPmicPwrOn->GetPonRtStatus (MRR_PMIC_DEVICE_PRIMARY, IrqType, &RtStatus))) {
    return FALSE;
  }
  return RtStatus;
}

/* ──────────────────────────────── 键队列 ─────────────────────────────── */

STATIC
BOOLEAN
MrrKeyBufferEmpty (
  VOID
  )
{
  return (BOOLEAN)(mKeyRead == mKeyWrite);
}

STATIC
VOID
MrrKeyBufferFlush (
  VOID
  )
{
  mKeyRead  = 0;
  mKeyWrite = 0;
}

STATIC
VOID
MrrKeyBufferPush (
  IN CONST EFI_INPUT_KEY  *Key
  )
{
  mKeyBuffer[mKeyWrite] = *Key;
  mKeyWrite = (mKeyWrite + 1) % MRR_KEY_BUFFER_SIZE;
  if (mKeyWrite == mKeyRead) {
    mKeyRead = (mKeyRead + 1) % MRR_KEY_BUFFER_SIZE;   /* 覆盖最旧按键 */
  }
}

STATIC
BOOLEAN
MrrKeyBufferPop (
  OUT EFI_INPUT_KEY  *Key
  )
{
  if (MrrKeyBufferEmpty ()) {
    return FALSE;
  }
  *Key   = mKeyBuffer[mKeyRead];
  mKeyRead = (mKeyRead + 1) % MRR_KEY_BUFFER_SIZE;
  return TRUE;
}

STATIC
VOID
MrrNotifyKey (
  IN EFI_KEY_DATA  *Data
  )
{
  LIST_ENTRY  *Link;
  MRR_KEY_NOTIFY  *Notify;

  if (!mNotifyInited) {
    return;
  }
  for (Link = GetFirstNode (&mNotifyList); !IsNull (&mNotifyList, Link); Link = GetNextNode (&mNotifyList, Link)) {
    Notify = BASE_CR (Link, MRR_KEY_NOTIFY, Link);
    if ((Notify->KeyData.Key.ScanCode == Data->Key.ScanCode) &&
        (Notify->KeyData.Key.UnicodeChar == Data->Key.UnicodeChar))
    {
      Notify->KeyNotificationFn (Data);
    }
  }
}

/* ─────────────────────────────── 轮询状态机 ───────────────────────────── */

/**
  采样三个按键；在按压前沿产生一个 EFI 按键（写入缓冲并触发通知）。
  @retval TRUE  本次采样产生了一个新按键
  @retval FALSE 无新按键
**/
STATIC
BOOLEAN
MrrButtonsPoll (
  VOID
  )
{
  BOOLEAN        VolUp;
  BOOLEAN        VolDown;
  BOOLEAN        Power;
  EFI_INPUT_KEY  Key;
  EFI_KEY_DATA   Data;

  VolUp   = MrrVolUpPressed ();
  VolDown = MrrPonKeyPressed (EFI_PM_PON_IRQ_RESIN_ON);
  Power   = MrrPonKeyPressed (EFI_PM_PON_IRQ_KPDPWR_ON);

  if (!(VolUp || VolDown || Power)) {
    mPressSessionActive = FALSE;
    return FALSE;
  }
  if (mPressSessionActive) {
    return FALSE;
  }
  mPressSessionActive = TRUE;

  ZeroMem (&Key, sizeof (Key));
  if (VolUp && VolDown) {
    Key.ScanCode = SCAN_ESC;              /* 组合键：返回（SimpleInit uefi_keyboard.c L75/91） */
  } else if (VolUp) {
    Key.ScanCode = SCAN_UP;               /* SimpleInit L80-84 → LV_KEY_PREV */
  } else if (VolDown) {
    Key.ScanCode = SCAN_DOWN;             /* SimpleInit L85-89 → LV_KEY_NEXT */
  } else {
    Key.ScanCode = SCAN_SUSPEND;          /* 电源键=确认：SimpleInit L74/90 → LV_KEY_ENTER */
    Key.UnicodeChar = CHAR_CARRIAGE_RETURN;   // 让标准 UEFI 菜单也能"确认"
  }
  Key.UnicodeChar = 0;

  Data.Key                     = Key;
  Data.KeyState.KeyShiftState  = 0;
  Data.KeyState.KeyToggleState = 0;

  MrrKeyBufferPush (&Key);
  MrrNotifyKey (&Data);
  DEBUG ((
    DEBUG_ERROR,
    "MrrButtonsDxe: key ScanCode=0x%02x (vol_up=%d vol_down=%d power=%d)\n",
    (UINTN)Key.ScanCode,
    (UINTN)VolUp,
    (UINTN)VolDown,
    (UINTN)Power
    ));
  return TRUE;
}

STATIC
VOID
EFIAPI
MrrOnWaitForKey (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  (VOID)Context;
  if (MrrButtonsPoll () || !MrrKeyBufferEmpty ()) {
    gBS->SignalEvent (Event);
  }
}

STATIC
VOID
EFIAPI
MrrOnPollTimer (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  (VOID)Event;
  (VOID)Context;
  if (MrrButtonsPoll ()) {
    gBS->SignalEvent (mWaitKeyEvt);
  }
}

/* ───────────────────── EFI_SIMPLE_TEXT_INPUT_PROTOCOL ─────────────────── */

STATIC
EFI_STATUS
EFIAPI
MrrTextInReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  IN BOOLEAN                         ExtendedVerification
  )
{
  (VOID)This;
  (VOID)ExtendedVerification;
  MrrKeyBufferFlush ();
  mPressSessionActive = FALSE;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MrrTextInReadKeyStroke (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  OUT EFI_INPUT_KEY                  *Key
  )
{
  EFI_TPL  OldTpl;

  (VOID)This;
  if (Key == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl = gBS->RaiseTPL (TPL_CALLBACK);
  MrrButtonsPoll ();
  if (!MrrKeyBufferPop (Key)) {
    gBS->RestoreTPL (OldTpl);
    return EFI_NOT_READY;
  }
  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL  mTextIn = {
  MrrTextInReset,
  MrrTextInReadKeyStroke,
  NULL                     /* WaitForKey：入口点里填 mWaitKeyEvt */
};

/* ──────────────────── EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL ───────────────── */

STATIC
EFI_STATUS
EFIAPI
MrrTextInExReset (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN BOOLEAN                            ExtendedVerification
  )
{
  (VOID)This;
  (VOID)ExtendedVerification;
  MrrKeyBufferFlush ();
  mPressSessionActive = FALSE;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MrrTextInExReadKeyStrokeEx (
  IN  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  OUT EFI_KEY_DATA                       *KeyData
  )
{
  if (KeyData == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  KeyData->KeyState.KeyShiftState  = 0;
  KeyData->KeyState.KeyToggleState = 0;
  return MrrTextInReadKeyStroke ((EFI_SIMPLE_TEXT_INPUT_PROTOCOL *)This, &KeyData->Key);
}

STATIC
EFI_STATUS
EFIAPI
MrrTextInExSetState (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN EFI_KEY_TOGGLE_STATE               *KeyToggleState
  )
{
  (VOID)This;
  (VOID)KeyToggleState;
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
MrrTextInExRegisterKeyNotify (
  IN  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN  EFI_KEY_DATA                       *KeyData,
  IN  EFI_KEY_NOTIFY_FUNCTION            KeyNotificationFunction,
  OUT EFI_HANDLE                         *NotifyHandle
  )
{
  LIST_ENTRY      *Link;
  MRR_KEY_NOTIFY  *Notify;

  (VOID)This;
  if ((KeyData == NULL) || (NotifyHandle == NULL) || (KeyNotificationFunction == NULL) || (!mNotifyInited)) {
    return EFI_INVALID_PARAMETER;
  }

  for (Link = GetFirstNode (&mNotifyList); !IsNull (&mNotifyList, Link); Link = GetNextNode (&mNotifyList, Link)) {
    Notify = BASE_CR (Link, MRR_KEY_NOTIFY, Link);
    if ((Notify->KeyData.Key.ScanCode == KeyData->Key.ScanCode) &&
        (Notify->KeyData.Key.UnicodeChar == KeyData->Key.UnicodeChar) &&
        (Notify->KeyNotificationFn == KeyNotificationFunction))
    {
      *NotifyHandle = Notify->NotifyHandle;
      return EFI_SUCCESS;
    }
  }

  Notify = AllocateZeroPool (sizeof (MRR_KEY_NOTIFY));
  if (Notify == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Notify->KeyNotificationFn = KeyNotificationFunction;
  Notify->NotifyHandle      = (EFI_HANDLE)Notify;
  CopyMem (&Notify->KeyData, KeyData, sizeof (EFI_KEY_DATA));
  InsertTailList (&mNotifyList, &Notify->Link);
  *NotifyHandle = Notify->NotifyHandle;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MrrTextInExUnregisterKeyNotify (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN EFI_HANDLE                         NotificationHandle
  )
{
  LIST_ENTRY      *Link;
  MRR_KEY_NOTIFY  *Notify;

  (VOID)This;
  if ((NotificationHandle == NULL) || (!mNotifyInited)) {
    return EFI_INVALID_PARAMETER;
  }
  for (Link = GetFirstNode (&mNotifyList); !IsNull (&mNotifyList, Link); Link = GetNextNode (&mNotifyList, Link)) {
    Notify = BASE_CR (Link, MRR_KEY_NOTIFY, Link);
    if (Notify->NotifyHandle == NotificationHandle) {
      RemoveEntryList (&Notify->Link);
      FreePool (Notify);
      return EFI_SUCCESS;
    }
  }
  return EFI_INVALID_PARAMETER;
}

STATIC EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  mTextInEx = {
  MrrTextInExReset,
  MrrTextInExReadKeyStrokeEx,
  NULL,                    /* WaitForKeyEx：入口点里填 mWaitKeyEvt */
  MrrTextInExSetState,
  MrrTextInExRegisterKeyNotify,
  MrrTextInExUnregisterKeyNotify
};

/* ─────────────────────────── 设备路径 / GPIO 配置 ─────────────────────── */

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH        Vendor;
  EFI_DEVICE_PATH_PROTOCOL  End;
} MRR_KEYPAD_DEVICE_PATH;
#pragma pack()

STATIC MRR_KEYPAD_DEVICE_PATH  mKeypadDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(sizeof (VENDOR_DEVICE_PATH)),
        (UINT8)((sizeof (VENDOR_DEVICE_PATH)) >> 8)
      }
    },
    MRR_KEYPAD_DEVICE_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      (UINT8)(sizeof (EFI_DEVICE_PATH_PROTOCOL)),
      (UINT8)((sizeof (EFI_DEVICE_PATH_PROTOCOL)) >> 8)
    }
  }
};

/**
  把 vol_up 引脚配成数字输入（仅当它还不是数字输入时）。
  正常启动路径里 XBL（华为 ABL）已在早期配好该按键引脚（上拉），此时不动它；
  需要兜底时按 Qualcomm SM8250 参考序列配置（ButtonsLib.c EnableInput：
  CfgMode/SetVoltageSource/SetOutDrvStr/SetOutSrcCfg；协议没有设置上拉的接口）。
**/
STATIC
EFI_STATUS
MrrConfigureVolUpGpio (
  VOID
  )
{
  EFI_PM_GPIO_STATUS_TYPE  GpioStatus;
  EFI_STATUS               Status;

  Status = mPmicGpio->StatusGet (MRR_PMIC_DEVICE_PRIMARY, MRR_VOL_UP_GPIO, &GpioStatus);
  if (!EFI_ERROR (Status) && GpioStatus.GpioEnable && (GpioStatus.GpioConfig == EFI_PM_GPIO_DIG_IN)) {
    DEBUG ((DEBUG_ERROR, "MrrButtonsDxe: VOL+ GPIO6 已是数字输入，沿用前级固件配置\n"));
    return EFI_SUCCESS;
  }

  Status = mPmicGpio->CfgMode (MRR_PMIC_DEVICE_PRIMARY, MRR_VOL_UP_GPIO, EFI_PM_GPIO_DIG_IN);
  if (!EFI_ERROR (Status)) {
    Status = mPmicGpio->SetVoltageSource (MRR_PMIC_DEVICE_PRIMARY, MRR_VOL_UP_GPIO, EFI_PM_GPIO_VIN0);
  }
  if (!EFI_ERROR (Status)) {
    Status = mPmicGpio->SetOutDrvStr (MRR_PMIC_DEVICE_PRIMARY, MRR_VOL_UP_GPIO, EFI_PM_GPIO_OUT_DRV_STR_LOW);
  }
  if (!EFI_ERROR (Status)) {
    Status = mPmicGpio->SetOutSrcCfg (MRR_PMIC_DEVICE_PRIMARY, MRR_VOL_UP_GPIO, EFI_PM_GPIO_SRC_GND);
  }
  return Status;
}

/* ─────────────────────────────── 入口点 ─────────────────────────────── */

EFI_STATUS
EFIAPI
MrrButtonsDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  (VOID)SystemTable;
  mPmicGpio  = NULL;
  mPmicPwrOn = NULL;

  Status = gBS->LocateProtocol (&gQcomPmicGpioProtocolGuid, NULL, (VOID **)&mPmicGpio);
  if (EFI_ERROR (Status)) {
    mPmicGpio = NULL;
    DEBUG ((DEBUG_ERROR, "MrrButtonsDxe: 未找到 PmicGpio 协议 (%r)，音量+ 不可用\n", Status));
  }
  Status = gBS->LocateProtocol (&gQcomPmicPwrOnProtocolGuid, NULL, (VOID **)&mPmicPwrOn);
  if (EFI_ERROR (Status)) {
    mPmicPwrOn = NULL;
    DEBUG ((DEBUG_ERROR, "MrrButtonsDxe: 未找到 PmicPwrOn 协议 (%r)，音量-/电源 不可用\n", Status));
  }
  if ((mPmicGpio == NULL) && (mPmicPwrOn == NULL)) {
    DEBUG ((DEBUG_ERROR, "MrrButtonsDxe: 两个 PMIC 协议都缺失，放弃安装输入设备\n"));
    return EFI_DEVICE_ERROR;
  }

  if (mPmicGpio != NULL) {
    Status = MrrConfigureVolUpGpio ();
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MrrButtonsDxe: VOL+ GPIO 配置失败 (%r)\n", Status));
    }
  }

  MrrKeyBufferFlush ();
  mPressSessionActive = FALSE;
  InitializeListHead (&mNotifyList);
  mNotifyInited = TRUE;

  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_CALLBACK, MrrOnWaitForKey, NULL, &mWaitKeyEvt);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, MrrOnPollTimer, NULL, &mPollTimer);
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mWaitKeyEvt);
    return Status;
  }
  Status = gBS->SetTimer (mPollTimer, TimerPeriodic, MRR_BUTTON_TIMER_US);
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mPollTimer);
    gBS->CloseEvent (mWaitKeyEvt);
    return Status;
  }

  mTextIn.WaitForKey    = mWaitKeyEvt;
  mTextInEx.WaitForKeyEx = mWaitKeyEvt;

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gEfiSimpleTextInProtocolGuid,
                  &mTextIn,
                  &gEfiSimpleTextInputExProtocolGuid,
                  &mTextInEx,
                  &gEfiDevicePathProtocolGuid,
                  &mKeypadDevicePath,
                  NULL
                  );
  DEBUG ((
    DEBUG_ERROR,
    "MrrButtonsDxe: installed (%r) — VOL+ = pm8150 GPIO6 active-low, VOL- = PON RESIN, PWR = PON KPDPWR\n",
    Status
    ));
  return Status;
}
