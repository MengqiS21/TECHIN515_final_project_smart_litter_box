"""
health_analyzer.py
──────────────────
Reads visit:JSON lines from the weight_node serial port, stores records
in a local CSV, and runs health trend analysis and anomaly detection per cat.

Algorithm overview
──────────────────
1. Data ingestion : background thread reads "visit:{...}" lines from serial → appends to visits.csv
2. Feature engineering:
     cat_weight_g  - estimated cat body weight for this visit
     excrement_g   - waste weight deposited during this visit
     duration_s    - time spent inside the box
     visit_freq    - visit count for the day
     hour_of_day   - hour of visit (0-23)
3. Health metrics (7-day rolling window):
     weight_trend  - 7-day mean + linear regression slope
     excrement_avg - average waste weight per visit
     freq_avg      - average daily visit frequency
4. Anomaly detection:
     Z-score (>2.5σ)  - flags individual outlier visits
     Linear slope      - detects sustained weight loss trend
     Isolation Forest  - multi-feature anomaly detection (requires ≥20 records)
5. Output: importable by Streamlit app.py, or run standalone to print a report

Dependencies: pip install pyserial pandas numpy scipy scikit-learn
"""

import json
import time
import threading
import csv
from datetime import datetime, timedelta
from pathlib import Path

import pandas as pd
import numpy as np
from scipy import stats
from sklearn.preprocessing import StandardScaler
from sklearn.ensemble import IsolationForest

# ── Configuration ────────────────────────────────────────────────────────────
CSV_PATH    = Path(__file__).parent / "visits.csv"
SERIAL_PORT = "/dev/cu.usbmodem1101"
BAUD_RATE   = 115200

# Cat roster (body weight baseline; auto-refined over time)
CAT_CONFIG = {
    1: {"name": "Wesley", "weight_baseline_g": 4300, "color": "#7C6BB5"},
    2: {"name": "Pupu",   "weight_baseline_g": 4700, "color": "#E88FB4"},
}

# Anomaly detection thresholds
WEIGHT_DROP_ALERT_G = 200   # alert if weight drops more than this over 7 days
EXCREMENT_LOW_G     = 5     # single visit waste < 5 g is abnormally low
EXCREMENT_HIGH_G    = 150   # single visit waste > 150 g is abnormally high
VISIT_HIGH_DAILY    = 6     # more than 6 visits/day triggers an alert
VISIT_LOW_DAILY     = 1     # fewer than 1 visit/day triggers an alert
DURATION_HIGH_S     = 600   # stay longer than 10 min may indicate straining
ZSCORE_THRESHOLD    = 2.5   # Z-score cutoff for single-visit anomaly

# ── CSV schema ───────────────────────────────────────────────────────────────
CSV_FIELDS = [
    "timestamp", "cat_id", "cat_name", "method", "confidence",
    "duration_s", "cat_weight_g", "excrement_g",
    "entry_ms", "exit_ms"
]

# ── CSV helpers ──────────────────────────────────────────────────────────────
def init_csv():
    if not CSV_PATH.exists():
        with open(CSV_PATH, "w", newline="") as f:
            csv.DictWriter(f, fieldnames=CSV_FIELDS).writeheader()


def append_visit(record: dict):
    """Append one visit record to the CSV file."""
    init_csv()
    cat_name = CAT_CONFIG.get(record.get("cat_id", 0), {}).get("name", "Unknown")
    row = {
        "timestamp":    datetime.now().isoformat(timespec="seconds"),
        "cat_id":       record.get("cat_id", 0),
        "cat_name":     record.get("cat", cat_name),
        "method":       record.get("method", ""),
        "confidence":   record.get("conf", 0.0),
        "duration_s":   record.get("duration_s", 0),
        "cat_weight_g": record.get("cat_weight_g", 0),
        "excrement_g":  record.get("excrement_g", 0),
        "entry_ms":     record.get("entry_ms", 0),
        "exit_ms":      record.get("exit_ms", 0),
    }
    with open(CSV_PATH, "a", newline="") as f:
        csv.DictWriter(f, fieldnames=CSV_FIELDS).writerow(row)


