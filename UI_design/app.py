import streamlit as st
import pandas as pd
import numpy as np
import threading
import time
from datetime import datetime, timedelta
from collections import deque

# ── Page config ──────────────────────────────────────────────────────────────
st.set_page_config(
    page_title="Smart Cat Litter Box Monitor",
    page_icon="🐾",
    layout="wide",
)

# ── Custom CSS ────────────────────────────────────────────────────────────────
st.markdown("""
<style>
  @import url('https://fonts.googleapis.com/css2?family=Nunito:wght@400;600;700;800&display=swap');
  @import url('https://fonts.googleapis.com/css2?family=Material+Symbols+Rounded:opsz,wght,FILL,GRAD@20..48,100..700,0..1,-50..200&display=swap');

  html, body, [class*="css"] {
    font-family: 'Nunito', sans-serif !important;
    background-color: #FBFAF6;
    color: #2A2440;
  }

  /* Main background */
  .stApp { background-color: #FBFAF6; }

  /* Hide Streamlit chrome: top bar only */
  header[data-testid="stHeader"],
  div[data-testid="stToolbar"],
  div[data-testid="stDecoration"] {
    display: none !important;
  }

  /* Hide sidebar collapse/expand buttons and broken icon text */
  button[data-testid="stSidebarCollapseButton"],
  [data-testid="stSidebarCollapsedControl"],
  [data-testid="stIconMaterial"] {
    display: none !important;
  }
  /* Restore sensible top spacing once header is gone (main column only) */
  .main .block-container {
    padding-top: 1.25rem !important;
  }

  /* Sidebar — same warm off-white as main; avoid blue drag-select on labels */
  [data-testid="stSidebar"],
  [data-testid="stSidebar"] > div {
    background-color: #FBFAF6 !important;
    border-right: 1px solid #E4DFEA;
    user-select: none;
    -webkit-user-select: none;
    -moz-user-select: none;
  }
  [data-testid="stSidebar"] * {
    font-family: 'Nunito', sans-serif !important;
  }
  [data-testid="stSidebar"] input,
  [data-testid="stSidebar"] textarea {
    user-select: text;
    -webkit-user-select: text;
    -moz-user-select: text;
  }
  [data-testid="stSidebar"] *::selection {
    background: #E0DCD4;
    color: #2A2440;
  }
  [data-testid="stSidebar"] .stVerticalBlock,
  [data-testid="stSidebar"] [data-testid="stVerticalBlock"] {
    background-color: transparent !important;
  }

  /* Section breaks: thin neutral line (Streamlit divider) */
  hr {
    margin: 1.5rem 0 !important;
    border: none !important;
    border-top: 1px solid #E4DFEA !important;
    background: none !important;
  }

  /* Big weight number */
  .weight-number {
    font-size: 72px;
    font-weight: 800;
    color: #7C6BB5;
    line-height: 1;
    letter-spacing: -2px;
  }
  .weight-unit {
    font-size: 28px;
    font-weight: 600;
    color: #7A7490;
    margin-left: 6px;
  }

  /* Section headers */
  .section-title {
    font-size: 20px;
    font-weight: 800;
    color: #2A2440;
    margin-bottom: 4px;
  }
  .section-sub {
    font-size: 13px;
    color: #7A7490;
    margin-bottom: 16px;
  }

  /* Stats pill */
  .stat-pill {
    display: inline-block;
    background: #EEEAF7;
    border-radius: 50px;
    padding: 6px 16px;
    margin: 4px 6px 4px 0;
    font-size: 13px;
    color: #2A2440;
  }
  .stat-label { color: #7A7490; font-size: 11px; display: block; }
  .stat-val   { font-weight: 800; font-size: 16px; color: #7C6BB5; }

  /* Cat avatar */
  .cat-avatar {
    width: 64px; height: 64px;
    border-radius: 50%;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    font-size: 28px;
    font-weight: 800;
    color: #fff;
    margin-right: 18px;
    vertical-align: middle;
  }
  .cat-name {
    font-size: 26px; font-weight: 800; color: #2A2440;
    vertical-align: middle;
  }
  .confidence-badge {
    display: inline-block;
    background: #7C6BB5;
    color: #fff;
    border-radius: 50px;
    padding: 3px 14px;
    font-size: 13px;
    font-weight: 700;
    margin-left: 12px;
    vertical-align: middle;
  }

  /* Alerts — text-first, light separator only */
  .alert-row {
    border-bottom: 1px solid #EDE9F2;
    padding: 10px 0 14px 0;
    margin-bottom: 0;
    font-size: 14px;
    color: #2A2440;
  }
  .alert-icon { margin-right: 8px; color: #7A7490; }

  /* Connection dot */
  .dot-green { color: #22C55E; font-size: 18px; }
  .dot-red   { color: #EF4444; font-size: 18px; }

  [data-testid="stDataFrame"] { border-radius: 6px; overflow: hidden; }

  /* Headings */
  h1 { font-family: 'Nunito', sans-serif !important; font-weight: 800 !important; }
  h2, h3 { font-family: 'Nunito', sans-serif !important; font-weight: 700 !important; }

</style>
""", unsafe_allow_html=True)

