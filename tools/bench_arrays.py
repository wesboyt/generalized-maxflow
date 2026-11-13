"""Times gmaxflow.solve (JSON) against gmaxflow.max_flow_arrays on the same
time-expanded trading lattices: L layers of a cash node and a holding node,
five buy and five sell levels per layer, hold arcs between layers.

    python tools/bench_arrays.py [--layers 10 40 160] [--repeat 200]
"""
import argparse
import random
import time

import gmaxflow


def lattice(layers, seed):
    rnd = random.Random(seed)
    frm, to, cap, mul = [], [], [], []
    s, t = 0, 1
    cash = lambda j: 2 + 2 * j          # noqa: E731
    hold = lambda j: 3 + 2 * j          # noqa: E731
    frm.append(s); to.append(cash(0)); cap.append(100.0); mul.append(1.0)
    mid = 0.5
    for j in range(layers):
        mid = min(0.95, max(0.05, mid + rnd.gauss(0, 0.02)))
        for k in range(5):
            ask, bid = mid + 0.01 * (k + 1), mid - 0.01 * (k + 1)
            n = rnd.uniform(5, 50)
            frm.append(cash(j)); to.append(hold(j)); cap.append(ask * n); mul.append(1 / (ask + 0.0175))
            frm.append(hold(j)); to.append(cash(j)); cap.append(n); mul.append(bid - 0.0175)
        nxt_c, nxt_h = (cash(j + 1), hold(j + 1)) if j + 1 < layers else (t, t)
        frm += [cash(j), hold(j)]; to += [nxt_c, nxt_h]; cap += [-1.0, -1.0]
        mul += [1.0, 1.0 if j + 1 < layers else (1.0 if rnd.random() < mid else 1e-9)]
    return 2 + 2 * layers, s, t, frm, to, cap, mul


def as_json(n, s, t, frm, to, cap, mul):
    return {"source": s, "sink": t, "nodes": list(range(n)), "options": {"collect_paths": False},
            "edges": [{"from": a, "to": b, "capacity": (None if c < 0 else c), "multiplier": m}
                      for a, b, c, m in zip(frm, to, cap, mul)]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--layers", type=int, nargs="+", default=[10, 40, 160])
    ap.add_argument("--repeat", type=int, default=200)
    a = ap.parse_args()
    try:
        import numpy as np
    except ImportError:
        np = None
    print("layers  edges   json us/solve  arrays us/solve  speed-up")
    for L in a.layers:
        g = lattice(L, 7)
        js = as_json(*g)
        n, s, t, frm, to, cap, mul = g
        if np is not None:
            frm, to = np.array(frm, np.int32), np.array(to, np.int32)
            cap, mul = np.array(cap), np.array(mul)
        v1 = gmaxflow.solve(js)["max_flow"]
        v2 = gmaxflow.max_flow_arrays(n, s, t, frm, to, cap, mul)
        assert abs(v1 - v2) <= 1e-9 * max(1.0, abs(v1)), (v1, v2)
        t0 = time.perf_counter()
        for _ in range(a.repeat):
            gmaxflow.solve(js)
        tj = (time.perf_counter() - t0) / a.repeat
        t0 = time.perf_counter()
        for _ in range(a.repeat):
            gmaxflow.max_flow_arrays(n, s, t, frm, to, cap, mul)
        ta = (time.perf_counter() - t0) / a.repeat
        print("%6d %6d %14.1f %16.1f %9.1fx" % (L, len(g[3]), tj * 1e6, ta * 1e6, tj / ta))


if __name__ == "__main__":
    main()
