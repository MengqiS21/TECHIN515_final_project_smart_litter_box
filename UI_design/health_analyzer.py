"""
health_analyzer.py
──────────────────
读取 weight_node 串口的 visit:JSON 行，存入本地 CSV，
并对每只猫进行健康趋势分析 + 异常检测。

算法概述
────────
1. 数据解析：从串口实时读 "visit:{...}" 行，追加到 visits.csv
2. 特征工程：
     cat_weight_g    - 当次猫咪体重估算
     excrement_g     - 当次排泄物重量
     duration_s      - 停留时长
     visit_freq      - 当天访问次数
     hour_of_day     - 访问时间（0-23）
3. 健康指标（滑动窗口 7 天）：
     weight_trend    - 体重 7 日均值 + 线性趋势斜率
     excrement_avg   - 平均排泄物重量
     freq_avg        - 平均每日访问频率
4. 异常检测：
     Z-score (>2σ)   - 快速标记单次异常
     线性回归斜率     - 检测长期体重下降趋势
5. 结果：可被 Streamlit app.py 直接导入，或单独运行打印报告

依赖：pip install pyserial pandas numpy scipy scikit-learn
"""

import json
import re
import time
import threading
import csv
import os
from datetime import datetime, timedelta
from pathlib import Path
from collections import defaultdict

import pandas as pd
import numpy as np
from scipy import stats
from sklearn.preprocessing import StandardScaler
from sklearn.ensemble import IsolationForest

# ── 配置 ────────────────────────────────────────────────────────
CSV_PATH   = Path(__file__).parent / "visits.csv"
SERIAL_PORT = "/dev/cu.usbmodem1101"
BAUD_RATE   = 115200

# 猫咪配置（体重基准，可在使用过程中自动修正）
CAT_CONFIG = {
    1: {"name": "Wesley", "weight_baseline_g": 4300, "color": "#7C6BB5"},
    2: {"name": "Pupu",   "weight_baseline_g": 4700, "color": "#E88FB4"},
}

# 异常检测阈值
WEIGHT_DROP_ALERT_G     = 200   # 7 日内体重下降超过此值触发警告
EXCREMENT_LOW_G         = 5     # 单次排泄物 < 5g 视为异常
EXCREMENT_HIGH_G        = 150   # 单次排泄物 > 150g 视为异常
VISIT_HIGH_DAILY        = 6     # 每日访问 > 6 次触发警告
VISIT_LOW_DAILY         = 1     # 每日访问 < 1 次触发警告
DURATION_HIGH_S         = 600   # 停留 > 10 分钟触发警告
ZSCORE_THRESHOLD        = 2.5   # Z-score 单次异常阈值

# ── CSV 字段 ─────────────────────────────────────────────────────
CSV_FIELDS = [
    "timestamp", "cat_id", "cat_name", "method", "confidence",
    "duration_s", "cat_weight_g", "excrement_g",
    "entry_ms", "exit_ms"
]

# ── CSV 初始化 ────────────────────────────────────────────────────
def init_csv():
    if not CSV_PATH.exists():
        with open(CSV_PATH, "w", newline="") as f:
            csv.DictWriter(f, fieldnames=CSV_FIELDS).writeheader()


def append_visit(record: dict):
    """将一条访问记录追加写入 CSV"""
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


# ── 串口解析线程 ──────────────────────────────────────────────────
def parse_and_store_serial(port: str, stop_event: threading.Event):
    """后台线程：读串口，遇到 visit:JSON 行则保存"""
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


# ── 数据加载 ─────────────────────────────────────────────────────
def load_visits(days: int = 30) -> pd.DataFrame:
    """加载最近 N 天的访问记录"""
    if not CSV_PATH.exists():
        return pd.DataFrame(columns=CSV_FIELDS)
    df = pd.read_csv(CSV_PATH, parse_dates=["timestamp"])
    cutoff = datetime.now() - timedelta(days=days)
    df = df[df["timestamp"] >= cutoff].copy()
    df["date"]        = df["timestamp"].dt.date
    df["hour_of_day"] = df["timestamp"].dt.hour
    return df


