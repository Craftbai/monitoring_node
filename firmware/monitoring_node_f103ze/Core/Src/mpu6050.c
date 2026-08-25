#include "mpu6050.h"
#include "monitoring_bus.h"
#include "i2c.h"
#include "gpio.h"

#define MPU6050_REG_SMPLRT_DIV       0x19U
#define MPU6050_REG_CONFIG           0x1AU
#define MPU6050_REG_ACCEL_CONFIG     0x1CU
#define MPU6050_REG_FIFO_EN          0x23U
#define MPU6050_REG_INT_ENABLE       0x38U
#define MPU6050_REG_INT_STATUS       0x3AU
#define MPU6050_REG_ACCEL_XOUT_H     0x3BU
#define MPU6050_REG_USER_CTRL        0x6AU
#define MPU6050_REG_PWR_MGMT_1       0x6BU
#define MPU6050_REG_FIFO_COUNTH      0x72U
#define MPU6050_REG_FIFO_R_W         0x74U
#define MPU6050_REG_WHO_AM_I         0x75U

#define MPU6050_USER_RESET           0x04U
#define MPU6050_USER_FIFO_RESET      0x04U
#define MPU6050_USER_FIFO_ENABLE     0x40U
#define MPU6050_FIFO_ACCEL_XYZ       0x08U

static volatile uint8_t g_data_ready_pending;

static void MPU6050_RecoverBus(void)
{
  GPIO_InitTypeDef gpio = {0};

  /* 先释放 I2C 外设，再用 GPIO 给被从机拉住的 SCL 发送恢复脉冲。 */
  (void)HAL_I2C_DeInit(&hi2c1);
  __HAL_RCC_GPIOB_CLK_ENABLE();
  gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9;
  gpio.Mode = GPIO_MODE_OUTPUT_OD;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOB, &gpio);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, GPIO_PIN_SET);
  for (uint8_t i = 0U; i < 9U; i++)
  {
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_RESET);
    HAL_Delay(1U);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_SET);
    HAL_Delay(1U);
  }
  /* SCL、SDA 同时释放，形成一个 STOP 条件。 */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8 | GPIO_PIN_9, GPIO_PIN_SET);
  MX_I2C1_Init();
}

static mpu6050_status_t MPU6050_Write(uint8_t reg, uint8_t value)
{
  if (MonitoringI2c_MemWrite(MPU6050_I2C_ADDRESS, reg, &value, 1U, 20U) != HAL_OK)
  {
    MPU6050_RecoverBus();
    return MPU6050_ERROR_BUS;
  }
  return MPU6050_OK;
}

static mpu6050_status_t MPU6050_Read(uint8_t reg, uint8_t *data, uint16_t length)
{
  if (data == NULL || length == 0U)
  {
    return MPU6050_ERROR_ARGUMENT;
  }

  if (MonitoringI2c_MemRead(MPU6050_I2C_ADDRESS, reg, data, length, 50U) != HAL_OK)
  {
    MPU6050_RecoverBus();
    return MPU6050_ERROR_BUS;
  }
  return MPU6050_OK;
}

mpu6050_status_t MPU6050_Init(void)
{
  uint8_t who_am_i = 0U;

  if (MPU6050_Read(MPU6050_REG_WHO_AM_I, &who_am_i, 1U) != MPU6050_OK)
  {
    return MPU6050_ERROR_BUS;
  }
  if (who_am_i != MPU6050_WHO_AM_I_VALUE)
  {
    return MPU6050_ERROR_ID;
  }

  if (MPU6050_Write(MPU6050_REG_PWR_MGMT_1, 0x01U) != MPU6050_OK ||
      MPU6050_Write(MPU6050_REG_CONFIG, 0x00U) != MPU6050_OK ||
      /* CONFIG=0 打开数字低通后，内部采样基准为 1 kHz；分频值 0
       * 才能保持 1 kHz 原始输出，采集层再按 4/5 形成有效 800 Hz。
       * 旧值 9 会把实际输出降为 100 Hz。 */
      MPU6050_Write(MPU6050_REG_SMPLRT_DIV, 0U) != MPU6050_OK ||
      MPU6050_Write(MPU6050_REG_ACCEL_CONFIG, 0x00U) != MPU6050_OK ||
      MPU6050_Write(MPU6050_REG_INT_ENABLE, 0x11U) != MPU6050_OK)
  {
    return MPU6050_ERROR_BUS;
  }

  /* 初始化只配置器件，FIFO 在每个采集周期开始时单独启动。 */
  return MPU6050_OK;
}

mpu6050_status_t MPU6050_StartCapture(void)
{
  if (MPU6050_Write(MPU6050_REG_USER_CTRL, MPU6050_USER_RESET) != MPU6050_OK ||
      MPU6050_Write(MPU6050_REG_FIFO_EN, MPU6050_FIFO_ACCEL_XYZ) != MPU6050_OK ||
      MPU6050_Write(MPU6050_REG_USER_CTRL, MPU6050_USER_FIFO_ENABLE) != MPU6050_OK)
  {
    return MPU6050_ERROR_BUS;
  }

  g_data_ready_pending = 0U;
  return MPU6050_OK;
}

mpu6050_status_t MPU6050_StopCapture(void)
{
  if (MPU6050_Write(MPU6050_REG_FIFO_EN, 0x00U) != MPU6050_OK ||
      MPU6050_Write(MPU6050_REG_USER_CTRL, MPU6050_USER_FIFO_RESET) != MPU6050_OK)
  {
    return MPU6050_ERROR_BUS;
  }
  return MPU6050_OK;
}

mpu6050_status_t MPU6050_ReadFifoCount(uint16_t *count)
{
  uint8_t data[2];
  uint8_t int_status;

  if (count == NULL)
  {
    return MPU6050_ERROR_ARGUMENT;
  }
  if (MPU6050_Read(MPU6050_REG_INT_STATUS, &int_status, 1U) != MPU6050_OK ||
      MPU6050_Read(MPU6050_REG_FIFO_COUNTH, data, sizeof(data)) != MPU6050_OK)
  {
    return MPU6050_ERROR_BUS;
  }

  if ((int_status & 0x10U) != 0U)
  {
    (void)MPU6050_Write(MPU6050_REG_USER_CTRL, MPU6050_USER_FIFO_RESET);
    return MPU6050_ERROR_FIFO;
  }

  *count = ((uint16_t)data[0] << 8) | data[1];
  if (*count > 1024U)
  {
    (void)MPU6050_Write(MPU6050_REG_USER_CTRL, MPU6050_USER_FIFO_RESET);
    return MPU6050_ERROR_FIFO;
  }
  return MPU6050_OK;
}

mpu6050_status_t MPU6050_ReadSample(mpu6050_sample_t *sample)
{
  uint8_t data[6];

  if (sample == NULL)
  {
    return MPU6050_ERROR_ARGUMENT;
  }
  if (MPU6050_Read(MPU6050_REG_FIFO_R_W, data, sizeof(data)) != MPU6050_OK)
  {
    return MPU6050_ERROR_BUS;
  }

  sample->x = (int16_t)(((uint16_t)data[0] << 8) | data[1]);
  sample->y = (int16_t)(((uint16_t)data[2] << 8) | data[3]);
  sample->z = (int16_t)(((uint16_t)data[4] << 8) | data[5]);
  g_data_ready_pending = 0U;
  return MPU6050_OK;
}

void MPU6050_NotifyDataReadyFromISR(void)
{
  g_data_ready_pending = 1U;
}

uint8_t MPU6050_DataReadyPending(void)
{
  return g_data_ready_pending;
}
