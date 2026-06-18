# Reinforcement Learning

The heart of the current project: a self-play system that trains a combat agent to win
1v1 PvP. C++ owns data collection and the replay buffer; Python/JAX owns the neural
network, loss, and optimizer. Code lives in `bot/src/rl/` (C++ + `python/dqn.py`) and
`bot/src/state/machine/{training,intelligenceActor}.*`.

## Algorithm at a glance

- **Double DQN (DDQN)** - online net selects the next action, target net evaluates it.
- **Prioritized Experience Replay (PER)** - sum-tree buffer, priority = `|TD error|`,
  alpha = 0.5, importance-sampling beta annealed 0.4 -> 1.0.
- **n-step returns** - `kTdLookahead = 4`.
- **Polyak (soft) target updates** - tau = 0.0004 every 16 steps.
- **epsilon-greedy** exploration annealed 1.0 -> 0.01 over 50k steps.
- **Framework:** JAX + Flax **NNX** + Optax (AdamW + global-norm clip), Orbax checkpoints.

All hyperparameters are `static constexpr` in `rl/trainingManager.hpp` (lines ~79-101):
batch 256, replay capacity 100k, min-before-training 40k, gamma 0.997, lr 1e-6, dropout
0.05, 4 PvP pairs, checkpoint every 10k steps.

## Self-play structure

`TrainingManager` doesn't train one agent against the environment - it pits bot characters
against each other. It defines character pairings and PvP positions, creates a `Session`
per character (clientless), logs them in, warps them together, and starts PvP. Both
characters run the policy and **both generate training transitions**, so every match
produces experience from two perspectives. Reward is symmetric (my HP up / your HP down).

## Observation space (what the agent sees)

Defined in `rl/observation.hpp` and **sized at compile time** from two config headers:

- `rl/skills.hpp` -> `kSkillIdsForObservations` (currently **empty**)
- `rl/items.hpp` -> `kItemIdsForObservations` (currently **one**: ref id 5, an HP potion)

An `Observation` concatenates, as f32s:

| Block | Source | Floats each | Count now | Floats |
|---|---|---|---|---|
| per-skill availability | `SkillModelData` | 1 | 0 | 0 |
| per-item [available?, normalized count] | `ItemModelData` | 2 | 1 | 2 |
| our HP (normalized 0..1) | `VitalModelData` | 1 | 1 | 1 |
| opponent HP (normalized 0..1) | `VitalModelData` | 1 | 1 | 1 |

So **the observation is currently 4 floats.** (Our-MP is present but commented out.)
`model_data/` provides the encoders: `NormalizedModelData<T>` (clamp `(v)/max` to 0..1),
`BooleanModelData`, `ItemModelData`, `SkillModelData`, `VitalModelData`. To grow the
observation, add ref ids to those two headers - everything downstream resizes
automatically.

### Temporal context (observation stacking)

The agent isn't fed a single observation; it gets the **last 8** (`kPastObservationStackSize`)
observations plus the current one, so it can perceive change over time. The model input
(`modelInputs.*`, serialized to numpy in `jaxInterface.cpp`) is five arrays:

| Array | Shape (single) | Meaning |
|---|---|---|
| `pastObservationStack` | (8, 4) | the last 8 observations (older->newer) |
| `pastObservationTimestamps` | (8, 1) | ms since each, normalized |
| `pastActions` | (8, A) | one-hot of the action taken at each past step |
| `pastMask` | (8, 1) | 1 = valid, 0 = padding (early in an episode) |
| `currentObservation` | (4,) | the current observation |

Training batches prepend a batch dim of 256.

## Action space (what the agent can do)

`rl/actionSpace.cpp` builds the discrete action set as:
`[Sleep, CommonAttack] + skills(from skills.hpp) + items(from items.hpp)`.

With the current config that is **3 actions**: index 0 `Sleep` (500 ms no-op), index 1
`CommonAttack` (basic attack on the opponent), index 2 `UseItem` (the HP potion). Adding
skill/item ids grows the action space (and `pastActions` width) automatically.

Each action is itself a small `StateMachine` (`rl/action.*`): `Sleep`, `CommonAttack`,
`TargetedSkill`, `TargetlessSkill`, `UseItem`, `CancelAction`. The `intelligenceActor`
state machine asks the active "intelligence" for an action index, instantiates that
action, and runs it.

### Action masking

When the bot can't send a packet right now (mid-action), all actions except `Sleep` get a
`-inf` mask added to their Q-values before argmax, so only `Sleep` is selectable.

## Reward

`TrainingManager::calculateReward()` (per transition):

