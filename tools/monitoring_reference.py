"""监测节点的主机侧最小参考算法。

该文件只用于验证公式和告警状态机，不模拟 STM32 外设、DMA、FIFO 或 Stop。
实现使用 Python 标准库，便于在没有传感器时先固定算法行为。
"""

from __future__ import annotations

import cmath
import math
from dataclasses import dataclass
from enum import Enum
from typing import Iterable, List


def _values(values: Iterable[float]) -> List[float]:
    result = [float(value) for value in values]
    if not result:
        raise ValueError("输入序列不能为空")
    return result


def rms(values: Iterable[float]) -> float:
    data = _values(values)
    return math.sqrt(sum(value * value for value in data) / len(data))


def peak_to_peak(values: Iterable[float]) -> float:
    data = _values(values)
    return max(data) - min(data)


def crest_factor(values: Iterable[float]) -> float:
    data = _values(values)
    value_rms = rms(data)
    return max(abs(value) for value in data) / value_rms if value_rms else 0.0


def band_energy(values: Iterable[float], sample_rate_hz: int,
                low_hz: float, high_hz: float) -> float:
    """计算未归一化 DFT 频带能量，频带端点包含在内。"""
    data = _values(values)
    if sample_rate_hz <= 0 or low_hz < 0 or high_hz < low_hz:
        raise ValueError("采样率或频带参数无效")

    count = len(data)
    low_bin = max(0, math.ceil(low_hz * count / sample_rate_hz))
    high_bin = min(count // 2, math.floor(high_hz * count / sample_rate_hz))
    energy = 0.0
    for index in range(low_bin, high_bin + 1):
        component = sum(
            value * cmath.exp(-2j * math.pi * index * sample / count)
            for sample, value in enumerate(data)
        )
        energy += component.real * component.real + component.imag * component.imag
    return energy


class AlertState(str, Enum):
    NORMAL = "NORMAL"
    PENDING = "PENDING"
    ACTIVE = "ACTIVE"
    RECOVERING = "RECOVERING"


@dataclass
class AlertTracker:
    confirm_cycles: int = 3
    recover_cycles: int = 3
    state: AlertState = AlertState.NORMAL
    count: int = 0
    recovery_count: int = 0

    def update(self, valid: bool, over_limit: bool, recovered: bool) -> AlertState:
        if not valid:
            return self.state

        if over_limit:
            self.count = min(self.count + 1, self.confirm_cycles)
            self.recovery_count = 0
            self.state = (AlertState.ACTIVE if self.count >= self.confirm_cycles
                          else AlertState.PENDING)
            return self.state

        if self.state in (AlertState.ACTIVE, AlertState.RECOVERING) and recovered:
            self.recovery_count = min(self.recovery_count + 1, self.recover_cycles)
            self.count = self.recovery_count
            self.state = (AlertState.NORMAL if self.recovery_count >= self.recover_cycles
                          else AlertState.RECOVERING)
            if self.state == AlertState.NORMAL:
                self.recovery_count = 0
            return self.state

        if self.state in (AlertState.ACTIVE, AlertState.RECOVERING):
            # 进入迟滞区但没有达到恢复阈值时，保持当前告警状态。
            return self.state

        self.count = 0
        self.recovery_count = 0
        self.state = AlertState.NORMAL
        return self.state