# ── Shared state (cache_resource: survives Streamlit reruns) ──────────────────
MAX_POINTS = 60


@st.cache_resource
def get_state():
    return {
        "connected": False,
        "serial_error": "",
        "weight_buf": deque(maxlen=MAX_POINTS),
        "time_buf": deque(maxlen=MAX_POINTS),
        "lock": threading.Lock(),
        "stop_event": threading.Event(),
        "ser": None,
    }


def serial_reader(port: str, baud: int = 115200):
    state = get_state()
    try:
        import serial  # type: ignore
    except ImportError:
        with state["lock"]:
            state["serial_error"] = "pyserial not installed."
        return

    ser = None
    try:
        ser = serial.Serial(port, baud, timeout=1)
        with state["lock"]:
            state["connected"] = True
            state["serial_error"] = ""
            state["ser"] = ser

        while not state["stop_event"].is_set():
            try:
                raw = ser.readline().decode("utf-8", errors="ignore").strip()
                if "val:" in raw:
                    val_str = raw.split("val:")[-1].strip()
                    value = float(val_str)
                    now = datetime.now()
                    with state["lock"]:
                        state["weight_buf"].append(value)
                        state["time_buf"].append(now)
            except (ValueError, UnicodeDecodeError):
                pass
            except Exception:
                break

    except Exception as e:
        with state["lock"]:
            state["connected"] = False
            state["serial_error"] = str(e)
    finally:
        if ser and ser.is_open:
            ser.close()
        with state["lock"]:
            state["connected"] = False
            state["ser"] = None


def start_reader(port: str):
    state = get_state()
    state["stop_event"].set()
    time.sleep(0.3)
    state["stop_event"].clear()
    t = threading.Thread(target=serial_reader, args=(port,), daemon=True)
    t.start()


# ── Placeholder data helpers ──────────────────────────────────────────────────
def make_visit_history():
    rng = np.random.default_rng(42)
    cats = ["Wesley", "Pupu"]
    rows = []
    base = datetime.now()
    for i in range(10):
        cat = cats[i % 2]
        visit_time = base - timedelta(minutes=15 * (i + 1))
        weight = rng.integers(3900, 5200)
        duration = round(rng.uniform(1.5, 5.5), 1)
        status = "⚠️ Anomaly" if (i == 2 or i == 7) else "✅ Normal"
        rows.append({
            "Time": visit_time.strftime("%H:%M"),
            "Cat": cat,
            "Weight (g)": weight,
            "Duration (min)": duration,
            "Status": status,
        })
    return pd.DataFrame(rows)


def make_daily_weight():
    rng = np.random.default_rng(7)
    dates = [datetime.now() - timedelta(days=i) for i in range(13, -1, -1)]
    labels = [d.strftime("%m/%d") for d in dates]
    wesley = 4300 + rng.integers(-150, 200, 14)
    pupu = 4700 + rng.integers(-120, 180, 14)
    return pd.DataFrame({"Date": labels, "Wesley (g)": wesley, "Pupu (g)": pupu})


