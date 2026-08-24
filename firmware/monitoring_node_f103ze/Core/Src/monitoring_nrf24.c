#include "monitoring_nrf24.h"
#include "monitoring_config.h"

#define NRF24_CMD_R_REGISTER      0x00U
#define NRF24_CMD_W_REGISTER      0x20U
#define NRF24_CMD_NOP             0xFFU
#define NRF24_CMD_R_RX_PAYLOAD    0x61U
#define NRF24_CMD_W_TX_PAYLOAD    0xA0U
#define NRF24_CMD_FLUSH_TX        0xE1U
#define NRF24_CMD_FLUSH_RX        0xE2U
#define NRF24_REGISTER_MASK       0x1FU

#define NRF24_REG_CONFIG          0x00U
#define NRF24_REG_EN_AA           0x01U
#define NRF24_REG_EN_RXADDR       0x02U
#define NRF24_REG_SETUP_AW        0x03U
#define NRF24_REG_SETUP_RETR      0x04U
#define NRF24_REG_RF_CH           0x05U
#define NRF24_REG_RF_SETUP        0x06U
#define NRF24_REG_STATUS          0x07U
#define NRF24_REG_RX_ADDR_P0      0x0AU
#define NRF24_REG_TX_ADDR         0x10U
#define NRF24_REG_RX_PW_P0        0x11U
#define NRF24_REG_FIFO_STATUS     0x17U
#define NRF24_REG_DYNPD           0x1CU
#define NRF24_REG_FEATURE         0x1DU

#define NRF24_STATUS_RX_DR        0x40U
#define NRF24_STATUS_TX_DS        0x20U
#define NRF24_STATUS_MAX_RT       0x10U
#define NRF24_CONFIG_EN_CRC       0x08U
#define NRF24_CONFIG_CRCO         0x04U
#define NRF24_CONFIG_PWR_UP       0x02U
#define NRF24_CONFIG_PRIM_RX      0x01U

#define NRF24_DEFAULT_ADDRESS     {0xE7U, 0xE7U, 0xE7U, 0xE7U, 0xE7U}

static monitoring_nrf24_bus_t g_nrf24_bus;
static volatile uint8_t g_nrf24_irq_pending;
static uint8_t g_nrf24_online;

void MonitoringNrf24_Bind(const monitoring_nrf24_bus_t *bus)
{
  if (bus == NULL || bus->transfer == NULL || bus->chip_select == NULL)
  {
    g_nrf24_bus.transfer = NULL;
    g_nrf24_bus.chip_select = NULL;
    g_nrf24_bus.chip_enable = NULL;
    g_nrf24_bus.ready = 0U;
    g_nrf24_online = 0U;
    return;
  }

  g_nrf24_bus = *bus;
  g_nrf24_bus.ready = (bus->ready != 0U) ? 1U : 0U;
  g_nrf24_irq_pending = 0U;
  g_nrf24_online = 0U;
  g_nrf24_bus.chip_select(0U);
  if (g_nrf24_bus.chip_enable != NULL)
  {
    g_nrf24_bus.chip_enable(0U);
  }
}

static monitoring_nrf24_status_t MonitoringNrf24_Exchange(
  const uint8_t *tx, uint8_t *rx, uint16_t length);

static monitoring_nrf24_status_t MonitoringNrf24_WriteBlock(
  uint8_t reg, const uint8_t *data, uint8_t length)
{
  uint8_t tx[33] = {0U};
  uint8_t rx[33] = {0U};

  if (data == NULL || length == 0U || length > 32U)
  {
    return MONITOR_NRF24_ARGUMENT;
  }
  tx[0] = (uint8_t)(NRF24_CMD_W_REGISTER | (reg & NRF24_REGISTER_MASK));
  for (uint8_t i = 0U; i < length; i++)
  {
    tx[i + 1U] = data[i];
  }
  return MonitoringNrf24_Exchange(tx, rx, (uint16_t)length + 1U);
}

static monitoring_nrf24_status_t MonitoringNrf24_Command(uint8_t command)
{
  uint8_t tx = command;
  uint8_t rx = 0U;
  return MonitoringNrf24_Exchange(&tx, &rx, 1U);
}

monitoring_nrf24_status_t MonitoringNrf24_ReadStatus(uint8_t *status)
{
  uint8_t tx = NRF24_CMD_NOP;
  uint8_t rx = 0U;
  monitoring_nrf24_status_t result;

  if (status == NULL)
  {
    return MONITOR_NRF24_ARGUMENT;
  }
  result = MonitoringNrf24_Exchange(&tx, &rx, 1U);
  if (result == MONITOR_NRF24_OK)
  {
    *status = rx;
  }
  return result;
}

