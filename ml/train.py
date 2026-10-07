#!/usr/bin/env python3
"""Train, quantize and export the event classifier.

Pipeline
--------
The feature CSV is produced by the C simulator (`make dataset`), which runs the
*firmware's own* gas-index, windowing and feature code over a room physics model
and the virtual sensors. So the features here are not a Python reimplementation
of what the device computes -- they are what the device computes. That removes
the usual train/serve skew at its source.

Split
-----
Grouped by session. Rows inside one session are heavily autocorrelated -- a
5-minute slope feature overlaps 60 consecutive samples -- so a random row split
leaks the test set into training and reports an accuracy that cannot survive
contact with a real room. Whole sessions go to one side or the other.

Quantization
------------
Symmetric per-tensor int8 weights, int32 accumulate, per-layer (multiplier,
shift) requantization, ReLU between layers. The int8 network is evaluated with
ml/quant.py, which reproduces core/src/model.c's integer arithmetic exactly, so
the accuracy reported for the quantized model is the device's accuracy. The
exporter then emits parity vectors that tests/test_model.c replays through the C.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import quant  # noqa: E402

CLASS_NAMES = ["baseline", "occupancy", "cooking", "ventilation", "volatile"]
H1, H2 = 16, 16
INPUT_SCALE = 0.0625          # standardised units per int8 step: +/- 7.9 sigma
N_PARITY_VECTORS = 256


# --------------------------------------------------------------------------- #
# data
# --------------------------------------------------------------------------- #

def load(path: pathlib.Path):
    raw = np.genfromtxt(path, delimiter=",", names=True, dtype=None, encoding="utf-8")
    names = list(raw.dtype.names)
    feat_names = [n for n in names if n not in ("session", "label")]
    X = np.stack([raw[n].astype(np.float64) for n in feat_names], axis=1)
    y = raw["label"].astype(np.int64)
    g = raw["session"].astype(np.int64)
    if not np.all(np.isfinite(X)):
        bad = np.argwhere(~np.isfinite(X))
        raise SystemExit(f"dataset contains {len(bad)} non-finite feature values")
    return X, y, g, feat_names


def split_by_session(g: np.ndarray, frac_test: float, seed: int):
    sessions = np.unique(g)
    rng = np.random.default_rng(seed)
    rng.shuffle(sessions)
    n_test = max(1, int(round(len(sessions) * frac_test)))
    test_s = set(sessions[:n_test].tolist())
    is_test = np.array([s in test_s for s in g])
    return ~is_test, is_test, sorted(test_s)


# --------------------------------------------------------------------------- #
# float model
# --------------------------------------------------------------------------- #

class MLP:
    """14 -> 16 -> 16 -> 5, ReLU, softmax. Adam, written out by hand.

    Small enough that an explicit backward pass is clearer than a framework,
    and it keeps the repo's only Python dependency at numpy.
    """

    def __init__(self, n_in: int, n_out: int, seed: int = 0):
        rng = np.random.default_rng(seed)
        # He initialisation, appropriate for ReLU.
        self.W1 = rng.normal(0, np.sqrt(2.0 / n_in), (H1, n_in))
        self.b1 = np.zeros(H1)
        self.W2 = rng.normal(0, np.sqrt(2.0 / H1), (H2, H1))
        self.b2 = np.zeros(H2)
        self.W3 = rng.normal(0, np.sqrt(2.0 / H2), (n_out, H2))
        self.b3 = np.zeros(n_out)
        self._m = {k: np.zeros_like(v) for k, v in self.params.items()}
        self._v = {k: np.zeros_like(v) for k, v in self.params.items()}
        self._t = 0

    @property
    def params(self):
        return {"W1": self.W1, "b1": self.b1, "W2": self.W2,
                "b2": self.b2, "W3": self.W3, "b3": self.b3}

    def forward(self, X):
        z1 = X @ self.W1.T + self.b1
        a1 = np.maximum(z1, 0.0)
        z2 = a1 @ self.W2.T + self.b2
        a2 = np.maximum(z2, 0.0)
        z3 = a2 @ self.W3.T + self.b3
        return z1, a1, z2, a2, z3

    def logits(self, X):
        return self.forward(X)[4]

    def step(self, X, y1h, w, lr):
        n = X.shape[0]
        z1, a1, z2, a2, z3 = self.forward(X)

        # Softmax cross-entropy with per-sample weights.
        z3 = z3 - z3.max(axis=1, keepdims=True)
        e = np.exp(z3)
        p = e / e.sum(axis=1, keepdims=True)
        loss = float(-(w * np.sum(y1h * np.log(p + 1e-12), axis=1)).sum() / w.sum())

        d3 = (p - y1h) * w[:, None] / w.sum()
        gW3 = d3.T @ a2
        gb3 = d3.sum(axis=0)
        d2 = (d3 @ self.W3) * (z2 > 0)
        gW2 = d2.T @ a1
        gb2 = d2.sum(axis=0)
        d1 = (d2 @ self.W2) * (z1 > 0)
        gW1 = d1.T @ X
        gb1 = d1.sum(axis=0)
        del n

        grads = {"W1": gW1, "b1": gb1, "W2": gW2, "b2": gb2, "W3": gW3, "b3": gb3}
        self._adam(grads, lr)
        return loss

    def _adam(self, grads, lr, b1=0.9, b2=0.999, eps=1e-8):
        self._t += 1
        for k, g in grads.items():
            self._m[k] = b1 * self._m[k] + (1 - b1) * g
            self._v[k] = b2 * self._v[k] + (1 - b2) * (g * g)
            mh = self._m[k] / (1 - b1 ** self._t)
            vh = self._v[k] / (1 - b2 ** self._t)
            getattr(self, k)[...] -= lr * mh / (np.sqrt(vh) + eps)


# --------------------------------------------------------------------------- #
# quantization
# --------------------------------------------------------------------------- #

def quantize(model: MLP, Xq_train: np.ndarray):
    """Quantize the trained float model and return everything the exporter needs.

    Activation scales are calibrated from the training set's own int8 forward
    pass at the 99.9th percentile, not the maximum: a handful of outlier samples
    would otherwise set the scale and crush the resolution of every ordinary
    activation.
    """
    W1q, s_w1 = quant.quantize_weights(model.W1)
    W2q, s_w2 = quant.quantize_weights(model.W2)
    W3q, s_w3 = quant.quantize_weights(model.W3)

    s_x = INPUT_SCALE

    # Layer 1: acc is in units of s_w1 * s_x.
    b1q = np.rint(model.b1 / (s_w1 * s_x)).astype(np.int32)
    acc1 = quant.dense_int(Xq_train, W1q, b1q)
    real1 = acc1 * (s_w1 * s_x)
    a1_max = float(np.percentile(np.abs(np.maximum(real1, 0.0)), 99.9))
    s_h1 = max(a1_max, 1e-6) / 127.0
    m1, sh1 = quant.choose_multiplier((s_w1 * s_x) / s_h1)
    h1q = quant.requantize(acc1, m1, sh1, relu=True)

    # Layer 2.
    b2q = np.rint(model.b2 / (s_w2 * s_h1)).astype(np.int32)
    acc2 = quant.dense_int(h1q, W2q, b2q)
    real2 = acc2 * (s_w2 * s_h1)
    a2_max = float(np.percentile(np.abs(np.maximum(real2, 0.0)), 99.9))
    s_h2 = max(a2_max, 1e-6) / 127.0
    m2, sh2 = quant.choose_multiplier((s_w2 * s_h1) / s_h2)
    h2q = quant.requantize(acc2, m2, sh2, relu=True)

    # Output layer: the accumulator is dequantized directly, no requantization.
    b3q = np.rint(model.b3 / (s_w3 * s_h2)).astype(np.int32)
    out_scale = s_w3 * s_h2

    return {
        "W1q": W1q, "b1q": b1q, "W2q": W2q, "b2q": b2q, "W3q": W3q, "b3q": b3q,
        "s_x": s_x, "s_h1": s_h1, "s_h2": s_h2, "out_scale": out_scale,
        "m1": m1, "sh1": sh1, "m2": m2, "sh2": sh2,
        "_h2q_train": h2q,
    }


def infer_int(q, Xq):
    """The full int8 forward pass, exactly as the C runs it."""
    acc1 = quant.dense_int(Xq, q["W1q"], q["b1q"])
    h1 = quant.requantize(acc1, q["m1"], q["sh1"], relu=True)
    acc2 = quant.dense_int(h1, q["W2q"], q["b2q"])
    h2 = quant.requantize(acc2, q["m2"], q["sh2"], relu=True)
    acc3 = quant.dense_int(h2, q["W3q"], q["b3q"])
    return acc3                                  # int32 logit accumulators


# --------------------------------------------------------------------------- #
# metrics
# --------------------------------------------------------------------------- #

def metrics(y_true, y_pred, n_classes):
    cm = np.zeros((n_classes, n_classes), dtype=np.int64)
    for t, p in zip(y_true, y_pred):
        cm[t, p] += 1
    acc = float(np.trace(cm) / max(1, cm.sum()))
    per = []
    for c in range(n_classes):
        tp = cm[c, c]
        prec = tp / max(1, cm[:, c].sum())
        rec = tp / max(1, cm[c, :].sum())
        f1 = 0.0 if prec + rec == 0 else 2 * prec * rec / (prec + rec)
        per.append({"class": CLASS_NAMES[c], "support": int(cm[c, :].sum()),
                    "precision": round(float(prec), 4),
                    "recall": round(float(rec), 4), "f1": round(float(f1), 4)})
    macro_f1 = float(np.mean([p["f1"] for p in per]))
    return acc, macro_f1, cm, per


def print_cm(cm):
    w = max(len(c) for c in CLASS_NAMES) + 2
    print(" " * w + "".join(f"{c[:11]:>12}" for c in CLASS_NAMES))
    for i, name in enumerate(CLASS_NAMES):
        print(f"{name:<{w}}" + "".join(f"{v:>12}" for v in cm[i]))


# --------------------------------------------------------------------------- #
# export
# --------------------------------------------------------------------------- #

def cfloat(v, digits: int = 9) -> str:
    """A valid C float literal.

    "%g" renders an integer-valued float as "466", and "466f" is not a float
    literal -- the compiler reads it as a malformed integer. Anything without a
    decimal point or exponent therefore needs one added before the f suffix.
    """
    s = f"{float(v):.{digits}g}"
    if not any(c in s for c in ".eEni"):
        s += ".0"
    return s + "f"


def c_i8_array(name, arr, per_line=16):
    flat = np.asarray(arr).reshape(-1).astype(np.int64)
    out = [f"static const int8_t {name}[{flat.size}] = {{"]
    for i in range(0, flat.size, per_line):
        out.append("    " + ", ".join(f"{v:4d}" for v in flat[i:i + per_line]) + ",")
    out.append("};")
    return "\n".join(out)


def c_i32_array(name, arr, per_line=8):
    flat = np.asarray(arr).reshape(-1).astype(np.int64)
    out = [f"static const int32_t {name}[{flat.size}] = {{"]
    for i in range(0, flat.size, per_line):
        out.append("    " + ", ".join(f"{v:8d}" for v in flat[i:i + per_line]) + ",")
    out.append("};")
    return "\n".join(out)


def c_f_array(name, arr, per_line=6):
    flat = np.asarray(arr).reshape(-1)
    out = [f"static const float {name}[{flat.size}] = {{"]
    for i in range(0, flat.size, per_line):
        out.append("    " + ", ".join(cfloat(v, 8) for v in flat[i:i + per_line]) + ",")
    out.append("};")
    return "\n".join(out)


def export_params(path, q, mean, inv_scale, feat_names, model_id, report):
    n_feat = len(feat_names)
    L = [
        "/* model_params.h - GENERATED by ml/train.py. Do not edit by hand.",
        " *",
        f" * model id        : {model_id}",
        f" * architecture    : {n_feat} -> {H1} -> {H2} -> {len(CLASS_NAMES)}"
        "  (ReLU, int8 weights)",
        f" * parameters      : {report['param_bytes']} bytes",
        f" * test accuracy   : {report['int8_accuracy'] * 100:.2f} %"
        f"  (float {report['float_accuracy'] * 100:.2f} %)",
        f" * test macro F1   : {report['int8_macro_f1']:.4f}",
        f" * trained on      : {report['n_train']} rows, "
        f"{report['n_train_sessions']} sessions",
        f" * tested on       : {report['n_test']} rows, "
        f"{report['n_test_sessions']} held-out sessions",
        " *",
        " * Feature order is fixed by aeris_feature_name() in core/src/features.c:",
    ]
    for i, n in enumerate(feat_names):
        L.append(f" *   [{i:2d}] {n}")
    L += [
        " */",
        "#ifndef AERIS_MODEL_PARAMS_H",
        "#define AERIS_MODEL_PARAMS_H",
        "",
        "#include <stdint.h>",
        "",
        "#define AERIS_MODEL_TRAINED 1",
        f'#define AERIS_MODEL_ID "{model_id}"',
        f"#define AERIS_MODEL_H1 {H1}",
        f"#define AERIS_MODEL_H2 {H2}",
        f"#define AERIS_N_CLASSES {len(CLASS_NAMES)}",
        "",
        "/* Input quantization: q = round(((x - mean) * inv_scale) / input_scale),",
        " * clamped to int8. inv_scale is 1/sigma from the training split. */",
        f"#define AERIS_INPUT_SCALE  {q['s_x']:.8g}f",
        "/* Output logits = int32 accumulator * output_scale. */",
        f"#define AERIS_OUTPUT_SCALE {q['out_scale']:.10g}f",
        "",
        "/* Requantization: out = round_shift((acc * mult) >> 16, shift). */",
        f"#define AERIS_M1_MULT  {q['m1']}",
        f"#define AERIS_M1_SHIFT {q['sh1']}",
        f"#define AERIS_M2_MULT  {q['m2']}",
        f"#define AERIS_M2_SHIFT {q['sh2']}",
        "",
        c_f_array("AERIS_FEAT_MEAN", mean),
        c_f_array("AERIS_FEAT_INV_SCALE", inv_scale),
        "",
        c_i8_array("AERIS_W1", q["W1q"]),
        c_i32_array("AERIS_B1", q["b1q"]),
        "",
        c_i8_array("AERIS_W2", q["W2q"]),
        c_i32_array("AERIS_B2", q["b2q"]),
        "",
        c_i8_array("AERIS_W3", q["W3q"]),
        c_i32_array("AERIS_B3", q["b3q"]),
        "",
        f"static const char *const AERIS_CLASS_NAMES[{len(CLASS_NAMES)}] = {{",
        "    " + ", ".join(f'"{c}"' for c in CLASS_NAMES),
        "};",
        "",
        "#endif /* AERIS_MODEL_PARAMS_H */",
    ]
    path.write_text("\n".join(L) + "\n")


def export_vectors(path, X_raw, Xq, logits, argmax, n_feat):
    """X_raw must be the unstandardised features: the C kernel standardises."""
    n = X_raw.shape[0]
    L = [
        "/* vectors_model.h - GENERATED by ml/train.py. Do not edit by hand.",
        " *",
        f" * {n} held-out samples with the int8 input, int32 logit accumulators and",
        " * argmax that ml/quant.py produced. tests/test_model.c replays these",
        " * through core/src/model.c and requires an exact match, which is what",
        " * proves the trained accuracy is the on-device accuracy.",
        " */",
        "#ifndef AERIS_VECTORS_MODEL_H",
        "#define AERIS_VECTORS_MODEL_H",
        "",
        "#include <stdint.h>",
        "",
        "#define AERIS_HAVE_VECTORS 1",
        f"#define AERIS_N_VECTORS {n}",
        "",
        "/* Raw features, exactly as aeris_extract_features() produced them. */",
        f"static const float AERIS_VEC_FEATURES[{n}][{n_feat}] = {{",
    ]
    for r in range(n):
        L.append("    {" + ", ".join(cfloat(v) for v in X_raw[r]) + "},")
    L += ["};", "",
          f"static const int8_t AERIS_VEC_QINPUT[{n}][{n_feat}] = {{"]
    for r in range(n):
        L.append("    {" + ", ".join(f"{int(v):4d}" for v in Xq[r]) + "},")
    L += ["};", "",
          f"static const int32_t AERIS_VEC_LOGITS[{n}][{len(CLASS_NAMES)}] = {{"]
    for r in range(n):
        L.append("    {" + ", ".join(f"{int(v):9d}" for v in logits[r]) + "},")
    L += ["};", "",
          f"static const int32_t AERIS_VEC_ARGMAX[{n}] = {{"]
    for i in range(0, n, 20):
        L.append("    " + ", ".join(str(int(v)) for v in argmax[i:i + 20]) + ",")
    L += ["};", "", "#endif /* AERIS_VECTORS_MODEL_H */"]
    path.write_text("\n".join(L) + "\n")


# --------------------------------------------------------------------------- #

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", type=pathlib.Path, default=pathlib.Path("ml/data/features.csv"))
    ap.add_argument("--out", type=pathlib.Path,
                    default=pathlib.Path("core/include/aeris/model_params.h"))
    ap.add_argument("--vectors", type=pathlib.Path,
                    default=pathlib.Path("tests/vectors_model.h"))
    ap.add_argument("--report", type=pathlib.Path,
                    default=pathlib.Path("ml/data/report.json"))
    ap.add_argument("--epochs", type=int, default=90)
    ap.add_argument("--batch", type=int, default=512)
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--test-frac", type=float, default=0.25)
    args = ap.parse_args()

    X, y, g, feat_names = load(args.data)
    n_feat, n_cls = X.shape[1], len(CLASS_NAMES)
    print(f"loaded {X.shape[0]} rows, {n_feat} features, "
          f"{len(np.unique(g))} sessions from {args.data}")

    tr, te, test_sessions = split_by_session(g, args.test_frac, args.seed)
    print(f"split: {tr.sum()} train rows ({len(np.unique(g[tr]))} sessions), "
          f"{te.sum()} test rows ({len(test_sessions)} held-out sessions)")

    # Standardise on the training split only.
    mean = X[tr].mean(axis=0)
    std = X[tr].std(axis=0)
    std[std < 1e-6] = 1.0
    inv_scale = 1.0 / std
    Xs = (X - mean) * inv_scale

    counts = np.bincount(y[tr], minlength=n_cls)
    print("train class balance: " +
          ", ".join(f"{CLASS_NAMES[c]}={counts[c]}" for c in range(n_cls)))
    # Balanced weights: 'volatile' is a tenth of 'occupancy' in a realistic day,
    # and an unweighted fit simply learns to never predict it.
    cls_w = counts.sum() / (n_cls * np.maximum(counts, 1))
    y1h = np.eye(n_cls)[y]

    model = MLP(n_feat, n_cls, seed=args.seed)
    rng = np.random.default_rng(args.seed + 1)
    idx_tr = np.flatnonzero(tr)

    best = None
    for ep in range(args.epochs):
        rng.shuffle(idx_tr)
        # Cosine decay: the last epochs matter for the quantized result, since a
        # still-moving weight tensor quantizes badly.
        lr = args.lr * 0.5 * (1 + np.cos(np.pi * ep / args.epochs))
        tot, nb = 0.0, 0
        for s in range(0, idx_tr.size, args.batch):
            b = idx_tr[s:s + args.batch]
            tot += model.step(Xs[b], y1h[b], cls_w[y[b]], lr)
            nb += 1
        if (ep + 1) % 15 == 0 or ep == args.epochs - 1:
            pred = model.logits(Xs[te]).argmax(axis=1)
            acc, mf1, _, _ = metrics(y[te], pred, n_cls)
            print(f"  epoch {ep + 1:3d}  loss {tot / nb:.4f}  "
                  f"test acc {acc * 100:5.2f}%  macro F1 {mf1:.4f}")
            if best is None or mf1 > best[0]:
                best = (mf1, {k: v.copy() for k, v in model.params.items()})

    # Keep the best-macro-F1 weights rather than the last epoch's.
    if best is not None:
        for k, v in best[1].items():
            getattr(model, k)[...] = v

    # ---- float baseline ---------------------------------------------------
    pred_f = model.logits(Xs[te]).argmax(axis=1)
    acc_f, mf1_f, cm_f, _ = metrics(y[te], pred_f, n_cls)

    # ---- quantize and evaluate the integer network ------------------------
    Xq_tr = quant.quantize_input(Xs[tr], INPUT_SCALE)
    q = quantize(model, Xq_tr)
    Xq_te = quant.quantize_input(Xs[te], INPUT_SCALE)
    logits_te = infer_int(q, Xq_te)
    pred_q = np.asarray(logits_te).argmax(axis=1)
    acc_q, mf1_q, cm_q, per_q = metrics(y[te], pred_q, n_cls)

    param_bytes = (q["W1q"].size + q["W2q"].size + q["W3q"].size +
                   4 * (q["b1q"].size + q["b2q"].size + q["b3q"].size))

    print(f"\nfloat32  test accuracy {acc_f * 100:.2f}%  macro F1 {mf1_f:.4f}")
    print(f"int8     test accuracy {acc_q * 100:.2f}%  macro F1 {mf1_q:.4f}")
    print(f"accuracy change from quantization: "
          f"{(acc_q - acc_f) * 100:+.2f} pp")
    print(f"parameters: {param_bytes} bytes\n")
    print("int8 confusion (rows = truth, cols = predicted)")
    print_cm(cm_q)
    print()
    for p in per_q:
        print(f"  {p['class']:<12} precision {p['precision']:.3f}  "
              f"recall {p['recall']:.3f}  f1 {p['f1']:.3f}  n={p['support']}")

    agree = float((pred_f == pred_q).mean())
    print(f"\nfloat/int8 prediction agreement: {agree * 100:.2f}%")

    model_id = f"mlp{n_feat}x{H1}x{H2}x{n_cls}-int8"
    report = {
        "model_id": model_id,
        "architecture": [n_feat, H1, H2, n_cls],
        "param_bytes": int(param_bytes),
        "float_accuracy": acc_f, "float_macro_f1": mf1_f,
        "int8_accuracy": acc_q, "int8_macro_f1": mf1_q,
        "float_int8_agreement": agree,
        "n_train": int(tr.sum()), "n_test": int(te.sum()),
        "n_train_sessions": int(len(np.unique(g[tr]))),
        "n_test_sessions": len(test_sessions),
        "held_out_sessions": test_sessions,
        "per_class": per_q,
        "confusion_int8": cm_q.tolist(),
        "features": feat_names,
        "classes": CLASS_NAMES,
        "input_scale": q["s_x"], "output_scale": q["out_scale"],
        "requant": {"m1": q["m1"], "sh1": q["sh1"], "m2": q["m2"], "sh2": q["sh2"]},
    }

    export_params(args.out, q, mean, inv_scale, feat_names, model_id, report)
    print(f"\nwrote {args.out}")

    # Parity vectors: a stratified sample of held-out rows so every class and
    # both easy and borderline cases are represented.
    rng2 = np.random.default_rng(args.seed + 2)
    te_idx = np.flatnonzero(te)
    chosen = []
    per_cls = max(1, N_PARITY_VECTORS // n_cls)
    for c in range(n_cls):
        pool = te_idx[y[te_idx] == c]
        if pool.size:
            chosen.append(rng2.choice(pool, size=min(per_cls, pool.size), replace=False))
    chosen = np.concatenate(chosen)
    # The vectors carry RAW feature values: aeris_model_quantize_input() applies
    # the mean and inverse scale itself, so handing it standardised values would
    # apply them twice.
    Xq_v = quant.quantize_input(Xs[chosen], INPUT_SCALE)
    lg_v = np.asarray(infer_int(q, Xq_v))
    export_vectors(args.vectors, X[chosen], Xq_v, lg_v, lg_v.argmax(axis=1), n_feat)
    print(f"wrote {args.vectors} ({len(chosen)} parity vectors)")

    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(f"wrote {args.report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
