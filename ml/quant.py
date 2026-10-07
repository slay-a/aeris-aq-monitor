"""Integer arithmetic that mirrors core/src/model.c exactly.

Every operation here is a Python int operation chosen to match the C kernel
bit for bit: the same int32 accumulators, the same 16-bit multiplier shift, the
same round-half-away-from-zero right shift, the same int8 saturation. That is
what makes the accuracy measured in training the accuracy the device delivers,
and it is what tests/test_model.c re-verifies against emitted vectors.
"""
from __future__ import annotations

import numpy as np

INT8_MIN, INT8_MAX = -128, 127


def sat8(v: int) -> int:
    return INT8_MIN if v < INT8_MIN else (INT8_MAX if v > INT8_MAX else v)


def rshift_round(x: int, shift: int) -> int:
    """Rounding right shift, symmetric about zero.

    Mirrors rshift_round() in model.c. A plain arithmetic shift biases every
    negative activation downward, which over three layers is enough to move the
    argmax on borderline samples -- so both sides must round the same way.
    """
    if shift <= 0:
        return x
    half = 1 << (shift - 1)
    if x >= 0:
        return (x + half) >> shift
    return -(((-x) + half) >> shift)


def choose_multiplier(scale: float, target_bits: int = 20) -> tuple[int, int]:
    """Express a real scale factor as (mult, shift) for the C requantizer.

    The C side computes ((acc * mult) >> 16) >> shift, so the represented value
    is mult / 2**(16 + shift). shift is chosen to place mult in
    [2**target_bits, 2**(target_bits+1)), which keeps about six decimal digits
    of the scale while leaving acc * mult comfortably inside int64 and the
    post-shift value inside int32.
    """
    if scale <= 0:
        raise ValueError("scale must be positive")
    shift = 0
    while True:
        mult = int(round(scale * (1 << (16 + shift))))
        if mult >= (1 << target_bits):
            break
        shift += 1
        if shift > 40:
            raise ValueError(f"cannot represent scale {scale}")
    # Trim back if we overshot into an unnecessarily large multiplier.
    while shift > 0 and mult >= (1 << (target_bits + 1)):
        shift -= 1
        mult = int(round(scale * (1 << (16 + shift))))
    return mult, shift


def quantize_weights(w: np.ndarray) -> tuple[np.ndarray, float]:
    """Symmetric per-tensor int8 weight quantization."""
    amax = float(np.max(np.abs(w)))
    if amax == 0.0:
        return np.zeros_like(w, dtype=np.int8), 1.0
    scale = amax / 127.0
    q = np.clip(np.rint(w / scale), INT8_MIN, INT8_MAX).astype(np.int8)
    return q, scale


def quantize_input(x_std: np.ndarray, input_scale: float) -> np.ndarray:
    """Mirror aeris_model_quantize_input(), including its clamping order.

    The float-domain clamp before the integer conversion is deliberate and
    matches the C: a feature far outside the training range scales past
    INT32_MAX, where the cast is undefined and in practice wraps to the opposite
    end of the int8 range.
    """
    scaled = np.where(np.isfinite(x_std), x_std, 0.0) / input_scale
    scaled = np.clip(scaled, -128.0, 127.0)
    # np.rint is round-half-to-even; lrintf() in the C uses the current FP
    # rounding mode, which is also round-half-to-even by default. Matching
    # here matters only for exact .5 cases, but those do occur.
    return np.rint(scaled).astype(np.int32).clip(INT8_MIN, INT8_MAX).astype(np.int8)


def dense_int(q_in: np.ndarray, w_q: np.ndarray, b_q: np.ndarray) -> np.ndarray:
    """int8 x int8 -> int32 accumulate, as the C inner loop does.

    Computed in int64 then checked against the int32 range so an overflow is a
    loud failure here rather than silent wraparound on the device.
    """
    acc = q_in.astype(np.int64) @ w_q.astype(np.int64).T + b_q.astype(np.int64)
    if np.any(acc > np.iinfo(np.int32).max) or np.any(acc < np.iinfo(np.int32).min):
        raise OverflowError("accumulator exceeds int32 on the device")
    return acc.astype(np.int64)


def requantize(acc: np.ndarray, mult: int, shift: int, relu: bool) -> np.ndarray:
    """Vectorised equivalent of the requantize step in dense()."""
    scaled = acc * mult
    stage = scaled >> 16                      # arithmetic shift, as in C
    half = (1 << (shift - 1)) if shift > 0 else 0
    if shift > 0:
        pos = (stage + half) >> shift
        neg = -(((-stage) + half) >> shift)
        out = np.where(stage >= 0, pos, neg)
    else:
        out = stage
    if relu:
        out = np.maximum(out, 0)
    return np.clip(out, INT8_MIN, INT8_MAX).astype(np.int8)
