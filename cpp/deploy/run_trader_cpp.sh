#!/bin/bash
# Respawn loop for the C++ trader, for a tmux pane: ./cpp/deploy/run_trader_cpp.sh [CONFIG]
# Same role as `while true; do python websocket_trading_bot_hybrid.py ...; sleep 3; done`.
#   Ctrl-C once: graceful stop (in-flight orders drain). Ctrl-C again within 3 s: end the loop.
#   control_bot /restart: CONTROL_RESTART_COMMAND=pkill -f cpp/build/hybrid_bot stops the
#   bot and this loop starts it again. This script's own command line does not contain
#   "cpp/build/hybrid_bot", so that pkill can never kill the loop itself.
cd "$(dirname "$0")/../.." || exit 1     # repo root: .env, configs, key file resolve here
CONFIG="${1:-config_websocket.yaml}"
while true; do
  ./cpp/build/hybrid_bot "$CONFIG" 2>&1 | tee -ia trader.log   # -i: Ctrl-C doesn't cut the shutdown log
  echo "$(date -u '+%F %T') trader exited; restarting in 3 s (Ctrl-C to stop the loop)" | tee -ia trader.log
  sleep 3
done
