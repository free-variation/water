# Regression in Water

This document explains how Water fits linear and logistic regressions — and
to do that honestly it has to explain the *mathematics*, because the code is the
math made executable. So it does both at once: each idea is derived far enough
that you could reconstruct it, and then connected to the word that performs it.
The aim is that by the end you understand not just *what* `linear-regression` and
`logistic-regression` return, but *why* every step is the step it is.

It assumes you've seen means, variance, vectors and matrices, and the idea of a
sampling distribution — a second-semester course. Throughout, `n` is the number
of observations and `k` the number of predictors; one observation is a row of
predictors `x = (x₁, …, x_k)` paired with an outcome `y`. The statistics layer
lives in `lib/statistics.h2o`, on top of the matrix kernels and an FFI to LAPACK.

---

## 1. The shared backbone: the linear predictor

Both models are built from one object, the **linear predictor** — a weighted sum
of the predictors plus a constant:

```
η = β₀ + β₁x₁ + β₂x₂ + ⋯ + β_k x_k
```

The `βⱼ` are the **coefficients** we learn from data. `β₀` is the **intercept**:
the value of `η` when every predictor is zero. Each other `βⱼ` is the amount `η`
moves when `xⱼ` rises by one unit with the others held fixed.

It pays to write this in matrix form. Stack the `n` observations' predictor rows
into a matrix `X`, one row per observation, one column per predictor — *plus a
leading column of `1`s*, so that `β₀` has a constant to multiply (every row's
first entry is 1, so `β₀·1` is the intercept). Then the `n` linear predictors,
computed all at once, are the matrix–vector product

```
η = Xβ
```

That single column of ones is the only structural trick here, and it's why
Water builds a design matrix in two steps: `dataset>matrix` turns the chosen
predictor columns into the numeric matrix, and `with-intercept` prepends the
column of ones. From here, everything is two choices:

1. **How `η` connects to the outcome** — the *link*. Linear regression uses `η`
   directly; logistic regression bends it through a curve to make a probability.
2. **What makes coefficients "good"** — the *fit criterion*. Linear regression
   minimizes squared error; logistic regression maximizes likelihood. We'll see
   these are the same principle in two costumes.

---

## 2. Linear regression

### The model

We assume the outcome *is* the linear predictor, plus random noise:

```
y = Xβ + ε
```

`ε` collects everything about `y` the predictors don't explain. We treat it as
random, centered at zero (so the model is right *on average*) with some spread
`σ`. The model's claim: knowing the predictors, your best guess for `y` is `Xβ`,
and you'll typically miss by about `σ`.

### What "best fit" means, precisely

For candidate coefficients `β`, observation `i` has a **fitted value**
`ŷᵢ = xᵢᵀβ` and a **residual** `eᵢ = yᵢ − ŷᵢ` — the gap between what we saw and
what the line predicts. A good fit makes the residuals small, and we measure
total smallness by the **sum of squared residuals**:

```
SSR(β) = Σᵢ (yᵢ − ŷᵢ)²  =  ‖y − Xβ‖²
```

The **least-squares** estimate `β̂` is the `β` that makes `SSR` smallest. That is
the whole definition of "line of best fit." Two questions remain, and the doc
that just dropped a formula here is exactly the one that doesn't teach: *why
squared error*, and *how do we actually find the minimum*.

### Why squared error — and what it implies

Squaring does three things. It **punishes big misses**: doubling a residual
quadruples its cost, so the fit fights hardest against the worst errors rather
than spreading blame evenly. It is **smooth**: as a function of `β`, `SSR` is a
bowl with a single lowest point and no kinks, so calculus can find that point
cleanly (absolute error, `Σ|eᵢ|`, has corners and ties). And it **matches normal
noise**: if `ε` is bell-shaped, the least-squares `β̂` is provably the *most
likely* coefficients — the maximum-likelihood estimate. So squared error isn't
arbitrary; it is what "the errors are normal" *means*. Hold onto that — logistic
regression keeps the likelihood idea and loses the squared-error shortcut.

### Deriving the solution

`SSR(β) = ‖y − Xβ‖²` is quadratic in `β`, so its minimum is where the gradient
(the slope in every coefficient direction) is zero. Differentiating and setting
to zero:

```
∇SSR(β) = −2 Xᵀ(y − Xβ) = 0     ⟹     XᵀX β = Xᵀy
```

Those last equations — one per coefficient — are the **normal equations**. Read
the stationarity condition `Xᵀ(y − Xβ) = 0` directly and the geometry appears:
`y − Xβ` is the residual vector, and `Xᵀ(residual) = 0` says the residual is
**orthogonal to every predictor column**. That is the whole picture in one line:

