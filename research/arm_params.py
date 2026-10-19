"""Write the params file mm_sim reads for a GLFT arm (Step 7a). Every number comes from the
frozen dev calibration (config/calib_dev.json) plus gamma; nothing here is tuned.
A and k are the probe values for the SAME queue rule and latency as the run (Addendum A1)."""
import json

CALIB = 'config/calib_dev.json'
SEED = 20261020                                    # pre-registered S3r coin-flip seed


def write_params(path, arm, gamma, rule, lat_ms, calib=None):
    c = calib or json.load(open(CALIB))
    p = c['probes'][rule][f'{lat_ms}ms']
    g = c['g_tau_ticks']
    assert c['alpha_source'] == 'g_tau' and len(g) == 10
    with open(path, 'w') as f:
        f.write(f"arm {arm}\ngamma {gamma!r}\nsigma {c['sigma_ticks_per_sqrt_s']!r}\n"
                f"A {p['A']!r}\nk {p['k01']!r}\nkappa {c['kappa']!r}\nseed {SEED}\n"
                f"g_tau {' '.join(repr(v) for v in g)}\n")
    return path
