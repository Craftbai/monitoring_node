/* =============================================================================
 * monitoring_acquisition.h - 三通道采集服务层
 *
 * 职责：为上层提供统一的采样窗口接口，协调温度/振动/电流三个传感器的
 *       并行采集，管理 DMA 双缓冲、FIFO 轮询和有效标志设置。
 *
 * 采集策略：
 *   - 温度（DS18B20）: 先启动转换（750ms），在等待期间采集其他通道
 *   - 振动（MPU6050）: FIFO 轮询 + 中断唤醒，1kHz 采样 → 800Hz 等效窗口
 *   - 电流（ACS712）: TIM3 触发 ADC1 DMA，1600Hz 采样，半传输/全传输中断
 *
 * Stop 模式支持：
 *   - Stop(): 停止所有采集外设，清理中断标志
 *   - Resume(): Stop 唤醒后重新初始化外设（DeInit → Init → 校准）
 * ============================================================================= */

#ifndef MONITORING_ACQUISITION_H
#define MONITORING_ACQUISITION_H

#include "monitoring_tasks.h"

/**
 * @brief  执行一次三通道采集（温度/振动/电流）
 * @param  block: 采样块指针（由调用者从块池取得）
 * @return 采样块的 flags 字段（MONITOR_SAMPLE_FLAG_* 位图）
 *
 * @note   采集流程（并行推进，2.5 秒超时）：
 *         1. 启动 DS18B20 温度转换（非阻塞，750ms 后台转换）
 *         2. 启动 MPU6050 FIFO 采集 + ADC DMA 采集
 *         3. 轮询三个通道的完成状态：
 *            - 温度: 每 5ms 轮询 DS18B20 转换状态，800ms 超时
 *            - 振动: 等待 PE2 中断或 1ms 超时，读 FIFO 直到 1280 样本（→ 1024 有效）
 *            - 电流: 等待 DMA 半传输/全传输中断，复制到块的 current 数组
 *         4. 三个通道都完成或超时后，设置有效标志并返回
 *
 * @note   有效标志设置规则：
 *         - 振动: vibration_sample_count == 1024 且无溢出时清除 INVALID 标志
 *         - 电流: adc_half_copied && adc_full_copied && 无错误时清除 INVALID 标志
 *         - 温度: DS18B20_ReadTemperatureRaw 成功时清除 INVALID 标志
 *
 * @note   错误处理：
 *         - 传感器未响应: 设置 MISSING 标志
 *         - FIFO 溢出/DMA 错误: 设置 OVERFLOW 标志
 *         - 采集超时: 设置 TIMEOUT 标志
 *         - ADC 饱和: 设置 SATURATION 标志
 */
uint32_t MonitoringAcquisition_Capture(monitor_sample_block_t *block);

/**
 * @brief  停止所有采集外设（进 Stop 前调用）
 * @return 1=成功停止，0=停止失败
 *
 * @note   Stop 门禁检查的一部分，失败时拒绝进入 Stop 模式
 * @note   只有本周期确实启动过 DMA，才检查 HAL 返回值（避免误判）
 * @note   停止顺序: TIM3 → ADC DMA → MPU6050 FIFO
 */
uint8_t MonitoringAcquisition_Stop(void);

/**
 * @brief  Stop 唤醒后恢复采集外设
 * @return 1=成功恢复，0=恢复失败
 *
 * @note   恢复流程: DeInit 旧状态 → MX_*_Init 重新配置 → ADC 校准 → MPU6050 初始化
 * @note   传感器缺失不会导致恢复失败（属于通道降级，不阻止下一周期运行）
 * @note   ADC 校准失败会导致恢复失败（基础外设故障）
 */
uint8_t MonitoringAcquisition_Resume(void);

#endif /* MONITORING_ACQUISITION_H */
