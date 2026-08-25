# 系统完整运行时序分析

## 目录
1. [系统启动流程](#系统启动流程)
2. [任务创建与初始化](#任务创建与初始化)
3. [正常运行周期](#正常运行周期)
4. [任务切换机制](#任务切换机制)
5. [同步机制详解](#同步机制详解)

---

## 系统启动流程

### 📋 启动顺序（上电 → 第一个任务）

```
┌─────────────────────────────────────────────────────────────┐
│ 1. 硬件复位（上电 / 按复位键 / 看门狗复位）                  │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 2. Bootloader（固化在 ROM，自动执行）                        │
│    - 从 0x08000000 加载程序                                  │
│    - 设置堆栈指针                                            │
│    - 跳转到 Reset_Handler                                    │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 3. Reset_Handler（startup_stm32f103xe.s）                   │
│    - 初始化 .data 段（从 Flash 复制到 RAM）                  │
│    - 清零 .bss 段（未初始化的全局变量）                       │
│    - 调用 SystemInit()                                       │
│    - 调用 __libc_init_array()（C++ 全局对象构造）           │
│    - 跳转到 main()                                           │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 4. main() 函数开始（main.c）                                 │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 5. HAL_Init()                                                │
│    - 配置 NVIC 优先级分组                                    │
│    - 初始化 SysTick（1ms 中断）                              │
│    - 启用指令/数据缓存（如果支持）                            │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 6. SystemClock_Config()                                      │
│    - 配置 HSE（8 MHz 外部晶振）                              │
│    - 配置 PLL（72 MHz 主频）                                 │
│    - 配置总线分频（AHB/APB1/APB2）                           │
│    - 配置 Flash 等待周期                                     │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 7. MX_GPIO_Init()                                            │
│    - 初始化所有 GPIO（LED/按键/传感器引脚）                  │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 8. MX_DMA_Init()                                             │
│    - 初始化 DMA1（用于 ADC）                                 │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 9. MX_RTC_Init()                                             │
│    - 配置 RTC 时钟源（LSE 32.768 kHz）                       │
│    - 设置预分频器                                            │
│    - 使能 RTC 闹钟中断                                       │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 10. MX_ADC1_Init()                                           │
│     - 配置 ADC1（12 位分辨率）                               │
│     - 配置 DMA 传输                                          │
│     - 配置采样时间                                           │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 11. MX_I2C1_Init()                                           │
│     - 配置 I2C1（100 kHz / 400 kHz）                         │
│     - 配置 GPIO 复用功能                                     │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 12. MX_SPI2_Init()                                           │
│     - 配置 SPI2（主机模式）                                  │
│     - 配置时钟极性/相位                                      │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 13. MX_TIM3_Init()                                           │
│     - 配置 TIM3（触发 ADC 采样，1 kHz）                      │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 14. MX_USART1_UART_Init()                                    │
│     - 配置 UART1（115200 bps）                               │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 15. MX_FREERTOS_Init()（创建 RTOS 对象）                     │
│     ↓                                                        │
│     MonitoringTasks_Create()                                 │
│     ├─ 初始化通信接口（I2C/SPI/1-Wire）                      │
│     ├─ 初始化传感器和模块                                    │
│     │  ├─ DS18B20_Init()                                     │
│     │  ├─ MPU6050_Init()                                     │
│     │  ├─ ADC 校准                                           │
│     │  └─ MonitoringNrf24_Init()                             │
│     ├─ 创建互斥量（g_log_mutex）                             │
│     ├─ 创建事件组                                            │
│     │  ├─ g_monitor_cycle_event                              │
│     │  └─ g_monitor_control_event                            │
│     ├─ 创建队列                                              │
│     │  ├─ g_cycle_request_queue（深度 2）                    │
│     │  ├─ g_sample_free_queue（深度 2）                      │
│     │  ├─ g_sample_ready_queue（深度 2）                     │
│     │  └─ g_result_queue（深度 2）                           │
│     ├─ 创建信号量（g_report_done_sem）                       │
│     ├─ 初始化采样块池（2 个块放入 free 队列）                │
│     ├─ 初始化告警系统                                        │
│     └─ 创建 6 个任务                                         │
│        ├─ AcquisitionTask（优先级：高 40）                      │
│        ├─ ProcessingTask（优先级：中高 32）                     │
│        ├─ ReportTask（优先级：中低 16）                         │
│        ├─ CycleTask（优先级：中 24）                            │
│        ├─ HealthTask（优先级：低 8）                           │
│        └─ WatchdogTask（优先级：低 8）                         │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 16. osKernelStart()                                          │
│     - FreeRTOS 内核启动                                      │
│     - 永不返回（如果成功）                                   │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 17. 第一个任务开始运行                                       │
│     - FreeRTOS 选择优先级最高的就绪任务                      │
│     - 初始状态：所有任务都在阻塞等待                          │
│       ├─ AcquisitionTask：等待 cycle_request_queue           │
│       ├─ ProcessingTask：等待 sample_ready_queue             │
│       ├─ ReportTask：等待 result_queue                       │
│       ├─ CycleTask：等待 RTC 闹钟事件（首次手动触发）        │
│       ├─ HealthTask：延时 10 秒                              │
│       └─ WatchdogTask：延时 1 秒                             │
│                                                              │
│     - CycleTask 首次手动触发 RTC 闹钟，启动第一个周期        │
└─────────────────────────────────────────────────────────────┘
```

---

## 任务创建与初始化

### 🔧 任务创建代码（monitoring_tasks.c）

```c
void MonitoringTasks_Create(void) {
  // 1. 初始化硬件
  MonitoringBus_Init();        // I2C/SPI
  MonitoringDrivers_Init();    // 传感器

  // 2. 创建互斥量（保护 UART 日志）
  g_log_mutex = osMutexNew(&log_mutex_attributes);

  // 3. 创建事件组
  g_monitor_cycle_event = osEventFlagsNew(&cycle_event_attributes);
  g_monitor_control_event = osEventFlagsNew(&control_event_attributes);

  // 4. 创建队列（所有队列深度 = 2）
  g_cycle_request_queue = osMessageQueueNew(2, sizeof(monitor_cycle_request_t), ...);
  g_sample_free_queue   = osMessageQueueNew(2, sizeof(monitor_sample_block_t*), ...);
  g_sample_ready_queue  = osMessageQueueNew(2, sizeof(monitor_sample_block_t*), ...);
  g_result_queue        = osMessageQueueNew(2, sizeof(monitor_cycle_result_t), ...);

  // 5. 创建信号量（二值信号量，用于上报完成通知）
  g_report_done_sem = osSemaphoreNew(1, 0, &report_done_sem_attributes);

  // 6. 初始化采样块池（2 个块）
  for (int i = 0; i < MONITOR_SAMPLE_POOL_SIZE; i++) {
    monitor_sample_block_t *block = &g_sample_pool[i];
    osMessageQueuePut(g_sample_free_queue, &block, 0, 0);  // 放入 free 队列
  }

  // 7. 初始化告警系统
  MonitoringAlerts_Init();

  // 8. 创建 6 个任务（动态分配：TCB 与栈来自 FreeRTOS 堆）
  g_acquisition_task = osThreadNew(MonitoringTasks_AcquisitionTask, NULL, &acquisition_task_attr);
  g_processing_task  = osThreadNew(MonitoringTasks_ProcessingTask,  NULL, &processing_task_attr);
  g_report_task      = osThreadNew(MonitoringTasks_ReportTask,      NULL, &report_task_attr);
  g_cycle_task       = osThreadNew(MonitoringTasks_RunCycleTask,    NULL, &cycle_task_attr);
  g_health_task      = osThreadNew(MonitoringTasks_HealthTask,      NULL, &health_task_attr);
  g_watchdog_task    = osThreadNew(MonitoringTasks_WatchdogTask,    NULL, &watchdog_task_attr);
}
```

### 📊 任务优先级（数字越大优先级越高）

```
优先级     任务名              栈大小    说明
────────────────────────────────────────────────
高 (48)    AcquisitionTask    2048      采集任务（时间敏感）
中高 (40)  ProcessingTask     2048      处理任务（FFT 计算）
中 (24)    CycleTask          1024      周期协调
中低 (16)  ReportTask         1024      上报任务
低 (8)     HealthTask         512       健康监测
低 (8)     WatchdogTask       512       看门狗
```

---

## 正常运行周期

### ⏱️ 完整周期时序图（300 秒生产模式）

```
时间轴 →
════════════════════════════════════════════════════════════════════════

0s: Stop 模式（低功耗，CPU 停止，RTC 运行）
    ┌─────────────────────────────────────────────────────────────┐
    │ 功耗：10-50 μA                                              │
    │ CPU：停止                                                   │
    │ RTC：运行（计时到 300 秒）                                  │
    └─────────────────────────────────────────────────────────────┘

300s: RTC 闹钟中断触发
    ↓
    HAL_RTC_AlarmAEventCallback()  // 中断服务程序
    ├─ osEventFlagsSet(g_monitor_cycle_event, MONITOR_EVENT_RTC_ALARM)
    └─ 返回

    系统从 Stop 模式唤醒
    ├─ 恢复系统时钟（HSE + PLL → 72 MHz）
    ├─ 恢复外设时钟
    └─ 继续执行被中断的代码

    CycleTask 被事件唤醒（从 osEventFlagsWait 返回）
    ↓

300s+10ms: CycleTask 生成周期请求
    ┌─────────────────────────────────────────────────────────────┐
    │ CycleTask                                                   │
    ├─────────────────────────────────────────────────────────────┤
    │ 1. 收到 RTC_ALARM 事件                                      │
    │ 2. 生成 cycle_request                                       │
    │    - cycle_id = ++g_health.cycles_started                   │
    │    - timestamp_ticks = HAL_GetTick()                        │
    │ 3. 发送到 cycle_request_queue                               │
    │    osMessageQueuePut(g_cycle_request_queue, &request, ...)  │
    │ 4. 等待上报完成信号量                                       │
    │    osSemaphoreAcquire(g_report_done_sem, 12000ms)           │
    │    [阻塞，等待 ReportTask 完成]                             │
    └─────────────────────────────────────────────────────────────┘
                          ↓
                   cycle_request_queue
                          ↓
300s+11ms: AcquisitionTask 开始采集
    ┌─────────────────────────────────────────────────────────────┐
    │ AcquisitionTask（优先级最高，被唤醒）                       │
    ├─────────────────────────────────────────────────────────────┤
    │ 1. 从 cycle_request_queue 取请求                            │
    │    osMessageQueueGet(g_cycle_request_queue, &request, ...)  │
    │ 2. 从 sample_free_queue 取空闲块                            │
    │    osMessageQueueGet(g_sample_free_queue, &block, 100ms)    │
    │ 3. 初始化采样块                                             │
    │    - block->cycle_id = request.cycle_id                     │
    │    - block->sequence = ++g_sample_sequence                  │
    │    - block->timestamp_ticks = request.timestamp_ticks       │
    │ 4. 开始采集（阻塞约 2.5 秒）                                │
    │    MonitoringAcquisition_Capture(block)                     │
    │    ├─ DS18B20 温度采集（750 ms）                            │
    │    ├─ MPU6050 振动采集（1280 ms，1280 点 @ 1 kHz）         │
    │    └─ ADC 电流采集（1024 ms，1024 点 @ 1 kHz）             │
    │ 5. 将填充后的块放入 sample_ready_queue                      │
    │    osMessageQueuePut(g_sample_ready_queue, &block, ...)     │
    │ 6. 返回阻塞，等待下一个 cycle_request                       │
    └─────────────────────────────────────────────────────────────┘
                          ↓
                   sample_ready_queue
                          ↓
302.5s: ProcessingTask 开始处理
    ┌─────────────────────────────────────────────────────────────┐
    │ ProcessingTask（被唤醒）                                    │
    ├─────────────────────────────────────────────────────────────┤
    │ 1. 从 sample_ready_queue 取采样块                           │
    │    osMessageQueueGet(g_sample_ready_queue, &block, ...)     │
    │ 2. 初始化结果结构                                           │
    │    memset(&result, 0, sizeof(result))                       │
    │ 3. 执行算法处理（约 50-100 ms）                             │
    │    MonitoringAlgorithm_Process(block, &result)              │
    │    ├─ 温度：保存原始值                                      │
    │    ├─ 振动：去直流 → Q15 → 汉宁窗 → FFT → RMS → 频段能量   │
    │    ├─ 电流：去直流 → Q15 → RMS                             │
    │    └─ 告警：评估各通道 → 更新状态机                         │
    │ 4. 将结果放入 result_queue                                  │
    │    osMessageQueuePut(g_result_queue, &result, ...)          │
    │ 5. 归还采样块到 sample_free_queue                           │
    │    osMessageQueuePut(g_sample_free_queue, &block, ...)      │
    │ 6. 返回阻塞，等待下一个 sample_ready                        │
    └─────────────────────────────────────────────────────────────┘
                          ↓
                      result_queue
                          ↓
302.6s: ReportTask 开始上报
    ┌─────────────────────────────────────────────────────────────┐
    │ ReportTask（被唤醒）                                        │
    ├─────────────────────────────────────────────────────────────┤
    │ 1. 从 result_queue 取结果                                   │
    │    osMessageQueueGet(g_result_queue, &result, ...)          │
    │ 2. 通过 UART 输出详细日志（约 100 ms）                      │
    │    MONITOR_LOG("[RTOS] cycle=%lu temp=%.2f ...", ...)       │
    │ 3. 通过 NRF24L01 无线上报（约 5 ms）                        │
    │    if (MonitoringNrf24_IsReady()) {                         │
    │      MonitoringNrf24_SendPayload(payload, len);             │
    │    }                                                        │
    │ 4. 释放上报完成信号量                                       │
    │    osSemaphoreRelease(g_report_done_sem)                    │
    │    [CycleTask 被唤醒]                                       │
    │ 5. 返回阻塞，等待下一个 result                              │
    └─────────────────────────────────────────────────────────────┘
                          ↓
                   g_report_done_sem
                          ↓
302.7s: CycleTask 收到信号量，准备进入 Stop
    ┌─────────────────────────────────────────────────────────────┐
    │ CycleTask（从信号量等待返回）                               │
    ├─────────────────────────────────────────────────────────────┤
    │ 1. 检查 Stop 门禁条件                                       │
    │    - 看门狗许可：g_watchdog_permit == 1                     │
    │    - 状态正常：g_health.state == IDLE                       │
    │    - 队列为空：所有队列深度 == 0                            │
    │ 2. 停止外设                                                 │
    │    - HAL_ADC_Stop_DMA(&hadc1)                               │
    │    - HAL_TIM_Base_Stop(&htim3)                              │
    │    - MPU6050_StopCapture()                                  │
    │ 3. 设置下一次 RTC 闹钟（300 秒后）                          │
    │    RTC_SetNextAlarm(300)                                    │
    │ 4. 等待 RTC 同步                                            │
    │    RTC_WaitForSync()                                        │
    │ 5. 进入 Stop 模式                                           │
    │    HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, ...)     │
    │    [CPU 停止，等待 RTC 闹钟]                                │
    └─────────────────────────────────────────────────────────────┘

302.8s - 602.8s: Stop 模式（低功耗，等待 300 秒）
    ┌─────────────────────────────────────────────────────────────┐
    │ 功耗：10-50 μA                                              │
    │ 所有任务挂起（除了 RTC）                                    │
    │ 等待下一个 RTC 闹钟...                                      │
    └─────────────────────────────────────────────────────────────┘

602.8s: RTC 闹钟再次触发，循环重复...
```

### 📊 时间开销分解

| 阶段 | 时间 | 功耗 | 说明 |
|------|------|------|------|
| 采集 | 2.5 秒 | 30-40 mA | 温度+振动+电流 |
| 处理 | 0.1 秒 | 40 mA | FFT 计算 |
| 上报 | 0.1 秒 | 30 mA | UART + NRF24 |
| 调度 | 0.1 秒 | 20 mA | 任务切换 |
| **运行总计** | **2.8 秒** | **平均 35 mA** | |
| Stop | 297.2 秒 | 0.03 mA | 低功耗等待 |
| **周期总计** | **300 秒** | **平均 0.7 mA** | |

---

## 任务切换机制

### 🔄 FreeRTOS 调度器工作原理

```
调度时机（任务切换发生的时刻）：
1. 系统节拍中断（SysTick，每 1 ms）
2. 任务主动阻塞（等待队列/信号量/延时）
3. 任务主动让出 CPU（osThreadYield）
4. 中断返回时（如果有更高优先级任务就绪）
```

### 📋 任务状态转换图

```
                就绪（Ready）
                     ↑
                     │ osThreadNew
           ┌─────────┴─────────┐
           │                   │
      调度器选中           osDelay
           │              osQueueGet(阻塞)
           ↓              osSemaphoreAcquire(阻塞)
    ┌──────────┐              │
    │ 运行     │              │
    │(Running) │              ↓
    └──────────┘        ┌──────────┐
           │            │ 阻塞     │
           │            │(Blocked) │
      时间片到           └──────────┘
      更高优先级就绪          │
           │                  │ 事件到达
           │                  │ 超时
           └──────────────────┘
```

### 🎯 任务切换示例

```
时刻 T0：AcquisitionTask 正在运行
    ├─ 优先级：48（最高）
    ├─ 状态：Running
    └─ 操作：调用 osMessageQueuePut(g_sample_ready_queue, ...)

时刻 T1：Put 操作完成
    ├─ ProcessingTask 等待的队列有数据了
    ├─ ProcessingTask 从 Blocked → Ready
    ├─ 但优先级 40 < 48，不抢占
    └─ AcquisitionTask 继续运行

时刻 T2：AcquisitionTask 调用 osMessageQueueGet（阻塞等待）
    ├─ AcquisitionTask：Running → Blocked
    ├─ 调度器检查就绪队列
    ├─ ProcessingTask 优先级最高（40）
    ├─ 任务切换：AcquisitionTask → ProcessingTask
    │  ├─ 保存 AcquisitionTask 上下文（寄存器 → 栈）
    │  ├─ 恢复 ProcessingTask 上下文（栈 → 寄存器）
    │  └─ 切换栈指针（SP）
    └─ ProcessingTask 从 osMessageQueueGet 返回，开始运行

时刻 T3：SysTick 中断（1 ms）
    ├─ 进入中断服务程序
    ├─ 调度器检查是否有更高优先级任务就绪
    ├─ 没有，返回继续运行 ProcessingTask
    └─ （如果有，会在中断返回时切换）

时刻 T4：ProcessingTask 调用 osMessageQueuePut（非阻塞）
    ├─ ReportTask 被唤醒（Blocked → Ready）
    ├─ 但优先级 16 < 40，不抢占
    └─ ProcessingTask 继续运行

时刻 T5：ProcessingTask 完成，再次调用 osMessageQueueGet（阻塞）
    ├─ ProcessingTask：Running → Blocked
    ├─ 调度器选择下一个任务
    ├─ ReportTask 优先级最高（16）
    ├─ 任务切换：ProcessingTask → ReportTask
    └─ ReportTask 开始运行
```

### ⚙️ 上下文切换细节

```
任务切换汇编代码（简化）：

PendSV_Handler:
    ; 1. 禁止中断
    CPSID   I
    
    ; 2. 保存当前任务上下文
    MRS     R0, PSP              ; 获取任务栈指针
    STMDB   R0!, {R4-R11}        ; 保存 R4-R11 到栈
    ; （R0-R3, R12, LR, PC, xPSR 由硬件自动保存）
    
    ; 3. 保存栈指针到任务控制块
    LDR     R1, =pxCurrentTCB    ; 当前任务控制块指针
    LDR     R1, [R1]
    STR     R0, [R1]             ; 保存 SP 到 TCB
    
    ; 4. 调用调度器选择下一个任务
    BL      vTaskSwitchContext   ; C 函数，更新 pxCurrentTCB
    
    ; 5. 恢复新任务上下文
    LDR     R1, =pxCurrentTCB    ; 新任务控制块指针
    LDR     R1, [R1]
    LDR     R0, [R1]             ; 加载新任务 SP
    
    LDMIA   R0!, {R4-R11}        ; 恢复 R4-R11
    MSR     PSP, R0              ; 更新任务栈指针
    
    ; 6. 使能中断并返回
    MOV     LR, #0xFFFFFFFD      ; 返回到线程模式，使用 PSP
    CPSIE   I
    BX      LR                   ; 中断返回，硬件自动恢复 R0-R3...
```

---

## 同步机制详解

### 1️⃣ 队列（Queue）

**用途**：任务间传递数据

```
生产者-消费者模式：

┌─────────────┐  osMessageQueuePut  ┌───────┐  osMessageQueueGet  ┌─────────────┐
│ Acquisition │ ─────────────────> │ Queue │ ─────────────────> │ Processing  │
│    Task     │                     │(深度2)│                     │    Task     │
└─────────────┘                     └───────┘                     └─────────────┘

队列满时：
- Put 操作阻塞（等待有空位）
- 或者超时返回失败

队列空时：
- Get 操作阻塞（等待有数据）
- 或者超时返回失败
```

**您的项目使用的队列**：

| 队列 | 深度 | 元素大小 | 用途 |
|------|------|---------|------|
| `cycle_request_queue` | 2 | `monitor_cycle_request_t` | CycleTask → AcquisitionTask |
| `sample_free_queue` | 2 | `monitor_sample_block_t*` | 空闲块池（指针） |
| `sample_ready_queue` | 2 | `monitor_sample_block_t*` | AcquisitionTask → ProcessingTask |
| `result_queue` | 2 | `monitor_cycle_result_t` | ProcessingTask → ReportTask |

**为什么深度都是 2？**
- 2 个采样块（块池大小）
- 一个在采集，一个在处理
- 流水线设计，无需更多缓冲

### 2️⃣ 信号量（Semaphore）

**用途**：任务同步（通知事件完成）

```
二值信号量（0 或 1）：

┌─────────────┐                     ┌─────────────┐
│ ReportTask  │                     │ CycleTask   │
│             │                     │             │
│ 完成上报    │                     │ 等待上报    │
│     │       │                     │     ↓       │
│     ├─ osSemaphoreRelease ───────>│ osSemaphoreAcquire
│     │       │                     │     │       │
│     └─ 继续 │                     │     └─ 收到信号，继续
└─────────────┘                     └─────────────┘

初始值：0（Take 会阻塞）
Release：值变为 1
Acquire：值变为 0，如果已经是 0 则阻塞
```

**您的项目使用的信号量**：

| 信号量 | 类型 | 用途 |
|--------|------|------|
| `g_report_done_sem` | 二值 | ReportTask 通知 CycleTask 上报完成 |

### 3️⃣ 事件组（Event Flags）

**用途**：多个事件的组合通知

```
事件标志位（位掩码）：

┌─────────────────────────────────────┐
│ g_monitor_cycle_event（32 位）      │
├─────────────────────────────────────┤
│ bit 0: MONITOR_EVENT_RTC_ALARM      │  RTC 闹钟中断
│ bit 1-31: 保留                       │
└─────────────────────────────────────┘

设置事件（从中断）：
osEventFlagsSet(g_monitor_cycle_event, MONITOR_EVENT_RTC_ALARM)

等待事件（任务中）：
uint32_t flags = osEventFlagsWait(
  g_monitor_cycle_event,
  MONITOR_EVENT_RTC_ALARM,  // 等待哪个标志
  osFlagsWaitAny,           // 任意一个满足即可
  osWaitForever             // 超时时间
);
```

**您的项目使用的事件组**：

| 事件组 | 标志位 | 用途 |
|--------|--------|------|
| `g_monitor_cycle_event` | `MONITOR_EVENT_RTC_ALARM` | RTC 中断 → CycleTask |
| `g_monitor_control_event` | `MONITOR_EVENT_CAPTURE` | MPU6050 中断 → AcquisitionTask |

### 4️⃣ 互斥量（Mutex）

**用途**：保护共享资源（防止竞争条件）

```
互斥量保护 UART：

任务 A                    任务 B
  │                         │
  ├─ osMutexAcquire ───┐    │
  │  [获得锁]           │    │
  ├─ UART_Transmit     │    ├─ osMutexAcquire
  │  ...               │    │  [阻塞，等待锁]
  ├─ osMutexRelease    │    │
  │  [释放锁] ──────────┼────>│  [获得锁]
  │                    │    ├─ UART_Transmit
  └─ 继续              │    │  ...
                       │    ├─ osMutexRelease
                       │    └─ 继续
```

**您的项目使用的互斥量**：

| 互斥量 | 用途 |
|--------|------|
| `g_log_mutex` | 保护 UART 日志输出（`MONITOR_LOG` 宏） |

---

## 中断与任务交互

### 📡 中断服务程序（ISR）规则

```
中断中可以做的：
✅ osEventFlagsSet（设置事件标志）
✅ osSemaphoreRelease（释放信号量）
✅ osMessageQueuePut（从 ISR 版本）
✅ 设置全局变量（volatile）

中断中不能做的：
❌ 阻塞操作（Acquire/Get 等待）
❌ 延时（osDelay）
❌ 复杂计算（尽量短）
❌ 调用非线程安全函数
```

### 🔔 中断触发流程

#### RTC 闹钟中断

```
硬件：RTC 闹钟到时
  ↓
NVIC 响应中断
  ↓
HAL_RTC_AlarmAEventCallback()  // 您实现的回调
  ├─ osEventFlagsSet(g_monitor_cycle_event, MONITOR_EVENT_RTC_ALARM)
  │  ├─ 设置事件标志位
  │  ├─ 检查是否有任务等待此事件
  │  ├─ CycleTask 从 Blocked → Ready
  │  └─ 标记需要任务切换（如果优先级更高）
  └─ 返回
  ↓
中断返回
  ↓
PendSV 中断触发（如果需要任务切换）
  ↓
任务切换：切换到 CycleTask
  ↓
CycleTask 从 osEventFlagsWait 返回
  ↓
CycleTask 继续执行
```

#### ADC DMA 中断

```
硬件：DMA 完成半/全缓冲区传输
  ↓
NVIC 响应中断
  ↓
HAL_ADC_ConvHalfCpltCallback()  // 前半部分完成
  ├─ g_adc_half_ready = 1U  // 设置标志
  └─ 返回

HAL_ADC_ConvCpltCallback()      // 全部完成
  ├─ g_adc_full_ready = 1U  // 设置标志
  └─ 返回
  ↓
中断返回
  ↓
AcquisitionTask 在轮询 g_adc_half_ready / g_adc_full_ready
  ↓
检测到标志，复制数据
  ↓
清除标志
```

---

## 总结

### 🎯 关键时间点

1. **0 ms**：上电复位
2. **~50 ms**：硬件初始化完成
3. **~100 ms**：FreeRTOS 启动，第一个任务运行
4. **~200 ms**：CycleTask 手动触发第一个周期
5. **~200 ms**：AcquisitionTask 开始采集
6. **~2700 ms**：采集 + 处理 + 上报完成
7. **~2800 ms**：进入 Stop 模式
8. **~300 秒**：RTC 闹钟唤醒，循环重复

### 📊 任务优先级与阻塞关系

```
优先级 ↑

48  AcquisitionTask ━━━[等待 cycle_request]━━━━━━━━━━━
                           ↓ [收到请求]
                    ━━━[采集 2.5s]━━━━━━━━━━━━━━━━━━━━
                           ↓ [Put sample_ready]
                    ━━━[等待 cycle_request]━━━━━━━━━━━

40  ProcessingTask  ━━━[等待 sample_ready]━━━━━━━━━━━
                                  ↓ [收到块]
                           ━━━[处理 0.1s]━━━━━━━━━━━━━
                                  ↓ [Put result]
                           ━━━[等待 sample_ready]━━━━━━

24  CycleTask       ━━━[等待 RTC 事件]━━━━━━━━━━━━━━━━
        [RTC 中断] → ━━━[生成请求]━━━━━━━━━━━━━━━━━━━
                           ↓ [Put cycle_request]
                    ━━━[等待 report_done 信号量]━━━━━━━
                                        ↓ [收到信号]
                    ━━━[进入 Stop]━━━━━━━━━━━━━━━━━━━

16  ReportTask      ━━━[等待 result]━━━━━━━━━━━━━━━━━
                                 ↓ [收到结果]
                          ━━━[上报 0.1s]━━━━━━━━━━━━━━
                                 ↓ [Release report_done]
                          ━━━[等待 result]━━━━━━━━━━━━

8   HealthTask      ━━━[延时 10s]━━━━━━━━━━━━━━━━━━━━
                          ↓ [超时]
                    ━━━[输出健康日志]━━━━━━━━━━━━━━━━
                          ↓
                    ━━━[延时 10s]━━━━━━━━━━━━━━━━━━━━

8   WatchdogTask    ━━━[延时 1s]━━━━━━━━━━━━━━━━━━━━━
                          ↓ [超时]
                    ━━━[检查心跳]━━━━━━━━━━━━━━━━━━━━
                          ↓ [喂狗]
                    ━━━[延时 1s]━━━━━━━━━━━━━━━━━━━━━

优先级 ↓
```

### 🔄 数据流总结

```
RTC 闹钟中断
    ↓
CycleTask（事件组唤醒）
    ↓ [cycle_request_queue]
AcquisitionTask（队列唤醒）
    ↓ [sample_ready_queue]
ProcessingTask（队列唤醒）
    ↓ [result_queue]
ReportTask（队列唤醒）
    ↓ [report_done_sem]
CycleTask（信号量唤醒）
    ↓
进入 Stop 模式
    ↓
等待下一次 RTC 闹钟...
```

---

**完成！这就是您整个系统的完整运行流程。** 🎉
