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

@st.cache_resource
def get_state():
    return {
        "connected":       False,
        "mqtt_error":      "",
        "weight_buf":      deque(maxlen=MAX_POINTS),
        "time_buf":        deque(maxlen=MAX_POINTS),
        "visits_buf":      deque(maxlen=50),   # real visit records from litterbox/visits
        "lock":            threading.Lock(),
        "stop_event":      threading.Event(),
        "mqtt_client":     None,
        "tare_baseline_g": None,
    }

# ─────────────────────────────────────────────────────────────
#  MQTT background reader
# ─────────────────────────────────────────────────────────────
def mqtt_reader(broker: str, port: int = 1883):
    state = get_state()
    try:
        import paho.mqtt.client as mqtt  # type: ignore
    except ImportError:
        with state["lock"]:
            state["mqtt_error"] = "paho-mqtt not installed. Run: pip install paho-mqtt"
        return

    def on_connect(client, userdata, flags, rc, props=None):
        if rc == 0:
            client.subscribe("litterbox/weight")
            client.subscribe("litterbox/visits")
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
                    state["weight_buf"].append(value)
                    state["time_buf"].append(datetime.now())
            except ValueError:
                pass
        elif topic == "litterbox/visits":
            try:
                import json
                record = json.loads(payload)
                record["_time"] = datetime.now().strftime("%H:%M:%S")
                with state["lock"]:
                    state["visits_buf"].appendleft(record)  # newest first
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
            state["connected"]   = False
            state["mqtt_client"] = None


def start_mqtt(broker: str):
    state = get_state()
    state["stop_event"].set()
    time.sleep(0.3)
    state["stop_event"].clear()
    with state["lock"]:
        state["tare_baseline_g"] = None
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


# ── Placeholder data helpers ──────────────────────────────────
def make_visit_history():
    rng  = np.random.default_rng(42)
    cats = ["Wesley", "Pupu"]
    rows = []
    base = datetime.now()
    for i in range(10):
        cat        = cats[i % 2]
        visit_time = base - timedelta(minutes=15 * (i + 1))
        weight     = rng.integers(3900, 5200)
        duration   = round(rng.uniform(1.5, 5.5), 1)
        status     = "⚠️ Anomaly" if (i == 2 or i == 7) else "✅ Normal"
        rows.append({"Time": visit_time.strftime("%H:%M"), "Cat": cat,
                     "Weight (g)": weight, "Duration (min)": duration, "Status": status})
    return pd.DataFrame(rows)

def make_daily_weight():
    rng    = np.random.default_rng(7)
    dates  = [datetime.now() - timedelta(days=i) for i in range(13, -1, -1)]
    labels = [d.strftime("%m/%d") for d in dates]
    return pd.DataFrame({"Date": labels,
                         "Wesley (g)": 4300 + rng.integers(-150, 200, 14),
                         "Pupu (g)":   4700 + rng.integers(-120, 180, 14)})

def make_daily_visits():
    rng    = np.random.default_rng(13)
    dates  = [datetime.now() - timedelta(days=i) for i in range(13, -1, -1)]
    labels = [d.strftime("%m/%d") for d in dates]
    return pd.DataFrame({"Date": labels,
                         "Wesley": rng.integers(2, 6, 14),
                         "Pupu":   rng.integers(1, 5, 14)})

def visit_history_status_style(val):
    if "Anomaly" in val:
        return "background-color: #FFF7ED; color: #92400E; font-weight: 700;"
    return ""