# ── Serial ingestion thread ───────────────────────────────────────────────────
def parse_and_store_serial(port: str, stop_event: threading.Event):
    """Background thread: reads serial port and saves every visit:JSON line."""
    try:
        import serial
    except ImportError:
        print("[health_analyzer] pyserial not installed")
        return

    try:
        ser = serial.Serial(port, BAUD_RATE, timeout=1)
        print(f"[health_analyzer] Listening on {port}")
        while not stop_event.is_set():
            raw = ser.readline().decode("utf-8", errors="ignore").strip()
            if raw.startswith("visit:"):
                json_str = raw[len("visit:"):]
                try:
                    record = json.loads(json_str)
                    append_visit(record)
                    print(f"[visit] {json_str}")
                except json.JSONDecodeError:
                    print(f"[WARN] Bad JSON: {json_str}")
    except Exception as e:
        print(f"[health_analyzer] Serial error: {e}")


# ── Data loading ──────────────────────────────────────────────────────────────
def load_visits(days: int = 30) -> pd.DataFrame:
    """Load visit records from the last N days."""
    if not CSV_PATH.exists():
        return pd.DataFrame(columns=CSV_FIELDS)
    df = pd.read_csv(CSV_PATH, parse_dates=["timestamp"])
    cutoff = datetime.now() - timedelta(days=days)
    df = df[df["timestamp"] >= cutoff].copy()
    df["date"]        = df["timestamp"].dt.date
    df["hour_of_day"] = df["timestamp"].dt.hour
    return df


# ── Feature engineering ───────────────────────────────────────────────────────
def compute_daily_features(df: pd.DataFrame) -> pd.DataFrame:
    """
    Aggregate per (date, cat_id) daily features:
      - mean_cat_weight_g : average body weight for the day
      - mean_excrement_g  : average waste weight per visit
      - visit_count       : number of visits
      - mean_duration_s   : average time spent per visit
    """
    if df.empty:
        return pd.DataFrame()

    daily = (
        df.groupby(["date", "cat_id", "cat_name"])
        .agg(
            mean_cat_weight_g=("cat_weight_g", "mean"),
            mean_excrement_g =("excrement_g",  "mean"),
            visit_count      =("cat_id",        "count"),
            mean_duration_s  =("duration_s",    "mean"),
        )
        .reset_index()
    )
    return daily


# ── Weight trend analysis ─────────────────────────────────────────────────────
def analyze_weight_trend(daily: pd.DataFrame, cat_id: int, window_days: int = 7):
    """
    Returns:
      recent_avg  - mean weight over the last window_days days
      slope_g_day - grams/day slope (negative = weight loss)
      total_drop  - total change over the window
    """
    cat_df = daily[daily["cat_id"] == cat_id].sort_values("date").tail(window_days)
    if len(cat_df) < 3:
        return {"recent_avg": None, "slope_g_day": None, "total_drop": None}

    x = np.arange(len(cat_df))
    y = cat_df["mean_cat_weight_g"].values
    slope, intercept, r, p, se = stats.linregress(x, y)

    return {
        "recent_avg":  float(y.mean()),
        "slope_g_day": float(slope),        # negative = losing weight
        "total_drop":  float(y[-1] - y[0])  # total change across the window
    }


# ── Z-score single-visit anomaly detection ────────────────────────────────────
def detect_single_anomalies(df: pd.DataFrame, cat_id: int) -> pd.DataFrame:
    """
    Compute Z-score across cat_weight_g, excrement_g, and duration_s.
    Rows exceeding the threshold are flagged as anomalies.
    """
    cat_df = df[df["cat_id"] == cat_id].copy()
    if len(cat_df) < 5:
        return pd.DataFrame()

    features = ["cat_weight_g", "excrement_g", "duration_s"]
    cat_df = cat_df.dropna(subset=features)

    scaler = StandardScaler()
    z = scaler.fit_transform(cat_df[features])
    cat_df["max_z"]  = np.abs(z).max(axis=1)
    cat_df["anomaly"] = cat_df["max_z"] > ZSCORE_THRESHOLD

    return cat_df[cat_df["anomaly"]].copy()


