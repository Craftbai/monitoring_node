/**
 ******************************************************************************
 * @file    monitoring_ds18b20.h
 * @brief   DS18B20 数字温度传感器驱动头文件
 * @author  监测节点项目组
 * @date    2026-08-19
 ******************************************************************************
 * @attention
 *
 * 硬件连接:
 *   - DQ  --> PG11 (需外接 4.7 kΩ 上拉到 3.3V)
 *   - VDD --> 3.3V
 *   - GND --> GND
 *
 * 协议说明:
 *   - 1-Wire 协议，软件位操作实现
 *   - 标准速度模式，时序精度要求 ±10 μs
 *   - 支持 9-12 位温度分辨率，转换时间 93.75-750 ms
 *
 ******************************************************************************
 */

#ifndef MONITORING_DS18B20_H
#define MONITORING_DS18B20_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "monitoring_status.h"
#include <stdint.h>
#include <stdbool.h>

/* DS18B20 命令定义 */
#define DS18B20_CMD_SEARCH_ROM      0xF0  /* 搜索 ROM（多设备） */
#define DS18B20_CMD_READ_ROM        0x33  /* 读 ROM（单设备） */
#define DS18B20_CMD_MATCH_ROM       0x55  /* 匹配 ROM（多设备） */
#define DS18B20_CMD_SKIP_ROM        0xCC  /* 跳过 ROM（单设备） */
#define DS18B20_CMD_ALARM_SEARCH    0xEC  /* 告警搜索 */

#define DS18B20_CMD_CONVERT_T       0x44  /* 启动温度转换 */
#define DS18B20_CMD_WRITE_SCRATCH   0x4E  /* 写暂存器 */
#define DS18B20_CMD_READ_SCRATCH    0xBE  /* 读暂存器 */
#define DS18B20_CMD_COPY_SCRATCH    0x48  /* 复制暂存器到 EEPROM */
#define DS18B20_CMD_RECALL_E2       0xB8  /* 从 EEPROM 恢复 */
#define DS18B20_CMD_READ_POWER      0xB4  /* 读供电模式 */

/* DS18B20 分辨率配置 */
typedef enum {
    DS18B20_RESOLUTION_9BIT  = 0x1F,  /* 9 位，93.75 ms，0.5°C */
    DS18B20_RESOLUTION_10BIT = 0x3F,  /* 10 位，187.5 ms，0.25°C */
    DS18B20_RESOLUTION_11BIT = 0x5F,  /* 11 位，375 ms，0.125°C */
    DS18B20_RESOLUTION_12BIT = 0x7F   /* 12 位，750 ms，0.0625°C */
} ds18b20_resolution_t;

/* DS18B20 ROM 结构（64 位唯一 ID） */
typedef struct {
    uint8_t family_code;              /* 家族代码（DS18B20 = 0x28） */
    uint8_t serial_number[6];         /* 48 位序列号 */
    uint8_t crc;                      /* CRC8 校验码 */
} ds18b20_rom_t;

/* 公共 API */

/**
 * @brief  初始化 DS18B20（复位并检测存在脉冲）
 * @retval MONITORING_OK: 器件存在
 *         MONITORING_ERROR_NOT_PRESENT: 器件不存在
 */
monitoring_status_t DS18B20_Init(void);

/**
 * @brief  读取 ROM 码（64 位唯一 ID）
 * @param  rom: 指向 ROM 结构体的指针
 * @retval MONITORING_OK: 读取成功
 *         MONITORING_ERROR_NOT_PRESENT: 器件不存在
 */
monitoring_status_t DS18B20_ReadROM(ds18b20_rom_t *rom);

/**
 * @brief  读取温度（原始 16 位值）
 * @param  temp_raw: 指向温度原始值的指针（LSB = 0.0625°C）
 * @retval MONITORING_OK: 读取成功
 *         MONITORING_ERROR_NOT_PRESENT: 器件不存在
 *         MONITORING_ERROR_CRC: CRC 校验失败
 */
monitoring_status_t DS18B20_ReadTemperature(int16_t *temp_raw);
monitoring_status_t DS18B20_StartTemperatureConversion(void);
/* 读取转换状态位；返回 1 表示转换完成，0 表示仍在转换。 */
uint8_t DS18B20_ConversionReady(void);
monitoring_status_t DS18B20_ReadTemperatureRaw(int16_t *temp_raw);

/**
 * @brief  读取温度（浮点值，单位：°C）
 * @param  temp_celsius: 指向温度值的指针（°C）
 * @retval MONITORING_OK: 读取成功
 *         其他错误码同上
 */
monitoring_status_t DS18B20_ReadTemperatureFloat(float *temp_celsius);

/**
 * @brief  配置温度分辨率
 * @param  resolution: 分辨率配置（9/10/11/12 位）
 * @retval MONITORING_OK: 配置成功
 *         MONITORING_ERROR_NOT_PRESENT: 器件不存在
 */
monitoring_status_t DS18B20_SetResolution(ds18b20_resolution_t resolution);

/**
 * @brief  获取当前分辨率配置
 * @param  resolution: 指向分辨率配置的指针
 * @retval MONITORING_OK: 读取成功
 *         MONITORING_ERROR_NOT_PRESENT: 器件不存在
 */
monitoring_status_t DS18B20_GetResolution(ds18b20_resolution_t *resolution);

/**
 * @brief  复位 DS18B20（重新初始化）
 * @retval MONITORING_OK: 复位成功
 *         MONITORING_ERROR_NOT_PRESENT: 器件不存在
 */
monitoring_status_t DS18B20_Reset(void);

/**
 * @brief  将原始温度值转换为摄氏度
 * @param  temp_raw: 16 位原始温度值
 * @retval 温度值（°C）
 */
float DS18B20_ConvertToFloat(int16_t temp_raw);

/* 内部函数（可选，用于调试或扩展） */

/**
 * @brief  计算 CRC8 校验码（Dallas/Maxim 1-Wire CRC）
 * @param  data: 数据缓冲区指针
 * @param  len: 数据长度
 * @retval CRC8 校验码
 */
uint8_t DS18B20_CRC8(const uint8_t *data, uint8_t len);

#ifdef __cplusplus
}
#endif

#endif /* MONITORING_DS18B20_H */