monitoring_nrf24_status_t MonitoringNrf24_ClearIrq(void)
{
  return MonitoringNrf24_WriteRegister(NRF24_REG_STATUS,
    NRF24_STATUS_RX_DR | NRF24_STATUS_TX_DS | NRF24_STATUS_MAX_RT);
}

monitoring_nrf24_status_t MonitoringNrf24_SetTxAddress(const uint8_t address[5])
{
  monitoring_nrf24_status_t status;

  if (address == NULL)
  {
    return MONITOR_NRF24_ARGUMENT;
  }
  status = MonitoringNrf24_WriteBlock(NRF24_REG_TX_ADDR, address, 5U);
  if (status != MONITOR_NRF24_OK)
  {
    return status;
  }
  /* 开启自动应答时，管道 0 必须与 TX 地址一致。 */
  return MonitoringNrf24_WriteBlock(NRF24_REG_RX_ADDR_P0, address, 5U);
}

monitoring_nrf24_status_t MonitoringNrf24_Init(void)
{
  static const uint8_t default_address[5] = NRF24_DEFAULT_ADDRESS;
  uint8_t status_value = 0U;
  uint8_t config_value = 0U;
  uint8_t channel_value = 0U;
  monitoring_nrf24_status_t status;

  /* 重初始化失败时不能沿用上一次的在线状态。 */
  g_nrf24_online = 0U;
  if (g_nrf24_bus.ready == 0U)
  {
    return MONITOR_NRF24_NOT_READY;
  }
  MonitoringNrf24_SetCe(0U);
  HAL_Delay(5U);
  status = MonitoringNrf24_ReadStatus(&status_value);
  if (status != MONITOR_NRF24_OK)
  {
    return status;
  }
  if (status_value == 0xFFU)
  {
    return MONITOR_NRF24_NOT_PRESENT;
  }

  /* 1 Mbps、0 dBm、5 字节地址、自动应答和 16 位 CRC。 */
  status = MonitoringNrf24_WriteRegister(NRF24_REG_CONFIG,
    NRF24_CONFIG_EN_CRC | NRF24_CONFIG_CRCO);
  if (status == MONITOR_NRF24_OK) status = MonitoringNrf24_WriteRegister(NRF24_REG_EN_AA, 0x01U);
  if (status == MONITOR_NRF24_OK) status = MonitoringNrf24_WriteRegister(NRF24_REG_EN_RXADDR, 0x01U);
  if (status == MONITOR_NRF24_OK) status = MonitoringNrf24_WriteRegister(NRF24_REG_SETUP_AW, 0x03U);
  if (status == MONITOR_NRF24_OK) status = MonitoringNrf24_WriteRegister(NRF24_REG_SETUP_RETR, 0x2FU);
  if (status == MONITOR_NRF24_OK) status = MonitoringNrf24_WriteRegister(NRF24_REG_RF_CH, MONITOR_NRF24_CHANNEL);
  if (status == MONITOR_NRF24_OK) status = MonitoringNrf24_WriteRegister(NRF24_REG_RF_SETUP, 0x06U);
  if (status == MONITOR_NRF24_OK) status = MonitoringNrf24_WriteRegister(NRF24_REG_RX_PW_P0, MONITOR_NRF24_PAYLOAD_SIZE);
  if (status == MONITOR_NRF24_OK) status = MonitoringNrf24_WriteRegister(NRF24_REG_DYNPD, 0x00U);
  if (status == MONITOR_NRF24_OK) status = MonitoringNrf24_WriteRegister(NRF24_REG_FEATURE, 0x00U);
  if (status == MONITOR_NRF24_OK) status = MonitoringNrf24_SetTxAddress(default_address);
  if (status == MONITOR_NRF24_OK) status = MonitoringNrf24_ClearIrq();
  if (status == MONITOR_NRF24_OK) status = MonitoringNrf24_Command(NRF24_CMD_FLUSH_TX);
  if (status == MONITOR_NRF24_OK) status = MonitoringNrf24_Command(NRF24_CMD_FLUSH_RX);
  if (status != MONITOR_NRF24_OK)
  {
    return status;
  }

  status = MonitoringNrf24_WriteRegister(NRF24_REG_CONFIG,
    NRF24_CONFIG_EN_CRC | NRF24_CONFIG_CRCO | NRF24_CONFIG_PWR_UP);
  if (status != MONITOR_NRF24_OK)
  {
    return status;
  }
  HAL_Delay(2U);
  status = MonitoringNrf24_ReadRegister(NRF24_REG_CONFIG, &config_value);
  if (status != MONITOR_NRF24_OK)
  {
    return status;
  }
  status = MonitoringNrf24_ReadRegister(NRF24_REG_RF_CH, &channel_value);
  if (status != MONITOR_NRF24_OK)
  {
    return status;
  }
  if (config_value != (NRF24_CONFIG_EN_CRC | NRF24_CONFIG_CRCO |
                       NRF24_CONFIG_PWR_UP) ||
      channel_value != MONITOR_NRF24_CHANNEL)
  {
    return MONITOR_NRF24_NOT_PRESENT;
  }
  g_nrf24_irq_pending = 0U;
  g_nrf24_online = 1U;
  return MONITOR_NRF24_OK;
}

