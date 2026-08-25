# 系统完整运行时序图

## 从上电到稳定运行的完整时序

```
时间轴 →
════════════════════════════════════════════════════════════════════════════════════

0ms: 上电复位
     │
     ├─ Bootloader
     ├─ Reset_Handler (初始化 .data/.bss)
     ├─ SystemInit()
     └─ 跳转到 main()

5ms: main() 开始
     │
     ├─ HAL_Init()
     ├─ SystemClock_Config() (72 MHz)
     ├─ MX_GPIO_Init()
     ├─ MX_DMA_Init()
     ├─ MX_RTC_Init()
     ├─ MX_ADC1_Init()
     ├─ MX_I2C1_Init()
     ├─ MX_SPI2_Init()
     ├─ MX_TIM3_Init()
     └─ MX_USART1_UART_Init()

50ms: MX_FREERTOS_Init()
      │
      └─ MonitoringTasks_Create()
         ├─ 初始化 I2C/SPI/1-Wire
         ├─ 初始化传感器 (DS18B20/MPU6050/NRF24)
         ├─ 创建 g_log_mutex
         ├─ 创建 g_monitor_cycle_event
         ├─ 创建 g_monitor_control_event
         ├─ 创建 g_cycle_request_queue
         ├─ 创建 g_sample_free_queue
         ├─ 创建 g_sample_ready_queue
         ├─ 创建 g_result_queue
         ├─ 创建 g_report_done_sem
         ├─ 初始化采样块池 (2 个块放入 free_queue)
         └─ 创建 6 个任务
            ├─ AcquisitionTask  (优先级 48, 栈 2048)
            ├─ ProcessingTask   (优先级 40, 栈 2048)
            ├─ ReportTask       (优先级 16, 栈 1024)
            ├─ CycleTask        (优先级 24, 栈 1024)
            ├─ HealthTask       (优先级 8,  栈 512)
            └─ WatchdogTask     (优先级 8,  栈 512)

100ms: osKernelStart()
       │
       └─ FreeRTOS 调度器启动

100ms: 所有任务初始状态
       ┌──────────────────────────────────────────────────────────────────┐
       │ AcquisitionTask:  [Blocked] 等待 cycle_request_queue            │
       │ ProcessingTask:   [Blocked] 等待 sample_ready_queue             │
       │ ReportTask:       [Blocked] 等待 result_queue                   │
       │ CycleTask:        [Blocked] 等待 RTC_ALARM 事件                 │
       │ HealthTask:       [Blocked] osDelay(10000)                      │
       │ WatchdogTask:     [Blocked] osDelay(1000)                       │
       │ defaultTask:      [Blocked] osDelay(500)                        │
       └──────────────────────────────────────────────────────────────────┘

200ms: CycleTask 手动触发第一个周期
       │
       └─ MonitoringTasks_RearmRtcAlarm("boot")
          ├─ RTC_SetNextAlarm(10)  // 测试模式 10 秒
          └─ RTC 闹钟设置完成

══════════════════════════════════════════════════════════════════════════════════
第一个周期开始 (假设 10 秒测试模式)
══════════════════════════════════════════════════════════════════════════════════

10.2s: RTC 闹钟中断触发
       │
       ├─ HAL_RTC_AlarmAEventCallback()
       │  └─ osEventFlagsSet(g_monitor_cycle_event, MONITOR_EVENT_RTC_ALARM)
       │
       └─ CycleTask 被唤醒 [Blocked → Ready]

10.21s: CycleTask 开始运行
        ┌─────────────────────────────────────────────────────────────────┐
        │ CycleTask                                                       │
        ├─────────────────────────────────────────────────────────────────┤
        │ 1. osEventFlagsWait 返回 (收到 RTC_ALARM)                      │
        │ 2. 生成 cycle_request:                                         │
        │    - cycle_id = 1                                              │
        │    - timestamp_ticks = 10210                                   │
        │ 3. osMessageQueuePut(cycle_request_queue, &request)            │
        │ 4. osDelay(50)  // 让出 CPU                                    │
        │ 5. osSemaphoreAcquire(report_done_sem, 12000)  [进入 Blocked]  │
        └─────────────────────────────────────────────────────────────────┘
                   │
                   │ cycle_request_queue
                   ↓

10.22s: AcquisitionTask 被唤醒 [Blocked → Ready → Running]
        ┌─────────────────────────────────────────────────────────────────┐
        │ AcquisitionTask (优先级最高 48)                                 │
        ├─────────────────────────────────────────────────────────────────┤
        │ 1. osMessageQueueGet 返回 (收到 cycle_request)                 │
        │ 2. osMessageQueueGet(sample_free_queue, &block, 100)           │
        │    └─ 取到空闲块                                               │
        │ 3. memset(block, 0, sizeof(*block))                            │
        │ 4. block->cycle_id = 1                                         │
        │    block->sequence = 1                                         │
        │    block->timestamp_ticks = 10210                              │
        │ 5. 开始采集...                                                 │
        └─────────────────────────────────────────────────────────────────┘

10.23s: 采集 - 温度 (DS18B20)
        ┌─────────────────────────────────────────────────────────────────┐
        │ DS18B20_Init()                      [10 ms]                     │
        │ DS18B20_StartConversion()           [5 ms]                      │
        │ HAL_Delay(750)                      [750 ms]  ← 等待转换        │
        │ DS18B20_ReadTemperature()           [10 ms]                     │
        │                                                                 │
        │ 结果: temperature_raw = 2512 (25.12°C)                          │
        └─────────────────────────────────────────────────────────────────┘

11.00s: 采集 - 振动 (MPU6050)
        ┌─────────────────────────────────────────────────────────────────┐
        │ MPU6050_StartCapture()              [5 ms]                      │
        │ ├─ 使能 FIFO                                                   │
        │ ├─ 设置采样率 1 kHz                                            │
        │ └─ 使能 DRDY 中断                                              │
        │                                                                 │
        │ [等待 FIFO 填满 1280 个样本]       [1280 ms]                    │
        │ ├─ 每 1 ms: DRDY 中断触发                                      │
        │ └─ 每 5 个样本保留 4 个 (等效 800 Hz)                          │
        │                                                                 │
        │ MPU6050_ReadCapture()               [50 ms]                     │
        │ ├─ 读取 FIFO (1280 × 6 字节)                                   │
        │ ├─ 解析为 X/Y/Z 三轴数据                                       │
        │ └─ 存入 block->vibration[3][1024]                              │
        │                                                                 │
        │ 结果: 1024 点 × 3 轴振动数据                                    │
        └─────────────────────────────────────────────────────────────────┘

12.33s: 采集 - 电流 (ADC + DMA)
        ┌─────────────────────────────────────────────────────────────────┐
        │ HAL_ADC_Start_DMA(&hadc1, buffer, 1024)  [1 ms]                 │
        │ HAL_TIM_Base_Start(&htim3)  // 1 kHz 触发  [1 ms]               │
        │                                                                 │
        │ [DMA 自动采集 1024 点]              [1024 ms]                    │
        │ ├─ TIM3 每 1 ms 触发一次 ADC                                    │
        │ ├─ DMA 自动搬运到 g_adc_dma_buffer                             │
        │ ├─ 半满中断 (512 点): g_adc_half_ready = 1                     │
        │ └─ 全满中断 (1024 点): g_adc_full_ready = 1                    │
        │                                                                 │
        │ 轮询检查标志并复制数据:                                         │
        │ ├─ while (!g_adc_half_ready) { 检查超时 }                       │
        │ ├─ memcpy(block->current[0:512], buffer[0:512])                │
        │ ├─ while (!g_adc_full_ready) { 检查超时 }                       │
        │ └─ memcpy(block->current[512:1024], buffer[512:1024])          │
        │                                                                 │
        │ HAL_ADC_Stop_DMA(&hadc1)                                        │
        │ HAL_TIM_Base_Stop(&htim3)                                       │
        │                                                                 │
        │ 结果: 1024 点电流 ADC 值                                         │
        └─────────────────────────────────────────────────────────────────┘

13.36s: 采集完成
        ┌─────────────────────────────────────────────────────────────────┐
        │ AcquisitionTask                                                 │
        ├─────────────────────────────────────────────────────────────────┤
        │ 6. MonitoringAcquisition_Capture 返回                           │
        │    └─ 总耗时: ~3.1 秒                                           │
        │ 7. osMessageQueuePut(sample_ready_queue, &block, 0, 100)       │
        │    └─ ProcessingTask 被唤醒 [Blocked → Ready]                  │
        │ 8. g_health.samples_ready++                                     │
        │ 9. 循环回到 osMessageQueueGet(cycle_request_queue, ...)        │
        │    └─ 队列空，进入 [Blocked]                                   │
        └─────────────────────────────────────────────────────────────────┘
                   │
                   │ sample_ready_queue
                   ↓

13.37s: ProcessingTask 开始运行 [Blocked → Running]
        ┌─────────────────────────────────────────────────────────────────┐
        │ ProcessingTask (优先级 40)                                      │
        ├─────────────────────────────────────────────────────────────────┤
        │ 1. osMessageQueueGet 返回 (收到 sample block)                  │
        │ 2. start_ticks = HAL_GetTick()                                 │
        │ 3. memset(&result, 0, sizeof(result))                          │
        │ 4. result.cycle_id = 1                                         │
        │ 5. 开始算法处理...                                              │
        └─────────────────────────────────────────────────────────────────┘

13.37s: 算法处理 - 温度
        ┌─────────────────────────────────────────────────────────────────┐
        │ result.temperature_centi = block->temperature_raw               │
        │ result.temperature_centi = 2512  (25.12°C)                      │
        │                                                  [< 1 ms]        │
        └─────────────────────────────────────────────────────────────────┘

13.37s: 算法处理 - 振动 (X/Y/Z 三轴，每轴独立处理)
        ┌─────────────────────────────────────────────────────────────────┐
        │ 处理振动 X 轴:                                  [~30 ms]         │
        │ ├─ 1. 去直流 (减去均值)                        [2 ms]           │
        │ │     mean = sum(samples) / 1024                                │
        │ │     samples[i] -= mean                                        │
        │ │                                                               │
        │ ├─ 2. 转换为 Q15 定点数                        [3 ms]           │
        │ │     q15_samples[i] = (int16_t)(samples[i] * 32768 / range)   │
        │ │                                                               │
        │ ├─ 3. 加汉宁窗                                 [5 ms]           │
        │ │     q15_window[i] = q15_samples[i] * hanning[i]              │
        │ │                                                               │
        │ ├─ 4. 1024 点 CFFT (CMSIS-DSP)                 [15 ms]          │
        │ │     arm_cfft_q15(&arm_cfft_sR_q15_len1024, q15_window, ...)  │
        │ │                                                               │
        │ ├─ 5. 计算幅度谱                               [3 ms]           │
        │ │     arm_cmplx_mag_q15(q15_window, magnitude, 512)            │
        │ │                                                               │
        │ ├─ 6. 计算 RMS                                 [1 ms]           │
        │ │     arm_rms_q15(q15_window, 1024, &rms_q15)                  │
        │ │     result.vibration_rms_mg[0] = rms_q15 * scale             │
        │ │                                                               │
        │ └─ 7. 提取频段能量                             [1 ms]           │
        │       ├─ 0-10 Hz:   energy_band[0]                             │
        │       ├─ 10-40 Hz:  energy_band[1]                             │
        │       ├─ 40-150 Hz: energy_band[2]                             │
        │       └─ 150-400 Hz: energy_band[3]                            │
        │                                                                 │
        │ 重复处理 Y 轴:                                  [~30 ms]         │
        │ 重复处理 Z 轴:                                  [~30 ms]         │
        │                                                                 │
        │ 结果: RMS、频段能量 × 3 轴                                       │
        └─────────────────────────────────────────────────────────────────┘

13.46s: 算法处理 - 电流
        ┌─────────────────────────────────────────────────────────────────┐
        │ 1. 计算均值 (零点偏置)                         [2 ms]           │
        │    sum = 0                                                      │
        │    for (i = 0; i < 1024; i++)                                  │
        │      sum += block->current_samples[i]                          │
        │    mean = sum / 1024                                           │
        │                                                                 │
        │ 2. 去直流 + 转 Q15                             [3 ms]           │
        │    for (i = 0; i < 1024; i++)                                  │
        │      dc_removed = current_samples[i] - mean                    │
        │      q15_samples[i] = dc_removed * scale                       │
        │                                                                 │
        │ 3. 计算 RMS                                    [1 ms]           │
        │    arm_rms_q15(q15_samples, 1024, &rms_q15)                    │
        │                                                                 │
        │ 4. 转换为 mA                                   [< 1 ms]         │
        │    result.current_milliamp = rms_q15 * COUNTS_PER_AMP / 83     │
        │                                                                 │
        │ 结果: current_milliamp = 120 mA                                 │
        └─────────────────────────────────────────────────────────────────┘

13.47s: 告警评估
        ┌─────────────────────────────────────────────────────────────────┐
        │ MonitoringAlerts_Evaluate(&result)             [2 ms]           │
        │                                                                 │
        │ 检查温度:                                                        │
        │ ├─ if (temperature > 8500)  // 85°C                            │
        │ │    g_over_count[0]++                                         │
        │ │    if (g_over_count[0] >= 3)  // 连续 3 次                   │
        │ │      alert_state = ACTIVE                                    │
        │ └─ 当前: 25.12°C < 85°C → NORMAL                               │
        │                                                                 │
        │ 检查电流:                                                        │
        │ ├─ if (current > 2000)  // 2000 mA                             │
        │ └─ 当前: 120 mA < 2000 mA → NORMAL                             │
        │                                                                 │
        │ 检查振动 (X/Y/Z):                                                │
        │ ├─ if (vib_rms > 1000)  // 1000 mg (1g)                        │
        │ └─ 当前: 15/18/12 mg < 1000 mg → NORMAL                        │
        │                                                                 │
        │ 结果: alert_mask = 0x00 (无告警)                                │
        │       alert_states = 0x00 (全部 NORMAL)                         │
        └─────────────────────────────────────────────────────────────────┘

13.47s: 处理完成
        ┌─────────────────────────────────────────────────────────────────┐
        │ ProcessingTask                                                  │
        ├─────────────────────────────────────────────────────────────────┤
        │ 6. result.processing_time_ms = HAL_GetTick() - start_ticks     │
        │    └─ 约 100 ms                                                │
        │ 7. osMessageQueuePut(result_queue, &result, 0, 100)            │
        │    └─ ReportTask 被唤醒 [Blocked → Ready]                      │
        │ 8. osMessageQueuePut(sample_free_queue, &block, 0, 0)          │
        │    └─ 归还采样块到空闲池                                       │
        │ 9. 循环回到 osMessageQueueGet(sample_ready_queue, ...)         │
        │    └─ 队列空，进入 [Blocked]                                   │
        └─────────────────────────────────────────────────────────────────┘
                   │
                   │ result_queue
                   ↓

13.48s: ReportTask 开始运行 [Blocked → Running]
        ┌─────────────────────────────────────────────────────────────────┐
        │ ReportTask (优先级 16)                                          │
        ├─────────────────────────────────────────────────────────────────┤
        │ 1. osMessageQueueGet 返回 (收到 result)                        │
        │ 2. 开始上报...                                                  │
        └─────────────────────────────────────────────────────────────────┘

13.48s: UART 日志输出
        ┌─────────────────────────────────────────────────────────────────┐
        │ osMutexAcquire(g_log_mutex, ...)               [< 1 ms]         │
        │                                                                 │
        │ MONITOR_LOG("[RTOS] cycle=%lu state=%s valid=0x%08lx           │
        │   errors=0x%08lx sample=0x%08lx temp=%s%ld.%02ld               │
        │   current=%ldmA vib=%ld/%ld/%ld alert=0x%08lx                  │
        │   alert_state=0x%08lx count=%u/%u/%u/%u/%u                     │
        │   process_ms=%lu\r\n",                                          │
        │   1, "EVALUATE", 0x00000007, 0x00000000, 0x00000000,           │
        │   "", 25, 12, 120, 15, 18, 12, 0x00000000,                     │
        │   0x00000000, 0, 0, 0, 0, 0, 100);                             │
        │                                                 [~100 ms]       │
        │ osMutexRelease(g_log_mutex)                                    │
        │                                                                 │
        │ 输出内容:                                                        │
        │ [RTOS] cycle=1 state=EVALUATE valid=0x00000007                 │
        │        temp=25.12°C current=120mA vib=15/18/12mg               │
        │        alert=0x00000000 process_ms=100                         │
        └─────────────────────────────────────────────────────────────────┘

13.58s: NRF24L01 无线上报
        ┌─────────────────────────────────────────────────────────────────┐
        │ if (MonitoringNrf24_IsReady())                 [< 1 ms]         │
        │ {                                                               │
        │   1. 编码载荷 (压缩为 32 字节):                [2 ms]           │
        │      ├─ cycle_id: 4 字节                                       │
        │      ├─ timestamp: 4 字节                                      │
        │      ├─ temperature: 2 字节                                    │
        │      ├─ current: 2 字节                                        │
        │      ├─ vib_x/y/z: 6 字节                                      │
        │      ├─ alert_mask: 1 字节                                     │
        │      ├─ alert_states: 1 字节                                   │
        │      ├─ valid_mask: 1 字节                                     │
        │      └─ reserved: 9 字节                                       │
        │                                                                 │
        │   2. 发送载荷:                                 [5 ms]           │
        │      MonitoringNrf24_SendPayload(payload, 32)                  │
        │      ├─ 写 TX FIFO                                             │
        │      ├─ 拉高 CE (10 μs)                                        │
        │      ├─ 等待发送完成 (最多 3 次重传)                            │
        │      └─ 检查状态寄存器                                         │
        │                                                                 │
        │   3. 统计:                                                      │
        │      if (status == OK)                                         │
        │        g_health.nrf24_reports++                                │
        │      else                                                       │
        │        g_health.nrf24_errors++                                 │
        │ }                                                               │
        └─────────────────────────────────────────────────────────────────┘

13.59s: 上报完成
        ┌─────────────────────────────────────────────────────────────────┐
        │ ReportTask                                                      │
        ├─────────────────────────────────────────────────────────────────┤
        │ 3. g_health.reports_sent++                                      │
        │ 4. osSemaphoreRelease(g_report_done_sem)                       │
        │    └─ CycleTask 被唤醒 [Blocked → Ready]                       │
        │ 5. 循环回到 osMessageQueueGet(result_queue, ...)               │
        │    └─ 队列空，进入 [Blocked]                                   │
        └─────────────────────────────────────────────────────────────────┘
                   │
                   │ report_done_sem
                   ↓

13.59s: CycleTask 被唤醒 [Blocked → Ready → Running]
        ┌─────────────────────────────────────────────────────────────────┐
        │ CycleTask                                                       │
        ├─────────────────────────────────────────────────────────────────┤
        │ 1. osSemaphoreAcquire 返回 (收到信号量)                        │
        │ 2. 检查 Stop 门禁条件:                         [1 ms]           │
        │    ├─ g_watchdog_permit == 1  ✅                               │
        │    ├─ g_health.state == IDLE  ✅                               │
        │    └─ 所有队列深度 == 0  ✅                                    │
        │ 3. MonitoringTasks_EnterStop()                                  │
        └─────────────────────────────────────────────────────────────────┘

14.00s: 进入 Stop 模式
        ┌─────────────────────────────────────────────────────────────────┐
        │ MonitoringTasks_EnterStop()                                     │
        ├─────────────────────────────────────────────────────────────────┤
        │ 1. 停止 ADC:                                   [< 1 ms]         │
        │    HAL_ADC_Stop_DMA(&hadc1)                                     │
        │                                                                 │
        │ 2. 停止 TIM3:                                  [< 1 ms]         │
        │    HAL_TIM_Base_Stop(&htim3)                                    │
        │                                                                 │
        │ 3. 停止 MPU6050:                               [5 ms]           │
        │    MPU6050_StopCapture()                                        │
        │                                                                 │
        │ 4. 设置 RTC 闹钟 (10 秒后):                    [10 ms]          │
        │    RTC_SetNextAlarm(10)                                         │
        │    ├─ current_counter = RTC_GetCounter()                       │
        │    ├─ alarm_counter = current_counter + 10                     │
        │    └─ RTC_SetAlarmCounter(alarm_counter)                       │
        │                                                                 │
        │ 5. 等待 RTC 同步:                              [5 ms]           │
        │    RTC_WaitForSync()                                            │
        │    └─ while (!(RTC->CRL & RTC_CRL_RTOFF))                      │
        │                                                                 │
        │ 6. 进入 Stop 模式:                             [< 1 ms]         │
        │    HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON,             │
        │                          PWR_STOPENTRY_WFI)                     │
        │    └─ WFI 指令 (Wait For Interrupt)                            │
        │                                                                 │
        │ ═══════════════════════════════════════════════════════════════ │
        │ CPU 停止，进入低功耗模式                                         │
        │ 功耗: 10-50 μA                                                  │
        │ 等待 RTC 闹钟中断唤醒...                                         │
        │ ═══════════════════════════════════════════════════════════════ │
        └─────────────────────────────────────────────────────────────────┘

14.00s - 24.00s: Stop 模式 (低功耗等待 10 秒)
                 ┌──────────────────────────────────────────────────────┐
                 │ [CPU 停止]                                           │
                 │ [外设停止]                                           │
                 │ [SRAM 保持]                                          │
                 │ [RTC 运行]                                           │
                 │ 功耗: ~30 μA                                         │
                 └──────────────────────────────────────────────────────┘

══════════════════════════════════════════════════════════════════════════════════
第二个周期开始 (从 Stop 模式唤醒)
══════════════════════════════════════════════════════════════════════════════════

24.00s: RTC 闹钟中断触发
        │
        ├─ 系统从 Stop 模式唤醒
        ├─ 恢复系统时钟 (HSE + PLL → 72 MHz)           [~5 ms]
        ├─ 恢复外设时钟
        └─ 继续执行 (从 WFI 指令后继续)

24.01s: MonitoringTasks_EnterStop() 返回
        ┌─────────────────────────────────────────────────────────────────┐
        │ MonitoringTasks_EnterStop()                                     │
        ├─────────────────────────────────────────────────────────────────┤
        │ 7. 唤醒后恢复外设:                             [50 ms]          │
        │    MonitoringDrivers_Resume()                                   │
        │    ├─ 恢复 I2C/SPI                                             │
        │    ├─ 重新初始化 DS18B20                                       │
        │    ├─ 重新初始化 MPU6050                                       │
        │    ├─ 恢复 ADC                                                 │
        │    ├─ 重新初始化 NRF24L01                                      │
        │    └─ 恢复 TIM3                                                │
        │                                                                 │
        │ 8. 返回到 CycleTask                                             │
        └─────────────────────────────────────────────────────────────────┘

24.06s: CycleTask 继续运行
        │
        └─ 循环回到 osEventFlagsWait(RTC_ALARM, ...)

24.07s: HAL_RTC_AlarmAEventCallback() 被调用
        │
        └─ osEventFlagsSet(g_monitor_cycle_event, MONITOR_EVENT_RTC_ALARM)
           └─ CycleTask 立即被唤醒

24.08s: 重复第一个周期的流程...
        │
        ├─ CycleTask 生成 cycle_request (cycle_id = 2)
        ├─ AcquisitionTask 采集 (~3.1 秒)
        ├─ ProcessingTask 处理 (~0.1 秒)
        ├─ ReportTask 上报 (~0.1 秒)
        ├─ CycleTask 进入 Stop 模式
        └─ 等待下一个 RTC 闹钟 (10 秒后)...

══════════════════════════════════════════════════════════════════════════════════

34.20s: 第三个周期...
44.30s: 第四个周期...
54.40s: 第五个周期...
...
```

