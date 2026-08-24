/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    rtc.h
  * @brief   This file contains all the function prototypes for
  *          the rtc.c file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __RTC_H__
#define __RTC_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

extern RTC_HandleTypeDef hrtc;

/* USER CODE BEGIN Private defines */

#define RTC_CLK_NONE  0U
#define RTC_CLK_LSE   1U
#define RTC_CLK_LSI   2U

/* 参考正点原子 RTC 例程：BKP DR1 保存实际 RTC 时钟源。 */
#define RTC_BKP_SOURCE_LSE  0x5050U
#define RTC_BKP_SOURCE_LSI  0x5051U

/* 应用层初始化标记单独放在 BKP DR2，避免覆盖时钟源标记。 */
#define RTC_BKP_INIT_MAGIC  0x5AA5U
#define RTC_BKP_CONFIG_VERSION 0x0001U
/* 版本参与校验，避免仅使用 INIT_MAGIC 导致配置升级无法触发重置。 */
#define RTC_BKP_VALID_TAG ((uint16_t)(RTC_BKP_INIT_MAGIC ^ RTC_BKP_CONFIG_VERSION))
extern uint8_t rtc_clk_source;
extern uint8_t rtc_cold_boot;

/* USER CODE END Private defines */

void MX_RTC_Init(void);

/* USER CODE BEGIN Prototypes */

/* STM32F1 使用 32 位 RTC 秒计数器，应用层通过这些接口访问计数器和 Alarm。 */
HAL_StatusTypeDef RTC_WaitForSync(void);
uint32_t RTC_GetCounter(void);
HAL_StatusTypeDef RTC_SetCounter(uint32_t counter);
HAL_StatusTypeDef RTC_SetAlarmCounter(uint32_t counter);

/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __RTC_H__ */

