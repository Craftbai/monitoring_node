/**
 ******************************************************************************
 * @file    monitoring_ds18b20.c
 * @brief   DS18B20 数字温度传感器驱动实现
 * @author  监测节点项目组
 * @date    2026-08-19
 ******************************************************************************
 * @attention
 *
 * 1-Wire 时序要求（标准速度）：
 *   - 复位脉冲：480-960 μs 低电平
 *   - 存在脉冲：60-240 μs 低电平响应
 *   - 写 0：60-120 μs 低电平
 *   - 写 1：1-15 μs 低电平
 *   - 读时隙：1-15 μs 低电平启动，然后采样
 *
 ******************************************************************************
 */

#include "monitoring_ds18b20.h"
#include "gpio.h"

/* 1-Wire 引脚定义 */
#define DS18B20_PORT        GPIOG
#define DS18B20_PIN         GPIO_PIN_11

/* 1-Wire 时序参数（单位：微秒） */
#define DS18B20_RESET_PULSE         480
#define DS18B20_PRESENCE_WAIT       70
#define DS18B20_PRESENCE_FINISH     410
#define DS18B20_WRITE_0_LOW         60
#define DS18B20_WRITE_1_LOW         1
#define DS18B20_WRITE_RECOVERY      5
#define DS18B20_READ_START          1
#define DS18B20_READ_SAMPLE         14
#define DS18B20_READ_RECOVERY       45

/* 转换超时（单位：毫秒，最大分辨率需 750 ms） */
#define DS18B20_CONVERT_TIMEOUT     800

/* 内部函数声明 */
static void DS18B20_SetPinOutput(void);
static void DS18B20_SetPinInput(void);
static void DS18B20_SetPinHigh(void);
static void DS18B20_SetPinLow(void);
static uint8_t DS18B20_ReadPin(void);
static void DS18B20_DelayUs(uint32_t us);
static void DS18B20_WriteBit(uint8_t bit);
static uint8_t DS18B20_ReadBit(void);
static void DS18B20_WriteByte(uint8_t byte);
static uint8_t DS18B20_ReadByte(void);

/**
 * @brief  微秒延时函数（基于 DWT 或 SysTick）
 * @param  us: 延时时间（微秒）
 * @note   首次调用时由本函数懒初始化 DWT 周期计数器
 */
static void DS18B20_DelayUs(uint32_t us)
{
    static uint8_t dwt_ready = 0U;
    uint32_t start;
    uint32_t cycles;

    /* 1-Wire 时序使用 Cortex-M3 DWT 周期计数器，避免简单空循环随优化级别漂移。 */
    if (dwt_ready == 0U)
    {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CYCCNT = 0U;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
        dwt_ready = 1U;
    }

    start = DWT->CYCCNT;
    cycles = us * (HAL_RCC_GetHCLKFreq() / 1000000U);
    while ((DWT->CYCCNT - start) < cycles)
    {
    }
}

/**
 * @brief  设置引脚为输出模式
 */
static void DS18B20_SetPinOutput(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = DS18B20_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;  /* 开漏输出 */
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(DS18B20_PORT, &GPIO_InitStruct);
}

/**
 * @brief  设置引脚为输入模式
 */
static void DS18B20_SetPinInput(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = DS18B20_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(DS18B20_PORT, &GPIO_InitStruct);
}

/**
 * @brief  拉高引脚
 */
static void DS18B20_SetPinHigh(void)
{
    HAL_GPIO_WritePin(DS18B20_PORT, DS18B20_PIN, GPIO_PIN_SET);
}

/**
 * @brief  拉低引脚
 */
static void DS18B20_SetPinLow(void)
{
    HAL_GPIO_WritePin(DS18B20_PORT, DS18B20_PIN, GPIO_PIN_RESET);
}

/**
 * @brief  读取引脚状态
 * @retval 0: 低电平，1: 高电平
 */
static uint8_t DS18B20_ReadPin(void)
{
    return HAL_GPIO_ReadPin(DS18B20_PORT, DS18B20_PIN) == GPIO_PIN_SET ? 1 : 0;
}

/**
 * @brief  写一位数据
 * @param  bit: 0 或 1
 */
