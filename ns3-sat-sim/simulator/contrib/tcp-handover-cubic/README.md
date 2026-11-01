# TCP Handover CUBIC

The Hypatia tree contains eight ns-3 congestion-control variants for evaluating
handover-aware CUBIC in the satellite-network simulator. Versions 1.0 through
7.0, the imported sender-only ML-CUBIC inference code, and its embedded model
weights all live in this module. See `ML-CUBIC.md` for upstream provenance and
standalone `TcpMlCubic` usage:

- `TcpHandoverCubic` (HARP-Cubic 1.0) completely preserves cwnd after a
  detected handover-reordering event.
- `TcpHarpCubicV2` (HARP-Cubic 2.0) uses three direction-aware beta levels and
  keeps normal PRR recovery.
- `TcpHarpCubicV21` (HARP-Cubic 2.1) is the conservative sensitivity variant;
  it keeps V2 unchanged except that DATA-path beta is `0.80` instead of `0.85`.
- `TcpHarpCubicV3` (HARP-Cubic 3.0) keeps V2's detector and beta values, then
  caps the handover target using the flight size observed at recovery entry.
- `TcpHarpCubicV4` (HARP-Cubic 4.0) keeps V3's recovery behavior unchanged,
  but timestamps a handover when an active TCP packet first uses a new first
  hop instead of giving TCP the forwarding-state update time directly.
- `TcpHarpCubicV5` (HARP-Cubic 5.0) carries the handover signal in TCP packets
  and echoes it back to the sender instead of directly notifying TCP locally.
- `TcpHarpCubicV6` (HARP-Cubic 6.0) keeps V5's packet-carried signal and safe
  target, replacing the fixed ratio gate with a conservative adaptive ACK-gap
  gate shared by DATA and ACK signals.
- `TcpHarpCubicV7` (HARP-Cubic 7.0) preserves the complete V6 handover
  mechanism and consults sender-only ML only on V6's normal-loss path.

## Behavior

The module observes the cumulative ACK and the furthest SACK edge (FACK).
When recovery occurs, it computes:

```text
ack_range_segments = (FACK - ACK) / segment_size
ratio = ack_range_segments / cwnd_segments
```

A recovery is a handover-reordering candidate when:

```text
time_since_handover <= DetectionWindow
ratio >= AckRangeRatioThreshold
```

A candidate starts `ProtectionWindow`. With `TcpHarpRecovery` selected, normal
TCP loss bookkeeping and fast retransmit still run, but both CUBIC's
multiplicative decrease and PRR's temporary actual-cwnd decrease are bypassed
during protection. Outside protection, `TcpHarpRecovery` follows ns-3 PRR.

Default experiment parameters:

| Attribute | Default | Meaning |
| --- | ---: | --- |
| `EnableHandoverProtection` | `true` | Enables cwnd preservation |
| `DetectionWindow` | `250ms` | Candidate-detection interval after handover |
| `ProtectionWindow` | `250ms` | Protection interval after detection |
| `AckRangeRatioThreshold` | `0.015` | ACK-range/cwnd threshold |

These values are experiment parameters, not universal LEO thresholds.

## HARP-Cubic 2.0

V2 keeps HARP-Cubic 1.0 unchanged as an experimental baseline.  A recovery
must occur no later than 250ms after a handover and its ACK-range ratio must
reach 0.015.  The path direction then selects one of three beta levels:

```text
normal loss             -> beta = 0.70
ACK-path handover       -> beta = 0.75
DATA-path handover      -> beta = 0.85
```

The 250ms interval starts at the actual route handover, not at the later TCP
detection.  A handover epoch creates only one target cwnd, so a follow-up
recovery caused by the same event does not multiply beta a second time.

V2 attributes and defaults:

| Attribute | Default | Meaning |
| --- | ---: | --- |
| `EnableAdaptiveBeta` | `true` | Enables adaptive beta |
| `HandoverWindow` | `250ms` | Detection/protection time from handover |
| `AckRangeRatioThreshold` | `0.015` | Ratio where adaptation starts |
| `NormalBeta` | `0.70` | Beta used for normal loss |
| `AckHandoverBeta` | `0.75` | Beta used for ACK-path handover |
| `DataHandoverBeta` | `0.85` | Beta used for DATA-path handover |

## HARP-Cubic 2.1

V2.1 is intentionally isolated from V2 so both versions can be compared in
the same experiment.  Detection, the 250ms window, normal beta, ACK beta, and
PRR are identical to V2.  Only the DATA-path beta changes:

```text
normal loss             -> beta = 0.70
ACK-path handover       -> beta = 0.75
DATA-path handover      -> beta = 0.80
```

