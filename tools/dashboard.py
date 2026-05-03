import os
from pathlib import Path

import pandas as pd
import plotly.graph_objects as go
import streamlit as st
from plotly.subplots import make_subplots
from streamlit_autorefresh import st_autorefresh


CSV_PATH = Path(os.environ.get("TRADING_LOG_PATH", "trading_log.csv"))
REFRESH_MS = 1000


st.set_page_config(page_title="Trading Dashboard", layout="wide")
st.title("Real-time Trading Dashboard")

st_autorefresh(interval=REFRESH_MS, key="trading_log_refresh")

st.caption(f"Polling {CSV_PATH} every {REFRESH_MS / 1000:.0f}s")

if not CSV_PATH.exists():
    st.warning(
        "No log file found yet. Start the C++ engine so it begins writing trading_log.csv."
    )
    st.stop()

try:
    df = pd.read_csv(CSV_PATH)
except Exception as exc:  # noqa: BLE001
    st.error(f"Failed to read CSV: {exc}")
    st.stop()

required = {"timestamp_us", "event_type", "price", "size", "realized_pnl"}
missing = required.difference(df.columns)
if missing:
    st.error(f"CSV is missing required columns: {sorted(missing)}")
    st.stop()

# Coerce to expected dtypes (robust to partial writes)
df["timestamp_us"] = pd.to_numeric(df["timestamp_us"], errors="coerce")
df["price"] = pd.to_numeric(df["price"], errors="coerce")
df["size"] = pd.to_numeric(df["size"], errors="coerce")
df["realized_pnl"] = pd.to_numeric(df["realized_pnl"], errors="coerce")
df["event_type"] = df["event_type"].astype(str).str.slice(0, 1)

# Drop obviously broken rows (e.g., mid-write)
df = df.dropna(subset=["timestamp_us", "realized_pnl"]).copy()
if df.empty:
    st.info("Log file is empty (or only has headers) so far.")
    st.stop()

# Sort and build a stable x-axis
# We intentionally treat timestamp_us as an arbitrary monotonic clock (not necessarily epoch).
df = df.sort_values("timestamp_us")
t0 = float(df["timestamp_us"].iloc[0])
df["t_seconds"] = (df["timestamp_us"] - t0) / 1_000_000.0

trades = df[df["event_type"] == "T"].copy()

current_realized = float(df["realized_pnl"].iloc[-1])
trade_count = int(len(trades))

col1, col2 = st.columns(2)
col1.metric("Realized PnL (latest)", f"{current_realized:.6f}")
col2.metric("Trade events", trade_count)

fig = make_subplots(specs=[[{"secondary_y": True}]])

# Realized PnL time series
fig.add_trace(
    go.Scatter(
        x=df["t_seconds"],
        y=df["realized_pnl"],
        mode="lines",
        name="Realized PnL",
    ),
    secondary_y=False,
)

# Trade price scatter overlay
if not trades.empty:
    fig.add_trace(
        go.Scatter(
            x=trades["t_seconds"],
            y=trades["price"],
            mode="markers",
            name="Trade Price",
            marker=dict(size=7),
            customdata=trades[["size"]],
            hovertemplate="t=%{x:.6f}s<br>price=%{y}<br>size=%{customdata[0]}<extra></extra>",
        ),
        secondary_y=True,
    )

fig.update_layout(
    title="Realized PnL (line) with Trade Prices (markers)",
    xaxis_title="Time (s since start)",
    legend=dict(orientation="h", yanchor="bottom", y=1.02, xanchor="left", x=0),
    margin=dict(l=40, r=40, t=60, b=40),
)

fig.update_yaxes(title_text="Realized PnL", secondary_y=False)
fig.update_yaxes(title_text="Trade Price", secondary_y=True)

st.plotly_chart(fig, use_container_width=True)
