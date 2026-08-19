/**
 ******************************************************************************
 * @file    ds18b20.c
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

#include "ds18b20.h"
#include "gpio.h"

/* 1-Wire 引脚定义 */
#define DS18B20_PORT        GPIOG
#define DS18B20_PIN         GPIO_PIN_9

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
 * @note   需要在 main.c 中初始化 DWT 计数器
 */
static void DS18B20_DelayUs(uint32_t us)
{
    /* 方法 1: 使用 DWT（需要在 SystemClock_Config 后启用）
    uint32_t start = DWT->CYCCNT;
    uint32_t cycles = us * (SystemCoreClock / 1000000);
    while ((DWT->CYCCNT - start) < cycles);
    */

    /* 方法 2: 简单延迟循环（适用于 72 MHz，需根据实际校准） */
    volatile uint32_t count = us * (SystemCoreClock / 10000000);
    while (count--);
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
 * @retval DS18B20_OK: 器件存在
 *         DS18B20_ERROR_NOT_PRESENT: 器件不存在
 */
ds18b20_status_t DS18B20_Init(void)
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
    return (presence == 0) ? DS18B20_OK : DS18B20_ERROR_NOT_PRESENT;
}

/**
 * @brief  读取 ROM 码（64 位唯一 ID）
 * @param  rom: 指向 ROM 结构体的指针
 * @retval DS18B20_OK: 读取成功
 *         DS18B20_ERROR_NOT_PRESENT: 器件不存在
 */
ds18b20_status_t DS18B20_ReadROM(ds18b20_rom_t *rom)
{
    uint8_t rom_data[8];

    /* 初始化器件 */
    if (DS18B20_Init() != DS18B20_OK) {
        return DS18B20_ERROR_NOT_PRESENT;
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
        rom->serial_number[i] = rom_data[6 - i];  /* 反向存储，高位在前 */
    }
    rom->crc = rom_data[7];

    /* 验证 CRC */
    uint8_t crc_calc = DS18B20_CRC8(rom_data, 7);
    if (crc_calc != rom->crc) {
        return DS18B20_ERROR_CRC;
    }

    return DS18B20_OK;
}

/**
 * @brief  读取温度（原始 16 位值）
 * @param  temp_raw: 指向温度原始值的指针（LSB = 0.0625°C）
 * @retval DS18B20_OK: 读取成功
 *         DS18B20_ERROR_NOT_PRESENT: 器件不存在
 *         DS18B20_ERROR_CRC: CRC 校验失败
 */
ds18b20_status_t DS18B20_ReadTemperature(int16_t *temp_raw)
{
    uint8_t scratchpad[9];
    uint8_t temp_lsb, temp_msb;

    /* 初始化器件 */
    if (DS18B20_Init() != DS18B20_OK) {
        return DS18B20_ERROR_NOT_PRESENT;
    }

    /* 启动温度转换 */
    DS18B20_WriteByte(DS18B20_CMD_SKIP_ROM);  /* 单设备模式 */
    DS18B20_WriteByte(DS18B20_CMD_CONVERT_T);

    /* 等待转换完成（12 位分辨率需 750 ms） */
    HAL_Delay(DS18B20_CONVERT_TIMEOUT);

    /* 重新初始化 */
    if (DS18B20_Init() != DS18B20_OK) {
        return DS18B20_ERROR_NOT_PRESENT;
    }

    /* 读取暂存器 */
    DS18B20_WriteByte(DS18B20_CMD_SKIP_ROM);
    DS18B20_WriteByte(DS18B20_CMD_READ_SCRATCH);

    for (uint8_t i = 0; i < 9; i++) {
        scratchpad[i] = DS18B20_ReadByte();
    }

    /* 验证 CRC */
    uint8_t crc_calc = DS18B20_CRC8(scratchpad, 8);
    if (crc_calc != scratchpad[8]) {
        return DS18B20_ERROR_CRC;
    }

    /* 合成 16 位温度值 */
    temp_lsb = scratchpad[0];
    temp_msb = scratchpad[1];
    *temp_raw = (int16_t)((temp_msb << 8) | temp_lsb);

    return DS18B20_OK;
}

/**
 * @brief  读取温度（浮点值，单位：°C）
 * @param  temp_celsius: 指向温度值的指针（°C）
 * @retval DS18B20_OK: 读取成功
 *         其他错误码同上
 */
ds18b20_status_t DS18B20_ReadTemperatureFloat(float *temp_celsius)
{
    int16_t temp_raw;
    ds18b20_status_t status;

    status = DS18B20_ReadTemperature(&temp_raw);
    if (status != DS18B20_OK) {
        return status;
    }

    *temp_celsius = DS18B20_ConvertToFloat(temp_raw);

    return DS18B20_OK;
}

/**
 * @brief  配置温度分辨率
 * @param  resolution: 分辨率配置（9/10/11/12 位）
 * @retval DS18B20_OK: 配置成功
 *         DS18B20_ERROR_NOT_PRESENT: 器件不存在
 */
ds18b20_status_t DS18B20_SetResolution(ds18b20_resolution_t resolution)
{
    /* 初始化器件 */
    if (DS18B20_Init() != DS18B20_OK) {
        return DS18B20_ERROR_NOT_PRESENT;
    }

    /* 写入配置寄存器 */
    DS18B20_WriteByte(DS18B20_CMD_SKIP_ROM);
    DS18B20_WriteByte(DS18B20_CMD_WRITE_SCRATCH);
    DS18B20_WriteByte(0x00);  /* TH（高温告警，本项目不用） */
    DS18B20_WriteByte(0x00);  /* TL（低温告警，本项目不用） */
    DS18B20_WriteByte(resolution);  /* Config 寄存器 */

    /* 保存到 EEPROM */
    if (DS18B20_Init() != DS18B20_OK) {
        return DS18B20_ERROR_NOT_PRESENT;
    }
    DS18B20_WriteByte(DS18B20_CMD_SKIP_ROM);
    DS18B20_WriteByte(DS18B20_CMD_COPY_SCRATCH);
    HAL_Delay(10);  /* 等待写入完成 */

    return DS18B20_OK;
}

/**
 * @brief  获取当前分辨率配置
 * @param  resolution: 指向分辨率配置的指针
 * @retval DS18B20_OK: 读取成功
 *         DS18B20_ERROR_NOT_PRESENT: 器件不存在
 */
ds18b20_status_t DS18B20_GetResolution(ds18b20_resolution_t *resolution)
{
    uint8_t scratchpad[9];

    /* 初始化器件 */
    if (DS18B20_Init() != DS18B20_OK) {
        return DS18B20_ERROR_NOT_PRESENT;
    }

    /* 读取暂存器 */
    DS18B20_WriteByte(DS18B20_CMD_SKIP_ROM);
    DS18B20_WriteByte(DS18B20_CMD_READ_SCRATCH);

    for (uint8_t i = 0; i < 9; i++) {
        scratchpad[i] = DS18B20_ReadByte();
    }

    /* Config 寄存器在字节 4 */
    *resolution = (ds18b20_resolution_t)scratchpad[4];

    return DS18B20_OK;
}

/**
 * @brief  复位 DS18B20（重新初始化）
 * @retval DS18B20_OK: 复位成功
 *         DS18B20_ERROR_NOT_PRESENT: 器件不存在
 */
ds18b20_status_t DS18B20_Reset(void)
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