uint8_t MonitoringNrf24_IsReady(void)
{
  return g_nrf24_online;
}

monitoring_nrf24_status_t MonitoringNrf24_SendPayload(const uint8_t *data,
                                                      uint8_t length)
{
  uint32_t start_tick;
  uint8_t status_value = 0U;
  monitoring_nrf24_status_t status;

  if (data == NULL || length == 0U || length > MONITOR_NRF24_PAYLOAD_SIZE)
  {
    return MONITOR_NRF24_ARGUMENT;
  }
  if (g_nrf24_online == 0U)
  {
    return MONITOR_NRF24_NOT_READY;
  }
  MonitoringNrf24_SetCe(0U);
  (void)MonitoringNrf24_ClearIrq();
  (void)MonitoringNrf24_Command(NRF24_CMD_FLUSH_TX);
  status = MonitoringNrf24_WritePayload(data, length);
  if (status != MONITOR_NRF24_OK)
  {
    return status;
  }

  g_nrf24_irq_pending = 0U;
  MonitoringNrf24_SetCe(1U);
  start_tick = HAL_GetTick();
  while ((HAL_GetTick() - start_tick) < MONITOR_NRF24_SEND_TIMEOUT_MS)
  {
    status = MonitoringNrf24_ReadStatus(&status_value);
    if (status != MONITOR_NRF24_OK)
    {
      MonitoringNrf24_SetCe(0U);
      return status;
    }
    if ((status_value & NRF24_STATUS_TX_DS) != 0U)
    {
      MonitoringNrf24_SetCe(0U);
      (void)MonitoringNrf24_ClearIrq();
      return MONITOR_NRF24_OK;
    }
    if ((status_value & NRF24_STATUS_MAX_RT) != 0U)
    {
      MonitoringNrf24_SetCe(0U);
      (void)MonitoringNrf24_ClearIrq();
      (void)MonitoringNrf24_Command(NRF24_CMD_FLUSH_TX);
      return MONITOR_NRF24_MAX_RETRY;
    }
    HAL_Delay(1U);
  }
  MonitoringNrf24_SetCe(0U);
  return MONITOR_NRF24_TIMEOUT;
}

monitoring_nrf24_status_t MonitoringNrf24_StartListening(void)
{
  monitoring_nrf24_status_t status;

  if (g_nrf24_online == 0U)
  {
    return MONITOR_NRF24_NOT_READY;
  }
  (void)MonitoringNrf24_ClearIrq();
  status = MonitoringNrf24_WriteRegister(NRF24_REG_CONFIG,
    NRF24_CONFIG_EN_CRC | NRF24_CONFIG_CRCO |
    NRF24_CONFIG_PWR_UP | NRF24_CONFIG_PRIM_RX);
  if (status == MONITOR_NRF24_OK)
  {
    HAL_Delay(2U);
    MonitoringNrf24_SetCe(1U);
  }
  return status;
}

monitoring_nrf24_status_t MonitoringNrf24_StopListening(void)
{
  if (g_nrf24_online == 0U)
  {
    return MONITOR_NRF24_NOT_READY;
  }
  MonitoringNrf24_SetCe(0U);
  return MonitoringNrf24_WriteRegister(NRF24_REG_CONFIG,
    NRF24_CONFIG_EN_CRC | NRF24_CONFIG_CRCO | NRF24_CONFIG_PWR_UP);
}

uint8_t MonitoringNrf24_Available(void)
{
  uint8_t status = 0U;
  uint8_t fifo_status = 0U;

  if (g_nrf24_online == 0U ||
      MonitoringNrf24_ReadStatus(&status) != MONITOR_NRF24_OK)
  {
    return 0U;
  }
  if ((status & NRF24_STATUS_RX_DR) != 0U)
  {
    return 1U;
  }
  if (MonitoringNrf24_ReadRegister(NRF24_REG_FIFO_STATUS,
                                   &fifo_status) != MONITOR_NRF24_OK)
  {
    return 0U;
  }
  return (fifo_status & 0x01U) == 0U ? 1U : 0U;
}

