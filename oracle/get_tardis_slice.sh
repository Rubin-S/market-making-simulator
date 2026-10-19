#!/usr/bin/env bash
# Stream the head of the Tardis free first-of-month files (range requests return 403, so stream + head).
# 4M book rows + 1.5M trade rows of 2026-10-01 cover about 50 minutes: plenty for a 10-minute slice.
# HTTP 429 = the free quota is used up; wait a few hours and retry.
# usage: ./get_tardis_slice.sh 2026/10/01 4000000 1500000
set -euo pipefail
DAY=${1:-2026/10/01}; NB=${2:-4000000}; NT=${3:-1500000}
B=https://datasets.tardis.dev/v1/binance-futures
mkdir -p data
code=$(curl -s -o /dev/null -w '%{http_code}' -r 0-0 "$B/trades/$DAY/BTCUSDT.csv.gz" || true)
echo "probe HTTP $code (403 on a range probe is normal; 429 = quota exhausted)"
(curl -s "$B/incremental_book_L2/$DAY/BTCUSDT.csv.gz" | gzip -dc 2>/dev/null | head -n "$NB" > data/tardis_incremental_book_L2_head.csv) || true
(curl -s "$B/trades/$DAY/BTCUSDT.csv.gz" | gzip -dc 2>/dev/null | head -n "$NT" > data/tardis_trades_head.csv) || true
wc -l data/tardis_*_head.csv
