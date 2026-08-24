/* =============================================================================
 * monitoring_algorithm.c - 信号处理与特征提取层实现
 *
 * Q15 定点运算策略：
 *   STM32F103ZET6 无硬件 FPU，使用 CMSIS-DSP 库的 Q15 定点函数：
 *   - Q15 格式：16 位有符号整数，表示 [-1.0, +1.0) 范围
 *   - 映射关系：-32768 → -1.0，0 → 0.0，32767 → +0.999969
 *   - 乘法归一化：(a * b) >> 15（保持 Q15 格式）
 *   - 优势：单周期硬件乘法，无浮点软件模拟开销
 *
 * 振动处理链路（每个轴独立）：
 *   1. 原始 counts → Q15 归一化
 *      MPU6050 ±2g 量程，16384 counts/g
 *      Q15 值 = (原始值 × 32767) / 16384 ≈ 原始值 × 2
 *      （实际实现：原始值 × Q15_MAX / MPU6050_COUNTS_PER_G）
 *
 *   2. 去直流（减去均值）
 *      使用 CMSIS-DSP arm_mean_q15() 计算均值，逐点相减
 *      目的：FFT 前去除直流分量，避免 DC bin 能量过大
 *
 *   3. 低通滤波（3 点 FIR）
 *      系数 [10923, 10922, 10922]（Q15 格式，和 ≈ 32767 ≈ 1.0）
 *      y[n] = 0.3333×x[n] + 0.3333×x[n-1] + 0.3333×x[n-2]
 *      目的：平滑高频噪声，保留主要振动成分
 *
 *   4. 三角窗（Triangular window）
 *      window[i] = i×64        (i <= 512)
 *                = (1023-i)×64  (i > 512)
 *      Q15 乘法：(signal × window) >> 15
 *      目的：减少 FFT 频谱泄漏，避免在 F103 上逐点 cos 计算
 *      注：×64 是因为 Q15 格式下，64/32768 ≈ 2/1024（三角窗峰值增益）
 *
 *   5. RMS（有效值）
 *      使用 CMSIS-DSP arm_rms_q15()
 *      结果从 Q15 转回 mg：(rms_q15 × 1000) / 32767
 *
 *   6. 峰峰值与峰值因子
 *      峰峰值 = 最大值 - 最小值（先转 mg）
 *      峰值因子 = (峰值 / RMS) × 1000（×1000 避免小数）
 *
 *   7. FFT 与频带能量
 *      使用 CMSIS-DSP arm_rfft_q15()，1024 点实数 FFT
 *      输出格式：交错实部/虚部，2048 个 q15_t
 *      频带能量：50-400Hz 范围内所有 bin 的 (real² + imag²) 之和
 *      bin 索引计算：bin = (频率 × FFT长度) / 采样率
 *      能量归一化：右移 20 位（避免溢出 uint32_t）
 *
 * 电流处理链路：
 *   1. 去零点偏置
 *      ACS712 输出 = Vcc/2（零点）+ 灵敏度×电流
 *      零点标定值：2068 counts（实测值，对应 1.65V）
 *      centered = raw - 2068
 *
 *   2. 转 Q15 格式
 *      Q15 值 = (centered × 32767) / 2048
 *      （2048 是 ADC 半量程，对应 ±Vcc/2 的电流摆幅）
 *
 *   3. RMS
 *      使用 CMSIS-DSP arm_rms_q15()
 *      结果转回 ADC counts：(rms_q15 × 2048) / 32767
 *
 *   4. 转物理单位
 *      电流（mA）= (rms_counts × 1000) / 83
 *      （83 counts/A 是 ACS712-05B 的灵敏度）
 *
 * 饱和检测：
 *   - 每次 Q15 转换前检查是否超出 [-32768, 32767]
 *   - 超出则饱和计数 +1，并钳位到边界
 *   - 最终累计值写入 result->algorithm_saturation_count
 *   - 若 > 0，设置 MONITOR_ERROR_ALGORITHM 标志
 * ============================================================================= */

#include "monitoring_algorithm.h"
#include "monitoring_alerts.h"
#include "arm_math.h"

#include <string.h>

/* ===== 常量定义 ===== */

/* Q15 格式最大值（对应 +0.999969） */
#define Q15_MAX_VALUE              32767L

/* ADC 半量程（12 位 ADC，Vref/2 对应 2048 counts） */
#define CURRENT_ADC_HALF_SCALE     2048L

/* MPU6050 灵敏度：±2g 量程，16384 counts/g */
#define MPU6050_COUNTS_PER_G       16384L

