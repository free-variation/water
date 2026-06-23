# Linear and Logistic Regression from First Principles

This document is a conceptual primer on the two workhorse models of applied
statistics: **linear regression** and **logistic regression**. It assumes you
have seen means, variance, the normal distribution, and the idea of a sampling
distribution — roughly a second-semester introductory course. It explains the
*mathematics*, not any particular implementation. By the end you should
understand:

- What a regression model is trying to do, and the *linear predictor* that both
  models share
- What "line of best fit" really means, and why we minimize *squared* error
- Why a straight line is the wrong tool for a yes/no outcome, and how the
  logistic curve fixes it
- What "maximum likelihood" means and why logistic regression has no formula —
  only an iterative procedure
- How a single idea (a linear predictor passed through a *link*) unifies both
- How we attach uncertainty — standard errors and confidence intervals — to an
  estimate, both classically and by *resampling* (the bootstrap)

Throughout, `n` is the number of observations and `k` the number of predictors.
A single observation is a row of predictor values `x = (x₁, …, x_k)` paired with
an outcome `y`.

---

## 1. The common backbone: the linear predictor

Both models start from the same object, the **linear predictor**:

```
η = β₀ + β₁x₁ + β₂x₂ + ⋯ + β_k x_k
```

It is just a weighted sum of the predictors plus a constant. The numbers
`β₀, β₁, …, β_k` are the **coefficients** we want to learn from data. `β₀` is the
**intercept** — the value of `η` when every predictor is zero. The other `βⱼ`
say how much `η` moves when predictor `xⱼ` increases by one unit, holding the
others fixed.

Collect the predictors of all `n` observations into a table `X` (one row per
observation, one column per predictor, plus a leading column of `1`s so that
`β₀` has something to multiply). Then *all* the linear predictors at once are the
matrix–vector product `Xβ`. Everything below is a story about two choices:

1. **How `η` connects to the outcome.** Linear regression uses `η` directly as
   the prediction. Logistic regression bends `η` through a curve to produce a
   probability.
2. **What makes a set of coefficients "good."** Linear regression minimizes
   squared error; logistic regression maximizes likelihood. These turn out to be
   the same principle wearing two coats.

---

## 2. Linear regression

### The model

We assume the outcome is the linear predictor plus random noise:

```
y = β₀ + β₁x₁ + ⋯ + β_k x_k + ε
```

`ε` is the **error** — everything about `y` not explained by the predictors. We
treat it as random, centered at zero (the model is right *on average*), with
some spread `σ`. The model says: if you knew the predictors, your best guess for
`y` is the linear predictor, and you'll be off by a typical amount `σ`.

### What "best fit" means

For any candidate coefficients `β`, observation `i` has a **fitted value**
`ŷᵢ = β₀ + β₁xᵢ₁ + ⋯` and a **residual**

```
eᵢ = yᵢ − ŷᵢ
```

the gap between what we saw and what the line predicts. A good fit makes the
residuals small. We measure "small" by the **sum of squared residuals**:

```
SSR(β) = e₁² + e₂² + ⋯ + e_n²  =  Σᵢ (yᵢ − ŷᵢ)²
```

The **least-squares** estimate `β̂` is the choice of coefficients that makes
`SSR` as small as possible. That is the entire definition of "line of best fit."

### Why squared error?

Three reasons:

- **It punishes big misses.** Doubling a residual quadruples its contribution, so
  the fit works hard to avoid large errors rather than spreading blame evenly.
- **It is smooth.** `SSR` is a gentle bowl-shaped function of `β` with a single
  lowest point and no kinks, so calculus finds the minimum cleanly. (Absolute
  error, `Σ|eᵢ|`, has corners and many tied solutions.)
- **It matches normal noise.** If the errors `ε` are normally distributed, then
  the least-squares `β̂` is exactly the *most likely* set of coefficients — the
  maximum-likelihood estimate. Squared error isn't an arbitrary choice; it's what
  "the errors are bell-shaped" implies. (Keep this in mind: logistic regression
  keeps the *likelihood* idea and drops the *squared-error* shortcut.)

### Solving it: the geometry of projection

Minimizing a smooth bowl means finding where its slope is zero. Setting the
derivative of `SSR` to zero (one equation per coefficient) gives the **normal
equations**, whose solution is

```
β̂ = (XᵀX)⁻¹ Xᵀy
```

You do not need to compute this by hand to understand it. The geometric picture
is the point:

> Think of `y` as a single point in `n`-dimensional space. The fitted values
> `Xβ` — every combination of the predictor columns — sweep out a flat subspace
> (a plane through the origin). Least squares finds the point **in that plane
> closest to `y`**. "Closest" in the squared-distance sense means dropping a
> perpendicular: the residual vector is at a right angle to the plane.