## HARP-Cubic 3.0

V3 retains V2's `0.70 / 0.75 / 0.85` beta policy.  Only the first detected
handover Recovery target changes:

```text
safe_target = clamp(bytes_in_flight_at_entry,
                    normal_cubic_target,
                    adaptive_handover_target)
```

If entry flight lies between the two targets, V3 holds that entry-flight value.
If it is below the normal target, V3 uses the normal target; if it is above the
adaptive target, V3 uses the adaptive target.  Follow-up Recovery behavior is
identical to V2, and V3 does not add an arbitrary post-recovery timer.

## HARP-Cubic 5.0

V5 removes the direct routing-to-TCP notification. A changed first hop marks
outgoing TCP packets; DATA marks are echoed by the receiver, while ACK marks
are observed directly by the sender. The sender still uses the fixed 0.015
ACK-range/cwnd ratio and V3's bounded safe target. Repeated marks from the
same epoch do not extend the 250ms signal window.

## HARP-Cubic 6.0

V6 is the conservative adaptive detector. A Recovery is protected only when
both conditions hold:

```text
fresh packet-carried HARP signal
ack_range_segments >= dynamic_gap_threshold
```

Otherwise it uses normal CUBIC beta 0.70. There is no small-gap beta 0.75
tier. DATA and ACK signals use the same beta 0.85 after passing the same gate.
Every follow-up Recovery must pass the gap gate again; the 250ms interval is a
signal-validity window, not unconditional protection. DATA and ACK direction
is retained for logging only: a direction change inside an active episode
shares the saved safe target and cannot extend that episode's fixed deadline.

The per-flow threshold starts at seven segments. Normal-Recovery samples are
held for 250ms before confirmation so a delayed HARP signal can remove them.
After at least three confirmed samples, V6 retains the latest nine and uses:

```text
max(7, ceil(median + max(3, 3 * MAD)))
```

The median and median absolute deviation make one extreme normal-loss outlier
unlikely to move the threshold. This remains a conservative heuristic: a real
loss that coincides with a fresh signal and also has a large gap can still be
misclassified.

When V5 and V6 are compared directly, note that the experiment changes both
the detector (fixed ratio to adaptive gap) and ACK-path response (0.75 to the
shared 0.85). The comparison therefore measures the complete V6 policy, not an
isolated threshold-only change.

## HARP-Cubic 7.0

V7 derives from V6 and keeps its packet-carried signal, adaptive ACK-gap gate,
safe-target clamp, fixed active-episode deadline, late-signal revision, and
PRR behavior. A qualified HARP Recovery always remains owned by V6 and uses
the V6 safe target with handover beta 0.85. Only a Recovery that reaches V6's
normal-loss path is eligible for the embedded sender-only v2-A classifier:

```text
qualified HARP / active episode / late signal -> V6 safe target, beta 0.85
normal loss and p <= 0.00025                  -> random-loss beta 0.95
normal loss otherwise                         -> V6 normal beta 0.70
```

The model warms for five valid 100ms sampling intervals by default. Until it
is ready, V7 follows V6 exactly. `AssistEnabled=false` provides shadow mode:
inference and traces remain available, while all congestion responses remain
the V6 responses. The classifier uses only sender-observable TCP state and
does not consume simulator ground-truth loss labels.

## Select the algorithm

Build from the Hypatia repository root with `bash hypatia_build.sh`, or run
`bash build.sh --debug_all` inside `ns3-sat-sim/`. The build script applies
the following integration patches automatically and only once:

- `ns-3.45-tcp-cubic-loss-hook.patch`
- `ns-3.45-harp-core.patch`
- `basic-sim-per-flow-tcp.patch`

The module-local `patches/ns3-ack-observer-hook.patch` is a limited porting
aid; it is not the complete V5/V6/V7 TCP core integration and should not be
applied separately during a normal repository build.

Set this in the run's `config_ns3.properties`:

```text
tcp_socket_type=TcpHandoverCubic
```

Also select the matching recovery type on the ns-3 command line:

```text
--ns3::TcpL4Protocol::RecoveryType=ns3::TcpHarpRecovery
```

For HARP-Cubic 2.0, use:

```text
tcp_socket_type=TcpHarpCubicV2
```

V2 uses ns-3's default `TcpPrrRecovery`; do not select `TcpHarpRecovery`,
because complete cwnd preservation would override the adaptive-beta behavior.

Its parameters can be overridden without rebuilding, for example:

```bash
./ns3 run "main_satnet --run_dir=RUN_DIR --ns3::TcpHarpCubicV2::DataHandoverBeta=0.85"
```

For HARP-Cubic 2.1, use:

```text
tcp_socket_type=TcpHarpCubicV21
```

V2.1 also uses ns-3's default `TcpPrrRecovery`.

For HARP-Cubic 3.0, use:

```text
tcp_socket_type=TcpHarpCubicV3
```

V3 also uses ns-3's default `TcpPrrRecovery`.

For HARP-Cubic 4.0, use:

```text
tcp_socket_type=TcpHarpCubicV4
```

V4 retains V3's threshold, beta values, 250ms window, safe target and PRR.
Its packet-path notifications also merge DATA and ACK route changes observed
within 1ms into one `DATA+ACK` event.  The merged event uses the later observed
time and DATA beta, so a slightly later pure ACK cannot overwrite the DATA-path
handover that affects payload delivery.

For HARP-Cubic 5.0 or 6.0, use respectively:

```text
tcp_socket_type=TcpHarpCubicV5
tcp_socket_type=TcpHarpCubicV6
```

Both use `TcpPrrRecovery` and the packet-marking attributes already provided
by `ArbiterSingleForward`. V6's main parameters are
`GapThresholdFloor=7`, `GapMargin=3`, `GapMadMultiplier=3`,
`GapBaselineWindow=9`, and `GapBaselineMinSamples=3`.

For HARP-Cubic 7.0, use:

```text
tcp_socket_type=TcpHarpCubicV7
```

V7 also uses `TcpPrrRecovery` and the V5/V6 packet-marking attributes. Its ML
defaults are `MonitorInterval=100ms`, `WarmupIntervals=5`,
`AssistEnabled=true`, `RandomThreshold=0.00025`, `RandomBeta=0.95`,
`V2BandwidthWindow=5s`, and `V2BandwidthQuantile=0.90`. On the ns-3 command
line, inherited attributes must use the TypeId that defines them, so V6 policy
overrides retain the `TcpHarpCubicV6` prefix, for example:

```bash
./ns3 run "main_satnet --run_dir=RUN_DIR \
  --ns3::TcpHarpCubicV6::NormalBeta=0.70 \
  --ns3::TcpHarpCubicV6::HandoverBeta=0.85 \
  --ns3::TcpHarpCubicV7::AssistEnabled=true"
```

Enable both components when inspecting V7: `TcpHarpCubicV6` records inherited
handover classifications and `TcpHarpCubicV7` records `HarpV7MlDecision`.

## A/B comparison without rebuilding

Protection enabled:

```bash
./ns3 run "main_satnet --run_dir=RUN_DIR --ns3::TcpHandoverCubic::EnableHandoverProtection=true --ns3::TcpL4Protocol::RecoveryType=ns3::TcpHarpRecovery"
```

Baseline behavior using the same class and instrumentation:

```bash
./ns3 run "main_satnet --run_dir=RUN_DIR --ns3::TcpHandoverCubic::EnableHandoverProtection=false"
```

Useful log filter:

```bash
NS_LOG='TcpHandoverCubic=level_info|prefix_time' ./ns3 run "main_satnet --run_dir=RUN_DIR" 2>&1 \
  | grep --line-buffered -E 'HANDOVER_EVENT|TCP_HANDOVER_NOTIFICATION|ACK=.*FACK=|BeforeRecovery|TimeSinceHandover|Classification|PreserveCwnd'
```

## Integration points

The implementation is isolated in this contrib module. The following hooks are
required outside it:

1. `TcpSocketBase` passes each received ACK to the optional
   `TcpCongestionOps::ObserveAck` hook. Other congestion controls inherit the
   no-op implementation.
2. `ArbiterSingleForwardHelper` calls `TcpHandoverCubic::NotifyHandover()` and
   sends DATA/ACK direction to `TcpHarpCubicV2::NotifyHandover()` and
   `TcpHarpCubicV21::NotifyHandover()` / `TcpHarpCubicV3::NotifyHandover()` for
   the selected endpoint route update.
3. `ArbiterSingleForward` separately observes the first hop actually used by
   active TCP packets and calls `TcpHarpCubicV4::NotifyHandover()` only when
   that used next hop changes. The first observed hop only initializes the
   baseline and is not reported as a handover.
4. For V5/V6/V7, `ArbiterSingleForward` serializes a version-neutral HARP mark in
   TCP packets. `TcpSocketBase` echoes DATA marks, while congestion control
   learns the event only from an ACK it actually receives. V6 does not add
   another routing callback or modify the packet format. V7 inherits that path
   unchanged and adds no simulator drop-cause callback.

The endpoint IDs in the current handover hook are `1584` and `1585`, matching
the two-ground-station Starlink scenarios used by this experiment.
