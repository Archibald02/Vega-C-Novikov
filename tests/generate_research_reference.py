"""Developer-only numerical oracle; Python/statsmodels are not used by the Windows app."""
from pathlib import Path
import numpy as np
import statsmodels
from statsmodels.tsa.stattools import acf, pacf, adfuller, kpss

x = np.array([100 + .03*i + np.sin(i*.7) + .7*np.cos(i*1.91) + .2*np.sin(i*.031)
              for i in range(240)])
a = adfuller(x, maxlag=2, autolag=None, regression="c")
k = kpss(x, regression="c", nlags=4)
lines = ["# Generated independently with statsmodels " + statsmodels.__version__]
for name, values in [("x", x), ("acf", acf(x, nlags=20, fft=False, adjusted=False)),
                     ("pacf", pacf(x, nlags=20, method="ywm"))]:
    lines.extend(f"{name} {i} {value:.17g}" for i, value in enumerate(values))
for name, value in [("adf", a[0]), ("crit5", a[4]["5%"]), ("kpss", k[0])]:
    lines.append(f"{name} 0 {value:.17g}")
Path(__file__).with_name("fixtures").joinpath("research_reference.tsv").write_text("\n".join(lines) + "\n")
