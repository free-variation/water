# Water — loadable library reference

The words below are not in the base image; they are defined by loading a file
from `lib/`. `"lib/statistics.h2o" load` brings in the linear-algebra,
regression, generalized-linear-model, and gradient-boosting words. Once a
library is loaded its words answer to `help`, `man`, and `apropos` exactly like
built-ins — this file is the source `gen-help.py` reads for them, alongside
`reference.md`.

The statistics library is native-only: it reaches BLAS/LAPACK (and, for the
xgboost words, libxgboost) through the FFI, which the wasm build excludes.

## Linear algebra (lib/statistics.h2o)

| Word | Stack effect | Summary |
| --- | --- | --- |
| `svd` | `( A -- U S VT )` | Thin singular value decomposition via LAPACKE dgesvd: `A = U diag(S) VT`, with `S` the 1×min(m,n) singular values. Column signs of U/VT are not canonical, so pin goldens on S and the reconstruction, not raw U/VT entries |
| `fit-linear` | `( m y -- beta )` | Ordinary least squares via LAPACKE dgelsd; `m` is observations×predictors (observations ≥ predictors), `y` the observations×1 response, `beta` the predictors×1 coefficients |
| `fit-augmented` | `( augmented -- beta )` | Least squares of an `[X | y]` block whose last column is the response |

## Regression (lib/statistics.h2o)

| Word | Stack effect | Summary |
| --- | --- | --- |
| `linear-regression` | `( dataset predictors response replications -- summaries )` | OLS with nonparametric bootstrap inference: a `{ :estimate :se :bias :ci-low :ci-high }` frame per coefficient over `replications` refits |
| `fit-logistic` | `( X y max-iterations tolerance -- beta )` | Binary logistic regression by Firth-penalized IRLS (estimates stay finite under separation); `X` includes the intercept column, `y` in {0,1} |
| `fit-logistic-ridge` | `( X y max-iterations tolerance lambda -- beta )` | L2-penalized logistic by IRLS; `lambda` penalizes ‖beta‖²/2 with the intercept column unpenalized, `lambda` 0 the plain MLE (no Firth) |
| `fit-augmented-logistic` | `( augmented -- beta )` | Firth logistic fit of an `[X | y]` block |
| `logistic-regression` | `( dataset predictors response replications -- summaries )` | Firth logistic with the bootstrap per-coefficient summaries of `linear-regression` |
| `cv-logistic-ridge` | `( X y units lambdas n-folds -- fr )` | k-fold cross-validation of ridge logistic over a `lambdas` grid, returning `{ :lambdas :deviances :best }`; `X` excludes the intercept (added internally, unpenalized), `units` index rows so per-cluster index arrays give cluster CV |
| `pcv-logistic-ridge` | `( X y units lambdas n-folds -- fr )` | `cv-logistic-ridge` with the (lambda, fold) cells evaluated under `pmap`; results are identical, the cells being deterministic |

## Generalized linear models (lib/statistics.h2o)

| Word | Stack effect | Summary |
| --- | --- | --- |
| `fit-glm` | `( X y family max-iterations tolerance -- beta )` | IRLS for a family object — a frame of three stack quotations `:inverse-link ( eta -- mu )`, `:mean-derivative ( eta -- dmu/deta )`, `:variance ( mu -- V )`. Each step solves a weighted least squares via `fit-linear`. Provided families: `gaussian-identity`, `poisson-log`, `gamma-log`, `binomial-logit` |
| `fit-gamma` | `( X y max-iterations tolerance -- beta )` | Gamma regression, log link — `fit-glm` with `gamma-log` |
| `fit-poisson` | `( X y max-iterations tolerance -- beta )` | Poisson regression, log link — `fit-glm` with `poisson-log` |
| `fit-multinomial` | `( X y reference-class max-iterations tolerance -- beta )` | Multinomial (softmax) logistic by Newton–Raphson, baseline-category parametrization with `reference-class` as the baseline; `y` holds integer labels 0..K−1, `beta` is predictors×(K−1), one coefficient column per non-reference class. As the plain MLE it diverges under separation |
| `fit-multinomial-ridge` | `( X y reference-class max-iterations tolerance lambda -- beta )` | `fit-multinomial` with an L2 penalty λ·‖β‖²/2 on every coefficient except each class's intercept; `lambda` 0 is the plain MLE (what `fit-multinomial` calls), `lambda` > 0 keeps the estimate finite under separation |
| `predict-multinomial` | `( beta X reference-class -- probabilities )` | Softmax probabilities from a `fit-multinomial`/`fit-multinomial-ridge` model: n×K, columns in label order 0..K−1 (the `reference-class` column is `1/Σ` weights). Each row sums to 1 |

## Gradient boosting (lib/statistics.h2o)

| Word | Stack effect | Summary |
| --- | --- | --- |
| `fit-xgb` | `( X y fit-params -- booster )` | Train an XGBoost booster on features `X` (n×k) and response `y` (n×1); `fit-params` are keyed by xgboost parameter name, `:rounds` drives the boosting loop (default 100). Native-only (libxgboost via FFI) |
| `xgb-predict` | `( booster X -- predictions )` | n×1 scores for an n×k feature matrix; the booster is not freed |
| `xgb-free` | `( booster -- )` | Free a booster handle |
| `xgb-importance` | `( booster importance-type -- scores )` | k×1 per-feature importance, row i is feature i; `importance-type` is `"gain"`, `"weight"`, `"cover"`, `"total_gain"`, or `"total_cover"`. Rank with `matrix>array argsort reverse` |