/* 毫重力单位：1g = 1000mg */
#define MPU6050_MG_PER_G           1000L

/* FFT 频带范围：50Hz（低频噪声截止）到 400Hz（奈奎斯特频率的一半） */
#define FFT_LOW_HZ                 50U
#define FFT_HIGH_HZ                400U

/* RFFT 输出数组大小：实数 FFT 输出交错实部/虚部，长度 = 2 × 输入长度 */
#define MONITOR_RFFT_OUTPUT_SAMPLES (MONITOR_VIBRATION_SAMPLES * 2U)

/* ===== 静态工作区（避免栈溢出） ===== */

/* ===== 静态工作区（避免栈溢出） ===== */

/* 振动处理工作区（1024 点 Q15，每个 2 字节，共 2KB） */
static q15_t g_q15_window[MONITOR_VIBRATION_SAMPLES];      /* 加窗后的信号 */
static q15_t g_filter_input[MONITOR_VIBRATION_SAMPLES];    /* 去直流后的信号 */
static q15_t g_filter_output[MONITOR_VIBRATION_SAMPLES];   /* 滤波后的信号 */

/* 电流处理工作区（1024 点 Q15，2KB） */
static q15_t g_current_q15[MONITOR_CURRENT_SAMPLES];

/* FFT 输出缓冲区（2048 点 Q15，4KB）
 * 旧版 CMSIS-DSP 的 arm_rfft_q15() 使用交错的实部/虚部格式，输出区
 * 按 2 × fftLenReal 个 q15_t 写入。不能按”有效频点数”只分配 1024 个
 * 元素，否则处理第一个真实振动窗口时会越界破坏相邻 SRAM。 */
static q15_t g_fft_output[MONITOR_RFFT_OUTPUT_SAMPLES];

/* FFT 实例（CMSIS-DSP 内部结构，包含旋转因子表指针） */
static arm_rfft_instance_q15 g_rfft;

/* FFT 初始化标志（首次调用 ProcessAxis 时初始化，避免重复初始化） */
static uint8_t g_rfft_ready;

/* 三点对称低通 FIR 滤波器系数（Q15 格式）
 * [10923, 10922, 10922] 对应浮点 [0.3333, 0.3333, 0.3333]
 * 系数和 = 32767 ≈ 1.0（Q15 格式），保持信号幅度不变 */
static const q15_t g_vibration_fir_coefficients[3] = {10923, 10922, 10922};

/* ===== 内部辅助函数 ===== */

/**
 * @brief  Q15 饱和钳位
 * @param  value: int32_t 值
 * @return 钳位到 [-32768, 32767] 的 q15_t
 * @note   用于防止 Q15 运算溢出
 */
static q15_t MonitoringAlgorithm_SaturateQ15(int32_t value)
{
  if (value > 32767L)
  {
    return 32767;
  }
  if (value < -32768L)
  {
    return -32768;
  }
  return (q15_t)value;
}

/**
 * @brief  int32_t 绝对值
 * @param  value: 输入值
 * @return 绝对值
 */
static int32_t MonitoringAlgorithm_Abs32(int32_t value)
{
  return value < 0L ? -value : value;
}

/**
 * @brief  3 点 FIR 低通滤波器（Q15 定点）
 * @param  input: 输入信号（Q15 数组）
 * @param  output: 输出信号（Q15 数组）
 * @param  count: 样本数
 * @note   滤波器结构：y[n] = 0.3333×x[n] + 0.3333×x[n-1] + 0.3333×x[n-2]
 * @note   延时状态在函数内部，每次调用独立（不跨轴/跨周期）
 */
static void MonitoringAlgorithm_FilterQ15(const q15_t *input,
                                          q15_t *output,
                                          uint16_t count)
{
  q15_t delay_1 = 0;  /* x[n-1] */
  q15_t delay_2 = 0;  /* x[n-2] */

  for (uint16_t i = 0U; i < count; i++)
  {
    /* Q15 乘法累加：(a × b) >> 15，使用 int64_t 避免中间溢出 */
    int64_t accumulator = (int64_t)g_vibration_fir_coefficients[0] * input[i] +
                          (int64_t)g_vibration_fir_coefficients[1] * delay_1 +
                          (int64_t)g_vibration_fir_coefficients[2] * delay_2;
    output[i] = MonitoringAlgorithm_SaturateQ15((int32_t)(accumulator >> 15));
    delay_2 = delay_1;
    delay_1 = input[i];
  }
}

