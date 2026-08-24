/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    rtc.c
  * @brief   RTC 初始化与备份域管理（正点原子实验15适配版）
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "rtc.h"

/* USER CODE BEGIN 0 */
#include "stm32f1xx_hal_rtc_ex.h"

/* 实际生效的 RTC 时钟源。 */
uint8_t rtc_clk_source = RTC_CLK_NONE;

/* 本次启动是否需要重新设置 RTC 初始计数。 */
uint8_t rtc_cold_boot = 0U;

#define RTC_INITIAL_COUNTER 10U
#define RTC_REG_TIMEOUT_MS  100U

static uint8_t RTC_CounterIsValid(uint32_t counter)
{
  /* 0xFFFFFFFF 是复位/未初始化状态；其余 32 位值均可作为秒计数。 */
  return counter != 0xFFFFFFFFUL ? 1U : 0U;
}

static HAL_StatusTypeDef RTC_WaitRtoff(void)
{
  uint32_t tick_start = HAL_GetTick();

  while ((RTC->CRL & RTC_CRL_RTOFF) == 0U)
  {
    if ((HAL_GetTick() - tick_start) > RTC_REG_TIMEOUT_MS)
    {
      return HAL_TIMEOUT;
    }
  }
  return HAL_OK;
}

static HAL_StatusTypeDef RTC_WriteCounterRegister(uint32_t value,
                                                   uint8_t alarm_register)
{
  if (RTC_WaitRtoff() != HAL_OK)
  {
    return HAL_TIMEOUT;
  }

  /* 正点原子 F1 例程的写法：RTOFF -> CNF -> 高/低寄存器 -> 清 CNF。 */
  SET_BIT(RTC->CRL, RTC_CRL_CNF);
  if (alarm_register == 0U)
  {
    RTC->CNTH = (uint16_t)(value >> 16U);
    RTC->CNTL = (uint16_t)(value & 0xFFFFU);
  }
  else
  {
    RTC->ALRH = (uint16_t)(value >> 16U);
    RTC->ALRL = (uint16_t)(value & 0xFFFFU);
  }
  CLEAR_BIT(RTC->CRL, RTC_CRL_CNF);
  return RTC_WaitRtoff();
}

/* USER CODE END 0 */

RTC_HandleTypeDef hrtc;

/* RTC init function */
void MX_RTC_Init(void)
{

  /* USER CODE BEGIN RTC_Init 0 */

  /* USER CODE END RTC_Init 0 */

  RTC_TimeTypeDef sTime = {0};
  RTC_DateTypeDef DateToUpdate = {0};

  /* USER CODE BEGIN RTC_Init 1 */

  /* USER CODE END RTC_Init 1 */

  /** Initialize RTC Only
  */
  hrtc.Instance = RTC;
  hrtc.Init.AsynchPrediv = RTC_AUTO_1_SECOND;
  hrtc.Init.OutPut = RTC_OUTPUTSOURCE_ALARM;
  if (HAL_RTC_Init(&hrtc) != HAL_OK)
  {
    Error_Handler();
  }

  /* USER CODE BEGIN Check_RTC_BKUP */
  /*
   * CubeMX 的 RTC_Calendar 模式会在下面生成 HAL_RTC_SetTime/SetDate。
   * STM32F1 的这两个 HAL 接口会改写 RTC 32 位秒计数器，不能每次复位
   * 都执行。用用户代码区包住生成片段，重新生成代码后该保护仍会保留。
   * 首次上电的计数器初始化由 RTC_Init 2 中的备份域判断完成。
   */
  #if 0
  /* USER CODE END Check_RTC_BKUP */

  /** Initialize RTC and set the Time and Date
  */
  sTime.Hours = 0x0;
  sTime.Minutes = 0x0;
  sTime.Seconds = 0x10;

  if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BCD) != HAL_OK)
  {
    Error_Handler();
  }
  DateToUpdate.WeekDay = RTC_WEEKDAY_MONDAY;
  DateToUpdate.Month = RTC_MONTH_JANUARY;
  DateToUpdate.Date = 0x1;
  DateToUpdate.Year = 0x0;

  if (HAL_RTC_SetDate(&hrtc, &DateToUpdate, RTC_FORMAT_BCD) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN RTC_Init 2 */

  #endif
  /* 生成的日历变量仅属于被屏蔽的兼容片段，避免编译器产生未使用警告。 */
  (void)sTime;
  (void)DateToUpdate;

  /*
   * STM32F1 的 HAL 日历日期实际保存在 SRAM，且 SetTime/SetDate 会重新写
   * RTC 计数器。应用只在备份域失效或配置版本变化时写入初始计数，热启动
   * 保留 RTC->CNTH/CNTL，避免每次复位都把时间改回初值。
   */
  if (HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR1) != RTC_BKP_SOURCE_LSE ||
      HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR2) !=
        RTC_BKP_VALID_TAG ||
      RTC_CounterIsValid(RTC_GetCounter()) == 0U)
  {
    if (RTC_SetCounter(RTC_INITIAL_COUNTER) != HAL_OK)
    {
      Error_Handler();
    }
    HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR1, RTC_BKP_SOURCE_LSE);
    HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR2, RTC_BKP_VALID_TAG);
    rtc_cold_boot = 1U;
  }
  else
  {
    rtc_cold_boot = 0U;
  }
  rtc_clk_source = RTC_CLK_LSE;

  (void)RTC_WaitForSync();

  /* USER CODE END RTC_Init 2 */

}