> The fitted values `Xβ` — all combinations of the predictor columns — sweep out
> a flat subspace. Least squares finds the point in that subspace closest to `y`,
> and "closest" means dropping a perpendicular. `ŷ` is the shadow of `y` on the
> space the predictors can reach; the residual is the part of `y` no combination
> of predictors can explain.

Linear regression *is* orthogonal projection, and the algebra and the geometry
are the same statement.

### How Water actually solves it — and why not by that formula

The normal equations have a tidy closed form, `β̂ = (XᵀX)⁻¹Xᵀy`, and a naive
implementation would compute exactly that. Water does **not**, for a
numerical reason worth understanding. Forming `XᵀX` *squares the conditioning* of
the problem: roughly, the ratio between the most and least informative directions
in `X` gets squared, and any near-redundancy among predictors (two columns nearly
collinear) — already a strain — becomes a catastrophe, with the inverse
amplifying rounding error wildly. You can lose half your significant digits just
by building `XᵀX`.

The cure is to never form it. Water solves the least-squares problem
*directly* through the **singular value decomposition** (SVD). The SVD factors any
matrix as

```
X = U Σ Vᵀ
```

where `U` and `V` have orthonormal columns and `Σ` is diagonal with the
non-negative **singular values** σ₁ ≥ σ₂ ≥ … (the intrinsic "gains" of `X`, biggest
to smallest). In these coordinates the least-squares solution is immediate — each
singular direction is solved independently, scaling by `1/σⱼ` — and the squaring
never happens, so the conditioning is the honest conditioning of `X` itself. The
SVD also handles a *rank-deficient* `X` gracefully: a zero (or tiny) singular value
is simply dropped instead of blowing up an inverse, yielding the minimum-norm
solution.

`fit-linear` is this solve. It marshals the design matrix and the response across
the FFI to LAPACK's `dgelsd` — an SVD-based least-squares driver — and returns
`β̂`. The SVD is the workhorse of this whole document: it reappears inside the
logistic fit below, both as the solver for each step and as the source of the
leverage values Firth needs.

### Reading the coefficients

`βⱼ` is the expected change in `y` per one-unit increase in `xⱼ`, the others held
fixed; its sign is direction, its magnitude depends on `xⱼ`'s units. `β₀` is the
predicted `y` when all predictors are zero — often a mathematical anchor, not a
real case.

---

## 3. Logistic regression

### Why the straight line breaks

Now `y` is **binary** — `1` ("yes", "survived") or `0`. Fitting a line fails for
two reasons that aren't fixable by tweaking. A line is **unbounded**: it will
predict `1.3` or `−0.4`, which cannot be probabilities. And the noise is **not
bell-shaped**: a `0/1` outcome has none of the symmetric, constant spread least
squares assumes — its variance even depends on the probability itself (a coin you
think is fair varies more than one you think is rigged). So we change *what we
model*: not the `0/1` value, but the **probability** that it is `1`.

### The sigmoid, and why this curve

Let `p = P(y = 1)`. We want `p` to ride on the linear predictor `η` but stay
trapped in `(0, 1)`. Rather than guess a squashing curve, *demand* the property
that makes the model interpretable: that the predictors act **linearly on the
log-odds**. The odds of the event are `p/(1−p)`, and asking for

```
log( p / (1 − p) ) = η = β₀ + β₁x₁ + ⋯
```

and solving for `p` *forces* the curve. Exponentiate: `p/(1−p) = e^η`. Solve for
`p`: `p = e^η / (1 + e^η) = 1 / (1 + e^(−η))`. That is the **logistic** (or
**sigmoid**) function `σ(η)`:

```
p = σ(η) = 1 / (1 + e^(−η))
```

— S-shaped, with `σ(−∞)=0`, `σ(0)=½`, `σ(+∞)=1`. So the sigmoid isn't an
arbitrary choice of squashing function; it is *the* function whose inverse, the
**logit** `log(p/(1−p))`, is linear. The straight line lives on the log-odds
scale, the curve lives on the probability scale, and the logit is the bridge.
This is why `βⱼ` reads as "a one-unit rise in `xⱼ` adds `βⱼ` to the log-odds," and
why `e^(βⱼ)` is the **odds ratio** — the factor the odds multiply by. Water's
`sigmoid` word applies `σ` element-wise to a vector of linear predictors.

### What makes coefficients "good": likelihood, derived

There are no residuals to square. Instead: *which coefficients make the data we
actually saw most probable?* That is **maximum likelihood**.

Each observation contributes the probability the model assigned to what happened —
`pᵢ` if `yᵢ = 1`, and `1 − pᵢ` if `yᵢ = 0`. Those two cases collapse into one
expression by using `yᵢ` as an exponent switch:

```
P(yᵢ) = pᵢ^(yᵢ) · (1 − pᵢ)^(1 − yᵢ)
```