# ── Sidebar ───────────────────────────────────────────────────
with st.sidebar:
    st.markdown("### Connection")
    broker_ip = st.text_input(
        "MQTT Broker IP",
        value="localhost",
        help="Your Mac's local IP, e.g. 192.168.1.x  (run: ipconfig getifaddr en0)"
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
            state["connected"]      = False
            state["tare_baseline_g"] = None

    state = get_state()
    with state["lock"]:
        is_connected = state["connected"]
        err_msg      = state["mqtt_error"]

    if is_connected:
        st.markdown('<span class="dot-green">●</span> Connected via MQTT', unsafe_allow_html=True)
        if st.button("⚖️ Tare (Zero)", use_container_width=True):
            with state["lock"]:
                mc = state["mqtt_client"]
            if mc:
                mc.publish("litterbox/cmd", "tare")
                st.toast("Tare command sent — scale zeroed!", icon="⚖️")
    else:
        st.markdown('<span class="dot-red">●</span> Disconnected', unsafe_allow_html=True)
        if err_msg:
            st.caption(err_msg)

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

# ── Section 4: Health Trends (placeholder) ───────────────────
st.markdown('<div class="section-title">📈 Health Trends</div>', unsafe_allow_html=True)
st.markdown('<div class="section-sub">14-day overview — placeholder data</div>', unsafe_allow_html=True)
col_w, col_v = st.columns(2)
weight_df = make_daily_weight()
visits_df = make_daily_visits()
with col_w:
    st.markdown("**Daily Average Weight (g)**")
    st.line_chart(weight_df.set_index("Date")[["Wesley (g)", "Pupu (g)"]],
                  color=["#7C6BB5", "#E88FB4"], use_container_width=True, height=240)
with col_v:
    st.markdown("**Daily Visit Count**")
    st.line_chart(visits_df.set_index("Date")[["Wesley", "Pupu"]],
                  color=["#7C6BB5", "#E88FB4"], use_container_width=True, height=240)
st.markdown("""
<div style="font-size:13px; color:#7A7490; margin-top:8px; display:flex; gap:18px; flex-wrap:wrap;">
  <span style="display:flex; align-items:center; gap:7px;">
    <span style="width:11px; height:11px; border-radius:999px; background:#7C6BB5; display:inline-block;"></span>
    <span style="color:#2A2440;">Wesley</span></span>
  <span style="display:flex; align-items:center; gap:7px;">
    <span style="width:11px; height:11px; border-radius:999px; background:#E88FB4; display:inline-block;"></span>
    <span style="color:#2A2440;">Pupu</span></span>
</div>""", unsafe_allow_html=True)
st.divider()

# ── Section 5: Anomaly Alerts (placeholder) ──────────────────
st.markdown('<div class="section-title">🚨 Anomaly Alerts</div>', unsafe_allow_html=True)
st.markdown('<div class="section-sub">Recent health flags — placeholder data</div>', unsafe_allow_html=True)
alerts = [
    ("⚠️", "2026-05-08 09:14", "Wesley's weight dropped 210 g over the last 3 days. Consider a vet check-up."),
    ("⚠️", "2026-05-07 18:02", "Pupu visited 6 times yesterday — above the normal baseline of 2–4 visits/day."),
    ("ℹ️", "2026-05-06 11:45", "Wesley's average visit duration increased to 5.2 min (baseline: 3.1 min)."),
    ("✅", "2026-05-05 08:30", "All metrics normal for both cats today."),
]
for icon, ts, msg in alerts:
    st.markdown(f"""<div class="alert-row">
      <span class="alert-icon">{icon}</span>
      <b style="font-size:11px; color:#7A7490;">{ts}</b><br>{msg}
    </div>""", unsafe_allow_html=True)

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
        if connected:
            weight_num_slot.markdown("""
            <div style="margin:12px 0; color:#7A7490; font-size:16px;">
              ⏳ Waiting for first reading…
            </div>""", unsafe_allow_html=True)
        else:
            weight_num_slot.markdown("""
            <div style="margin:12px 0; padding:14px 20px; background:#FFF7ED;
                        border-left:4px solid #F5A623; border-radius:0 12px 12px 0;
                        color:#92400E; font-size:15px;">
              📡 Not connected. Click <b>Connect</b> and make sure Mosquitto is running
              on this Mac and the Weight Node is on the same WiFi.
            </div>""", unsafe_allow_html=True)
            chart_slot.empty()
            stats_slot.empty()

    # ── Cat Identification (most recent visit) ────────────────
    with state["lock"]:
        visits = list(state["visits_buf"])

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

    time.sleep(0.5)