static void DS18B20_WriteBit(uint8_t bit)
{
    DS18B20_SetPinOutput();

    if (bit) {
        /* 写 1: 拉低 1 μs，然后释放 */
        DS18B20_SetPinLow();
        DS18B20_DelayUs(DS18B20_WRITE_1_LOW);
        DS18B20_SetPinHigh();
        DS18B20_DelayUs(DS18B20_WRITE_0_LOW - DS18B20_WRITE_1_LOW);
    } else {
        /* 写 0: 拉低 60 μs，然后释放 */
        DS18B20_SetPinLow();
        DS18B20_DelayUs(DS18B20_WRITE_0_LOW);
        DS18B20_SetPinHigh();
    }

    DS18B20_DelayUs(DS18B20_WRITE_RECOVERY);
}

/**
 * @brief  读一位数据
 * @retval 0 或 1
 */
static uint8_t DS18B20_ReadBit(void)
{
    uint8_t bit;

    DS18B20_SetPinOutput();

    /* 拉低 1 μs 启动读时隙 */
    DS18B20_SetPinLow();
    DS18B20_DelayUs(DS18B20_READ_START);

    /* 释放总线并切换到输入模式 */
    DS18B20_SetPinHigh();
    DS18B20_SetPinInput();

    /* 等待并采样 */
    DS18B20_DelayUs(DS18B20_READ_SAMPLE);
    bit = DS18B20_ReadPin();

    /* 完成读时隙 */
    DS18B20_DelayUs(DS18B20_READ_RECOVERY);

    DS18B20_SetPinOutput();
    DS18B20_SetPinHigh();

    return bit;
}

/**
 * @brief  写一字节数据（LSB first）
 * @param  byte: 要写入的字节
 */
static void DS18B20_WriteByte(uint8_t byte)
{
    for (uint8_t i = 0; i < 8; i++) {
        DS18B20_WriteBit(byte & 0x01);
        byte >>= 1;
    }
}

/**
 * @brief  读一字节数据（LSB first）
 * @retval 读取的字节
 */
static uint8_t DS18B20_ReadByte(void)
{
    uint8_t byte = 0;

    for (uint8_t i = 0; i < 8; i++) {
        byte >>= 1;
        if (DS18B20_ReadBit()) {
            byte |= 0x80;
        }
    }

    return byte;
}

/**
 * @brief  初始化 DS18B20（复位并检测存在脉冲）
 * @retval MONITORING_OK: 器件存在
 *         MONITORING_ERROR_NOT_PRESENT: 器件不存在
 */
monitoring_status_t DS18B20_Init(void)
{
    uint8_t presence;

    DS18B20_SetPinOutput();

    /* 发送复位脉冲（拉低至少 480 μs） */
    DS18B20_SetPinLow();
    DS18B20_DelayUs(DS18B20_RESET_PULSE);

    /* 释放总线并切换到输入模式 */
    DS18B20_SetPinHigh();
    DS18B20_SetPinInput();

    /* 等待存在脉冲（60-240 μs 低电平） */
    DS18B20_DelayUs(DS18B20_PRESENCE_WAIT);
    presence = DS18B20_ReadPin();

    /* 完成复位时隙 */
    DS18B20_DelayUs(DS18B20_PRESENCE_FINISH);

    DS18B20_SetPinOutput();
    DS18B20_SetPinHigh();

    /* 存在脉冲应为低电平（presence = 0） */
    return (presence == 0) ? MONITORING_OK : MONITORING_ERROR_NOT_PRESENT;
}

/**
 * @brief  读取 ROM 码（64 位唯一 ID）
 * @param  rom: 指向 ROM 结构体的指针
 * @retval MONITORING_OK: 读取成功
 *         MONITORING_ERROR_NOT_PRESENT: 器件不存在
 */
