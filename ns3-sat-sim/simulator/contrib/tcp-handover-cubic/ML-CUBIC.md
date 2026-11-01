# ML-CUBIC integration

The `tcp-handover-cubic` module vendors the ns-3 implementation from
`ml-cubic-random-loss` so Hypatia can build and select `ns3::TcpMlCubic`
without depending on another checkout. The same module contains the
Hypatia-local `ns3::TcpHarpCubicV7`, which adds that sender-only v2-A inference
to HARP-Cubic 6.0 without replacing its handover policy.

- Upstream: https://github.com/aa22aa2244/ml-cubic-random-loss
- Upstream commit: `da86c58` (`Package ML-CUBIC v2-A random-loss mechanism`)
- Upstream package version: `0.1.0`
- Imported path: `ns3-module/hybrid-cubic`
- Target simulator: ns-3.45

The embedded model weights are unchanged. The module requires the small
`TcpCubic::GetLossReductionFactor()` extension hook maintained by the
surrounding Hypatia integration. Hypatia carries one local correctness fix in
`TcpMlCubic`'s copy constructor so `GrowthTraceEnabled` remains unchanged when
ns-3 forks the congestion-control object for a socket.

When updating this copy, replace the four imported model files
(`tcp-hybrid-cubic.{cc,h}` and both weight headers) together and reapply the
copy-constructor fix, then rerun the inference-parity check before accepting
new weights or controller behavior.

## Use in Hypatia

Select the controller and endpoint GSL loss source in a run's
`config_ns3.properties`:

```properties
tcp_socket_type=TcpMlCubic

enable_gsl_random_loss=true
gsl_random_loss_rate=0.005
gsl_random_loss_direction=data
gsl_random_loss_source_node_id=1584
gsl_random_loss_target_node_id=1585
gsl_random_loss_stream=1
```

`gsl_random_loss_direction` accepts `data`, `ack`, or `both`. DATA loss is
installed on the target ground station's receiving GSL interfaces; ACK loss is
installed on the source ground station's receiving GSL interfaces. This keeps
the configured probability independent of the number of ISL hops and preserves
one deterministic RNG sequence across GSL handovers. It affects every packet
received at the selected endpoint, so use distinct endpoints when experiments
contain unrelated concurrent flows. When loss is disabled, omit the other
`gsl_random_loss_*` keys because BasicSimulation rejects unused configuration
keys.

For a shadow-mode v2-A run (inference enabled, standard CUBIC response retained):

```bash
./ns3 run "main_satnet --run_dir=/path/to/run \
  --ns3::TcpMlCubic::UseV2Model=true \
  --ns3::TcpMlCubic::AssistEnabled=false \
  --ns3::TcpMlCubic::MonitorInterval=100ms \
  --ns3::TcpMlCubic::WarmupIntervals=5 \
  --ns3::TcpMlCubic::RandomThreshold=0.00025 \
  --ns3::TcpMlCubic::RandomBeta=0.95 \
  --ns3::TcpMlCubic::GrowthTraceEnabled=false"
```

Set `AssistEnabled=true` only when the active ML response is desired. The
deployed controller has no Python or PyTorch runtime dependency.

## HARP-Cubic 7.0

`TcpHarpCubicV7` derives from `TcpHarpCubicV6`. The complete V6 mechanism
remains authoritative: packet-carried DATA/ACK signals, the adaptive ACK-gap
gate, bounded safe target, fixed active-episode deadline, late-signal revision,
and PRR behavior are unchanged. The v2-A model is consulted only after V6 has
classified a Recovery as normal loss.

The decision order is:

```text
qualified HARP signal / active episode / late signal -> V6 safe target (beta 0.85)
V6 normal loss, model warm and p <= 0.00025          -> ML random beta 0.95
V6 normal loss, otherwise                            -> V6 normal beta 0.70
```

Consequently, ML can never override a qualified handover. Before warm-up, or
with `AssistEnabled=false`, V7 takes the same congestion response as V6. The
model uses sender-observable TCP telemetry only; it does not read the
simulator's configured packet-drop cause. `RandomBeta` is also floored at the
inherited V6 normal beta, so a non-default value cannot make recovery more
aggressive than V6.

Select V7 in `config_ns3.properties`:

```properties
tcp_socket_type=TcpHarpCubicV7
```

V7 uses the same packet-marking path and default `TcpPrrRecovery` as V6. A
representative active configuration is:

```bash
./ns3 run "main_satnet --run_dir=RUN_DIR \
  --ns3::TcpL4Protocol::RecoveryType=ns3::TcpPrrRecovery \
  --ns3::TcpHarpCubicV7::MonitorInterval=100ms \
  --ns3::TcpHarpCubicV7::WarmupIntervals=5 \
  --ns3::TcpHarpCubicV7::AssistEnabled=true \
  --ns3::TcpHarpCubicV7::RandomThreshold=0.00025 \
  --ns3::TcpHarpCubicV7::RandomBeta=0.95 \
  --ns3::TcpHarpCubicV7::V2BandwidthWindow=5s \
  --ns3::TcpHarpCubicV7::V2BandwidthQuantile=0.90 \
  --ns3::ArbiterSingleForward::EnableHarpV5PacketMarking=true \
  --ns3::ArbiterSingleForward::HarpV5MarkDuration=250ms \
  --ns3::TcpSocketBase::HarpV5EchoDuration=250ms"
```

V7-specific attributes and defaults:

| Attribute | Default | Meaning |
| --- | ---: | --- |
| `MonitorInterval` | `100ms` | Sender feature-sampling interval |
| `WarmupIntervals` | `5` | Valid delivery-rate intervals required before ML control |
| `AssistEnabled` | `true` | Allows ML to alter only the V6 normal-loss beta |
| `RandomThreshold` | `0.00025` | Maximum congestion probability classified as random loss |
| `RandomBeta` | `0.95` | Beta used for a high-confidence random loss |
| `V2BandwidthWindow` | `5s` | Delivery-rate history for the v2-A estimator |
| `V2BandwidthQuantile` | `0.90` | Delivery-rate quantile used as bandwidth estimate |

When overriding inherited policy attributes on the ns-3 command line, use
their defining V6 TypeId (for example,
`--ns3::TcpHarpCubicV6::NormalBeta=0.70`), while the seven attributes above
use the V7 TypeId.

The `CongestionProbability`, `SelectedNormalBeta`, and `RandomAssist` trace
sources expose the ML decision. To see both the inherited V6 classification
and the V7 ML result, enable both log components:

```bash
NS_LOG='TcpHarpCubicV6=level_info|prefix_time:TcpHarpCubicV7=level_info|prefix_time' \
  ./ns3 run "main_satnet --run_dir=RUN_DIR"
```

## Verify

From `ns3-sat-sim/simulator`:

```bash
./ns3 build ml-cubic-inference-check main_satnet
./ns3 run "ml-cubic-inference-check --v2=1"
./ns3 run "test-runner --suite=tcp-harp-cubic-v7 --verbose"
```

The v2-A parity output begins with `0,0.000570779` and ends with
`4,0.999870539`.
