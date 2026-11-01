# Taipei → Buenos Aires HARP-Cubic 7.0 comparison

這個研究情境使用相同的 Starlink-550 forwarding state、seed、endpoint、PRR，
以及相同的 scheduled GSL DATA-loss schedule、rate 與 stream，比較：

- `TcpCubic`
- `TcpHarpCubicV6`
- `TcpHarpCubicV7`

DATA flow 是 ground station `1584 → 1585`。三組 TCP 都啟用相同的 on-wire HARP
mark/echo plumbing，使封包格式與額外負擔一致；`TcpCubic` 不使用 HARP 訊號改變
Recovery，V6/V7 才會執行 HARP policy。V7 只在 V6 已判定為 normal loss 的路徑上
使用 sender-only ML。

## Scheduled GSL loss

只有下列 250 ms 視窗注入 `0.005` DATA loss，其餘時間 rate 為 `0`：

| 時間 (s) | GSL 方向 | rate |
|---:|:---|---:|
| 20.000–20.250 | uplink | 0.005 |
| 50.000–50.250 | downlink | 0.005 |
| 90.000–90.250 | uplink | 0.005 |
| 120.000–120.250 | downlink | 0.005 |
| 140.000–140.250 | uplink | 0.005 |
| 180.000–180.250 | downlink | 0.005 |

uplink 與 downlink 嚴格交替且不重疊。排程與已知 handover time 至少相隔一秒，
因此人工注入的 random-loss window 不會與那些 handover 重疊。這個安排不代表背景
網路不可能發生其他 loss 或 reordering，也不能單獨證明某次 Recovery 的根因。

實際 GSL drop 會記錄在每個 run 的
`logs_ns3/gsl_random_loss_drops.csv`，包含時間、方向、rate 與排程 label。

V6/V7 共用 HARP 6.0 的封包標記、dynamic gap threshold、late-signal safe update
與 `HandoverBeta=0.85`。V7 的 active ML normal-loss path 使用
`RandomThreshold=0.00025` 與 `RandomBeta=0.95`。

## Fresh clone 與建置

從 repository root 執行：

```bash
git submodule update --init --recursive
bash hypatia_install_dependencies.sh
bash hypatia_build.sh
```

建置腳本會自動套用 HARP 所需的 ns-3 core、CUBIC hook 與 `basic-sim` patch，
不需要手動複製或修改 ns-3 source。完整 runner 另外需要 GNU `screen`。

## 快速驗證

先驗證三組設定、loss schedule、endpoint 與 handover guard；這一步不產生
forwarding state，也不執行 ns-3：

```bash
cd integration_tests/test_taipei_buenos_aires_v7_200Mbps
source ../../env/bin/activate
python3 step_0_validate.py
```

建置後可另外驗證 ML inference 與 HARP unit suite：

```bash
cd ../../ns3-sat-sim/simulator
./ns3 run "ml-cubic-inference-check --v2=1"
./ns3 run "test-runner --suite=tcp-harp-cubic-v6 --verbose"
./ns3 run "test-runner --suite=tcp-harp-cubic-v7 --verbose"
```

## 完整執行

回到情境目錄後執行：

```bash
cd ../../integration_tests/test_taipei_buenos_aires_v7_200Mbps
./run_test.sh
```

`run_test.sh` 會依序驗證設定、產生或重用 forwarding state、產生三組 run、執行
模擬、畫圖並執行 `step_5_verify.py`。若相容的 V6/V5 forwarding state 已存在，
會以 hard link 重用；否則第一步最多使用 14 個 worker 產生 state。模擬最多同時執行
兩個 ns-3 process。

這不是即時測試。依硬體與既有 forwarding state 而異，完整執行可能需要數十分鐘；
先前環境約需 50 分鐘。執行前請確認沒有其他 GNU `screen` session。

## 輸出與驗證

所有生成內容都在 `temp/`，該目錄已由 `.gitignore` 排除，不應 commit 到 GitHub。
主要輸出包括：

- `temp/runs/<run>/logs_ns3/console.txt`：ns-3 與 HARP/ML decision log
- `temp/runs/<run>/logs_ns3/gsl_random_loss_drops.csv`：實際 scheduled GSL drop
- `temp/data/<run>/tcp_flow_0_*.csv`：cwnd、progress、rate、RTT
- `temp/pdf/comparison/comparison_*.pdf`：三組疊圖

`step_5_verify.py` 會確認三組 random-loss 設定與 schedule 完全一致、drop 位於合法
window、必要 CSV/PDF 存在，以及 V7 console 內同時有 `HarpV6Decision`、完成 warm-up
的 `HarpV7MlDecision` 和至少一次 `assisted=1`。這些條件證明實驗路徑有執行，
不等同於證明每一個 Recovery 的物理根因。

## 研究限制

- 目前 handover hook 綁定 endpoint `1584/1585`，適用於這組雙 ground-station
  Starlink 情境，不是任意 topology 的通用設定。
- 在既有 raw trace 中，`165.437298s` 的第二次 Recovery 是較新 sequence 的獨立
  loss/reordering indication；目前 trace 尚不足以判定它是真實 packet drop，還是
  handover 引起的 reordering。因此不得把該事件直接標示成已證實的 congestion loss
  或 handover loss。
- V6 的 fresh signal 加 large-gap gate 是 conservative heuristic；真正 loss 若剛好
  與有效 handover signal 同時發生，仍可能被誤分類。
