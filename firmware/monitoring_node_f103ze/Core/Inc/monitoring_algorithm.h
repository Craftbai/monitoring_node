#ifndef MONITORING_ALGORITHM_H
#define MONITORING_ALGORITHM_H

#include "monitoring_tasks.h"

void MonitoringAlgorithm_Process(const monitor_sample_block_t *block,
                                 monitor_cycle_result_t *result);

#endif /* MONITORING_ALGORITHM_H */
