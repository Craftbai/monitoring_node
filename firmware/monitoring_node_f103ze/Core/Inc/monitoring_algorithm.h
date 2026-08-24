/* =============================================================================
 * monitoring_algorithm.h - 信号处理与特征提取层
 *
 * 职责：将采样块（温度标量、振动/电流时域波形）转换为物理量特征值
 *       （RMS、峰峰值、峰值因子、频带能量），并调用告警模块评估状态。
 *
 * 算法策略：
 *   - 无硬件 FPU（STM32F103ZET6），全部使用 CMSIS-DSP Q15 定点运算
 *   - 振动：去直流 → 低通滤波 → 三角窗 → RMS/峰峰值 → 1024 点 FFT
 *   - 电流：去零点偏置 → RMS（ACS712 输出直流偏置 + 交流电流成分）
 *   - 温度：标量，直接通过范围检查后设置有效标志
 *
 * Q15 定点格式：
 *   - 范围 [-1.0, +1.0) 映射到 [-32768, +32767]
 *   - 乘法后右移 15 位归一化：(a * b) >> 15
 *   - 适合无 FPU 的 Cortex-M3，避免浮点软件模拟
 *
 * 关键缩放因子：
 *   - MPU6050: ±2g 量程，16384 counts/g
 *   - ACS712: Vcc/2 零点偏置（2048 counts），灵敏度 83 counts/A
 *   - 三角窗: 峰值增益 ×64（Q15 格式下 64/32768 ≈ 2/1024）
 * ============================================================================= */

#ifndef MONITORING_ALGORITHM_H
#define MONITORING_ALGORITHM_H

#include "monitoring_tasks.h"

/**
 * @brief  处理采样块，提取特征并评估告警
 * @param  block: 采样块指针（输入，只读）
 * @param  result: 周期结果指针（输出，写入特征值和告警状态）
 *
 * @note   处理流程：
 *         1. 复制元数据（温度/采样率/样本数）
 *         2. 温度：范围检查（-55°C ~ 125°C），通过后设置 VALID_TEMPERATURE
 *         3. 振动：对 X/Y/Z 三轴分别执行：
 *            - 原始 counts → Q15 归一化（除以 16384）
 *            - 去直流（减去均值）
 *            - 低通滤波（3 点 FIR）
 *            - 三角窗（避免频谱泄漏）
 *            - RMS（CMSIS-DSP arm_rms_q15）
 *            - 峰峰值（最大值 - 最小值）
 *            - 峰值因子（峰值 / RMS × 1000）
 *            - FFT（CMSIS-DSP arm_rfft_q15，1024 点）
 *            - 频带能量（50-400Hz 范围内所有 bin 的功率和）
 *         4. 电流：
 *            - 去零点偏置（减去 2068 counts）
 *            - 转 Q15 格式（除以 2048）
 *            - RMS（CMSIS-DSP arm_rms_q15）
 *            - 转物理单位（除以 83 counts/A，得到 mA）
 *         5. 汇总错误标志（NO_SENSOR/TEMP/VIB/CURRENT/SATURATION/OVERFLOW）
 *         6. 调用 MonitoringAlerts_Update() 评估告警状态
 *
 * @note   有效标志设置规则：
 *         - 温度：flags 无 TEMP_INVALID 且数值在 [-55°C, 125°C] 范围内
 *         - 振动：flags 无 VIB_INVALID 且 vibration_sample_count == 1024
 *         - 电流：flags 无 CURRENT_INVALID/SATURATION 且 current_sample_count == 1024
 *
 * @note   饱和计数：
 *         - 振动去直流后若超出 Q15 范围，计数 +1
 *         - 振动加窗后若超出 Q15 范围，计数 +1
 *         - 电流去零点后若超出 Q15 范围，计数 +1
 *         - 最终累计值写入 result->algorithm_saturation_count
 *         - 若 > 0，设置 MONITOR_ERROR_ALGORITHM 标志
 */
void MonitoringAlgorithm_Process(const monitor_sample_block_t *block,
                                 monitor_cycle_result_t *result);

#endif /* MONITORING_ALGORITHM_H */
