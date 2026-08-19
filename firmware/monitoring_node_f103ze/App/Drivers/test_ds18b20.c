/**
 ******************************************************************************
 * @file    test_ds18b20.c
 * @brief   DS18B20 温度传感器测试程序
 * @author  监测节点项目组
 * @date    2026-08-19
 ******************************************************************************
 * @attention
 *
 * 测试场景：
 *   1. 器件检测和 ROM 读取
 *   2. 温度读取（室温、手握加热、冰水冷却）
 *   3. 分辨率配置验证
 *   4. 异常场景（断线、CRC 错误）
 *
 * 接线要求：
 *   - DS18B20 DQ  --> PG9 + 4.7kΩ 上拉到 3.3V
 *   - DS18B20 VDD --> 3.3V
 *   - DS18B20 GND --> GND
 *
 ******************************************************************************
 */

#include "main.h"
#include "usart.h"
#include "ds18b20.h"
#include <stdio.h>
#include <string.h>

/* 测试配置 */
#define TEST_INTERVAL_MS            2000    /* 测试间隔（毫秒） */
#define TEST_SAMPLE_COUNT           10      /* 每轮采样次数 */

/* UART 输出缓冲区 */
static char uart_buffer[256];

/**
 * @brief  UART 输出函数
 */
static void Test_Printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int len = vsnprintf(uart_buffer, sizeof(uart_buffer), format, args);
    va_end(args);

    if (len > 0) {
        HAL_UART_Transmit(&huart1, (uint8_t *)uart_buffer, len, 1000);
    }
}

/**
 * @brief  测试 1：器件检测和 ROM 读取
 */
static void Test_DS18B20_Detection(void)
{
    ds18b20_status_t status;
    ds18b20_rom_t rom;

    Test_Printf("\r\n========== 测试 1: 器件检测 ==========\r\n");

    /* 初始化检测 */
    status = DS18B20_Init();
    if (status == DS18B20_OK) {
        Test_Printf("[PASS] DS18B20 存在脉冲检测成功\r\n");
    } else {
        Test_Printf("[FAIL] DS18B20 不存在或连接异常\r\n");
        Test_Printf("       请检查: 1) DQ 引脚连接\r\n");
        Test_Printf("                2) 4.7kΩ 上拉电阻\r\n");
        Test_Printf("                3) VDD 和 GND 供电\r\n");
        return;
    }

    /* 读取 ROM 码 */
    status = DS18B20_ReadROM(&rom);
    if (status == DS18B20_OK) {
        Test_Printf("[PASS] ROM 读取成功\r\n");
        Test_Printf("       Family Code: 0x%02X\r\n", rom.family_code);
        Test_Printf("       Serial Number: ");
        for (int i = 0; i < 6; i++) {
            Test_Printf("%02X", rom.serial_number[i]);
        }
        Test_Printf("\r\n");
        Test_Printf("       CRC: 0x%02X\r\n", rom.crc);

        if (rom.family_code != 0x28) {
            Test_Printf("[WARN] Family Code 不是 0x28 (DS18B20 标准值)\r\n");
        }
    } else if (status == DS18B20_ERROR_CRC) {
        Test_Printf("[FAIL] ROM CRC 校验失败（数据传输异常）\r\n");
    } else {
        Test_Printf("[FAIL] ROM 读取失败\r\n");
    }
}

/**
 * @brief  测试 2：温度读取
 */
static void Test_DS18B20_Temperature(void)
{
    ds18b20_status_t status;
    int16_t temp_raw;
    float temp_celsius;

    Test_Printf("\r\n========== 测试 2: 温度读取 ==========\r\n");
    Test_Printf("开始连续采样 %d 次，间隔 %d ms\r\n", TEST_SAMPLE_COUNT, TEST_INTERVAL_MS);

    for (int i = 0; i < TEST_SAMPLE_COUNT; i++) {
        /* 读取温度 */
        status = DS18B20_ReadTemperature(&temp_raw);

        if (status == DS18B20_OK) {
            temp_celsius = DS18B20_ConvertToFloat(temp_raw);
            Test_Printf("[%02d] 温度: %+6.2f °C (原始值: 0x%04X)\r\n",
                       i + 1, temp_celsius, temp_raw);

            /* 温度合理性检查 */
            if (temp_celsius < -10.0f || temp_celsius > 50.0f) {
                Test_Printf("     [WARN] 温度超出预期范围 [-10, +50]°C\r\n");
            }
        } else if (status == DS18B20_ERROR_NOT_PRESENT) {
            Test_Printf("[%02d] [FAIL] 器件不存在（传感器断线）\r\n", i + 1);
        } else if (status == DS18B20_ERROR_CRC) {
            Test_Printf("[%02d] [FAIL] CRC 校验失败（数据损坏）\r\n", i + 1);
        } else {
            Test_Printf("[%02d] [FAIL] 读取失败（未知错误）\r\n", i + 1);
        }

        HAL_Delay(TEST_INTERVAL_MS);
    }

    Test_Printf("[INFO] 温度读取测试完成\r\n");
    Test_Printf("       手动验证: 尝试手握传感器，观察温度上升\r\n");
}