def make_daily_visits():
    rng = np.random.default_rng(13)
    dates = [datetime.now() - timedelta(days=i) for i in range(13, -1, -1)]
    labels = [d.strftime("%m/%d") for d in dates]
    wesley = rng.integers(2, 6, 14)
    pupu = rng.integers(1, 5, 14)
    return pd.DataFrame({"Date": labels, "Wesley": wesley, "Pupu": pupu})


def visit_history_status_style(val):
    if "Anomaly" in val:
        return "background-color: #FFF7ED; color: #92400E; font-weight: 700;"
    return ""


# ── Sidebar ───────────────────────────────────────────────────────────────────
with st.sidebar:
    st.markdown("### Connection")
    port = st.text_input("Serial port", value="/dev/cu.usbmodem1101", help="macOS: often under /dev/cu.usbmodem…")

    col_btn1, col_btn2 = st.columns(2)
    with col_btn1:
        connect_btn = st.button("Connect", use_container_width=True)
    with col_btn2:
        disconnect_btn = st.button("Disconnect", use_container_width=True)

    if connect_btn:
        start_reader(port)

    if disconnect_btn:
        state = get_state()
        state["stop_event"].set()
        with state["lock"]:
            state["connected"] = False

    state = get_state()
    with state["lock"]:
        is_connected = state["connected"]
        err_msg = state["serial_error"]

    if is_connected:
        st.markdown('<span class="dot-green">●</span> Connected', unsafe_allow_html=True)
        tare_btn = st.button("⚖️ Tare (Zero)", use_container_width=True)
        if tare_btn:
            state = get_state()
            with state["lock"]:
                ser_obj = state["ser"]
            if ser_obj and ser_obj.is_open:
                ser_obj.write(b't')
                st.toast("Tare command sent — scale zeroed!", icon="⚖️")
    else:
        st.markdown('<span class="dot-red">●</span> Disconnected', unsafe_allow_html=True)
        if err_msg:
            st.caption(err_msg)

    st.divider()

    st.markdown("**Hardware**")
    st.caption("Board: XIAO ESP32S3")
    st.caption("Sensor: HX711 load cell · 115200 baud")

    st.divider()

    st.markdown("**About**")
    st.caption(
        "Tracks litter visits, weight, and patterns. "
        "Built by Mengqi Shi, Yuna Xiong, and Xin Luo."
    )


# ── Header ────────────────────────────────────────────────────────────────────
st.markdown("""
<h1 style='font-size:38px; font-weight:800; color:#2A2440; margin-bottom:2px;'>
  🐾 Smart Cat Litter Box Monitor
</h1>
<p style='font-size:16px; color:#7A7490; margin-top:0; margin-bottom:28px;'>
  Keeping your cats healthy, one visit at a time.
</p>
""", unsafe_allow_html=True)

# ── Section 1: Live Weight Monitor ────────────────────────────────────────────
st.markdown('<div class="section-title">⚖️ Live Weight Monitor</div>', unsafe_allow_html=True)
st.markdown('<div class="section-sub">Real-time load cell reading from HX711 • updates every 500 ms</div>', unsafe_allow_html=True)

weight_num_slot = st.empty()
chart_slot = st.empty()
stats_slot = st.empty()

st.divider()

# ── Section 2: Cat Identification (Placeholder) ───────────────────────────────
st.markdown('<div class="section-title">🐱 Cat Identification</div>', unsafe_allow_html=True)
st.markdown('<div class="section-sub">ML model identifies which cat is using the box based on weight signature — placeholder data</div>', unsafe_allow_html=True)

ci1, ci2, ci3, ci4 = st.columns([1.2, 1, 1, 1])

with ci1:
    st.markdown("""
    <div style="display:flex; align-items:center; gap:14px; padding:12px 0;">
      <div style="width:64px; height:64px; border-radius:50%; background:#7C6BB5;
                  display:flex; align-items:center; justify-content:center;
                  font-size:30px; flex-shrink:0;">🐱</div>
      <div>
        <div style="font-size:22px; font-weight:800; color:#2A2440; line-height:1.1;">Wesley</div>
        <div style="font-size:12px; color:#7A7490; margin-top:3px;">Most Recent Visit</div>
      </div>
    </div>
    """, unsafe_allow_html=True)

