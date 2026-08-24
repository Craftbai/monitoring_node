/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"
#include "adc.h"
#include "dma.h"
#include "i2c.h"
#include "rtc.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "monitoring_tasks.h"
#include "ds18b20.h"
#include <stdio.h>
#include <stdarg.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define M2_DS18B20_TEST_MODE 0U
#define UART_LOG_BUFFER_SIZE 256U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
static const uint8_t start_msg[] = "monitoring_node: boot\r\n";

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void MX_FREERTOS_Init(void);
/* USER CODE BEGIN PFP */
void UART_Log(const char *fmt, ...);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USART1 阻塞打印。只用于启动日志和低速诊断，不进热路径。 */
void UART_Log(const char *fmt, ...)
{
  char buf[UART_LOG_BUFFER_SIZE];
  va_list ap;
  int len;

  va_start(ap, fmt);
  len = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if (len > 0)
  {
    if (len > (int)sizeof(buf) - 1)
    {
      len = (int)sizeof(buf) - 1;       /* vsnprintf 截断时返回的是期望长度 */
    }
    HAL_UART_Transmit(&huart1, (uint8_t *)buf, (uint16_t)len, 100U);
  }
}

#if M2_DS18B20_TEST_MODE
/* M2：DS18B20 最小板级验证。先读 ROM，再读取一次温度。 */
static const char *DS18B20_StatusText(ds18b20_status_t status)
{
  switch (status)
  {
    case DS18B20_OK:                 return "OK";
    case DS18B20_ERROR_NOT_PRESENT:  return "NOT_PRESENT";
    case DS18B20_ERROR_CRC:          return "CRC";
    case DS18B20_ERROR_INVALID_DATA: return "INVALID_DATA";
    case DS18B20_ERROR_TIMEOUT:      return "TIMEOUT";
    default:                         return "UNKNOWN";
  }
}

static void DS18B20_LogTemperature(int16_t raw)
{
  int32_t centi_celsius = ((int32_t)raw * 100) / 16;
  int32_t absolute_centi = (centi_celsius < 0) ? -centi_celsius : centi_celsius;

  UART_Log("[M2] temperature: %s%ld.%02ld C, raw=%d\r\n",
           (centi_celsius < 0) ? "-" : "",
           absolute_centi / 100,
           absolute_centi % 100,
           raw);
}

static void DS18B20_TestOnce(void)
{
  static uint8_t rom_reported = 0U;
  ds18b20_status_t status;
  ds18b20_rom_t rom;
  int16_t temperature_raw;

  if (rom_reported == 0U)
  {
    status = DS18B20_ReadROM(&rom);
    if (status == DS18B20_OK)
    {
      UART_Log("[M2] ROM: %02X-%02X%02X%02X%02X%02X%02X-%02X\r\n",
               rom.family_code,
               rom.serial_number[0], rom.serial_number[1],
               rom.serial_number[2], rom.serial_number[3],
               rom.serial_number[4], rom.serial_number[5],
               rom.crc);
      rom_reported = 1U;
    }
    else
    {
      UART_Log("[M2] ROM read failed: %s\r\n", DS18B20_StatusText(status));
      /* ROM 都未读到时，不再把全 0 暂存器误报为 0.00 C。 */
      return;
    }
  }

  status = DS18B20_ReadTemperature(&temperature_raw);
  if (status == DS18B20_OK)
  {
    DS18B20_LogTemperature(temperature_raw);
  }
  else
  {
    UART_Log("[M2] temperature read failed: %s\r\n", DS18B20_StatusText(status));
  }
}
#endif

/* 设置下一次 RTC 闹钟：当前时间 + interval_sec。
   用 BIN 格式读时间、BIN 格式设闹钟，规避 BCD 的秒进位问题。 */
HAL_StatusTypeDef RTC_SetNextAlarm(uint32_t interval_sec)
{
  if (interval_sec == 0U)
  {
    return HAL_ERROR;
  }
  return RTC_SetAlarmCounter(RTC_GetCounter() + interval_sec);
}


/* RTC 闹钟中断回调。中断上下文：只置标志，不做任何阻塞或浮点。 */
void HAL_RTC_AlarmAEventCallback(RTC_HandleTypeDef *hrtc)
{
  (void)hrtc;
  if (g_monitor_cycle_event != NULL)
  {
    (void)osEventFlagsSet(g_monitor_cycle_event, MONITOR_EVENT_RTC_ALARM);
  }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_RTC_Init();
  MX_ADC1_Init();
  MX_TIM3_Init();
  MX_I2C1_Init();
  MX_SPI2_Init();
  /* USER CODE BEGIN 2 */
  HAL_UART_Transmit(&huart1, (uint8_t *)start_msg, sizeof(start_msg) - 1U, 100U);

#if M2_DS18B20_TEST_MODE
  UART_Log("[M2] DS18B20 test start, DQ=PG11\r\n");
  UART_Log("[M2] DQ idle: %s\r\n",
           HAL_GPIO_ReadPin(DS18B20_DQ_GPIO_Port, DS18B20_DQ_Pin) == GPIO_PIN_SET ? "HIGH" : "LOW");
  DS18B20_TestOnce();
#else
  /* FreeRTOS 周期任务接管 RTC 事件，并在每次上报完成后进入 Stop。 */
  UART_Log("[BOOT] RTC clk : %s\r\n",
           (rtc_clk_source == RTC_CLK_LSE) ? "LSE" :
           (rtc_clk_source == RTC_CLK_LSI) ? "LSI (LSE failed)" : "NONE");
  UART_Log("[BOOT] boot type: %s\r\n",
           rtc_cold_boot ? "COLD (time reset)" : "WARM (time kept)");
  UART_Log("[BOOT] RTC counter: %lu\r\n",
           (unsigned long)RTC_GetCounter());
#endif
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();  /* Call init function for freertos objects (in cmsis_os2.c) */
  MX_FREERTOS_Init();

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE|RCC_OSCILLATORTYPE_LSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_RTC|RCC_PERIPHCLK_ADC;
  PeriphClkInit.RTCClockSelection = RCC_RTCCLKSOURCE_LSE;
  PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV6;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM4 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM4)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
