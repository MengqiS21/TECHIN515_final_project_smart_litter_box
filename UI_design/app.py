import streamlit as st
import pandas as pd
import numpy as np
import threading
import time
from datetime import datetime, timedelta
from collections import deque

# ── Page config ───────────────────────────────────────────────
st.set_page_config(
    page_title="Smart Cat Litter Box Monitor",
    page_icon="🐾",
    layout="wide",
    initial_sidebar_state="expanded",
)

# ── Custom CSS ────────────────────────────────────────────────
st.markdown("""
<style>
  @import url('https://fonts.googleapis.com/css2?family=Nunito:wght@400;600;700;800&display=swap');

  html, body { background-color: #FBFAF6; color: #2A2440; }
  .stApp { font-family: 'Nunito', sans-serif; background-color: #FBFAF6; }

  header[data-testid="stHeader"] {
    background-color: #FBFAF6 !important;
    box-shadow: none !important;
  }
  div[data-testid="stDecoration"] { display: none !important; }

  [data-testid="stSidebar"],
  [data-testid="stSidebar"] > div {
    background-color: #FBFAF6 !important;
    border-right: 1px solid #E4DFEA;
    user-select: none; -webkit-user-select: none;
  }
  [data-testid="stSidebar"] * { font-family: 'Nunito', sans-serif !important; }
  [data-testid="stSidebar"] input, [data-testid="stSidebar"] textarea {
    user-select: text; -webkit-user-select: text;
  }

  hr { margin: 1.5rem 0 !important; border: none !important;
       border-top: 1px solid #E4DFEA !important; background: none !important; }

  .weight-number { font-size: 72px; font-weight: 800; color: #7C6BB5;
                   line-height: 1; letter-spacing: -2px; }
  .weight-unit   { font-size: 28px; font-weight: 600; color: #7A7490; margin-left: 6px; }

  .section-title { font-size: 20px; font-weight: 800; color: #2A2440; margin-bottom: 4px; }
  .section-sub   { font-size: 13px; color: #7A7490; margin-bottom: 16px; }

  .stat-pill  { display: inline-block; background: #EEEAF7; border-radius: 50px;
                padding: 6px 16px; margin: 4px 6px 4px 0; font-size: 13px; color: #2A2440; }
  .stat-label { color: #7A7490; font-size: 11px; display: block; }
  .stat-val   { font-weight: 800; font-size: 16px; color: #7C6BB5; }

  .alert-row  { border-bottom: 1px solid #EDE9F2; padding: 10px 0 14px 0;
                font-size: 14px; color: #2A2440; }
  .alert-icon { margin-right: 8px; color: #7A7490; }

  .dot-green { color: #22C55E; font-size: 18px; }
  .dot-red   { color: #EF4444; font-size: 18px; }

  [data-testid="stDataFrame"] { border-radius: 6px; overflow: hidden; }
  h1 { font-family: 'Nunito', sans-serif !important; font-weight: 800 !important; }
  h2, h3 { font-family: 'Nunito', sans-serif !important; font-weight: 700 !important; }
</style>
""", unsafe_allow_html=True)

# ── Shared state (survives Streamlit reruns) ──────────────────
MAX_POINTS = 60

_APP_STATE = {
    "connected":       False,
    "mqtt_error":      "",
    "weight_buf":      deque(maxlen=MAX_POINTS),
    "time_buf":        deque(maxlen=MAX_POINTS),
    "visits_buf":      deque(maxlen=50),
    "visit_payloads":  set(),
    "mqtt_running":    False,
    "lock":            threading.Lock(),
    "stop_event":      threading.Event(),
    "mqtt_client":     None,
    "tare_baseline_g": None,
}


def get_state():
    return _APP_STATE


def dedupe_visits(visits):
    """Last line of defense if the same visit was appended twice."""
    seen = set()
    out = []
    for v in visits:
        key = (
            v.get("entry_ms"),
            v.get("exit_ms"),
            v.get("cat_id"),
            v.get("method"),
            round(float(v.get("cat_weight_g", 0)), 1),
        )
        if key in seen:
            continue
        seen.add(key)
        out.append(v)
    return out

