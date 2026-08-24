#include "monitoring_algorithm.h"
#include "monitoring_alerts.h"
#include "arm_math.h"

#include <string.h>

#define Q15_MAX_VALUE              32767L
#define CURRENT_ADC_HALF_SCALE     2048L
#define MPU6050_COUNTS_PER_G       16384L
#define MPU6050_MG_PER_G           1000L
#define FFT_LOW_HZ                 50U
#define FFT_HIGH_HZ                400U
#define MONITOR_RFFT_OUTPUT_SAMPLES (MONITOR_VIBRATION_SAMPLES * 2U)

static q15_t g_q15_window[MONITOR_VIBRATION_SAMPLES];
static q15_t g_filter_input[MONITOR_VIBRATION_SAMPLES];
static q15_t g_filter_output[MONITOR_VIBRATION_SAMPLES];
static q15_t g_current_q15[MONITOR_CURRENT_SAMPLES];
/*
 * 旧版 CMSIS-DSP 的 arm_rfft_q15() 使用交错的实部/虚部格式，输出区
 * 按 2 * fftLenReal 个 q15_t 写入。不能按“有效频点数”只分配 1024 个
 * 元素，否则处理第一个真实振动窗口时会越界破坏相邻 SRAM。
 */
static q15_t g_fft_output[MONITOR_RFFT_OUTPUT_SAMPLES];
static arm_rfft_instance_q15 g_rfft;
static uint8_t g_rfft_ready;

/* 三点对称低通 FIR，系数和约等于 1.0，适合 F103 的 Q15 运算。 */
static const q15_t g_vibration_fir_coefficients[3] = {10923, 10922, 10922};

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

static int32_t MonitoringAlgorithm_Abs32(int32_t value)
{
  return value < 0L ? -value : value;
}

static void MonitoringAlgorithm_FilterQ15(const q15_t *input,
                                          q15_t *output,
                                          uint16_t count)
{
  q15_t delay_1 = 0;
  q15_t delay_2 = 0;

  for (uint16_t i = 0U; i < count; i++)
  {
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