So linear regression is **projection**: `ŷ` is the shadow of `y` on the space the
predictors can reach, and the residual is the part of `y` no combination of
predictors can explain. The intercept is handled by including that column of
`1`s, so the "predictors" include a constant direction.

### Reading the coefficients

`βⱼ` is the expected change in `y` for a one-unit increase in `xⱼ`, holding the
other predictors fixed. The intercept `β₀` is the predicted `y` when all
predictors are zero (often only a mathematical anchor, not a meaningful case).
The sign tells you direction; the magnitude depends on the units of `xⱼ`.

---

## 3. Logistic regression

### Why the straight line breaks

Now the outcome is **binary**: `y` is `1` ("yes," "success," "survived") or `0`.
Could we just fit a line? No, for two fatal reasons:

- A line is unbounded. It will happily predict `1.3` or `−0.4`, which cannot be
  probabilities.
- The noise is not bell-shaped. A `0/1` outcome has none of the symmetric spread
  least squares assumes; its variance even depends on the probability itself.

So we change *what we model*. We do not model the `0/1` value directly. We model
the **probability** that it is `1`.

### Modeling a probability: the logistic curve

Let `p = P(y = 1)`. We want `p` to depend on the linear predictor `η` but stay
trapped in `(0, 1)`. The **logistic** (or **sigmoid**) function does exactly
that:

```
p = σ(η) = 1 / (1 + e^(−η))
```

It is an S-shaped curve: as `η → −∞`, `p → 0`; at `η = 0`, `p = ½`; as
`η → +∞`, `p → 1`. It squashes the whole real line into the open interval
`(0,1)`. So logistic regression is *linear regression's linear predictor sent
through a squashing curve.*

### The log-odds: where it's linear again

Invert the curve and something clean appears. The **odds** of the event are
`p / (1 − p)`, and their logarithm — the **log-odds** or **logit** — is just the
linear predictor:

```
log( p / (1 − p) ) = η = β₀ + β₁x₁ + ⋯
```

This is the heart of the model: **the log-odds are linear in the predictors.**
The curve lives on the probability scale; the straight line lives on the
log-odds scale; the logit is the bridge between them. This is why a coefficient
`βⱼ` is read as "a one-unit increase in `xⱼ` adds `βⱼ` to the log-odds," and why
`e^(βⱼ)` is the **odds ratio** — the multiplicative factor by which the odds
change.

### What makes coefficients "good": likelihood

There are no residuals to square here. Instead we ask: *which coefficients make
the data we actually observed most probable?* That is **maximum likelihood**.

For each observation the model assigns a probability to what happened: `pᵢ` if
`yᵢ = 1`, and `1 − pᵢ` if `yᵢ = 0`. Assuming observations are independent, the
probability of the *entire* dataset is the product of these, the **likelihood**:

```
L(β) = Πᵢ  pᵢ^(yᵢ) · (1 − pᵢ)^(1 − yᵢ)
```

We pick `β` to maximize it. In practice we maximize the **log-likelihood**
`Σᵢ [ yᵢ log pᵢ + (1 − yᵢ) log(1 − pᵢ) ]` instead — the log turns the product into
a sum and never moves the location of the maximum. (Recall from §2 that with
normal errors, least squares *is* maximum likelihood. Same principle: make the
observed data as probable as possible. Linear regression just happens to have a
shortcut formula; logistic regression does not.)

### Why there's no formula: iterative reweighted least squares

The log-likelihood is a smooth hill with a single peak, but setting its slope to
zero gives equations tangled up inside the sigmoid — no clean `β̂ = …`. So we
climb the hill step by step.

The key fact is that **each step is itself a weighted linear regression.**
Starting from a guess for `β`, you:

1. compute the current probabilities `pᵢ = σ(ηᵢ)`;
2. give each observation a **weight** `wᵢ = pᵢ(1 − pᵢ)`. This is largest at
   `p = ½` (an observation near the decision boundary is the most informative)
   and shrinks toward `0` for confident predictions;
3. form an adjusted target and run an ordinary **weighted** least-squares fit to
   get an improved `β`;
4. repeat until `β` stops changing.

This is **iteratively reweighted least squares (IRLS)** — really Newton's method
for finding the top of the hill, and every step reduces to the linear-regression
machinery from §2. A handful of iterations usually suffices.

### Perfect separation: when the peak runs off to infinity

Suppose some predictor splits the classes perfectly — every `y = 1` above a
threshold, every `y = 0` below. Then the model can always do *a little better* by
making the curve steeper, pushing a coefficient toward `±∞`. The likelihood has
no finite peak; the climb never settles. This is **separation**, and it is common
in small or tidy datasets.