static void MonitoringAlgorithm_PrepareVibration(const int16_t *input,
                                                  q15_t *output,
                                                  uint32_t *saturation_count)
{
  q15_t mean = 0;
  int32_t value;

  for (uint16_t i = 0U; i < MONITOR_VIBRATION_SAMPLES; i++)
  {
    output[i] = MonitoringAlgorithm_SaturateQ15(
      ((int32_t)input[i] * Q15_MAX_VALUE) / MPU6050_COUNTS_PER_G);
  }
  arm_mean_q15(output, MONITOR_VIBRATION_SAMPLES, &mean);

  for (uint16_t i = 0U; i < MONITOR_VIBRATION_SAMPLES; i++)
  {
    g_filter_input[i] = MonitoringAlgorithm_SaturateQ15(
      (int32_t)output[i] - (int32_t)mean);
  }

  /* 每个轴都是独立窗口，滤波延时状态不能跨轴、跨周期串接。 */
  MonitoringAlgorithm_FilterQ15(g_filter_input,
                                g_filter_output,
                                MONITOR_VIBRATION_SAMPLES);

  for (uint16_t i = 0U; i < MONITOR_VIBRATION_SAMPLES; i++)
  {
    /* 三角窗，避免在无硬件 FPU 的 F103 上逐点调用 cos。 */
    int32_t window = i <= 512U ? (int32_t)i * 64L
                               : (int32_t)(1023U - i) * 64L;
    value = ((int32_t)g_filter_output[i] * window) >> 15;
    if (value > 32767L || value < -32768L)
    {
      (*saturation_count)++;
    }
    g_q15_window[i] = MonitoringAlgorithm_SaturateQ15(value);
  }
  memcpy(output, g_q15_window, sizeof(g_q15_window));
}

static int32_t MonitoringAlgorithm_Q15ToMg(q15_t value)
{
  return ((int32_t)value * 2000L) / Q15_MAX_VALUE;
}

static uint32_t MonitoringAlgorithm_BandEnergy(const q15_t *fft,
                                               uint16_t sample_rate_hz)
{
  uint32_t energy = 0U;
  uint16_t low_bin;
  uint16_t high_bin;

  if (sample_rate_hz == 0U)
  {
    return 0U;
  }
  low_bin = (uint16_t)(((uint32_t)FFT_LOW_HZ * MONITOR_VIBRATION_SAMPLES) /
                       sample_rate_hz);
  high_bin = (uint16_t)(((uint32_t)FFT_HIGH_HZ * MONITOR_VIBRATION_SAMPLES) /
                        sample_rate_hz);
  if (high_bin >= MONITOR_VIBRATION_SAMPLES / 2U)
  {
    high_bin = (MONITOR_VIBRATION_SAMPLES / 2U) - 1U;
  }
  if (low_bin > high_bin)
  {
    return 0U;
  }
  for (uint16_t bin = low_bin; bin <= high_bin; bin++)
  {
    int32_t real = fft[bin * 2U];
    int32_t imag = fft[bin * 2U + 1U];
    energy += (uint32_t)((((int64_t)real * real) +
                          ((int64_t)imag * imag)) >> 20);
  }
  return energy;
}

/**
 * @brief  处理单轴振动数据，提取特征值
 * @param  input: MPU6050 原始值数组（int16_t，±2g 量程，1024 点）
 * @param  sample_rate_hz: 采样率（Hz，通常为 800Hz）
 * @param  rms_mg: 输出 RMS 有效值（mg）
 * @param  peak_to_peak_mg: 输出峰峰值（mg）
 * @param  crest_milli: 输出峰值因子×1000（无量纲）
 * @param  band_energy_milli: 输出频带能量×1000（50-400Hz）
 * @param  saturation_count: 饱和计数器（累加）
 *
 * @note   处理步骤：
 *         1. 预处理（去直流 + 滤波 + 加窗）
 *            调用 MonitoringAlgorithm_PrepareVibration()
 *         2. RMS 计算
 *            使用 CMSIS-DSP arm_rms_q15()，结果从 Q15 转回 mg
 *         3. 峰峰值计算
 *            遍历加窗后的信号，找最大值和最小值（转 mg 后计算）
 *         4. 峰值因子计算
 *            峰值因子 = 峰值 / RMS，×1000 避免小数
 *         5. FFT 与频带能量
 *            使用 CMSIS-DSP arm_rfft_q15()，1024 点实数 FFT
 *            计算 50-400Hz 频带内所有 bin 的功率和
 */