```
reward  = (ourHp - lastOurHp) / ourMaxHp          # losing HP is negative
        + (lastOppHp - oppHp) / oppMaxHp          # dealing damage is positive
if terminal: += +2 if opponent died, -2 if we died
```

Rewards are computed at sampling time (not stored), then accumulated over the 4-step
lookahead with gamma discounting, stopping early at a terminal step. Keeping reward
computation external makes it easy to reshape without touching the buffer.

## The intelligences (`rl/ai/`)

- `BaseIntelligence` - interface for "given an observation, pick an action".
- `RandomIntelligence` - uniform random (respecting the can-send-packet constraint).
- `DeepLearningIntelligence` - epsilon-greedy: with probability epsilon it defers to the
  random intelligence; otherwise it builds the stacked model input from its own deque of
  past observations/actions and calls `JaxInterface::selectAction`. It maintains the
  8-deep past stack itself.

## C++ / JAX boundary (`rl/jaxInterface.*`)

`JaxInterface` wraps every Python call through pybind11, holding the GIL only as needed
and coordinating with training via a mutex/condition variable (action selection pauses
training briefly). On `initialize()` it imports `rl.python.dqn`, constructs the
`DqnModel` (observationSize, stackSize, actionSpaceSize, dropout), deep-copies it as the
target net, and builds the AdamW optimizer.

- `selectAction(...)` -> converts the `ModelInputView` to numpy, builds the action mask,
  calls `dqn.selectAction`, returns `(actionIndex, qValues)`.
- `train(...)` -> converts a batch of past/current inputs + actions/terminals/rewards +
  IS weights to numpy and calls `dqn.jittedTrain`, returning global-norm, per-sample TD
  errors (fed back as new PER priorities), and Q-value stats.
- `updateTargetModelPolyak(tau)` / checkpoint save/load.

## The network (`python/dqn.py`)

`DqnModel` (Flax NNX):

1. **Per-timestep feature extractor** over the past stack. Each of the 8 timesteps is the
   concat of `[observation(4), timestamp(1), action_one_hot(A)]` -> Linear 128 -> 64 ->
   32, ReLU + dropout between.
2. **Flatten** the 8x32 extracted features, concat the 8 mask values and the current
   observation -> Linear 1024 -> 256 -> `actionSpaceSize` (the Q-values).

`selectAction` adds the action mask and returns `argmax`. `jittedTrain` (vmapped over the
batch) computes the DDQN target (`reward + gamma * Q_target(s', argmax_a Q_online(s',a))`,
or just `reward` if terminal), **Huber** loss weighted by IS weights, and TD errors.
`getOptaxAdamWOptimizer` = `clip_by_global_norm(1.0)` then AdamW (wd 1e-2, no decay on
biases). Checkpointing uses Orbax (`checkpointModel`, `loadModelCheckpoint`, optimizer
too).

## The training loop (`TrainingManager::run` / `train`)

1. Block until the replay buffer holds >= 40k transitions.
2. Sample 256 transitions by PER priority (with current beta).
3. Build stacked model inputs for each sample's past and current states; compute n-step
   discounted rewards.
4. Call `jaxInterface_.train(...)`; get TD errors + stats.
5. Write `|TD error|` back as new PER priorities.
6. Every 16 steps Polyak-update the target net (tau 0.0004). (Hard copy every 10k is the
   alternative path if Polyak is disabled.)
7. Every 10k steps save a checkpoint.
8. Periodically push metrics (reward, queue size, sample/train rates, Q-values) to the
   UI via `RlUserInterface`.

Meanwhile, on the gameplay side, characters keep fighting and calling
`reportObservationAndAction()`, which stores into `ObservationAndActionStorage` and adds a
transition to the replay buffer (both guarded by `replayBufferAndStorageMutex_`).

## Status / roadmap

Recent commits ("Finally successful training, but then forgetting") show this is active
research, not a finished product - tuning PER beta / epsilon annealing and target-update
cadence against catastrophic forgetting. `rl/docs/distributional_rl_plan.md` sketches a
planned migration to **C51 distributional RL** (categorical output over value atoms,
cross-entropy loss, projected Bellman target).

## Where to change things

| Want to... | Edit |
|---|---|
| add observation features | `rl/items.hpp`, `rl/skills.hpp` (+ a new `*ModelData` if a new kind) |
| add an action | `rl/actionSpace.cpp` + a new `Action` in `rl/action.*` |
| reshape reward | `TrainingManager::calculateReward` |
| tune hyperparameters | `rl/trainingManager.hpp` constants |
| change the network | `python/dqn.py` `DqnModel` |
| change PvP pairings | `TrainingManager::defineCharacterPairingsAndPositions` |
