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

with st.sidebar:
    st.header("Controls")
    auto_refresh = st.checkbox("Auto-refresh", value=True)
    st.caption("Tip: disable auto-refresh to stop scroll jumping.")

    active_strategy = "unknown"

if auto_refresh:
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

# Strategy metadata (optional). We log a one-time metadata event with event_type == 'M'
# and a 'strategy' column value.
if "strategy" in df.columns:
    meta = df[df["event_type"].astype(str).str.slice(0, 1) == "M"].copy()
    if not meta.empty:
        # Use the last non-empty strategy name (supports future strategy switches).
        s = meta["strategy"].astype(str).replace("", pd.NA).dropna()
        if not s.empty:
            active_strategy = str(s.iloc[-1])

with st.sidebar:
    st.metric("Active Strategy", active_strategy)

# Coerce to expected dtypes (robust to partial writes)
df["timestamp_us"] = pd.to_numeric(df["timestamp_us"], errors="coerce")
df["price"] = pd.to_numeric(df["price"], errors="coerce")
df["size"] = pd.to_numeric(df["size"], errors="coerce")
df["realized_pnl"] = pd.to_numeric(df["realized_pnl"], errors="coerce")
df["event_type"] = df["event_type"].astype(str).str.slice(0, 1)

# Drop metadata rows from plots.
df = df[df["event_type"].isin(["T", "P"])].copy()

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
pnl_points = df[df["event_type"] == "P"].copy()
if pnl_points.empty:
    # Fallback: if we don't have periodic mark-to-market points yet, plot whatever we have.
    pnl_points = df

current_pnl = float(pnl_points["realized_pnl"].iloc[-1])
trade_count = int(len(trades))

col1, col2 = st.columns(2)
col1.metric("PnL (latest)", f"{current_pnl:.6f}")
col2.metric("Trade events", trade_count)

fig = make_subplots(specs=[[{"secondary_y": True}]])

# PnL time series (from periodic 'P' points when available)
fig.add_trace(
    go.Scatter(
        x=pnl_points["t_seconds"],
        y=pnl_points["realized_pnl"],
        mode="lines",
        name="PnL",
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
    title="PnL (line) with Trade Prices (markers)",
    xaxis_title="Time (s since start)",
    legend=dict(orientation="h", yanchor="bottom", y=1.02, xanchor="left", x=0),
    margin=dict(l=40, r=40, t=60, b=40),
)

fig.update_yaxes(title_text="PnL", secondary_y=False)
fig.update_yaxes(title_text="Trade Price", secondary_y=True)

st.plotly_chart(fig, use_container_width=True)

# --- Price movements (mid-price from 'P' events) ---
st.subheader("Price Movements")

mid_points = df[df["event_type"] == "P"].copy()
if mid_points.empty:
    st.info("No mid-price ('P') points yet; price chart will appear after a few seconds.")
else:
    price_fig = go.Figure()
    price_fig.add_trace(
        go.Scatter(
            x=mid_points["t_seconds"],
            y=mid_points["price"],
            mode="lines",
            name="Mid Price",
        )
    )

    if not trades.empty:
        price_fig.add_trace(
            go.Scatter(
                x=trades["t_seconds"],
                y=trades["price"],
                mode="markers",
                name="Trade Price",
                marker=dict(size=7),
                customdata=trades[["size"]],
                hovertemplate="t=%{x:.6f}s<br>price=%{y}<br>size=%{customdata[0]}<extra></extra>",
            )
        )

    price_fig.update_layout(
        xaxis_title="Time (s since start)",
        yaxis_title="Price",
        legend=dict(orientation="h", yanchor="bottom", y=1.02, xanchor="left", x=0),
        margin=dict(l=40, r=40, t=10, b=40),
    )
    st.plotly_chart(price_fig, use_container_width=True)