---

## 后台任务时序（与主流程并行）

```
时间轴 →
════════════════════════════════════════════════════════════════════════════════════

0.5s: defaultTask 第一次运行
      ┌──────────────────────────────────────────────────────────────────┐
      │ defaultTask (优先级 24)                                          │
      ├──────────────────────────────────────────────────────────────────┤
      │ HAL_GPIO_TogglePin(STATUS_LED, ...)                              │
      │ osDelay(500)                        [进入 Blocked]               │
      └──────────────────────────────────────────────────────────────────┘

1.0s: defaultTask 再次运行
      │
      └─ 每 500 ms 翻转一次 LED (循环...)

1.0s: WatchdogTask 第一次运行
      ┌──────────────────────────────────────────────────────────────────┐
      │ WatchdogTask (优先级 8)                                          │
      ├──────────────────────────────────────────────────────────────────┤
      │ if (g_health.cycles_started == 0)  // 还未开始周期               │
      │   continue                                                       │
      │ osDelay(1000)                       [进入 Blocked]               │
      └──────────────────────────────────────────────────────────────────┘

2.0s: WatchdogTask 再次运行
      │
      └─ 每 1 秒检查一次心跳 (循环...)

10.0s: HealthTask 第一次运行
       ┌─────────────────────────────────────────────────────────────────┐
       │ HealthTask (优先级 8)                                           │
       ├─────────────────────────────────────────────────────────────────┤
       │ 1. MonitoringSpi_GetStatus(&spi_status)                         │
       │ 2. uxTaskGetStackHighWaterMark(每个任务)                        │
       │ 3. MONITOR_LOG("[HEALTH] state=%s cycles=%lu ...")              │
       │ 4. osDelay(10000)                   [进入 Blocked]              │
       └─────────────────────────────────────────────────────────────────┘

20.0s: HealthTask 再次运行
       │
       └─ 每 10 秒输出一次健康日志 (循环...)

在 Stop 模式期间 (14.00s - 24.00s):
       ┌─────────────────────────────────────────────────────────────────┐
       │ 所有任务挂起 (包括后台任务)                                      │
       │ 只有 RTC 在运行                                                  │
       └─────────────────────────────────────────────────────────────────┘

唤醒后 (24.00s+):
       ┌─────────────────────────────────────────────────────────────────┐
       │ 所有任务恢复运行                                                 │
       │ 继续各自的延时计数                                               │
       └─────────────────────────────────────────────────────────────────┘
```