monitoring_status_t DS18B20_ReadROM(ds18b20_rom_t *rom)
{
    uint8_t rom_data[8];

    if (rom == NULL)
    {
        return MONITORING_ERROR;
    }

    /* 初始化器件 */
    if (DS18B20_Init() != MONITORING_OK) {
        return MONITORING_ERROR_NOT_PRESENT;
    }

    /* 发送 READ ROM 命令 */
    DS18B20_WriteByte(DS18B20_CMD_READ_ROM);

    /* 读取 8 字节 ROM 数据 */
    for (uint8_t i = 0; i < 8; i++) {
        rom_data[i] = DS18B20_ReadByte();
    }

    /* 解析 ROM 结构 */
    rom->family_code = rom_data[0];
    for (uint8_t i = 0; i < 6; i++) {
        rom->serial_number[i] = rom_data[i + 1U];
    }
    rom->crc = rom_data[7];

    /* 验证 CRC */
    uint8_t crc_calc = DS18B20_CRC8(rom_data, 7);
    if (crc_calc != rom->crc) {
        return MONITORING_ERROR_CRC;
    }

    return MONITORING_OK;
}

/**
 * @brief  读取温度（原始 16 位值）
 * @param  temp_raw: 指向温度原始值的指针（LSB = 0.0625°C）
 * @retval MONITORING_OK: 读取成功
 *         MONITORING_ERROR_NOT_PRESENT: 器件不存在
 *         MONITORING_ERROR_CRC: CRC 校验失败
 */
monitoring_status_t DS18B20_ReadTemperature(int16_t *temp_raw)
{
    monitoring_status_t status = DS18B20_StartTemperatureConversion();
    if (status != MONITORING_OK)
    {
        return status;
    }
    HAL_Delay(DS18B20_CONVERT_TIMEOUT);
    return DS18B20_ReadTemperatureRaw(temp_raw);
}

monitoring_status_t DS18B20_StartTemperatureConversion(void)
{
    if (DS18B20_Init() != MONITORING_OK)
    {
        return MONITORING_ERROR_NOT_PRESENT;
    }
    DS18B20_WriteByte(DS18B20_CMD_SKIP_ROM);
    DS18B20_WriteByte(DS18B20_CMD_CONVERT_T);
    return MONITORING_OK;
}

uint8_t DS18B20_ConversionReady(void)
{
    /* 转换期间器件会在读时隙返回 0，完成后返回 1。 */
    DS18B20_SetPinInput();
    return DS18B20_ReadBit();
}

monitoring_status_t DS18B20_ReadTemperatureRaw(int16_t *temp_raw)
{
    uint8_t scratchpad[9];

    if (temp_raw == NULL)
    {
        return MONITORING_ERROR;
    }
    if (DS18B20_Init() != MONITORING_OK)
    {
        return MONITORING_ERROR_NOT_PRESENT;
    }
    DS18B20_WriteByte(DS18B20_CMD_SKIP_ROM);
    DS18B20_WriteByte(DS18B20_CMD_READ_SCRATCH);
    for (uint8_t i = 0U; i < 9U; i++)
    {
        scratchpad[i] = DS18B20_ReadByte();
    }
    if (DS18B20_CRC8(scratchpad, 8U) != scratchpad[8])
    {
        return MONITORING_ERROR_CRC;
    }
    *temp_raw = (int16_t)(((uint16_t)scratchpad[1] << 8) | scratchpad[0]);
    return MONITORING_OK;
}

/**
 * @brief  读取温度（浮点值，单位：°C）
 * @param  temp_celsius: 指向温度值的指针（°C）
 * @retval MONITORING_OK: 读取成功
 *         其他错误码同上
 */
monitoring_status_t DS18B20_ReadTemperatureFloat(float *temp_celsius)
{
    int16_t temp_raw;
    monitoring_status_t status;

    if (temp_celsius == NULL)
    {
        return MONITORING_ERROR;
    }

    status = DS18B20_ReadTemperature(&temp_raw);
    if (status != MONITORING_OK) {
        return status;
    }

    *temp_celsius = DS18B20_ConvertToFloat(temp_raw);

    return MONITORING_OK;
}

/**
 * @brief  配置温度分辨率
 * @param  resolution: 分辨率配置（9/10/11/12 位）
 * @retval MONITORING_OK: 配置成功
 *         MONITORING_ERROR_NOT_PRESENT: 器件不存在
 */