# ─────────────────────────────────────────────────────────────
#  MQTT background reader
# ─────────────────────────────────────────────────────────────
def _mqtt_ok(rc) -> bool:
    if hasattr(rc, "is_failure"):
        return not rc.is_failure
    return rc == 0


def mqtt_reader(broker: str, port: int = 1883):
    state = get_state()
    try:
        import paho.mqtt.client as mqtt  # type: ignore
    except ImportError:
        with state["lock"]:
            state["mqtt_error"] = "paho-mqtt not installed. Run: pip install paho-mqtt"
        return

    def on_connect(client, userdata, flags, rc, props=None):
        if _mqtt_ok(rc):
            client.subscribe("litterbox/weight", qos=0)
            client.subscribe("litterbox/visits", qos=1)
            with state["lock"]:
                state["connected"]  = True
                state["mqtt_error"] = ""
        else:
            with state["lock"]:
                state["connected"]  = False
                state["mqtt_error"] = f"MQTT connect failed (rc={rc})"

    def on_disconnect(client, userdata, rc, props=None, reason=None):
        with state["lock"]:
            state["connected"] = False

    def on_message(client, userdata, msg):
        topic   = msg.topic
        payload = msg.payload.decode("utf-8", errors="ignore").strip()
        if topic == "litterbox/weight":
            try:
                value = float(payload)
                with state["lock"]:
                    state["connected"] = True
                    state["weight_buf"].append(value)
                    state["time_buf"].append(datetime.now())
            except ValueError:
                pass
        elif topic == "litterbox/visits":
            try:
                import json
                record = json.loads(payload)
                now = datetime.now()
                record["_time"] = now.strftime("%H:%M:%S")
                record["_received_at"] = now
                with state["lock"]:
                    state["connected"] = True
                    if payload in state["visit_payloads"]:
                        return
                    state["visit_payloads"].add(payload)
                    if len(state["visit_payloads"]) > 200:
                        state["visit_payloads"].clear()
                        state["visit_payloads"].add(payload)
                    state["visits_buf"].appendleft(record)
            except Exception:
                pass

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect    = on_connect
    client.on_disconnect = on_disconnect
    client.on_message    = on_message

    try:
        client.connect(broker, port, keepalive=30)
        with state["lock"]:
            state["mqtt_client"] = client
        client.loop_start()

        while not state["stop_event"].is_set():
            time.sleep(0.2)

        client.loop_stop()
        client.disconnect()
    except Exception as e:
        with state["lock"]:
            state["connected"]  = False
            state["mqtt_error"] = str(e)
    finally:
        with state["lock"]:
            state["connected"]    = False
            state["mqtt_client"]  = None
            state["mqtt_running"] = False


def start_mqtt(broker: str):
    state = get_state()
    if state["mqtt_running"]:
        state["stop_event"].set()
        time.sleep(0.8)
    state["stop_event"].set()
    time.sleep(0.5)
    state["stop_event"].clear()
    with state["lock"]:
        state["tare_baseline_g"] = None
        state["visits_buf"].clear()
        state["visit_payloads"].clear()
        state["mqtt_running"] = True
    threading.Thread(target=mqtt_reader, args=(broker,), daemon=True).start()


# ── Display auto-tare ─────────────────────────────────────────
AUTO_TARE_STABLE_STD_G = 12.0
AUTO_TARE_BLEND        = 0.12
AUTO_TARE_NEAR_BASE_G  = 120.0
NET_ZERO_FLOOR_G       = 28.0
AUTO_TARE_MIN_SAMPLES  = 6

