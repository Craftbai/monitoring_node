#ifndef MONITORING_ACQUISITION_H
#define MONITORING_ACQUISITION_H

#include "monitoring_tasks.h"

void MonitoringAcquisition_Init(void);
uint32_t MonitoringAcquisition_Capture(monitor_sample_block_t *block);
uint8_t MonitoringAcquisition_Stop(void);
uint8_t MonitoringAcquisition_Resume(void);

#endif /* MONITORING_ACQUISITION_H */