monitoring_status_t DS18B20_SetResolution(ds18b20_resolution_t resolution)
{
    if (resolution != DS18B20_RESOLUTION_9BIT &&
        resolution != DS18B20_RESOLUTION_10BIT &&
        resolution != DS18B20_RESOLUTION_11BIT &&
        resolution != DS18B20_RESOLUTION_12BIT)
    {
        return MONITORING_ERROR;
    }

    /* 初始化器件 */
    if (DS18B20_Init() != MONITORING_OK) {
        return MONITORING_ERROR_NOT_PRESENT;
    }

    /* 写入配置寄存器 */
    DS18B20_WriteByte(DS18B20_CMD_SKIP_ROM);
    DS18B20_WriteByte(DS18B20_CMD_WRITE_SCRATCH);
    DS18B20_WriteByte(0x00);  /* TH（高温告警，本项目不用） */
    DS18B20_WriteByte(0x00);  /* TL（低温告警，本项目不用） */
    DS18B20_WriteByte(resolution);  /* Config 寄存器 */

    /* 保存到 EEPROM */
    if (DS18B20_Init() != MONITORING_OK) {
        return MONITORING_ERROR_NOT_PRESENT;
    }
    DS18B20_WriteByte(DS18B20_CMD_SKIP_ROM);
    DS18B20_WriteByte(DS18B20_CMD_COPY_SCRATCH);
    HAL_Delay(10);  /* 等待写入完成 */

    return MONITORING_OK;
}

/**
 * @brief  获取当前分辨率配置
 * @param  resolution: 指向分辨率配置的指针
 * @retval MONITORING_OK: 读取成功
 *         MONITORING_ERROR_NOT_PRESENT: 器件不存在
 */
monitoring_status_t DS18B20_GetResolution(ds18b20_resolution_t *resolution)
{
    uint8_t scratchpad[9];

    if (resolution == NULL)
    {
        return MONITORING_ERROR;
    }

    /* 初始化器件 */
    if (DS18B20_Init() != MONITORING_OK) {
        return MONITORING_ERROR_NOT_PRESENT;
    }

    /* 读取暂存器 */
    DS18B20_WriteByte(DS18B20_CMD_SKIP_ROM);
    DS18B20_WriteByte(DS18B20_CMD_READ_SCRATCH);

    for (uint8_t i = 0; i < 9; i++) {
        scratchpad[i] = DS18B20_ReadByte();
    }

    if (DS18B20_CRC8(scratchpad, 8U) != scratchpad[8])
    {
        return MONITORING_ERROR_CRC;
    }

    /* Config 寄存器在字节 4 */
    *resolution = (ds18b20_resolution_t)scratchpad[4];

    if (*resolution != DS18B20_RESOLUTION_9BIT &&
        *resolution != DS18B20_RESOLUTION_10BIT &&
        *resolution != DS18B20_RESOLUTION_11BIT &&
        *resolution != DS18B20_RESOLUTION_12BIT)
    {
        return MONITORING_ERROR;
    }

    return MONITORING_OK;
}

/**
 * @brief  复位 DS18B20（重新初始化）
 * @retval MONITORING_OK: 复位成功
 *         MONITORING_ERROR_NOT_PRESENT: 器件不存在
 */
monitoring_status_t DS18B20_Reset(void)
{
    return DS18B20_Init();
}

/**
 * @brief  将原始温度值转换为摄氏度
 * @param  temp_raw: 16 位原始温度值
 * @retval 温度值（°C）
 */
float DS18B20_ConvertToFloat(int16_t temp_raw)
{
    return (float)temp_raw * 0.0625f;
}

/**
 * @brief  计算 CRC8 校验码（Dallas/Maxim 1-Wire CRC）
 * @param  data: 数据缓冲区指针
 * @param  len: 数据长度
 * @retval CRC8 校验码
 */
uint8_t DS18B20_CRC8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0;

    if (data == NULL && len != 0U)
    {
        return 0U;
    }

    for (uint8_t i = 0; i < len; i++) {
        uint8_t byte = data[i];

        for (uint8_t j = 0; j < 8; j++) {
            uint8_t mix = (crc ^ byte) & 0x01;
            crc >>= 1;
            if (mix) {
                crc ^= 0x8C;
            }
            byte >>= 1;
        }
    }

    return crc;
}