def compute_net_weight(raw_g, weights, state, auto_tare):
    if not auto_tare:
        return raw_g, None
    if len(weights) < AUTO_TARE_MIN_SAMPLES:
        with state["lock"]:
            b = state["tare_baseline_g"]
        if b is None:
            return raw_g, None
        net = raw_g - b
        return (0.0 if net < NET_ZERO_FLOOR_G else net), b
    tail = np.array(weights[-AUTO_TARE_MIN_SAMPLES:], dtype=float)
    mn, sd = float(np.mean(tail)), float(np.std(tail))
    with state["lock"]:
        b = state["tare_baseline_g"]
        if sd < AUTO_TARE_STABLE_STD_G:
            if b is None:
                b = mn
            elif abs(mn - b) < AUTO_TARE_NEAR_BASE_G:
                b = (1.0 - AUTO_TARE_BLEND) * b + AUTO_TARE_BLEND * mn
            state["tare_baseline_g"] = b
    if b is None:
        return raw_g, None
    net = raw_g - b
    return (0.0 if net < NET_ZERO_FLOOR_G else net), b

def net_series_for_chart(weights, baseline):
    if baseline is None:
        return list(weights)
    return [max(0.0, w - baseline) for w in weights]


# ── Health trends & alerts from real MQTT visits ───────────────
TREND_DAYS = 14
VISITS_PER_DAY_WARN = 6
DURATION_WARN_S = 180.0
LOW_WEIGHT_WARN_G = 200.0
HIGH_EXCREMENT_WARN_G = 500.0


def _visit_timestamp(v):
    at = v.get("_received_at")
    if isinstance(at, datetime):
        return at
    return datetime.now()


def build_daily_trends_from_visits(visits, days=TREND_DAYS):
    """Aggregate litterbox/visits from this session into daily charts."""
    today = datetime.now().date()
    date_list = [today - timedelta(days=i) for i in range(days - 1, -1, -1)]
    labels = [d.strftime("%m/%d") for d in date_list]

    w_w, p_w = {d: [] for d in date_list}, {d: [] for d in date_list}
    w_c, p_c = {d: 0 for d in date_list}, {d: 0 for d in date_list}

    for v in dedupe_visits(visits):
        d = _visit_timestamp(v).date()
        if d not in w_c:
            continue
        cat = v.get("cat", "")
        cw = float(v.get("cat_weight_g", 0) or 0)
        if cat == "Wesley":
            w_c[d] += 1
            if cw > 0:
                w_w[d].append(cw)
        elif cat == "Pupu":
            p_c[d] += 1
            if cw > 0:
                p_w[d].append(cw)

    def day_avg(bucket):
        return round(sum(bucket) / len(bucket), 0) if bucket else np.nan

    return pd.DataFrame({
        "Date": labels,
        "Wesley (g)": [day_avg(w_w[d]) for d in date_list],
        "Pupu (g)": [day_avg(p_w[d]) for d in date_list],
        "Wesley": [w_c[d] for d in date_list],
        "Pupu": [p_c[d] for d in date_list],
    })