# ── 特征工程 ─────────────────────────────────────────────────────
def compute_daily_features(df: pd.DataFrame) -> pd.DataFrame:
    """
    按 (date, cat_id) 聚合每日特征：
      - mean_cat_weight_g : 当日平均体重
      - mean_excrement_g  : 当日平均排泄物重量
      - visit_count       : 当日访问次数
      - mean_duration_s   : 当日平均停留时长
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


# ── 体重趋势分析 ──────────────────────────────────────────────────
def analyze_weight_trend(daily: pd.DataFrame, cat_id: int, window_days: int = 7):
    """
    返回：
      recent_avg  - 最近 window_days 天均值
      slope_g_day - 每天体重变化斜率（负值=下降）
      total_drop  - 窗口内总变化
    """
    cat_df = daily[daily["cat_id"] == cat_id].sort_values("date").tail(window_days)
    if len(cat_df) < 3:
        return {"recent_avg": None, "slope_g_day": None, "total_drop": None}

    x = np.arange(len(cat_df))
    y = cat_df["mean_cat_weight_g"].values
    slope, intercept, r, p, se = stats.linregress(x, y)

    return {
        "recent_avg":  float(y.mean()),
        "slope_g_day": float(slope),       # 克/天，负值=体重在下降
        "total_drop":  float(y[-1] - y[0]) # 窗口内总变化
    }


# ── Z-score 单次异常检测 ──────────────────────────────────────────
def detect_single_anomalies(df: pd.DataFrame, cat_id: int) -> pd.DataFrame:
    """
    对 cat_weight_g, excrement_g, duration_s 计算 Z-score，
    超过阈值的行标记为异常。
    """
    cat_df = df[df["cat_id"] == cat_id].copy()
    if len(cat_df) < 5:
        return pd.DataFrame()

    features = ["cat_weight_g", "excrement_g", "duration_s"]
    cat_df = cat_df.dropna(subset=features)

    scaler = StandardScaler()
    z = scaler.fit_transform(cat_df[features])
    cat_df["max_z"] = np.abs(z).max(axis=1)
    cat_df["anomaly"] = cat_df["max_z"] > ZSCORE_THRESHOLD

    return cat_df[cat_df["anomaly"]].copy()


# ── Isolation Forest（多特征异常检测） ───────────────────────────
def detect_isolation_forest(df: pd.DataFrame, cat_id: int) -> pd.DataFrame:
    """
    用 IsolationForest 检测多维异常（体重 + 排泄物 + 时长 + 访问频率）。
    需要至少 20 条记录。
    """
    cat_df = df[df["cat_id"] == cat_id].copy()
    features = ["cat_weight_g", "excrement_g", "duration_s", "hour_of_day"]
    cat_df = cat_df.dropna(subset=features)

    if len(cat_df) < 20:
        return pd.DataFrame()

    clf = IsolationForest(contamination=0.05, random_state=42)
    X   = cat_df[features].values
    cat_df["if_label"] = clf.fit_predict(X)  # -1=anomaly
    cat_df["anomaly"]  = cat_df["if_label"] == -1

    return cat_df[cat_df["anomaly"]].copy()


# ── 规则异常 ─────────────────────────────────────────────────────
def rule_based_alerts(df: pd.DataFrame, daily: pd.DataFrame) -> list[dict]:
    """
    基于阈值的规则告警，返回告警列表。
    """
    alerts = []

    for cat_id, cfg in CAT_CONFIG.items():
        name = cfg["name"]
        cat_daily = daily[daily["cat_id"] == cat_id].sort_values("date")

        # 1. 体重下降趋势
        trend = analyze_weight_trend(daily, cat_id, window_days=7)
        if trend["slope_g_day"] is not None:
            if trend["slope_g_day"] < -(WEIGHT_DROP_ALERT_G / 7):
                alerts.append({
                    "level": "⚠️",
                    "cat":   name,
                    "msg":   f"{name} 体重过去 7 天每天平均下降 {abs(trend['slope_g_day']):.0f}g，建议检查饮食。",
                })
            if trend["total_drop"] is not None and trend["total_drop"] < -WEIGHT_DROP_ALERT_G:
                alerts.append({
                    "level": "⚠️",
                    "cat":   name,
                    "msg":   f"{name} 7 天内体重共下降 {abs(trend['total_drop']):.0f}g，建议就医检查。",
                })

        # 2. 排泄物重量异常（最近 3 次均值）
        recent_visits = df[df["cat_id"] == cat_id].sort_values("timestamp").tail(3)
        if len(recent_visits) >= 3:
            avg_excrement = recent_visits["excrement_g"].mean()
            if avg_excrement < EXCREMENT_LOW_G:
                alerts.append({
                    "level": "⚠️",
                    "cat":   name,
                    "msg":   f"{name} 最近 3 次排泄物均重仅 {avg_excrement:.1f}g，可能便秘或脱水。",
                })
            elif avg_excrement > EXCREMENT_HIGH_G:
                alerts.append({
                    "level": "⚠️",
                    "cat":   name,
                    "msg":   f"{name} 最近 3 次排泄物均重 {avg_excrement:.1f}g，偏多，请注意。",
                })

        # 3. 访问频率（昨天）
        if not cat_daily.empty:
            yesterday = (datetime.now() - timedelta(days=1)).date()
            yest_row = cat_daily[cat_daily["date"] == yesterday]
            if not yest_row.empty:
                freq = int(yest_row["visit_count"].values[0])
                if freq > VISIT_HIGH_DAILY:
                    alerts.append({
                        "level": "⚠️",
                        "cat":   name,
                        "msg":   f"{name} 昨天访问猫砂盆 {freq} 次，超出正常范围（≤{VISIT_HIGH_DAILY}次/天），可能泌尿问题。",
                    })
                elif freq < VISIT_LOW_DAILY:
                    alerts.append({
                        "level": "ℹ️",
                        "cat":   name,
                        "msg":   f"{name} 昨天仅访问猫砂盆 {freq} 次，请确认猫咪状态正常。",
                    })

        # 4. 停留时长异常（最近一次）
        recent_one = df[df["cat_id"] == cat_id].sort_values("timestamp").tail(1)
        if not recent_one.empty:
            dur = float(recent_one["duration_s"].values[0])
            if dur > DURATION_HIGH_S:
                alerts.append({
                    "level": "⚠️",
                    "cat":   name,
                    "msg":   f"{name} 最近一次在猫砂盆停留 {dur/60:.1f} 分钟，可能有排泄困难。",
                })

    if not alerts:
        alerts.append({"level": "✅", "cat": "All", "msg": "所有猫咪健康指标正常。"})

    return alerts


# ── 汇总分析（供 Streamlit 调用） ──────────────────────────────────
def get_health_summary(days: int = 14) -> dict:
    """
    返回完整健康摘要字典，包含：
      - alerts        : 告警列表
      - daily         : 每日特征 DataFrame
      - weight_trends : 体重趋势（每只猫）
      - anomalies     : 单次异常记录 DataFrame
    """
    df    = load_visits(days)
    if df.empty:
        return {"alerts": [{"level": "ℹ️", "cat": "All", "msg": "暂无访问记录"}],
                "daily": pd.DataFrame(), "weight_trends": {}, "anomalies": pd.DataFrame()}

    daily  = compute_daily_features(df)
    trends = {cat_id: analyze_weight_trend(daily, cat_id) for cat_id in CAT_CONFIG}

    # 合并两种异常检测结果
    anomaly_frames = []
    for cat_id in CAT_CONFIG:
        z_anom  = detect_single_anomalies(df, cat_id)
        if_anom = detect_isolation_forest(df, cat_id)
        if not z_anom.empty:  anomaly_frames.append(z_anom)
        if not if_anom.empty: anomaly_frames.append(if_anom)

    anomalies = pd.concat(anomaly_frames).drop_duplicates(subset=["timestamp"]) if anomaly_frames else pd.DataFrame()

    alerts = rule_based_alerts(df, daily)

    return {
        "alerts":        alerts,
        "daily":         daily,
        "weight_trends": trends,
        "anomalies":     anomalies,
        "raw":           df,
    }


# ── 单独运行时打印报告 ────────────────────────────────────────────
if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Smart Litter Box Health Analyzer")
    parser.add_argument("--serial", action="store_true", help="从串口实时采集数据")
    parser.add_argument("--port",   default=SERIAL_PORT,   help="串口名称")
    parser.add_argument("--report", action="store_true",   help="打印健康报告")
    args = parser.parse_args()

    init_csv()

    if args.serial:
        stop = threading.Event()
        t = threading.Thread(target=parse_and_store_serial, args=(args.port, stop), daemon=True)
        t.start()
        print(f"[health_analyzer] 实时采集中 ({args.port})，Ctrl+C 停止...")
        try:
            while True: time.sleep(1)
        except KeyboardInterrupt:
            stop.set()

    if args.report or not args.serial:
        summary = get_health_summary(days=14)
        print("\n" + "="*60)
        print("  猫咪健康报告（最近 14 天）")
        print("="*60)

        for cat_id, cfg in CAT_CONFIG.items():
            name  = cfg["name"]
            trend = summary["weight_trends"].get(cat_id, {})
            print(f"\n── {name} ──────────────────────────")
            if trend.get("recent_avg"):
                print(f"  7日均重    : {trend['recent_avg']:.0f} g")
                print(f"  体重斜率   : {trend['slope_g_day']:+.1f} g/天")
                print(f"  7日总变化  : {trend['total_drop']:+.0f} g")
            else:
                print("  数据不足（需至少 3 天记录）")

        print("\n── 告警 ────────────────────────────────")
        for a in summary["alerts"]:
            print(f"  {a['level']} [{a['cat']}] {a['msg']}")

        print(f"\n── 异常记录（共 {len(summary['anomalies'])} 条）──")
        if not summary["anomalies"].empty:
            print(summary["anomalies"][["timestamp", "cat_name", "cat_weight_g", "excrement_g", "duration_s"]].to_string(index=False))

        print("\n" + "="*60)
