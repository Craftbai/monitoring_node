/**
 ******************************************************************************
 * @file    test_ds18b20.h
 * @brief   DS18B20 温度传感器测试程序头文件
 * @author  监测节点项目组
 * @date    2026-08-19
 ******************************************************************************
 */

#ifndef __TEST_DS18B20_H
#define __TEST_DS18B20_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @brief  DS18B20 完整测试入口
 * @note   运行所有测试用例：器件检测、温度读取、分辨率配置、异常场景
 */
void Test_DS18B20_Run(void);

#ifdef __cplusplus
}
#endif

#endif /* __TEST_DS18B20_H */