def build_health_alerts_from_visits(visits):
    """Rule-based flags from real visit records (current MQTT session)."""
    visits = dedupe_visits(visits)
    now = datetime.now()
    alerts = []

    if not visits:
        return [("ℹ️", now.strftime("%Y-%m-%d %H:%M"),
                 "No visits yet this session — trends and alerts update after each litterbox visit.")]

    today = now.date()
    today_visits = [v for v in visits if _visit_timestamp(v).date() == today]

    for cat in ("Wesley", "Pupu"):
        n = sum(1 for v in today_visits if v.get("cat") == cat)
        if n >= VISITS_PER_DAY_WARN:
            alerts.append(("⚠️", now.strftime("%Y-%m-%d %H:%M"),
                           f"{cat} visited {n} times today (session) — above typical 2–4/day."))

    for v in visits[:12]:
        cat = v.get("cat", "Unknown")
        ts = _visit_timestamp(v).strftime("%Y-%m-%d %H:%M")
        dur = float(v.get("duration_s", 0) or 0)
        conf = float(v.get("conf", 0) or 0)
        cw = float(v.get("cat_weight_g", 0) or 0)
        exc = float(v.get("excrement_g", 0) or 0)
        method = v.get("method", "")

        if dur >= DURATION_WARN_S:
            alerts.append(("ℹ️", ts,
                           f"{cat}: long visit ({dur / 60:.1f} min)."))
        if 0 < conf < 0.6:
            alerts.append(("⚠️", ts,
                           f"{cat}: low identification confidence ({conf * 100:.0f}%)."))
        if 0 < cw < LOW_WEIGHT_WARN_G:
            alerts.append(("⚠️", ts,
                           f"{cat}: cat weight only {cw:.0f} g — check scale / calibration."))
        if exc >= HIGH_EXCREMENT_WARN_G:
            alerts.append(("ℹ️", ts,
                           f"{cat}: large baseline shift (+{exc:.0f} g) — litter added or re-tare."))
        if method == "WEIGHT" and cat in ("Wesley", "Pupu"):
            alerts.append(("ℹ️", ts,
                           f"{cat}: identified by weight only (camera missed or low conf)."))

    # Weight trend: compare last two visits per cat
    for cat in ("Wesley", "Pupu"):
        cat_visits = [v for v in visits if v.get("cat") == cat and float(v.get("cat_weight_g", 0) or 0) > 0]
        if len(cat_visits) >= 2:
            w0 = float(cat_visits[0].get("cat_weight_g", 0))
            w1 = float(cat_visits[1].get("cat_weight_g", 0))
            drop = w1 - w0
            if drop <= -200:
                alerts.append(("⚠️", _visit_timestamp(cat_visits[0]).strftime("%Y-%m-%d %H:%M"),
                               f"{cat}: weight down {abs(drop):.0f} g vs previous visit in this session."))

    if not alerts:
        alerts.append(("✅", now.strftime("%Y-%m-%d %H:%M"),
                       "No anomalies in recorded visits this session."))

    return alerts[:10]


# ── Sidebar ───────────────────────────────────────────────────
with st.sidebar:
    st.markdown("### Connection")
    broker_ip = st.text_input(
        "MQTT Broker IP",
        value="broker.hivemq.com",
        help="Public broker (matches weight_node.ino). Use localhost only if you run Mosquitto on this Mac."
    )

    col1, col2 = st.columns(2)
    with col1:
        connect_btn = st.button("Connect", use_container_width=True)
    with col2:
        disconnect_btn = st.button("Disconnect", use_container_width=True)

    if connect_btn:
        start_mqtt(broker_ip)

    if disconnect_btn:
        state = get_state()
        state["stop_event"].set()
        with state["lock"]:
            mc = state.get("mqtt_client")
            state["connected"]       = False
            state["mqtt_running"]    = False
            state["tare_baseline_g"] = None
            state["visits_buf"].clear()
            state["visit_payloads"].clear()
        if mc is not None:
            try:
                mc.loop_stop()
                mc.disconnect()
            except Exception:
                pass
        time.sleep(0.3)
        state["stop_event"].clear()

    # Status updates in the live loop below (while True freezes this static block)
    conn_status_slot = st.empty()
    if st.button("⚖️ Tare (Zero)", use_container_width=True, key="tare_mqtt"):
        state = get_state()
        with state["lock"]:
            mc = state.get("mqtt_client")
        if mc:
            mc.publish("litterbox/cmd", "tare")
            st.toast("Tare command sent — scale zeroed!", icon="⚖️")

    st.divider()
    st.markdown("**Live weight display**")
    st.checkbox(
        "Auto zero: show ~0 g when platform is stable and empty",
        value=True, key="auto_tare_sw",
    )
    if st.button("Set current reading as empty baseline", use_container_width=True, key="tare_snap_ui"):
        st.session_state["_tare_snap_now"] = True

    st.divider()
    st.markdown("**Hardware**")
    st.caption("Weight Node: XIAO ESP32S3 · HX711")
    st.caption("Camera Node: XIAO ESP32S3 Sense · PIR")
    st.caption("Protocol: ESP-NOW + WiFi MQTT")

    st.divider()
    st.markdown("**About**")
    st.caption("Tracks litter visits, weight, and patterns. "
               "Built by Mengqi Shi, Yuna Xiong, and Xin Luo.")


