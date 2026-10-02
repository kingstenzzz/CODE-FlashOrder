# FlashOrder-P preference-disagreement consensus path

`flashorder-P` follows the `flashorder-D` HotStuff project layout. The
preference-disagreement sorter is wired into `HotStuffCore::flashorder_propose`
and uses the same replica certificates collected by the consensus path.

The only adaptive clustering parameter is `k_d`:

\[
D_t = \frac{\sum_{i<j}2\min(P_{ij},P_{ji})}
           {\sum_{i<j}(P_{ij}+P_{ji})},\qquad
\delta_t = \operatorname{clip}(1+\lceil k_dD_t\rceil,1,m_t-1).
\]

The P variant does not use `E[delay]` to compute delta. The legacy timing
arguments remain in the C++ API only for compatibility with existing launchers.

## Python reference path

```powershell
python -m unittest discover -s flashorder-P/test -p 'test_preference_disagreement.py'
```

```python
from HG import PreferenceDisagreementSorter
result = PreferenceDisagreementSorter(41, 2, gamma=0.80, k_d=1.0).sort(certificate_rows)
```

## C++ consensus module

```powershell
cmake -S flashorder-P -B flashorder-P/build -DBUILD_EXAMPLES=OFF
cmake --build flashorder-P/build --target preference_disagreement_example
flashorder-P/build/preference_disagreement_example
```

The standalone C++ API accepts the same integer permutation certificate and
returns the ordered transaction IDs, clusters, `D_t`, δ, and the support
threshold. The integrated HotStuff path uses `FLASHORDER_P_KD` as the single
tunable parameter `K_D` (default `1.0`).