# ── Isolation Forest multi-feature anomaly detection ─────────────────────────
def detect_isolation_forest(df: pd.DataFrame, cat_id: int) -> pd.DataFrame:
    """
    Uses IsolationForest on (cat_weight_g, excrement_g, duration_s, hour_of_day).
    Requires at least 20 records for meaningful results.
    """
    cat_df = df[df["cat_id"] == cat_id].copy()
    features = ["cat_weight_g", "excrement_g", "duration_s", "hour_of_day"]
    cat_df = cat_df.dropna(subset=features)

    if len(cat_df) < 20:
        return pd.DataFrame()

    clf = IsolationForest(contamination=0.05, random_state=42)
    X   = cat_df[features].values
    cat_df["if_label"] = clf.fit_predict(X)   # -1 = anomaly
    cat_df["anomaly"]  = cat_df["if_label"] == -1

    return cat_df[cat_df["anomaly"]].copy()


# ── Rule-based alerts ─────────────────────────────────────────────────────────
def rule_based_alerts(df: pd.DataFrame, daily: pd.DataFrame) -> list[dict]:
    """Generate threshold-based health alerts; returns a list of alert dicts."""
    alerts = []

    for cat_id, cfg in CAT_CONFIG.items():
        name = cfg["name"]
        cat_daily = daily[daily["cat_id"] == cat_id].sort_values("date")

        # 1. Weight loss trend
        trend = analyze_weight_trend(daily, cat_id, window_days=7)
        if trend["slope_g_day"] is not None:
            if trend["slope_g_day"] < -(WEIGHT_DROP_ALERT_G / 7):
                alerts.append({
                    "level": "⚠️", "cat": name,
                    "msg": f"{name}'s weight has been dropping ~{abs(trend['slope_g_day']):.0f} g/day over the past 7 days. Check diet.",
                })
            if trend["total_drop"] is not None and trend["total_drop"] < -WEIGHT_DROP_ALERT_G:
                alerts.append({
                    "level": "⚠️", "cat": name,
                    "msg": f"{name} has lost {abs(trend['total_drop']):.0f} g over 7 days. Consider a vet visit.",
                })

        # 2. Waste weight anomaly (mean of last 3 visits)
        recent_visits = df[df["cat_id"] == cat_id].sort_values("timestamp").tail(3)
        if len(recent_visits) >= 3:
            avg_excrement = recent_visits["excrement_g"].mean()
            if avg_excrement < EXCREMENT_LOW_G:
                alerts.append({
                    "level": "⚠️", "cat": name,
                    "msg": f"{name}'s average waste over the last 3 visits is only {avg_excrement:.1f} g — possible constipation or dehydration.",
                })
            elif avg_excrement > EXCREMENT_HIGH_G:
                alerts.append({
                    "level": "⚠️", "cat": name,
                    "msg": f"{name}'s average waste over the last 3 visits is {avg_excrement:.1f} g — higher than normal.",
                })

        # 3. Daily visit frequency (yesterday)
        if not cat_daily.empty:
            yesterday = (datetime.now() - timedelta(days=1)).date()
            yest_row  = cat_daily[cat_daily["date"] == yesterday]
            if not yest_row.empty:
                freq = int(yest_row["visit_count"].values[0])
                if freq > VISIT_HIGH_DAILY:
                    alerts.append({
                        "level": "⚠️", "cat": name,
                        "msg": f"{name} visited the litter box {freq} times yesterday (normal: ≤{VISIT_HIGH_DAILY}/day). Possible urinary issue.",
                    })
                elif freq < VISIT_LOW_DAILY:
                    alerts.append({
                        "level": "ℹ️", "cat": name,
                        "msg": f"{name} only visited {freq} time(s) yesterday. Please check on the cat.",
                    })

        # 4. Unusually long visit duration (most recent visit)
        recent_one = df[df["cat_id"] == cat_id].sort_values("timestamp").tail(1)
        if not recent_one.empty:
            dur = float(recent_one["duration_s"].values[0])
            if dur > DURATION_HIGH_S:
                alerts.append({
                    "level": "⚠️", "cat": name,
                    "msg": f"{name} spent {dur/60:.1f} min in the box on the last visit — may indicate straining.",
                })

    if not alerts:
        alerts.append({"level": "✅", "cat": "All", "msg": "All health metrics are normal for both cats."})

    return alerts