# ── Header ────────────────────────────────────────────────────
st.markdown("""
<h1 style='font-size:38px; font-weight:800; color:#2A2440; margin-bottom:2px;'>
  🐾 Smart Cat Litter Box Monitor
</h1>
<p style='font-size:16px; color:#7A7490; margin-top:0; margin-bottom:28px;'>
  Keeping your cats healthy, one visit at a time.
</p>
""", unsafe_allow_html=True)

# ── Section 1: Live Weight ────────────────────────────────────
st.markdown('<div class="section-title">⚖️ Live Weight Monitor</div>', unsafe_allow_html=True)
st.markdown('<div class="section-sub">Real-time HX711 reading via MQTT · updates every 500 ms</div>', unsafe_allow_html=True)

weight_num_slot = st.empty()
chart_slot      = st.empty()
stats_slot      = st.empty()
st.divider()

# ── Section 2: Cat Identification ────────────────────────────
st.markdown('<div class="section-title">🐱 Cat Identification</div>', unsafe_allow_html=True)
cat_id_slot = st.empty()
st.divider()

# ── Section 3: Visit History ──────────────────────────────────
st.markdown('<div class="section-title">📋 Visit History</div>', unsafe_allow_html=True)
visit_history_slot = st.empty()
st.divider()

# ── Section 4–5: Health Trends & Alerts (live MQTT visits) ───
st.markdown('<div class="section-title">📈 Health Trends</div>', unsafe_allow_html=True)
health_sub_slot = st.empty()
health_charts_slot = st.empty()
health_legend_slot = st.empty()
st.divider()

st.markdown('<div class="section-title">🚨 Anomaly Alerts</div>', unsafe_allow_html=True)
alerts_sub_slot = st.empty()
alerts_slot = st.empty()

