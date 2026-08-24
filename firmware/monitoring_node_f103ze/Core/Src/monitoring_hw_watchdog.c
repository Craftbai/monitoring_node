/* =============================================================================
 * monitoring_hw_watchdog.c - 硬件独立看门狗（IWDG）控制实现
 *
 * IWDG 工作原理：
 *   STM32F1 的 IWDG 由独立的 LSI 时钟（约 40kHz）驱动，一旦启动后无法
 *   停止，只能通过系统复位或上电复位清除。计数器从重载值递减到 0，
 *   到达 0 时触发系统复位。应用程序必须在超时前写入 RELOAD 键喂狗。
 *
 * Stop 模式问题：
 *   STM32F103 在 Stop 模式下，主时钟（HSE/HSI/PLL）停止，但 LSI 是否
 *   停振取决于芯片版本和温度条件。部分芯片 LSI 继续运行，导致 IWDG
 *   在 Stop 期间仍然计时。如果 RTC 周期（300 秒）> IWDG 超时（26 秒），
 *   系统会在 Stop 期间复位，无法实现低功耗。
 *
 * 当前策略：
 *   首版禁用 IWDG，仅使用软件看门狗（watchdog_task）监测任务心跳。
 *   软件看门狗在 Stop 前会检查状态，若任务无进展则拒绝看门狗许可，
 *   强制系统保持运行并输出诊断日志。
 * ============================================================================= */

#include "monitoring_hw_watchdog.h"

#include "monitoring_config.h"
#include "stm32f1xx_hal.h"

/* ===== IWDG 寄存器键值 ===== */
#define MONITOR_IWDG_KEY_ENABLE       0xCCCCU   /* 启动 IWDG */
#define MONITOR_IWDG_KEY_RELOAD       0xAAAAU   /* 重载计数器（喂狗） */
#define MONITOR_IWDG_KEY_WRITE        0x5555U   /* 允许写 PR/RLR */

/* ===== IWDG 配置参数 ===== */
/* 预分频器：256（LSI 40kHz / 256 ≈ 156Hz） */
#define MONITOR_IWDG_PRESCALER_256    6U

/* 重载值：4095（最大值，超时时间 = 4095 / 156Hz ≈ 26.2 秒） */
#define MONITOR_IWDG_RELOAD_MAX       0x0FFFU

/* ===== 静态变量 ===== */
/* IWDG 启用标志（Init 时设置，IsEnabled 查询） */
static uint8_t g_iwdg_enabled;

/* ===== 公开 API 实现 ===== */

/**
 * @brief  初始化硬件独立看门狗（IWDG）
 * @note   当前版本：MONITOR_HW_IWDG_ENABLED = 0，函数体为空
 * @note   若启用：按以下顺序配置 IWDG
 *         1. 写 0x5555 到 KR，允许写 PR/RLR
 *         2. 写预分频器到 PR（6 = 256 分频）
 *         3. 写重载值到 RLR（0x0FFF = 4095）
 *         4. 写 0xAAAA 到 KR，重载计数器
 *         5. 写 0xCCCC 到 KR，启动 IWDG（不可逆）
 */
void MonitoringHardwareWatchdog_Init(void)
{
  g_iwdg_enabled = 0U;

#if MONITOR_HW_IWDG_ENABLED
  /* 仅提供可控的初始化入口，实际 Stop 策略由上层门禁保证。 */
  IWDG->KR = MONITOR_IWDG_KEY_WRITE;
  IWDG->PR = MONITOR_IWDG_PRESCALER_256;
  IWDG->RLR = MONITOR_IWDG_RELOAD_MAX;
  IWDG->KR = MONITOR_IWDG_KEY_RELOAD;
  IWDG->KR = MONITOR_IWDG_KEY_ENABLE;
  g_iwdg_enabled = 1U;
#endif
}

/**
 * @brief  喂狗（重载 IWDG 计数器）
 * @note   只有 IWDG 已启用时才执行喂狗操作
 * @note   写 0xAAAA 到 KR，计数器从 RLR 重新开始递减
 */
void MonitoringHardwareWatchdog_Refresh(void)
{
  if (g_iwdg_enabled != 0U)
  {
    IWDG->KR = MONITOR_IWDG_KEY_RELOAD;
  }
}

/**
 * @brief  查询 IWDG 是否已启用
 * @return 1=已启用，0=未启用
 * @note   Stop 门禁检查会调用此函数，若已启用则拒绝进 Stop
 */
uint8_t MonitoringHardwareWatchdog_IsEnabled(void)
{
  return g_iwdg_enabled;
}