# ── Full health summary (called by Streamlit) ─────────────────────────────────
def get_health_summary(days: int = 14) -> dict:
    """
    Returns a complete health summary dict:
      - alerts        : list of alert dicts
      - daily         : daily feature DataFrame
      - weight_trends : weight trend dict per cat
      - anomalies     : DataFrame of flagged anomaly visits
    """
    df = load_visits(days)
    if df.empty:
        return {
            "alerts":        [{"level": "ℹ️", "cat": "All", "msg": "No visit records yet."}],
            "daily":         pd.DataFrame(),
            "weight_trends": {},
            "anomalies":     pd.DataFrame(),
        }

    daily  = compute_daily_features(df)
    trends = {cat_id: analyze_weight_trend(daily, cat_id) for cat_id in CAT_CONFIG}

    # Merge results from both anomaly detectors
    anomaly_frames = []
    for cat_id in CAT_CONFIG:
        z_anom  = detect_single_anomalies(df, cat_id)
        if_anom = detect_isolation_forest(df, cat_id)
        if not z_anom.empty:  anomaly_frames.append(z_anom)
        if not if_anom.empty: anomaly_frames.append(if_anom)

    anomalies = (
        pd.concat(anomaly_frames).drop_duplicates(subset=["timestamp"])
        if anomaly_frames else pd.DataFrame()
    )

    alerts = rule_based_alerts(df, daily)

    return {
        "alerts":        alerts,
        "daily":         daily,
        "weight_trends": trends,
        "anomalies":     anomalies,
        "raw":           df,
    }


# ── Standalone report (run directly from terminal) ────────────────────────────
if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Smart Litter Box Health Analyzer")
    parser.add_argument("--serial", action="store_true", help="collect data from serial port in real time")
    parser.add_argument("--port",   default=SERIAL_PORT,  help="serial port name")
    parser.add_argument("--report", action="store_true",  help="print health report")
    args = parser.parse_args()

    init_csv()

    if args.serial:
        stop = threading.Event()
        t = threading.Thread(target=parse_and_store_serial, args=(args.port, stop), daemon=True)
        t.start()
        print(f"[health_analyzer] Collecting from {args.port} — press Ctrl+C to stop...")
        try:
            while True: time.sleep(1)
        except KeyboardInterrupt:
            stop.set()

    if args.report or not args.serial:
        summary = get_health_summary(days=14)
        print("\n" + "="*60)
        print("  Cat Health Report  (last 14 days)")
        print("="*60)

        for cat_id, cfg in CAT_CONFIG.items():
            name  = cfg["name"]
            trend = summary["weight_trends"].get(cat_id, {})
            print(f"\n── {name} {'─'*30}")
            if trend.get("recent_avg"):
                print(f"  7-day avg weight : {trend['recent_avg']:.0f} g")
                print(f"  Weight slope     : {trend['slope_g_day']:+.1f} g/day")
                print(f"  7-day net change : {trend['total_drop']:+.0f} g")
            else:
                print("  Not enough data (need at least 3 days of records)")

        print("\n── Alerts " + "─"*40)
        for a in summary["alerts"]:
            print(f"  {a['level']} [{a['cat']}] {a['msg']}")

        n_anom = len(summary["anomalies"])
        print(f"\n── Anomalous visits ({n_anom} total) " + "─"*20)
        if not summary["anomalies"].empty:
            print(summary["anomalies"][
                ["timestamp", "cat_name", "cat_weight_g", "excrement_g", "duration_s"]
            ].to_string(index=False))

        print("\n" + "="*60)