# ── Live update loop ──────────────────────────────────────────
while True:
    state = get_state()

    if st.session_state.pop("_tare_snap_now", False):
        with state["lock"]:
            wb = list(state["weight_buf"])
        if wb:
            with state["lock"]:
                state["tare_baseline_g"] = float(wb[-1])

    with state["lock"]:
        weights   = list(state["weight_buf"])
        times     = list(state["time_buf"])
        connected = state["connected"]
        err_msg   = state["mqtt_error"]
        n_visits  = len(state["visits_buf"])
        mc        = state.get("mqtt_client")

    mqtt_live = connected or len(weights) > 0 or n_visits > 0
    if mc is not None:
        try:
            mqtt_live = mqtt_live or mc.is_connected()
        except Exception:
            pass
    if mqtt_live:
        conn_status_slot.markdown(
            f'<span class="dot-green">●</span> Connected via MQTT · '
            f'{n_visits} visit(s) received',
            unsafe_allow_html=True,
        )
    else:
        hint = f"<br><span style='font-size:12px;color:#7A7490;'>{err_msg}</span>" if err_msg else ""
        conn_status_slot.markdown(
            f'<span class="dot-red">●</span> Disconnected{hint}',
            unsafe_allow_html=True,
        )

    if weights:
        raw_g      = float(weights[-1])
        auto_tare  = bool(st.session_state.get("auto_tare_sw", True))
        display_w, baseline = compute_net_weight(raw_g, weights, state, auto_tare)

        sub = ""
        if auto_tare and baseline is not None:
            sub = f'<span style="font-size:14px;color:#7A7490;margin-left:10px;">Baseline ≈ {baseline:,.0f} g (raw)</span>'
        weight_num_slot.markdown(f"""
        <div style="margin:12px 0 6px;">
          <span class="weight-number">{display_w:,.1f}</span>
          <span class="weight-unit">g{sub}</span>
        </div>""", unsafe_allow_html=True)

        nets = net_series_for_chart(weights, baseline if auto_tare else None)
        if nets:
            df_chart = pd.DataFrame({"Weight (g)": nets}, index=pd.to_datetime(times))
            chart_slot.line_chart(df_chart, color=["#7C6BB5"], use_container_width=True, height=200)

        arr = np.array(nets, dtype=float)
        stats_slot.markdown(f"""
        <div style="margin-top:10px;">
          <span class="stat-pill"><span class="stat-label">Min</span><span class="stat-val">{arr.min():,.1f} g</span></span>
          <span class="stat-pill"><span class="stat-label">Max</span><span class="stat-val">{arr.max():,.1f} g</span></span>
          <span class="stat-pill"><span class="stat-label">Mean</span><span class="stat-val">{arr.mean():,.1f} g</span></span>
          <span class="stat-pill"><span class="stat-label">Std Dev</span><span class="stat-val">{arr.std():,.1f} g</span></span>
          <span class="stat-pill"><span class="stat-label">Readings</span><span class="stat-val">{len(nets)}</span></span>
        </div>""", unsafe_allow_html=True)
    else:
        if mqtt_live:
            weight_num_slot.markdown("""
            <div style="margin:12px 0; color:#7A7490; font-size:16px;">
              ⏳ Waiting for first reading…
            </div>""", unsafe_allow_html=True)
        else:
            weight_num_slot.markdown("""
            <div style="margin:12px 0; padding:14px 20px; background:#FFF7ED;
                        border-left:4px solid #F5A623; border-radius:0 12px 12px 0;
                        color:#92400E; font-size:15px;">
              📡 Not connected. Click <b>Connect</b> — broker should be
              <b>broker.hivemq.com</b> (same as weight_node). Mac needs internet.
            </div>""", unsafe_allow_html=True)
            chart_slot.empty()
            stats_slot.empty()

    # ── Cat Identification (most recent visit) ────────────────
    with state["lock"]:
        visits = dedupe_visits(list(state["visits_buf"]))

    cat_colors = {1: "#7C6BB5", 2: "#E88FB4", 0: "#9E9E9E"}
    if visits:
        v = visits[0]
        cat_name   = v.get("cat", "Unknown")
        cat_id     = v.get("cat_id", 0)
        cat_w      = v.get("cat_weight_g", 0)
        conf       = v.get("conf", 0)
        dur        = v.get("duration_s", 0)
        exc        = v.get("excrement_g", 0)
        method     = v.get("method", "WEIGHT")
        visit_time = v.get("_time", "—")
        color      = cat_colors.get(cat_id, "#9E9E9E")
        conf_label = "High" if conf >= 0.8 else ("Mid" if conf >= 0.5 else "Low")
        conf_bg    = "#22C55E" if conf >= 0.8 else ("#F59E0B" if conf >= 0.5 else "#9E9E9E")

        ci1, ci2, ci3, ci4 = cat_id_slot.columns([1.2, 1, 1, 1])
        with ci1:
            st.markdown(f"""
            <div style="display:flex;align-items:center;gap:14px;padding:12px 0;">
              <div style="width:64px;height:64px;border-radius:50%;background:{color};
                          display:flex;align-items:center;justify-content:center;font-size:30px;">🐱</div>
              <div>
                <div style="font-size:22px;font-weight:800;color:#2A2440;">{cat_name}</div>
                <div style="font-size:12px;color:#7A7490;margin-top:3px;">Most Recent Visit · {method}</div>
              </div>
            </div>""", unsafe_allow_html=True)
        with ci2:
            st.markdown(f"""<div style="padding:12px 0;">
              <div style="font-size:12px;color:#7A7490;margin-bottom:4px;">Cat Weight</div>
              <div style="font-size:28px;font-weight:800;color:{color};">{cat_w:,.0f} g</div>
              <div style="font-size:12px;color:#7A7490;margin-top:4px;">Excrement: {exc:.1f} g</div>
            </div>""", unsafe_allow_html=True)
        with ci3:
            st.markdown(f"""<div style="padding:12px 0;">
              <div style="font-size:12px;color:#7A7490;margin-bottom:4px;">Confidence</div>
              <div style="font-size:28px;font-weight:800;color:#2A2440;">{conf*100:.1f}%</div>
              <span style="background:{conf_bg};color:#fff;border-radius:50px;
                           padding:2px 12px;font-size:12px;font-weight:700;">{conf_label}</span>
            </div>""", unsafe_allow_html=True)
        with ci4:
            st.markdown(f"""<div style="padding:12px 0;">
              <div style="font-size:12px;color:#7A7490;margin-bottom:4px;">Visit Time</div>
              <div style="font-size:22px;font-weight:800;color:#2A2440;">{visit_time}</div>
              <div style="font-size:14px;color:#7A7490;margin-top:4px;">Duration: {dur/60:.1f} min</div>
            </div>""", unsafe_allow_html=True)
    else:
        cat_id_slot.markdown("""
        <div style="color:#7A7490;font-size:15px;padding:16px 0;">
          No visits recorded yet — waiting for first cat visit…
        </div>""", unsafe_allow_html=True)

    # ── Visit History (real data) ─────────────────────────────
    if visits:
        rows = []
        for v in visits:
            cat_id = v.get("cat_id", 0)
            rows.append({
                "Time":           v.get("_time", "—"),
                "Cat":            v.get("cat", "Unknown"),
                "Method":         v.get("method", "—"),
                "Cat Weight (g)": round(v.get("cat_weight_g", 0)),
                "Excrement (g)":  round(v.get("excrement_g", 0), 1),
                "Duration (min)": round(v.get("duration_s", 0) / 60, 1),
                "Confidence":     f"{v.get('conf', 0)*100:.0f}%",
            })
        visit_history_slot.dataframe(
            pd.DataFrame(rows),
            use_container_width=True, hide_index=True, height=320
        )
    else:
        visit_history_slot.markdown("""
        <div style="color:#7A7490;font-size:14px;padding:12px 0;">
          No visits recorded yet in this session.
        </div>""", unsafe_allow_html=True)

    # ── Health Trends (from real visits) ───────────────────────
    n_visits = len(visits)
    health_sub_slot.markdown(
        f'<div class="section-sub">14-day view · {n_visits} visit(s) this MQTT session (live)</div>',
        unsafe_allow_html=True,
    )
    trends_df = build_daily_trends_from_visits(visits)
    cw, cv = health_charts_slot.columns(2)
    with cw:
        st.markdown("**Daily Average Cat Weight (g)**")
        w_chart = trends_df.set_index("Date")[["Wesley (g)", "Pupu (g)"]].replace(0, np.nan)
        if w_chart.notna().any().any():
            st.line_chart(w_chart, color=["#7C6BB5", "#E88FB4"],
                          use_container_width=True, height=240)
        else:
            st.caption("No cat weight data yet — complete a visit on the scale.")
    with cv:
        st.markdown("**Daily Visit Count**")
        st.line_chart(trends_df.set_index("Date")[["Wesley", "Pupu"]],
                      color=["#7C6BB5", "#E88FB4"], use_container_width=True, height=240)
    health_legend_slot.markdown("""
    <div style="font-size:13px; color:#7A7490; margin-top:8px; display:flex; gap:18px; flex-wrap:wrap;">
      <span style="display:flex; align-items:center; gap:7px;">
        <span style="width:11px; height:11px; border-radius:999px; background:#7C6BB5;"></span>
        <span style="color:#2A2440;">Wesley</span></span>
      <span style="display:flex; align-items:center; gap:7px;">
        <span style="width:11px; height:11px; border-radius:999px; background:#E88FB4;"></span>
        <span style="color:#2A2440;">Pupu</span></span>
    </div>""", unsafe_allow_html=True)

    # ── Anomaly Alerts (from real visits) ────────────────────
    alerts_sub_slot.markdown(
        '<div class="section-sub">Rules applied to visits recorded this session (live)</div>',
        unsafe_allow_html=True,
    )
    alert_lines = build_health_alerts_from_visits(visits)
    alerts_html = ""
    for icon, ts, msg in alert_lines:
        alerts_html += f"""<div class="alert-row">
          <span class="alert-icon">{icon}</span>
          <b style="font-size:11px; color:#7A7490;">{ts}</b><br>{msg}
        </div>"""
    alerts_slot.markdown(alerts_html, unsafe_allow_html=True)

    time.sleep(0.5)
