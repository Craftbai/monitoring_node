#ifndef MONITORING_MPU6050_H
#define MONITORING_MPU6050_H

#include "stm32f1xx_hal.h"
#include <stdint.h>

typedef enum
{
  MPU6050_OK = 0,
  MPU6050_ERROR_ARGUMENT,
  MPU6050_ERROR_BUS,
  MPU6050_ERROR_ID,
  MPU6050_ERROR_FIFO,
  MPU6050_ERROR_TIMEOUT
} mpu6050_status_t;

typedef struct
{
  int16_t x;
  int16_t y;
  int16_t z;
} mpu6050_sample_t;

#define MPU6050_I2C_ADDRESS       (0x68U << 1)
#define MPU6050_WHO_AM_I_VALUE    0x68U
/* DLPF 打开时器件输出为 1 kHz；采集层按 4/5 保留样本，形成有效 800 Hz。 */
#define MPU6050_SENSOR_RATE_HZ    1000U
#define MPU6050_SAMPLE_RATE_HZ    800U

mpu6050_status_t MPU6050_Init(void);
mpu6050_status_t MPU6050_StartCapture(void);
mpu6050_status_t MPU6050_StopCapture(void);
mpu6050_status_t MPU6050_ReadFifoCount(uint16_t *count);
mpu6050_status_t MPU6050_ReadSample(mpu6050_sample_t *sample);
void MPU6050_NotifyDataReadyFromISR(void);
uint8_t MPU6050_DataReadyPending(void);

#endif /* MONITORING_MPU6050_H */