void MonitoringNrf24_Sleep(void)
{
  if (g_nrf24_online != 0U)
  {
    MonitoringNrf24_SetCe(0U);
    (void)MonitoringNrf24_WriteRegister(NRF24_REG_CONFIG,
      NRF24_CONFIG_EN_CRC | NRF24_CONFIG_CRCO);
    g_nrf24_online = 0U;
  }
}

void MonitoringNrf24_SetCe(uint8_t active)
{
  if (g_nrf24_bus.ready != 0U && g_nrf24_bus.chip_enable != NULL)
  {
    g_nrf24_bus.chip_enable(active != 0U ? 1U : 0U);
  }
}

void MonitoringNrf24_NotifyIrqFromISR(void)
{
  g_nrf24_irq_pending = 1U;
}

uint8_t MonitoringNrf24_TakeIrq(void)
{
  uint8_t pending = g_nrf24_irq_pending;
  g_nrf24_irq_pending = 0U;
  return pending;
}

static monitoring_nrf24_status_t MonitoringNrf24_Exchange(
  const uint8_t *tx, uint8_t *rx, uint16_t length)
{
  if (tx == NULL || rx == NULL || length == 0U)
  {
    return MONITOR_NRF24_ARGUMENT;
  }
  if (g_nrf24_bus.ready == 0U || g_nrf24_bus.transfer == NULL ||
      g_nrf24_bus.chip_select == NULL)
  {
    return MONITOR_NRF24_NOT_READY;
  }

  g_nrf24_bus.chip_select(1U);
  if (g_nrf24_bus.transfer(tx, rx, length, 20U) != HAL_OK)
  {
    g_nrf24_bus.chip_select(0U);
    /* SPI 失败后寄存器状态不再可信，等待下一次初始化重新探测。 */
    g_nrf24_online = 0U;
    return MONITOR_NRF24_BUS_ERROR;
  }
  g_nrf24_bus.chip_select(0U);
  return MONITOR_NRF24_OK;
}

monitoring_nrf24_status_t MonitoringNrf24_ReadRegister(uint8_t reg,
                                                        uint8_t *value)
{
  uint8_t tx[2] = {(uint8_t)(NRF24_CMD_R_REGISTER | (reg & NRF24_REGISTER_MASK)), 0U};
  uint8_t rx[2] = {0U, 0U};
  monitoring_nrf24_status_t status;

  if (value == NULL)
  {
    return MONITOR_NRF24_ARGUMENT;
  }
  status = MonitoringNrf24_Exchange(tx, rx, sizeof(tx));
  if (status == MONITOR_NRF24_OK)
  {
    *value = rx[1];
  }
  return status;
}

monitoring_nrf24_status_t MonitoringNrf24_WriteRegister(uint8_t reg,
                                                         uint8_t value)
{
  uint8_t tx[2] = {(uint8_t)(NRF24_CMD_W_REGISTER | (reg & NRF24_REGISTER_MASK)), value};
  uint8_t rx[2] = {0U, 0U};
  return MonitoringNrf24_Exchange(tx, rx, sizeof(tx));
}

monitoring_nrf24_status_t MonitoringNrf24_ReadPayload(uint8_t *data,
                                                       uint8_t length)
{
  uint8_t tx[33] = {0U};
  uint8_t rx[33] = {0U};
  monitoring_nrf24_status_t status;

  if (data == NULL || length == 0U || length > 32U)
  {
    return MONITOR_NRF24_ARGUMENT;
  }
  tx[0] = NRF24_CMD_R_RX_PAYLOAD;
  status = MonitoringNrf24_Exchange(tx, rx, (uint16_t)length + 1U);
  if (status == MONITOR_NRF24_OK)
  {
    for (uint8_t i = 0U; i < length; i++)
    {
      data[i] = rx[i + 1U];
    }
    (void)MonitoringNrf24_WriteRegister(NRF24_REG_STATUS, NRF24_STATUS_RX_DR);
  }
  return status;
}

monitoring_nrf24_status_t MonitoringNrf24_WritePayload(const uint8_t *data,
                                                        uint8_t length)
{
  uint8_t tx[33] = {0U};
  uint8_t rx[33] = {0U};

  if (data == NULL || length == 0U || length > 32U)
  {
    return MONITOR_NRF24_ARGUMENT;
  }
  tx[0] = NRF24_CMD_W_TX_PAYLOAD;
  for (uint8_t i = 0U; i < length; i++)
  {
    tx[i + 1U] = data[i];
  }
  return MonitoringNrf24_Exchange(tx, rx, (uint16_t)length + 1U);
}