static void MonitoringAlgorithm_ProcessAxis(const int16_t *input,
                                            uint16_t sample_rate_hz,
                                            int32_t *rms_mg,
                                            int32_t *peak_to_peak_mg,
                                            uint32_t *crest_milli,
                                            uint32_t *band_energy_milli,
                                            uint32_t *saturation_count)
{
  q15_t rms_q15 = 0;
  int32_t minimum = 2147483647L;
  int32_t maximum = -2147483647L;
  int32_t peak = 0L;

  MonitoringAlgorithm_PrepareVibration(input, g_q15_window, saturation_count);
  arm_rms_q15(g_q15_window, MONITOR_VIBRATION_SAMPLES, &rms_q15);

  for (uint16_t i = 0U; i < MONITOR_VIBRATION_SAMPLES; i++)
  {
    int32_t value = MonitoringAlgorithm_Q15ToMg(g_q15_window[i]);
    if (value < minimum) minimum = value;
    if (value > maximum) maximum = value;
    if (MonitoringAlgorithm_Abs32(value) > peak) peak = MonitoringAlgorithm_Abs32(value);
  }

  if (g_rfft_ready == 0U)
  {
    g_rfft_ready = (arm_rfft_init_q15(&g_rfft, MONITOR_VIBRATION_SAMPLES,
                                      0U, 1U) == ARM_MATH_SUCCESS) ? 1U : 0U;
  }
  if (g_rfft_ready != 0U)
  {
    arm_rfft_q15(&g_rfft, g_q15_window, g_fft_output);
  }

  *rms_mg = ((int32_t)rms_q15 * MPU6050_MG_PER_G) / Q15_MAX_VALUE;
  *peak_to_peak_mg = maximum - minimum;
  *crest_milli = *rms_mg > 0L ? (uint32_t)((peak * 1000L) / *rms_mg) : 0U;
  *band_energy_milli = g_rfft_ready != 0U
                         ? MonitoringAlgorithm_BandEnergy(g_fft_output, sample_rate_hz)
                         : 0U;
}

/**
 * @brief  处理电流数据，提取有效值
 * @param  block: 采样块指针（只读）
 * @param  result: 周期结果指针（写入电流特征）
 * @param  saturation_count: 饱和计数器（累加）
 *
 * @note   处理步骤：
 *         1. 去零点偏置
 *            ACS712 输出 = Vcc/2（零点）+ 灵敏度×电流
 *            零点标定值：2068 counts（对应 1.65V）
 *         2. 转 Q15 格式
 *            Q15 = (centered × 32767) / 2048
 *            饱和检查：超出 [-32768, 32767] 时计数并钳位
 *         3. RMS 计算
 *            使用 CMSIS-DSP arm_rms_q15()
 *         4. 转回物理单位
 *            rms_raw = (rms_q15 × 2048) / 32767（ADC counts）
 *            milliamp = (rms_raw × 1000) / 83（mA）
 */
static void MonitoringAlgorithm_ProcessCurrent(const monitor_sample_block_t *block,
                                               monitor_cycle_result_t *result,
                                               uint32_t *saturation_count)
{
  uint64_t sum = 0U;
  q15_t rms_q15 = 0;
  int32_t centered;
  int32_t q15_value;

  for (uint16_t i = 0U; i < block->current_sample_count; i++)
  {
    sum += block->current[i];
  }
  if (block->current_sample_count == 0U)
  {
    return;
  }
  result->current_mean_raw = (uint16_t)(sum / block->current_sample_count);
  for (uint16_t i = 0U; i < block->current_sample_count; i++)
  {
    /* ACS712 的零点是偏置电压，电流有效值应相对标定零点计算。 */
    centered = (int32_t)block->current[i] - (int32_t)MONITOR_CURRENT_ZERO_RAW;
    q15_value = (centered * Q15_MAX_VALUE) / CURRENT_ADC_HALF_SCALE;
    if (q15_value > Q15_MAX_VALUE || q15_value < -32768L)
    {
      (*saturation_count)++;
    }
    g_current_q15[i] = MonitoringAlgorithm_SaturateQ15(q15_value);
  }
  arm_rms_q15(g_current_q15, block->current_sample_count, &rms_q15);
  result->current_rms_raw = (uint16_t)(((uint32_t)rms_q15 * CURRENT_ADC_HALF_SCALE) /
                                       Q15_MAX_VALUE);
  result->current_milliamp = ((int32_t)result->current_rms_raw * 1000L) /
                             (int32_t)MONITOR_CURRENT_COUNTS_PER_AMP;
}