---

## 时间开销总结

```
单个周期 (10 秒测试模式):
┌─────────────────────────────────────────────────────────────────┐
│ 阶段          │ 时间     │ 功耗     │ 说明                      │
├─────────────────────────────────────────────────────────────────┤
│ 采集 - 温度   │ 0.77 s   │ 30 mA    │ DS18B20 转换 750 ms      │
│ 采集 - 振动   │ 1.33 s   │ 35 mA    │ MPU6050 1280 点          │
│ 采集 - 电流   │ 1.03 s   │ 35 mA    │ ADC DMA 1024 点          │
│ 处理 - FFT    │ 0.10 s   │ 40 mA    │ 3 轴 FFT + RMS           │
│ 上报 - UART   │ 0.10 s   │ 25 mA    │ 详细日志                 │
│ 上报 - NRF24  │ 0.01 s   │ 13 mA    │ 无线发送                 │
│ 调度开销      │ 0.05 s   │ 20 mA    │ 任务切换                 │
├─────────────────────────────────────────────────────────────────┤
│ 运行总计      │ 3.39 s   │ 平均32mA │                          │
│ Stop 等待     │ 6.61 s   │ 0.03 mA  │ 低功耗                   │
├─────────────────────────────────────────────────────────────────┤
│ 周期总计      │ 10.00 s  │ 平均11mA │ 电池续航约 4 天          │
└─────────────────────────────────────────────────────────────────┘

单个周期 (300 秒生产模式):
┌─────────────────────────────────────────────────────────────────┐
│ 阶段          │ 时间     │ 功耗     │ 说明                      │
├─────────────────────────────────────────────────────────────────┤
│ 运行          │ 3.39 s   │ 32 mA    │ 同上                     │
│ Stop 等待     │ 296.61 s │ 0.03 mA  │ 低功耗                   │
├─────────────────────────────────────────────────────────────────┤
│ 周期总计      │ 300.00 s │ 平均0.7mA│ 电池续航约 58 天         │
└─────────────────────────────────────────────────────────────────┘
```

---

## 完