with ci2:
    st.markdown("""
    <div style="padding:12px 0;">
      <div style="font-size:12px; color:#7A7490; margin-bottom:4px;">Detected Weight</div>
      <div style="font-size:28px; font-weight:800; color:#7C6BB5; line-height:1;">4,312 g</div>
    </div>
    """, unsafe_allow_html=True)

with ci3:
    st.markdown("""
    <div style="padding:12px 0;">
      <div style="font-size:12px; color:#7A7490; margin-bottom:4px;">Confidence</div>
      <div style="font-size:28px; font-weight:800; color:#2A2440; line-height:1;">92.4%</div>
      <span style="display:inline-block; background:#7C6BB5; color:#fff; border-radius:50px;
                   padding:2px 12px; font-size:12px; font-weight:700; margin-top:4px;">High</span>
    </div>
    """, unsafe_allow_html=True)

with ci4:
    st.markdown("""
    <div style="padding:12px 0;">
      <div style="font-size:12px; color:#7A7490; margin-bottom:4px;">Visit Time</div>
      <div style="font-size:22px; font-weight:800; color:#2A2440; line-height:1.2;">10:32 AM</div>
      <div style="font-size:14px; color:#7A7490; margin-top:4px;">Duration: 3.2 min</div>
    </div>
    """, unsafe_allow_html=True)

st.caption("Roster (placeholder): Wesley ~4.3 kg, Pupu ~4.7 kg · weight-based K-NN classifier.")

st.divider()

# ── Section 3: Visit History (Placeholder) ────────────────────────────────────
st.markdown('<div class="section-title">📋 Visit History</div>', unsafe_allow_html=True)
st.markdown('<div class="section-sub">Recent litter box visits — placeholder data</div>', unsafe_allow_html=True)

history_df = make_visit_history()
styled = history_df.style.map(visit_history_status_style, subset=["Status"])
st.dataframe(
    styled,
    use_container_width=True,
    hide_index=True,
    height=360,
)

st.divider()

# ── Section 4: Health Trends (Placeholder) ────────────────────────────────────
st.markdown('<div class="section-title">📈 Health Trends</div>', unsafe_allow_html=True)
st.markdown('<div class="section-sub">14-day overview — placeholder data</div>', unsafe_allow_html=True)

col_w, col_v = st.columns(2)

weight_df = make_daily_weight()
visits_df = make_daily_visits()

with col_w:
    st.markdown("**Daily Average Weight (g)**")
    chart_w = weight_df.set_index("Date")[["Wesley (g)", "Pupu (g)"]]
    st.line_chart(
        chart_w,
        color=["#7C6BB5", "#E88FB4"],
        use_container_width=True,
        height=240,
    )

with col_v:
    st.markdown("**Daily Visit Count**")
    chart_v = visits_df.set_index("Date")[["Wesley", "Pupu"]]
    st.line_chart(
        chart_v,
        color=["#7C6BB5", "#E88FB4"],
        use_container_width=True,
        height=240,
    )

st.markdown("""
<div style="font-size:13px; color:#7A7490; margin-top:8px; display:flex; align-items:center; gap:18px; flex-wrap:wrap;">
  <span style="display:inline-flex; align-items:center; gap:7px;">
    <span style="display:inline-block; width:11px; height:11px; border-radius:999px; background:#7C6BB5;"></span>
    <span style="color:#2A2440;">Wesley</span>
  </span>
  <span style="display:inline-flex; align-items:center; gap:7px;">
    <span style="display:inline-block; width:11px; height:11px; border-radius:999px; background:#E88FB4;"></span>
    <span style="color:#2A2440;">Pupu</span>
  </span>
</div>
""", unsafe_allow_html=True)

st.divider()