The standard cure is a gentle **penalty** (the *Firth* correction): add a small,
principled term to the log-likelihood that nudges coefficients back toward zero
just enough to guarantee a finite peak, while barely affecting well-behaved
data. Conceptually it is a built-in skepticism that refuses to believe in
infinite certainty from finite data.

### Reading the coefficients

On the log-odds scale, `βⱼ` is additive: a one-unit increase in `xⱼ` changes the
log-odds by `βⱼ`. On the more intuitive odds scale, it is multiplicative:
`e^(βⱼ)` is the **odds ratio**. An `e^(βⱼ) = 2` means the odds of `y = 1` double
per unit of `xⱼ`. A positive `βⱼ` pushes the probability up, a negative one down,
and the *effect on the probability itself* is largest near `p = ½` (the steep
part of the S) and small out in the flat tails.

---

## 4. One idea, two models: generalized linear models

Step back and the two models are the same skeleton with one joint swapped:

| | linear predictor | how `η` maps to the outcome | "good fit" means |
|---|---|---|---|
| **Linear** | `η = Xβ` | identity: predict `y ≈ η` | least squares (= max likelihood, normal errors) |
| **Logistic** | `η = Xβ` | logistic: `p = σ(η)` | maximum likelihood |

Both build a linear predictor; both estimate by maximizing the probability of the
data. The only real difference is the **link** — the function tying the linear
predictor to the thing being predicted (identity for a continuous outcome, logit
for a binary one). Choosing other links and outcome types (counts, rates, …)
gives the broader family of **generalized linear models**; linear and logistic
regression are its two most-used members.

---

## 5. How sure are we? Inference by resampling

An estimate `β̂` is a single number computed from one sample. Drawn a different
sample from the same population, you'd get a slightly different `β̂`. The estimate
is itself **random**, and its spread across hypothetical samples — its **sampling
distribution** — is what standard errors and confidence intervals describe. The
question is how to see that spread when we only have *one* sample.

### The bootstrap idea

The move is to let the sample stand in for the population. If our data is
representative, then **resampling from our own data mimics drawing fresh samples
from the world.** Concretely:

1. Draw a new dataset of the same size by sampling rows **with replacement** —
   some rows appear twice, some not at all. This is a **bootstrap resample**.
2. Refit the model on it to get a `β*`.
3. Repeat many times (hundreds or thousands), collecting a whole *distribution*
   of `β*` values.

That collection is a stand-in for the sampling distribution of `β̂`, and we read
the uncertainty straight off it:

- **Standard error**: the standard deviation of the `β*` values — how much the
  estimate jumps around from resample to resample.
- **Confidence interval**: a central band of the `β*` distribution. A 95%
  interval is the range from its 2.5th to its 97.5th percentile.
- **Bias**: the gap between the average of the `β*` values and the original `β̂`;
  near zero means the estimator is well-behaved on this data.

The bootstrap needs no formula special to the model — the same resample-and-refit
recipe works for linear regression, logistic regression, a median, or anything
else you can compute. It also makes no assumption that the sampling distribution
is bell-shaped; it shows you the shape it actually has.

### The classical alternative

Before cheap computing, inference came from *formulas* derived under
assumptions. For linear regression with normal errors, the standard error of each
coefficient has a closed form (built from `σ` and `(XᵀX)⁻¹`), and intervals come
from the `t`-distribution. For logistic regression, the curvature of the
log-likelihood at its peak gives approximate standard errors. These formulas are
fast and, when their assumptions hold, agree closely with the bootstrap — a
useful cross-check, not a coincidence: both estimate the same sampling
distribution, one by theory and one by simulation.

---

## 6. Assumptions and caveats

No model is unconditionally true; each rests on assumptions worth stating.

- **Linearity.** Both models assume the predictors enter through a *linear*
  predictor. Linear regression assumes `y` is linear in the predictors; logistic
  assumes the *log-odds* are. Curved relationships need transformed predictors.
- **Independence.** The standard errors above assume observations don't lean on
  each other. Grouped or repeated-measures data (students within schools,
  measurements over time) violate this and need methods that model the grouping.
- **Linear regression also assumes** the error spread `σ` is roughly constant
  across predictor values, and — for the classical intervals — that the errors
  are approximately normal. The bootstrap relaxes the normality assumption.
- **Logistic regression** needs enough `0`s and `1`s spread across the predictor
  range; perfect or near-perfect separation destabilizes the plain fit (hence the
  penalty), and very rare events make estimates fragile.
- **Correlation is not causation.** A coefficient describes association in the
  data given the other predictors; it does not by itself license a causal claim.

The models are powerful precisely because they are simple. Understanding the
single idea behind them — a linear predictor, fit by making the data as probable
as possible, with uncertainty read off the sampling distribution — is what lets
you use them well and recognize when they will mislead you.
