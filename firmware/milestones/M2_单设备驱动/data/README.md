# M2 阶段数据目录

本目录用于保存 M2 阶段三个传感器的验证数据。

## 数据文件规范

### MPU6050 振动传感器
- `mpu6050_static.csv` - 静止场景（开发板平放，10 秒）
- `mpu6050_tap.csv` - 轻敲场景（5 次轻敲）
- `mpu6050_fan.csv` - 风扇运行场景（1 秒连续采样）

格式：
```csv
timestamp_ms,accel_x_raw,accel_y_raw,accel_z_raw,accel_x_g,accel_y_g,accel_z_g,scenario
0,245,-128,16384,0.015,-0.008,1.000,static
```

### DS18B20 温度传感器
- `ds18b20_room_temp.csv` - 室温测量（与温度计对照）
- `ds18b20_stability.csv` - 稳定性测试（连续 100 次采样）

格式：
```csv
timestamp_ms,temp_raw,temp_celsius,reference_celsius,scenario
0,425,26.56,26.5,room_temp
```

### ACS712 电流传感器
- `acs712_zero.csv` - 零点标定（无负载或短接）
- `acs712_load1.csv` - 负载 1 标定（小负载，约 0.5A）
- `acs712_load2.csv` - 负载 2 标定（中等负载，约 2A）

格式：
```csv
timestamp_ms,adc_raw,pa0_voltage,vout,current_calc,current_ref,load_type
0,2048,1.65,2.47,-0.03,0.00,zero
```

## 测量条件记录

每个数据文件应附带以下信息（可在文件头注释或单独 README 中说明）：

### MPU6050
- I2C 时钟频率（100 kHz / 400 kHz）
- 加速度量程（±2g / ±4g / ±8g / ±16g）
- 采样率分频值（SMPLRT_DIV）
- 低通滤波器配置（CONFIG 寄存器）
- 测量日期和环境条件

### DS18B20
- 分辨率配置（9/10/11/12 位）
- 转换时间（ms）
- 参考温度计型号和精度
- 测量日期和环境温度范围

### ACS712
- ADC 采样时间（cycles）
- ADC 时钟频率（MHz）
- 分压比（VOUT 到 PA0）
- 零点电压（V）
- 灵敏度（V/A）
- 参考万用表型号和精度
- 负载类型和规格
- 测量日期和供电电压

## 数据完整性检查

在标记 M2 阶段完成前，确认：
- [ ] 所有 CSV 文件都存在且非空
- [ ] 每个文件至少包含 10 行有效数据
- [ ] 时间戳列单调递增
- [ ] 原始数据和换算数据都已记录
- [ ] 参考测量值（如有）已填写
- [ ] 场景标签清晰可识别

## 数据用途

这些数据将在后续阶段用于：
- **M3 PC 算法参考**：作为真实传感器数据的输入，验证 Python 算法
- **M4 采集与 DSP**：对照 MCU 和 PC 的算法输出，验证 CMSIS-DSP 移植正确性
- **M7 整机验收**：作为基线数据，对比整机集成后的性能

保留原始数据文件，不要覆盖或删除。