/**
 * @brief  处理采样块，提取特征并评估告警
 * @param  block: 采样块指针（输入，只读）
 * @param  result: 周期结果指针（输出，写入特征值和告警状态）
 *
 * @note   处理流程：
 *         1. 复制元数据（温度/采样率/样本数）
 *         2. 温度：范围检查（-55°C ~ 125°C），通过后设置 VALID_TEMPERATURE
 *         3. 振动：对 X/Y/Z 三轴分别调用 ProcessAxis，设置 VALID_VIBRATION
 *         4. 电流：调用 ProcessCurrent，设置 VALID_CURRENT
 *         5. 汇总错误标志（NO_SENSOR/TEMP/VIB/CURRENT/SATURATION/OVERFLOW）
 *         6. 调用 MonitoringAlerts_Update() 评估告警状态
 *
 * @note   有效标志设置条件：
 *         - 温度：flags 无 TEMP_INVALID 且数值在 [-55°C, 125°C]
 *         - 振动：flags 无 VIB_INVALID 且 sample_count == 1024
 *         - 电流：flags 无 CURRENT_INVALID/SATURATION 且 sample_count == 1024
 */
void MonitoringAlgorithm_Process(const monitor_sample_block_t *block,
                                 monitor_cycle_result_t *result)
{
  uint32_t saturation_count = 0U;

  if (block == NULL || result == NULL)
  {
    return;
  }

  result->sample_flags = block->flags;
  result->vibration_sample_count = block->vibration_sample_count;
  result->current_sample_count = block->current_sample_count;
  result->vibration_rate_hz = block->vibration_rate_hz;
  result->current_rate_hz = block->current_rate_hz;
  result->temperature_centi = block->temperature_centi;

  if ((block->flags & MONITOR_SAMPLE_FLAG_TEMP_INVALID) == 0U &&
      block->temperature_centi >= -5500 && block->temperature_centi <= 12500)
  {
    result->valid_mask |= MONITOR_VALID_TEMPERATURE;
  }
  else if ((block->flags & MONITOR_SAMPLE_FLAG_TEMP_INVALID) == 0U)
  {
    result->sample_flags |= MONITOR_SAMPLE_FLAG_TEMP_INVALID;
    result->error_flags |= MONITOR_ERROR_TEMP;
  }
  if ((block->flags & MONITOR_SAMPLE_FLAG_VIB_INVALID) == 0U &&
      block->vibration_sample_count == MONITOR_VIBRATION_SAMPLES)
  {
    result->valid_mask |= MONITOR_VALID_VIBRATION;
    for (uint8_t axis = 0U; axis < MONITOR_VIBRATION_AXES; axis++)
    {
      MonitoringAlgorithm_ProcessAxis(block->vibration[axis],
        block->vibration_rate_hz,
        &result->vibration_rms_mg[axis],
        &result->vibration_peak_to_peak_mg[axis],
        &result->vibration_crest_milli[axis],
        &result->vibration_band_energy_milli[axis],
        &saturation_count);
    }
  }
  if ((block->flags & (MONITOR_SAMPLE_FLAG_CURRENT_INVALID |
                       MONITOR_SAMPLE_FLAG_SATURATION)) == 0U &&
      block->current_sample_count == MONITOR_CURRENT_SAMPLES)
  {
    result->valid_mask |= MONITOR_VALID_CURRENT;
    MonitoringAlgorithm_ProcessCurrent(block, result, &saturation_count);
  }

  if ((block->flags & MONITOR_SAMPLE_FLAG_NO_SENSOR) != 0U)
  {
    result->error_flags |= MONITOR_ERROR_NO_SENSOR;
  }
  if ((block->flags & MONITOR_SAMPLE_FLAG_TEMP_INVALID) != 0U)
  {
    result->error_flags |= MONITOR_ERROR_TEMP;
  }
  if ((block->flags & MONITOR_SAMPLE_FLAG_VIB_INVALID) != 0U)
  {
    result->error_flags |= MONITOR_ERROR_VIBRATION;
  }
  if ((block->flags & MONITOR_SAMPLE_FLAG_CURRENT_INVALID) != 0U)
  {
    result->error_flags |= MONITOR_ERROR_CURRENT;
  }
  if ((block->flags & MONITOR_SAMPLE_FLAG_SATURATION) != 0U)
  {
    result->error_flags |= MONITOR_ERROR_CURRENT;
  }
  if ((block->flags & MONITOR_SAMPLE_FLAG_OVERFLOW) != 0U)
  {
    result->error_flags |= MONITOR_ERROR_ACQUIRE_DROP;
  }
  result->algorithm_saturation_count = saturation_count;
  if (saturation_count != 0U)
  {
    result->error_flags |= MONITOR_ERROR_ALGORITHM;
  }
  MonitoringAlerts_Update(result);
}