# ── Section 5: Anomaly Alerts (Placeholder) ───────────────────────────────────
st.markdown('<div class="section-title">🚨 Anomaly Alerts</div>', unsafe_allow_html=True)
st.markdown('<div class="section-sub">Recent health flags — placeholder data</div>', unsafe_allow_html=True)

alerts = [
    ("⚠️", "2026-05-08 09:14", "Wesley's weight dropped 210 g over the last 3 days. Consider a vet check-up."),
    ("⚠️", "2026-05-07 18:02", "Pupu visited 6 times yesterday — above the normal baseline of 2–4 visits/day."),
    ("ℹ️", "2026-05-06 11:45", "Wesley's average visit duration increased to 5.2 min (baseline: 3.1 min)."),
    ("✅", "2026-05-05 08:30", "All metrics normal for both cats today."),
]

for icon, ts, msg in alerts:
    st.markdown(f"""
    <div class="alert-row">
      <span class="alert-icon">{icon}</span>
      <b style="font-size:11px; color:#7A7490;">{ts}</b><br>
      {msg}
    </div>
    """, unsafe_allow_html=True)

# ── Live update loop ──────────────────────────────────────────────────────────
while True:
    state = get_state()
    with state["lock"]:
        weights = list(state["weight_buf"])
        times = list(state["time_buf"])
        connected = state["connected"]

    if weights:
        current_w = weights[-1]
        valid = [w for w in weights if w >= 50]

        # Big number — hide negative readings
        if current_w < 50:
            weight_num_slot.markdown("""
            <div style="margin: 12px 0 6px;">
              <span class="weight-number" style="color:#C4BDD8;">--</span>
              <span class="weight-unit">g &nbsp;<span style="font-size:16px;color:#7A7490;">No load detected</span></span>
            </div>
            """, unsafe_allow_html=True)
        else:
            weight_num_slot.markdown(f"""
            <div style="margin: 12px 0 6px;">
              <span class="weight-number">{current_w:,.1f}</span>
              <span class="weight-unit">g</span>
            </div>
            """, unsafe_allow_html=True)

        # Line chart — only valid readings
        if valid:
            valid_times = [t for w, t in zip(weights, times) if w >= 50]
            df_chart = pd.DataFrame({"Weight (g)": valid}, index=pd.to_datetime(valid_times))
            chart_slot.line_chart(df_chart, color=["#7C6BB5"], use_container_width=True, height=200)
        else:
            chart_slot.empty()

        # Stats — only valid readings
        if valid:
            v_arr = np.array(valid)
            stats_slot.markdown(f"""
            <div style="margin-top:10px;">
              <span class="stat-pill"><span class="stat-label">Min</span><span class="stat-val">{v_arr.min():,.1f} g</span></span>
              <span class="stat-pill"><span class="stat-label">Max</span><span class="stat-val">{v_arr.max():,.1f} g</span></span>
              <span class="stat-pill"><span class="stat-label">Mean</span><span class="stat-val">{v_arr.mean():,.1f} g</span></span>
              <span class="stat-pill"><span class="stat-label">Std Dev</span><span class="stat-val">{v_arr.std():,.1f} g</span></span>
              <span class="stat-pill"><span class="stat-label">Readings</span><span class="stat-val">{len(valid)}</span></span>
            </div>
            """, unsafe_allow_html=True)
        else:
            stats_slot.empty()

    else:
        if connected:
            weight_num_slot.markdown("""
            <div style="margin:12px 0; color:#7A7490; font-size:16px;">
              ⏳ Waiting for first reading…
            </div>
            """, unsafe_allow_html=True)
        else:
            weight_num_slot.markdown("""
            <div style="margin:12px 0; padding:14px 20px; background:#FFF7ED;
                        border-left:4px solid #F5A623; border-radius:0 12px 12px 0;
                        color:#92400E; font-size:15px;">
              🔌 Serial port not connected. Click <b>Connect</b> in the sidebar,
              or check that the ESP32S3 is plugged in.
            </div>
            """, unsafe_allow_html=True)
            chart_slot.empty()
            stats_slot.empty()

    time.sleep(0.5)