void HAL_RTC_MspInit(RTC_HandleTypeDef* rtcHandle)
{

  if(rtcHandle->Instance==RTC)
  {
  /* USER CODE BEGIN RTC_MspInit 0 */

  /* USER CODE END RTC_MspInit 0 */
    HAL_PWR_EnableBkUpAccess();
    /* Enable BKP CLK enable for backup registers */
    __HAL_RCC_BKP_CLK_ENABLE();
    /* RTC clock enable */
    __HAL_RCC_RTC_ENABLE();

    /* RTC interrupt Init */
    HAL_NVIC_SetPriority(RTC_Alarm_IRQn, 15, 0);
    HAL_NVIC_EnableIRQ(RTC_Alarm_IRQn);
  /* USER CODE BEGIN RTC_MspInit 1 */

  /* USER CODE END RTC_MspInit 1 */
  }
}

void HAL_RTC_MspDeInit(RTC_HandleTypeDef* rtcHandle)
{

  if(rtcHandle->Instance==RTC)
  {
  /* USER CODE BEGIN RTC_MspDeInit 0 */

  /* USER CODE END RTC_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_RTC_DISABLE();

    /* RTC interrupt Deinit */
    HAL_NVIC_DisableIRQ(RTC_Alarm_IRQn);
  /* USER CODE BEGIN RTC_MspDeInit 1 */

  /* USER CODE END RTC_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

HAL_StatusTypeDef RTC_WaitForSync(void)
{
  return HAL_RTC_WaitForSynchro(&hrtc);
}

uint32_t RTC_GetCounter(void)
{
  uint16_t high_first;
  uint16_t high_second;
  uint16_t low;

  do
  {
    high_first = RTC->CNTH;
    low = RTC->CNTL;
    high_second = RTC->CNTH;
  } while (high_first != high_second);

  return ((uint32_t)high_second << 16U) | low;
}

HAL_StatusTypeDef RTC_SetCounter(uint32_t counter)
{
  HAL_StatusTypeDef status = RTC_WriteCounterRegister(counter, 0U);
  if (status == HAL_OK)
  {
    status = RTC_WaitForSync();
  }
  return status;
}

HAL_StatusTypeDef RTC_SetAlarmCounter(uint32_t counter)
{
  HAL_StatusTypeDef status;

  __HAL_RTC_ALARM_DISABLE_IT(&hrtc, RTC_IT_ALRA);
  __HAL_RTC_ALARM_EXTI_DISABLE_IT();
  __HAL_RTC_ALARM_CLEAR_FLAG(&hrtc, RTC_FLAG_ALRAF);
  __HAL_RTC_ALARM_EXTI_CLEAR_FLAG();

  status = RTC_WriteCounterRegister(counter, 1U);
  if (status != HAL_OK)
  {
    return status;
  }

  __HAL_RTC_ALARM_CLEAR_FLAG(&hrtc, RTC_FLAG_ALRAF);
  __HAL_RTC_ALARM_ENABLE_IT(&hrtc, RTC_IT_ALRA);
  __HAL_RTC_ALARM_EXTI_ENABLE_IT();
  __HAL_RTC_ALARM_EXTI_ENABLE_RISING_EDGE();
  return HAL_OK;
}

/* USER CODE END 1 */