When `yᵢ = 1` the second factor is `(1−pᵢ)^0 = 1`, leaving `pᵢ`; when `yᵢ = 0` the
first factor is `pᵢ^0 = 1`, leaving `1 − pᵢ`. The exponents simply *select* the
right factor. Assuming observations are independent, the probability of the whole
dataset is the product, the **likelihood** `L(β) = Πᵢ P(yᵢ)`. Products of many
small numbers are awkward, and the logarithm fixes that without moving the
maximum (it's monotone increasing) while turning the product into a sum — the
**log-likelihood**:

```
ℓ(β) = Σᵢ [ yᵢ log pᵢ + (1 − yᵢ) log(1 − pᵢ) ]
```

(This is the same principle as §2: with normal errors, *minimizing squared error*
already **was** *maximizing likelihood*. Linear regression just happened to have a
closed-form maximizer; logistic regression won't.)

### Why there's no formula — and why each step is a least-squares fit

Maximize `ℓ` the usual way: set its gradient to zero. The gradient (the **score**)
works out to a clean `Xᵀ(y − p)` — but `p = σ(Xβ)` buries `β` inside the sigmoid,
so `Xᵀ(y − σ(Xβ)) = 0` has no algebraic solution for `β`. We climb the hill
numerically with **Newton's method**, which steps using the local curvature (the
second derivative, or **Hessian**). For the log-likelihood the Hessian is
`−XᵀWX`, where `W` is diagonal with entries

```
wᵢ = pᵢ(1 − pᵢ)
```

This weight is exactly the **variance of a coin with probability `pᵢ`**. It peaks
at `p = ½` and shrinks toward zero as `p` approaches 0 or 1: an observation near
the decision boundary is the most informative, a confidently-predicted one barely
moves the fit.

Now the payoff. Write out Newton's step `β ← β + (XᵀWX)⁻¹ Xᵀ(y − p)` and regroup,
and it rearranges into

```
β_new = argmin over b of   Σᵢ wᵢ (zᵢ − xᵢᵀb)²,      where   zᵢ = ηᵢ + (yᵢ − pᵢ)/wᵢ
```

— **a weighted least-squares fit** of a synthetic target `z` (the *working
response*) on the same `X`, with weights `w`. So every Newton step *is* a linear
regression, the very problem §2 already solves. This is **iteratively reweighted
least squares (IRLS)**: from a guess for `β`, compute `p` and the weights, build
the working response, solve a weighted least squares for a better `β`, and repeat
until `β` stops moving. A handful of iterations suffices.

Water's `fit-logistic` is this loop, and it reuses §2's machinery exactly. A
weighted least squares is solved by *scaling each row by `√wᵢ`* and running an
ordinary fit, so each iteration computes `η = Xβ` (a matrix multiply via
`dgemm-nn`), `p = sigmoid(η)`, the weights `w` and their roots, the scaled design
and working response, and calls `fit-linear` — the same SVD/`dgelsd` solve — for
the next `β`. The same SVD, once per step.

### Separation, and the Firth correction

One thing breaks the climb. Suppose a predictor splits the classes *perfectly* —
every `y = 1` above some threshold, every `y = 0` below. Then the model can always
do a little better by making the curve steeper, driving a coefficient toward `±∞`;
the likelihood has no finite peak and the iteration never settles. This is
**separation**, and it's common in small or tidy datasets.

The cure Water applies is the **Firth correction**: a principled penalty that
adjusts each observation's residual by its **leverage**. Leverage `hᵢ` measures how
much observation `i` pulls its own fitted value — and it falls straight out of the
SVD of the weighted design: with `√W·X = UΣVᵀ`, the leverages are `hᵢ = Σⱼ Uᵢⱼ²`,
the squared row norms of `U`. (This is the second place the SVD earns its keep:
the same factorization that solves the step also hands back the leverages.) Firth
replaces the plain residual `yᵢ − pᵢ` with

```
(yᵢ − pᵢ) + hᵢ(½ − pᵢ)
```

which nudges each fitted probability toward `½` in proportion to its leverage —
hardest on exactly the high-leverage points that drive separation — and that nudge
is enough to guarantee a finite optimum while barely touching well-behaved data.
Conceptually it's a built-in skepticism that refuses to believe in infinite
certainty from finite data. `fit-logistic` carries this adjusted residual through
its working response, which is why it converges even on separated data.

### Reading the coefficients

On the log-odds scale `βⱼ` is additive (a unit of `xⱼ` adds `βⱼ` to the log-odds);
on the odds scale it's multiplicative (`e^(βⱼ)` is the odds ratio — `e^(βⱼ)=2`
doubles the odds per unit). A positive `βⱼ` pushes the probability up, and its
effect on the *probability itself* is largest near `p = ½` (the steep part of the
S) and slight out in the flat tails.

---

## 4. One idea, two models

Step back and the two are one skeleton with a single joint swapped:

| | linear predictor | link: how `η` becomes the outcome | fit criterion |
|---|---|---|---|
| **Linear** | `η = Xβ` | identity — predict `y ≈ η` | least squares (= max likelihood under normal errors) |
| **Logistic** | `η = Xβ` | logistic — `p = σ(η)` | maximum likelihood |

Both build a linear predictor; both estimate by making the observed data as
probable as possible. The only real difference is the **link** tying `η` to the
outcome — identity for a continuous `y`, logit for a binary one. Other links and
outcome types (counts, rates) give the broader family of **generalized linear
models**; these two are its most-used members, and Water implements exactly
this pair.

---

## 5. How sure are we? Inference by the bootstrap

An estimate `β̂` is one number from one sample. Draw a different sample and you'd
get a slightly different `β̂`: the estimate is itself **random**, and its spread
across hypothetical samples — its **sampling distribution** — is what a standard
error or confidence interval describes. The problem is seeing that spread when we
have only one sample.

### The idea

Let the sample stand in for the population. If the data is representative, then
**resampling our own data mimics drawing fresh samples from the world.** Concretely:

1. Draw a new dataset of the same size by sampling rows **with replacement** —
   some rows land twice, some not at all. That's a **bootstrap resample**.
2. Refit the model on it, getting a `β*`.
3. Repeat hundreds or thousands of times, collecting a whole distribution of `β*`.

That collection stands in for the sampling distribution of `β̂`, and the
uncertainty reads straight off it: the **standard error** is the standard
deviation of the `β*` values; a 95% **confidence interval** is their 2.5th-to-
97.5th percentile band; the **bias** is the gap between the average `β*` and the
original `β̂` (near zero means well-behaved). Water reports exactly these
three per coefficient.

The bootstrap needs no formula special to the model — the same resample-and-refit
recipe serves linear regression, logistic regression, a median, anything you can
compute — and it assumes nothing about the *shape* of the sampling distribution;
it shows you the shape the data actually produces.

### How Water does it

`regress-with` is the shared pipeline: build the design matrix (`dataset>matrix`
+ `with-intercept`) and the response, fit once on the full data for the point
estimate, then bootstrap. `resample-indices` draws a fresh set of row indices with
replacement; `select-rows` gathers those rows; the chosen fit (`fit-linear` for
`linear-regression`, the IRLS `fit-logistic` for `logistic-regression`) reruns on
each resample; and the per-coefficient standard error, percentile interval, and
bias are summarized from the collected `β*`.

The decisive structural fact: each resample's fit is **independent** of the others
— no shared state, no ordering — so the bootstrap is embarrassingly parallel. That
is why it's the natural workload for `pmap`: `bootstrap` runs the resamples
serially, `pbootstrap` spreads them across cores, and because a worker only reads
the shared data and produces its own `β*`, the parallel version computes the
identical distribution, just faster (see `multicore.md`).

### Why the bootstrap rather than classical formulas

Before cheap computing, inference came from closed-form formulas derived under
assumptions — for linear regression, a standard error built from `σ` and
`(XᵀX)⁻¹` with `t`-distribution intervals; for logistic, approximate errors from
the log-likelihood's curvature at the peak. When their assumptions hold they agree
closely with the bootstrap (a useful cross-check: both estimate the same sampling
distribution, one by theory, one by simulation). Water implements only the
bootstrap: it needs no per-model formula, makes no normality assumption, parallelizes
cleanly, and the cost it trades for that — refitting many times — is exactly what
`pmap` makes cheap.

---

## 6. Assumptions and caveats

No model is unconditionally true.

- **Linearity.** Both assume the predictors enter through a linear predictor —
  linear regression that `y` is linear in them, logistic that the *log-odds* are.
  Curved relationships need transformed predictors.
- **Independence.** The whole inferential story assumes observations don't lean on
  each other. Grouped or repeated-measures data (students within schools,
  measurements over time) break this and need methods that model the grouping —
  and they break the bootstrap's resample-rows step too, not just the formulas.
- **Linear regression** further assumes the error spread `σ` is roughly constant
  across predictor values; the classical interval also assumes normal errors,
  which the bootstrap drops.
- **Logistic regression** needs enough `0`s and `1`s across the predictor range;
  perfect or near-perfect separation destabilizes the plain fit (hence Firth), and
  very rare events make estimates fragile.
- **Correlation is not causation.** A coefficient is an association given the other
  predictors; it does not by itself license a causal claim.

The models are powerful because they are simple, and Water makes that
simplicity literal: one SVD-based least-squares solver does the linear fit, every
IRLS step of the logistic fit, *and* the leverages Firth needs — and one
resample-and-refit loop, parallel over `pmap`, supplies all the uncertainty.
Understanding the single idea — a linear predictor, fit by making the data as
probable as possible, with uncertainty read off the sampling distribution — is
what lets you use these well and recognize when they'll mislead you.