/**
 * @brief  测试 3：分辨率配置
 */
static void Test_DS18B20_Resolution(void)
{
    ds18b20_status_t status;
    ds18b20_resolution_t resolutions[] = {
        DS18B20_RESOLUTION_9BIT,
        DS18B20_RESOLUTION_10BIT,
        DS18B20_RESOLUTION_11BIT,
        DS18B20_RESOLUTION_12BIT
    };
    const char *res_names[] = {"9-bit", "10-bit", "11-bit", "12-bit"};

    Test_Printf("\r\n========== 测试 3: 分辨率配置 ==========\r\n");

    for (int i = 0; i < 4; i++) {
        Test_Printf("设置分辨率: %s\r\n", res_names[i]);

        /* 设置分辨率 */
        status = DS18B20_SetResolution(resolutions[i]);
        if (status != DS18B20_OK) {
            Test_Printf("[FAIL] 分辨率设置失败\r\n");
            continue;
        }

        /* 回读验证 */
        ds18b20_resolution_t readback;
        status = DS18B20_GetResolution(&readback);
        if (status == DS18B20_OK) {
            if (readback == resolutions[i]) {
                Test_Printf("[PASS] 分辨率回读匹配: 0x%02X\r\n", readback);
            } else {
                Test_Printf("[FAIL] 分辨率回读不匹配: 期望 0x%02X, 实际 0x%02X\r\n",
                           resolutions[i], readback);
            }
        } else {
            Test_Printf("[FAIL] 分辨率回读失败\r\n");
        }

        HAL_Delay(500);
    }

    /* 恢复默认 12 位分辨率 */
    DS18B20_SetResolution(DS18B20_RESOLUTION_12BIT);
    Test_Printf("[INFO] 恢复默认 12 位分辨率\r\n");
}

/**
 * @brief  测试 4：异常场景
 */
static void Test_DS18B20_Abnormal(void)
{
    ds18b20_status_t status;

    Test_Printf("\r\n========== 测试 4: 异常场景 ==========\r\n");
    Test_Printf("[手动测试] 请在 10 秒内拔掉传感器 DQ 引脚...\r\n");

    for (int i = 10; i > 0; i--) {
        Test_Printf("%d...\r\n", i);
        HAL_Delay(1000);
    }

    /* 尝试读取温度（应该失败） */
    int16_t temp_raw;
    status = DS18B20_ReadTemperature(&temp_raw);

    if (status == DS18B20_ERROR_NOT_PRESENT) {
        Test_Printf("[PASS] 正确检测到传感器断线\r\n");
    } else if (status == DS18B20_OK) {
        Test_Printf("[WARN] 传感器仍然响应（可能未拔掉）\r\n");
    } else {
        Test_Printf("[INFO] 检测到其他错误: %d\r\n", status);
    }

    Test_Printf("[手动测试] 请重新连接传感器并等待 5 秒...\r\n");
    HAL_Delay(5000);

    /* 尝试恢复 */
    status = DS18B20_Init();
    if (status == DS18B20_OK) {
        Test_Printf("[PASS] 传感器重新连接成功\r\n");
    } else {
        Test_Printf("[FAIL] 传感器仍未响应\r\n");
    }
}

/**
 * @brief  DS18B20 完整测试入口
 */
void Test_DS18B20_Run(void)
{
    Test_Printf("\r\n");
    Test_Printf("========================================\r\n");
    Test_Printf("    DS18B20 驱动验证测试套件\r\n");
    Test_Printf("    STM32F103ZET6 @ 72 MHz\r\n");
    Test_Printf("========================================\r\n");

    /* 运行测试用例 */
    Test_DS18B20_Detection();
    HAL_Delay(1000);

    Test_DS18B20_Temperature();
    HAL_Delay(1000);

    Test_DS18B20_Resolution();
    HAL_Delay(1000);

    Test_DS18B20_Abnormal();

    Test_Printf("\r\n========================================\r\n");
    Test_Printf("    测试完成\r\n");
    Test_Printf("========================================\r\n");
}
