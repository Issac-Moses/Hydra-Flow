/* ═══════════════════════════════════════════════════
   TankSync Dashboard Logic (v6.0)
   - Fixed: isRunning declaration order
   - Fixed: Tab switching
   - Fixed: Heartbeat display
   - Added: Full Analytics with Chart.js
   - Added: fetchLogs realtime timeline
═══════════════════════════════════════════════════ */

const SUPABASE_URL = 'https://ulyeoukxyaadfvekdmwc.supabase.co';
const SUPABASE_KEY = 'eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InVseWVvdWt4eWFhZGZ2ZWtkbXdjIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzM5ODk1NTcsImV4cCI6MjA4OTU2NTU1N30.w_37uATvP3SFiWPUvUtzPaGTs4oYQQok1uW7RXe5luE';

const db = supabase.createClient(SUPABASE_URL, SUPABASE_KEY);

const wrapTimeout = (promise, ms = 15000) =>
    Promise.race([promise, new Promise((_, rej) => setTimeout(() => rej(new Error('Timeout')), ms))]);

// ── STATE ─────────────────────────────────────────
let realtimeChannel = null;
let usageChart = null;
let currentTab = 'live';
let lastKnownData = null;
let localIp = localStorage.getItem('tanksync_local_ip') || '';
let localMode = localStorage.getItem('tanksync_local_mode') === 'true';

// ── NEW PREFERENCES ──────────────────────────────
let lang = localStorage.getItem('tanksync_lang') || 'en';
let voiceEnabled = localStorage.getItem('tanksync_voice') !== 'false';

const I18N_DICT = {
    en: {
        dashboard: "Dashboard", analytics: "Analytics", weather: "Local Weather",
        water_level: "Water Level", motor_control: "Motor Control", schedule: "Auto Schedule",
        logs: "Activity Logs", energy: "Energy Today", week_summary: "7-Day Summary",
        temp: "Temp", rain: "Rain Chance", humidity: "Humidity",
        motor_active: "MOTOR [ACTIVE]", tank_offline: "TANK [OFFLINE]",
        motor_on: "Motor has been started", motor_off: "Motor has been stopped",
        tank_full: "Warning: Tank is 100% full", tank_empty: "Alert: Tank is empty",
        bill_predict: "Monthly Predictor", daily_avg: "Daily Avg", efficiency: "Efficiency",
        insights: "System Insights", pumps: "Pumps", runtime: "Avg Run",
        history: "History", settings: "Settings", preferences: "Preferences"
    },
    ta: {
        dashboard: "முகப்பு", analytics: "புள்ளிவிவரம்", weather: "உள்ளூர் வானிலை",
        water_level: "நீர் மட்டம்", motor_control: "மோட்டார் கட்டுப்பாடு", schedule: "தானியங்கி அட்டவணை",
        logs: "நடவடிக்கை பதிவுகள்", energy: "இன்றைய மின்சாரம்", week_summary: "7-நாள் சுருக்கம்",
        temp: "வெப்பநிலை", rain: "மழை வாய்ப்பு", humidity: "ஈரப்பதம்",
        motor_active: "மோட்டார் [செயலில்]", tank_offline: "டேங்க் [ஆஃப்லைன்]",
        motor_on: "மோட்டார் இயக்கப்பட்டது", motor_off: "மோட்டார் நிறுத்தப்பட்டது",
        tank_full: "எச்சரிக்கை: டேங்க் 100% நிறைந்தது", tank_empty: "எச்சரிக்கை: டேங்க் காலியாக உள்ளது",
        bill_predict: "மாதாந்திர முன்னறிவிப்பு", daily_avg: "தினசரி சராசரி", efficiency: "செயல்திறன்",
        insights: "அமைப்பின் நுண்ணறிவு", pumps: "இயக்கங்கள்", runtime: "இயக்க நேரம்",
        history: "வரலாறு", settings: "அமைப்புகள்", preferences: "விருப்பங்கள்"
    }
};

// ── UI ELEMENTS ───────────────────────────────────
const elLevelText = () => document.getElementById('levelText');
const elLevelDesc = () => document.getElementById('levelDesc');
const elWaterFill = () => document.getElementById('waterFill');
const elTankVisual = () => document.querySelector('.tank-visual');
const elMotorState = () => document.getElementById('motorStateText');
const elMotorInd = () => document.getElementById('motorIndicator');
const elLastUpdated = () => document.getElementById('lastUpdated');
const elTankStatus = () => document.getElementById('tankStatus');
const elMotorNode = () => document.getElementById('motorNodeStatus');
const elTankNode = () => document.getElementById('tankNodeStatus');
const elScheduleList = () => document.getElementById('scheduleList');
const elScheduleInfo = () => document.getElementById('scheduleStatusInfo');
const elLogList = () => document.getElementById('logList');

// ── TAB SWITCHING ─────────────────────────────────
function switchTab(tabId) {
    currentTab = tabId;
    document.querySelectorAll('.tab-content').forEach(tab => tab.classList.remove('active'));
    document.querySelectorAll('.nav-links li').forEach(li => li.classList.remove('active'));

    const activeTab = document.getElementById(`tab-${tabId}`);
    const activeLink = document.querySelector(`.nav-links li[data-tab="${tabId}"]`);

    if (activeTab) activeTab.classList.add('active');
    if (activeLink) activeLink.classList.add('active');

    // Trigger data refresh based on tab
    if (tabId === 'analytics') fetchAnalytics();
    if (tabId === 'history') fetchLogs();
    if (tabId === 'schedules') fetchSchedules();
    if (tabId === 'weather') fetchWeather();
}

// ── CORE FETCH & UPDATE ───────────────────────────
async function fetchState() {
    try {
        const { data, error } = await wrapTimeout(
            db.from('motor_system').select('*').eq('id', 1).single()
        );
        if (error) throw error;
        if (data) {
            const prevStatus = lastKnownData?.motor_status;
            const prevLevel = lastKnownData?.water_level;

            updateUI(data);

            // Voice Alerts for state changes
            if (prevStatus === false && data.motor_status === true) announce('motor_on');
            if (prevStatus === true && data.motor_status === false) announce('motor_off');
            if (prevLevel < 100 && data.water_level === 100) announce('tank_full');
        }
    } catch (err) {
        console.error('[Sync Error]', err.message);
    }
}

function updateUI(data) {
    lastKnownData = data;  // ✅ Cache for the heartbeat interval

    // ── 1. Determine all state values FIRST ──────
    const level = data.water_level ?? 0;
    const isRunning = data.motor_status ?? false;
    const now = new Date();

    const tankLastSeen = data.tank_last_seen ? new Date(data.tank_last_seen) : new Date(0);
    const motorLastSeen = data.motor_last_seen ? new Date(data.motor_last_seen) : new Date(0);
    const tankOnline = (now - tankLastSeen) / 60000 < 2;
    const motorOnline = (now - motorLastSeen) / 60000 < 2;

    // ── 2. Water Level ───────────────────────────
    const levelTextEl = elLevelText();
    const levelDescEl = elLevelDesc();
    const waterFillEl = elWaterFill();
    const tankVisualEl = elTankVisual();

    if (levelTextEl) levelTextEl.innerText = `${level}%`;
    if (waterFillEl) waterFillEl.style.height = `${level}%`;

    if (tankVisualEl) {
        tankVisualEl.classList.remove('empty', 'filling', 'full');
        if (level >= 100) {
            tankVisualEl.classList.add('full');
            if (levelDescEl) levelDescEl.innerText = 'FULL';
        } else if (level === 0) {
            tankVisualEl.classList.add('empty');
            if (levelDescEl) levelDescEl.innerText = 'EMPTY';
        } else {
            tankVisualEl.classList.add('filling');
            if (levelDescEl) levelDescEl.innerText = isRunning ? 'FILLING...' : 'PARTIAL';
        }
    }

    // ── 3. Motor Indicator ───────────────────────
    const motorIndEl = elMotorInd();
    const motorStateEl = elMotorState();
    if (motorIndEl) {
        motorIndEl.classList.toggle('on', isRunning);
        motorIndEl.classList.toggle('off', !isRunning);
    }
    if (motorStateEl) motorStateEl.innerText = isRunning ? 'ON' : 'OFF';

    // ── 4. Last Sync ─────────────────────────────
    const lastUpdatedEl = elLastUpdated();
    if (lastUpdatedEl) {
        lastUpdatedEl.innerText = now.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
    }

    // ── 5. Tank Node Badge (header + card) ───────
    const tankStatusCardEl = elTankStatus();    // In "Water Level" card stats
    const tankNodeEl = elTankNode();      // In header heartbeats

    if (tankStatusCardEl) {
        tankStatusCardEl.innerText = tankOnline ? 'ACTIVE' : 'OFFLINE';
        tankStatusCardEl.classList.toggle('active', tankOnline);
        tankStatusCardEl.classList.toggle('offline', !tankOnline);
    }
    if (tankNodeEl) {
        const dot = tankNodeEl.querySelector('.pulse-dot');
        tankNodeEl.classList.toggle('active', tankOnline);
        tankNodeEl.lastChild.textContent = tankOnline ? ' TANK [ACTIVE]' : ' TANK [OFFLINE]';
        if (dot) dot.classList.toggle('offline', !tankOnline);
    }

    // ── 6. Motor Node Badge (header) ─────────────
    const motorNodeEl = elMotorNode();
    if (motorNodeEl) {
        const dot = motorNodeEl.querySelector('.pulse-dot');
        motorNodeEl.classList.toggle('active', motorOnline);
        motorNodeEl.lastChild.textContent = motorOnline ? ' MOTOR [ACTIVE]' : ' MOTOR [OFFLINE]';
        if (dot) dot.classList.toggle('offline', !motorOnline);
    }
}

// ── ADVANCED UTILITIES ───────────────────────────
function updateLanguage() {
    const btn = document.getElementById('btnLangToggleS');
    if (btn) btn.innerText = lang === 'en' ? '🌐 EN' : '🌐 தமிழ்';

    document.querySelectorAll('[data-i18n]').forEach(el => {
        const key = el.getAttribute('data-i18n');
        if (I18N_DICT[lang][key]) {
            el.innerText = I18N_DICT[lang][key];
        }
    });

    // Tab Labels
    const setLabel = (tid, key) => {
        const el = document.querySelector(`.nav-links li[data-tab="${tid}"] .label`);
        if (el) el.innerText = I18N_DICT[lang][key];
    };
    setLabel('live', 'dashboard');
    setLabel('analytics', 'analytics');
    setLabel('history', 'history');
    setLabel('schedules', 'schedule');
    setLabel('weather', 'weather');
    setLabel('settings', 'settings');
}

function announce(key) {
    if (!voiceEnabled) return;
    const text = I18N_DICT[lang][key];
    if (!text) return;

    const utterance = new SpeechSynthesisUtterance(text);
    utterance.lang = lang === 'en' ? 'en-US' : 'ta-IN';
    utterance.rate = 0.95;
    window.speechSynthesis.speak(utterance);
}

async function fetchWeather() {
    const el = document.getElementById('weatherContent');
    const fullEl = document.getElementById('weatherFullContent');
    try {
        const lat = 13.0827;
        const lon = 80.2707;

        const url = `https://api.open-meteo.com/v1/forecast?latitude=${lat}&longitude=${lon}&current=temperature_2m,relative_humidity_2m,precipitation,weather_code&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max&hourly=precipitation_probability&timezone=auto&forecast_days=7`;

        const res = await fetch(url);
        const data = await res.json();

        // ── 1. Dashboard Card (Today) ──
        const temp = data.current.temperature_2m;
        const hum = data.current.relative_humidity_2m;
        const rain = data.hourly.precipitation_probability[new Date().getHours()];
        const code = data.current.weather_code;

        if (document.getElementById('weatherTemp')) document.getElementById('weatherTemp').innerText = `${temp}°C`;
        if (document.getElementById('weatherHum')) document.getElementById('weatherHum').innerText = `${hum}%`;
        if (document.getElementById('weatherRain')) document.getElementById('weatherRain').innerText = `${rain}%`;

        let icon = '☀️'; let desc = 'Clear';
        if (code > 0) { icon = '⛅'; desc = 'Cloudy'; }
        if (code > 50) { icon = '🌧️'; desc = 'Rainy'; }
        if (code > 80) { icon = '⛈️'; desc = 'Storm'; }

        if (el) {
            el.innerHTML = `
                <div class="weather-main-info">
                    <span class="weather-icon">${icon}</span>
                    <span class="weather-desc">${desc}</span>
                </div>
            `;
        }

        // ── 2. Full Forecast (7 Days) ──
        if (fullEl) {
            let forecastHtml = '';
            for (let i = 0; i < 7; i++) {
                const dayDate = new Date(data.daily.time[i]);
                const dayName = i === 0 ? 'Today' : dayDate.toLocaleDateString([], { weekday: 'short' });
                const dayCode = data.daily.weather_code[i];
                const maxT = data.daily.temperature_2m_max[i];
                const minT = data.daily.temperature_2m_min[i];
                const prob = data.daily.precipitation_probability_max[i];

                let dIcon = '☀️';
                if (dayCode > 0) dIcon = '⛅';
                if (dayCode > 50) dIcon = '🌧️';
                if (dayCode > 80) dIcon = '⛈️';

                forecastHtml += `
                    <div class="forecast-day">
                        <span class="day-name">${dayName}</span>
                        <span class="day-icon">${dIcon}</span>
                        <span class="day-temp">${Math.round(maxT)}° / ${Math.round(minT)}°</span>
                        <span class="day-rain">💧 ${prob}%</span>
                    </div>
                `;
            }
            fullEl.innerHTML = forecastHtml;
        }
    } catch (err) {
        if (el) el.innerHTML = '<div class="weather-loading">Weather error</div>';
    }
}

// ── MOTOR COMMANDS ────────────────────────────────
async function setMotor(status) {
    if (localMode && localIp) {
        try {
            const endpoint = status ? '/on' : '/off';
            const res = await fetch(`http://${localIp}${endpoint}`, { mode: 'cors' });
            if (!res.ok) throw new Error('Local device unreachable');
            const data = await res.json();
            console.log('[Local] Motor command success:', data);

            // Announce local command
            announce(status ? 'motor_on' : 'motor_off');

            // Still update Supabase if possible so logs are recorded
            db.from('motor_system').update({ motor_status: status, manual_override: true, mode: 'LOCAL' }).eq('id', 1).then();
            return;
        } catch (err) {
            console.warn('[Local Mode Failed]', err.message);
            if (!confirm('Local control failed. Try Supabase (Internet)?')) return;
        }
    }

    try {
        const { error } = await wrapTimeout(
            db.from('motor_system')
                .update({ motor_status: status, manual_override: true, mode: 'ONLINE' })
                .eq('id', 1)
        );
        if (error) throw error;
    } catch (err) {
        alert('Motor command failed: ' + err.message);
    }
}

// ── SCHEDULE MANAGEMENT ───────────────────────────
async function fetchSchedules() {
    try {
        const { data, error } = await wrapTimeout(
            db.from('motor_schedules').select('*').order('on_time', { ascending: true })
        );
        if (error) throw error;
        renderSchedules(data || []);
    } catch (err) {
        console.error('[Schedule Error]', err.message);
        const el = elScheduleList();
        if (el) el.innerHTML = `<div class="empty-state error-state">⚠ Failed to load. <button onclick="fetchSchedules()" style="margin-left:8px;cursor:pointer;background:none;border:none;color:var(--primary);font-weight:700;font-size:0.9rem">RETRY</button></div>`;
    }
}

function renderSchedules(list) {
    const el = elScheduleList();
    if (!el) return;

    if (list.length === 0) {
        el.innerHTML = '<div class="empty-state">No active schedules.</div>';
        const infoEl = elScheduleInfo();
        if (infoEl) infoEl.innerText = 'Set multiple ON times throughout the day.';
        return;
    }

    el.innerHTML = list.map(item => `
        <div class="schedule-item">
            <div class="schedule-item-info">
                <span class="time">${item.on_time}</span>
                <span>Daily Auto-Start</span>
            </div>
            <div class="schedule-item-actions">
                <label class="toggle-switch">
                    <input type="checkbox" ${item.enabled ? 'checked' : ''} onchange="toggleSchedule(${item.id}, this.checked)">
                    <span class="toggle-slider"></span>
                </label>
                <button class="btn-delete" onclick="deleteSchedule(${item.id})" title="Delete">✕</button>
            </div>
        </div>
    `).join('');

    const activeList = list.filter(s => s.enabled);
    const infoEl = elScheduleInfo();

    // Find next schedule
    if (infoEl) {
        if (activeList.length > 0) {
            const now = new Date();
            const currentTime = now.getHours().toString().padStart(2, '0') + ':' + now.getMinutes().toString().padStart(2, '0');

            let next = activeList.find(s => s.on_time > currentTime);
            if (!next) next = activeList[0]; // first one tomorrow

            infoEl.innerHTML = `<span style="color:var(--primary)">Next: ${next.on_time}</span> • ${activeList.length} active`;
        } else {
            infoEl.innerText = 'Set multiple ON times throughout the day.';
        }
    }
}

async function addSchedule() {
    const inp = document.getElementById('newScheduleTime');
    if (!inp || !inp.value) return;
    try {
        document.getElementById('btnAddSchedule').disabled = true;
        const { error } = await wrapTimeout(db.from('motor_schedules').insert([{ on_time: inp.value, enabled: true }]));
        if (error) throw error;
        inp.value = '06:00';
        fetchSchedules(); // Manual refresh for responsiveness
    } catch (err) {
        alert('Add failed: ' + err.message);
    } finally {
        document.getElementById('btnAddSchedule').disabled = false;
    }
}

async function deleteSchedule(id) {
    if (!confirm('Delete this schedule?')) return;
    try {
        const { error } = await wrapTimeout(db.from('motor_schedules').delete().eq('id', id));
        if (error) throw error;
        fetchSchedules(); // Manual refresh
    } catch (err) { alert('Delete failed: ' + err.message); }
}

async function toggleSchedule(id, enabled) {
    try {
        const { error } = await wrapTimeout(db.from('motor_schedules').update({ enabled }).eq('id', id));
        if (error) throw error;
        fetchSchedules(); // Manual refresh
    } catch (err) { alert('Update failed: ' + err.message); }
}

// ── ACTIVITY LOGS ─────────────────────────────────
async function fetchLogs() {
    const el = elLogList();
    if (el) el.innerHTML = '<div class="schedule-loading">Loading activity logs...</div>';

    try {
        const { data, error } = await wrapTimeout(
            db.from('motor_logs').select('*').order('timestamp', { ascending: false }).limit(50)
        );
        if (error) throw error;

        const list = document.getElementById('logList');
        const count = document.getElementById('logCount');
        if (count) count.innerText = data?.length || 0;

        if (list) {
            renderLogs(data || []);
        }
    } catch (err) {
        console.error('[Logs Error]', err.message);
        if (el) el.innerHTML = `<div class="empty-state error-state">⚠ Failed to load logs. <button onclick="fetchLogs()" style="margin-left:8px;cursor:pointer;background:none;border:none;color:var(--secondary);font-weight:700">RETRY</button></div>`;
    }
}

function renderLogs(logs) {
    const el = elLogList();
    if (!el) return;

    if (logs.length === 0) {
        el.innerHTML = '<div class="empty-state">No recorded activity yet.</div>';
        return;
    }

    el.innerHTML = logs.map(log => {
        const t = new Date(log.timestamp);
        const time = t.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
        const date = t.toLocaleDateString([], { month: 'short', day: 'numeric' });
        const cls = log.status ? 'on' : 'off';
        const icon = log.status ? '▶' : '■';
        const label = log.status ? 'Motor STARTED' : 'Motor STOPPED';

        return `
        <div class="log-entry ${cls}">
            <div class="log-icon">${icon}</div>
            <div class="log-content">
                <span class="status">${label}</span>
                <span class="reason">Water Level: ${log.water_level}%</span>
            </div>
            <div class="log-time">
                <span class="time">${time}</span>
                <span class="date">${date}</span>
            </div>
        </div>`;
    }).join('');
}

// CSV Download
function downloadCSV() {
    db.from('motor_logs').select('*').order('timestamp', { ascending: false }).limit(100)
        .then(({ data }) => {
            if (!data || data.length === 0) return alert('No logs to download.');
            const header = 'Timestamp,Status,Water Level,Mode\n';
            const rows = data.map(r => `${r.timestamp},${r.status ? 'ON' : 'OFF'},${r.water_level}%,${r.mode}`).join('\n');
            const blob = new Blob([header + rows], { type: 'text/csv' });
            const url = URL.createObjectURL(blob);
            const a = document.createElement('a'); a.href = url; a.download = 'tanksync_logs.csv'; a.click();
        });
}

// ── ANALYTICS ─────────────────────────────────────
async function fetchAnalytics() {
    const chartCard = document.querySelector('.analytics-chart-card');
    try {
        const sevenDaysAgo = new Date();
        sevenDaysAgo.setDate(sevenDaysAgo.getDate() - 7);

        const { data: logs, error } = await wrapTimeout(
            db.from('motor_logs').select('*')
                .gte('timestamp', sevenDaysAgo.toISOString())
                .order('timestamp', { ascending: true })
        );
        if (error) throw error;

        // Get current system state
        const { data: sys } = await wrapTimeout(
            db.from('motor_system').select('motor_status, water_level').eq('id', 1).single()
        );

        buildAnalytics(logs || [], sys);

    } catch (err) {
        console.error('[Analytics Error]', err.message);
        const set = (id, val) => { const e = document.getElementById(id); if (e) e.innerText = val; };
        ['kwhUnits', 'totalCost', 'todayTotal', 'kwhWeek', 'costWeek', 'weekTotal', 'totalStarts', 'avgDaily', 'avgDailyStat'].forEach(id => set(id, 'ERR'));

        if (chartCard) {
            chartCard.style.opacity = '0.5';
            chartCard.title = 'Failed to load analytics data: ' + err.message;
        }
    }
}

function buildAnalytics(logs, sys) {
    // ── 1. Create a stable internal map using YYYY-MM-DD keys ──────
    const dailyMinutes = {};
    const dailyStarts = {};
    const displayLabels = [];
    const internalKeys = [];

    for (let i = 6; i >= 0; i--) {
        const d = new Date();
        d.setDate(d.getDate() - i);

        const internalKey = d.toISOString().split('T')[0]; // "2024-03-31"
        const displayLabel = d.toLocaleDateString([], { month: 'short', day: 'numeric' }); // "Mar 31"

        internalKeys.push(internalKey);
        displayLabels.push(displayLabel);
        dailyMinutes[internalKey] = 0;
        dailyStarts[internalKey] = 0;
    }

    // ── 2. Process Logs & Pair Events ────────────────────────────
    const totalLogs = logs.length;
    for (let i = 0; i < totalLogs; i++) {
        const log = logs[i];
        if (!log.status) continue; // Skip STOP events as we process them when we find START

        const onTime = new Date(log.timestamp);
        const dayKey = onTime.toISOString().split('T')[0];

        if (dailyMinutes[dayKey] === undefined) continue;
        dailyStarts[dayKey]++;

        // Find the next STOP event for this motor start
        let nextOff = logs.slice(i + 1).find(l => !l.status && new Date(l.timestamp) > onTime);

        let endTime;
        if (nextOff) {
            endTime = new Date(nextOff.timestamp);
        } else if (log.status && i === 0 && sys && sys.motor_status) {
            // If the latest log is ON and motor is currently running, use 'now'
            endTime = new Date();
        } else {
            // If no STOP found, assume 0 or safety limit? Let's skip or use cap.
            continue;
        }

        const durationMinutes = (endTime - onTime) / 60000;
        // Cap at 60 mins just in case, but system has 30m safety.
        dailyMinutes[dayKey] += Math.max(0, Math.min(durationMinutes, 60));
    }

    // ── 3. Calculate Totals ──────────────────────────────────────
    const PUMP_WATT = 750;
    const COST_PER_KWH = 8;

    const todayKey = new Date().toISOString().split('T')[0];
    const totalMinsToday = dailyMinutes[todayKey] || 0;
    const totalMinsWeek = Object.values(dailyMinutes).reduce((a, b) => a + b, 0);

    const kwhToday = (PUMP_WATT * totalMinsToday) / 60 / 1000;
    const costToday = kwhToday * COST_PER_KWH;
    const kwhWeek = (PUMP_WATT * totalMinsWeek) / 60 / 1000;
    const costWeek = kwhWeek * COST_PER_KWH;
    const totalStarts = Object.values(dailyStarts).reduce((a, b) => a + b, 0);
    const avgDailyMins = (totalMinsWeek / 7).toFixed(1);

    // ── 4. Update Stat Cards ─────────────────────────────────────
    const set = (id, val) => { const e = document.getElementById(id); if (e) e.innerText = val; };

    set('kwhUnits', kwhToday.toFixed(3));
    set('totalCost', `₹${costToday.toFixed(2)}`);
    set('todayTotal', `${Math.round(totalMinsToday)} min`);

    set('kwhWeek', kwhWeek.toFixed(2));
    set('costWeek', `₹${costWeek.toFixed(2)}`);
    set('weekTotal', `${Math.round(totalMinsWeek)} min`);

    set('totalStarts', totalStarts);
    set('avgDaily', `${avgDailyMins} min`);
    set('avgDailyStat', `${avgDailyMins} min`); // Updated ID

    if (sys) {
        set('analyticsLevel', `${sys.water_level}%`);
    }

    // ── 5. Bill Prediction & Health Logic ─────────────────────────
    const costSum = costWeek;
    const avgCost = costSum / 7;
    const projBill = avgCost * 30;

    set('dailyAvgCost', `₹${avgCost.toFixed(2)}`);
    set('projBill', `₹${projBill.toFixed(0)}`);
    const billSub = document.getElementById('projBillSub');
    if (billSub) billSub.innerText = lang === 'en' ? 'ESTIMATED MONTHLY COST' : 'மதிப்பிடப்பட்ட மாத செலவு';

    const pumpsPerDay = (totalStarts / 7).toFixed(1);
    const avgRun = totalStarts > 0 ? (totalMinsWeek / totalStarts) : 0;

    set('pumpsPerDay', pumpsPerDay);
    set('avgRuntime', `${avgRun.toFixed(1)} min`);

    const healthEl = document.getElementById('sysHealth');
    const tipsEl = document.getElementById('healthTips');
    if (healthEl && tipsEl) {
        let efficiencyMsg = 'OPTIMAL';
        let tipMsg = 'Your system is running efficiently. No anomalies detected.';

        if (avgRun < 5 && totalStarts > 10) {
            efficiencyMsg = 'POOR';
            healthEl.classList.add('offline');
            healthEl.classList.remove('active');
            tipMsg = 'Frequent short runs detected. Check for leaks.';
        } else if (totalStarts > 5) {
            efficiencyMsg = 'NORMAL';
            healthEl.classList.add('active');
            healthEl.classList.remove('offline');
            tipMsg = 'High usage detected. Plan runs to save energy.';
        }

        if (lang === 'ta') {
            if (efficiencyMsg === 'OPTIMAL') efficiencyMsg = 'சிறப்பானது';
            if (efficiencyMsg === 'NORMAL') efficiencyMsg = 'சாதாரணமானது';
            if (efficiencyMsg === 'POOR') efficiencyMsg = 'மோசமானது';
            tipMsg = efficiencyMsg === 'சிறப்பானது' ? 'சிஸ்டம் சிறப்பாக இயங்குகிறது.' : (efficiencyMsg === 'மோசமானது' ? 'கசிவுகள் உள்ளதா என சரிபார்க்கவும்.' : 'பயன்பாடு அதிகமாக உள்ளது.');
        }
        healthEl.innerText = efficiencyMsg;
        tipsEl.innerText = tipMsg;
    }

    // ── 6. Render Chart ──────────────────────────────────────────
    renderChart(displayLabels, internalKeys.map(k => dailyMinutes[k]), internalKeys.map(k => dailyStarts[k]));
}

function renderChart(labels, minuteData, startData) {
    const canvas = document.getElementById('usageChart');
    if (!canvas) return;
    const ctx = canvas.getContext('2d');

    if (usageChart) usageChart.destroy();

    usageChart = new Chart(ctx, {
        type: 'bar',
        data: {
            labels,
            datasets: [
                {
                    label: 'Run Time (min)',
                    data: minuteData,
                    backgroundColor: 'rgba(0, 229, 255, 0.25)',
                    borderColor: '#00E5FF',
                    borderWidth: 2,
                    borderRadius: 8,
                    yAxisID: 'y',
                },
                {
                    label: 'Motor Starts',
                    data: startData,
                    type: 'line',
                    borderColor: '#FF9100',
                    backgroundColor: 'rgba(255, 145, 0, 0.1)',
                    borderWidth: 2,
                    pointRadius: 5,
                    pointBackgroundColor: '#FF9100',
                    tension: 0.4,
                    yAxisID: 'y1',
                }
            ]
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            interaction: { mode: 'index', intersect: false },
            plugins: {
                legend: {
                    labels: { color: '#64748b', font: { family: 'Rajdhani', size: 13, weight: '700' } }
                },
                tooltip: { bodyFont: { family: 'Rajdhani' }, titleFont: { family: 'Orbitron' } }
            },
            scales: {
                x: {
                    ticks: { color: '#64748b', font: { family: 'Rajdhani', size: 12 } },
                    grid: { color: 'rgba(0,0,0,0.04)' }
                },
                y: {
                    beginAtZero: true,
                    position: 'left',
                    ticks: { color: '#00E5FF', font: { family: 'Orbitron', size: 11 } },
                    grid: { color: 'rgba(0,229,255,0.08)' },
                    title: { display: true, text: 'Minutes', color: '#00E5FF', font: { family: 'Rajdhani', size: 12 } }
                },
                y1: {
                    beginAtZero: true,
                    position: 'right',
                    ticks: { color: '#FF9100', font: { family: 'Orbitron', size: 11 }, stepSize: 1 },
                    grid: { drawOnChartArea: false },
                    title: { display: true, text: 'Starts', color: '#FF9100', font: { family: 'Rajdhani', size: 12 } }
                }
            }
        }
    });
}

// ── REALTIME ─────────────────────────────────────
function setupRealtime() {
    if (realtimeChannel) db.removeChannel(realtimeChannel);

    realtimeChannel = db.channel('system-live-v6')
        .on('postgres_changes', { event: '*', schema: 'public', table: 'motor_system' }, p => {
            if (p.new) updateUI(p.new);
        })
        .on('postgres_changes', { event: '*', schema: 'public', table: 'motor_schedules' }, () => {
            fetchSchedules();
        })
        .on('postgres_changes', { event: '*', schema: 'public', table: 'motor_logs' }, () => {
            fetchLogs();
            if (currentTab === 'analytics') fetchAnalytics();
        })
        .subscribe(s => console.log('[Realtime]', s));
}

// ── LIGHTBOX ──────────────────────────────────────
function openLightbox() {
    const lb = document.getElementById('lightbox');
    const img = document.getElementById('lightboxImg');
    const src = document.getElementById('profileImg')?.src;
    if (img) img.src = src || '';
    if (lb) lb.classList.add('active');
}

function closeLightbox() {
    const lb = document.getElementById('lightbox');
    if (lb) lb.classList.remove('active');
}

// ── LIFECYCLE ─────────────────────────────────────
document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'visible') {
        fetchState();
        fetchSchedules();
        fetchLogs();
        setupRealtime();
        if (currentTab === 'analytics') fetchAnalytics();
    }
});

// ── HEARTBEAT WATCHDOG (runs every 30s independently) ──────────
// KEY FIX: When boards are OFF, Realtime never fires.
// So we MUST re-check timestamps on a timer using cached lastKnownData.
function checkHeartbeats() {
    if (!lastKnownData) return;

    const now = new Date();
    const tankLastSeen = lastKnownData.tank_last_seen ? new Date(lastKnownData.tank_last_seen) : new Date(0);
    const motorLastSeen = lastKnownData.motor_last_seen ? new Date(lastKnownData.motor_last_seen) : new Date(0);
    const tankOnline = (now - tankLastSeen) / 60000 < 2;
    const motorOnline = (now - motorLastSeen) / 60000 < 2;

    // Tank card badge
    const tankStatusEl = document.getElementById('tankStatus');
    if (tankStatusEl) {
        tankStatusEl.innerText = tankOnline ? 'ACTIVE' : 'OFFLINE';
        tankStatusEl.classList.toggle('active', tankOnline);
        tankStatusEl.classList.toggle('offline', !tankOnline);
    }

    // Tank header badge
    const tankNodeEl = document.getElementById('tankNodeStatus');
    if (tankNodeEl) {
        const dot = tankNodeEl.querySelector('.pulse-dot');
        tankNodeEl.classList.toggle('active', tankOnline);
        tankNodeEl.lastChild.textContent = tankOnline ? ' TANK [ACTIVE]' : ' TANK [OFFLINE]';
        if (dot) { dot.classList.toggle('offline', !tankOnline); }
    }

    console.log(`[Watchdog] Tank: ${Math.floor((now - tankLastSeen) / 60000)}m ago → ${tankOnline ? 'ONLINE' : 'OFFLINE'} | Motor: ${Math.floor((now - motorLastSeen) / 60000)}m ago → ${motorOnline ? 'ONLINE' : 'OFFLINE'}`);
}

// ── BOOTSTRAP ─────────────────────────────────────
window.onload = () => {
    // ── 1. Tab Switching Listeners ──
    document.querySelectorAll('.nav-links li[data-tab]').forEach(li => {
        li.addEventListener('click', () => switchTab(li.dataset.tab));
    });

    // ── 2. Data Initialization ──
    fetchState();
    fetchSchedules();
    fetchLogs();
    setupRealtime();
    fetchWeather();
    updateLanguage();

    // ── 3. Motor & Schedule Buttons ──
    const btnOn = document.getElementById('btnOn');
    const btnOff = document.getElementById('btnOff');
    if (btnOn) btnOn.onclick = () => setMotor(true);
    if (btnOff) btnOff.onclick = () => setMotor(false);

    const btnAdd = document.getElementById('btnAddSchedule');
    if (btnAdd) btnAdd.onclick = addSchedule;

    const btnReport = document.getElementById('btnReport');
    if (btnReport) btnReport.onclick = downloadCSV;

    // ── 4. Settings: Local Control ──
    const ipInpS = document.getElementById('localIpInputS');
    const modeTglS = document.getElementById('localModeToggleS');

    if (ipInpS) {
        ipInpS.value = localIp;
        ipInpS.onchange = (e) => {
            localIp = e.target.value.trim();
            localStorage.setItem('tanksync_local_ip', localIp);
        };
    }
    
    if (modeTglS) {
        modeTglS.checked = localMode;
        modeTglS.onchange = (e) => {
            localMode = e.target.checked;
            localStorage.setItem('tanksync_local_mode', localMode);
            const badge = document.getElementById('modeText');
            if (badge) {
                badge.innerHTML = localMode 
                    ? '<span class="pulse-dot" style="background:#f1c40f"></span> LOCAL / ローカル'
                    : '<span class="pulse-dot sys-online"></span> ONLINE / オンライン';
            }
        };
        // Initial badge setup
        modeTglS.dispatchEvent(new Event('change'));
    }

    // ── 5. Settings: Preferences ──
    const btnLangS = document.getElementById('btnLangToggleS');
    const btnVoiceS = document.getElementById('btnVoiceToggleS');

    if (btnLangS) {
        btnLangS.onclick = () => {
            lang = (lang === 'en') ? 'ta' : 'en';
            localStorage.setItem('tanksync_lang', lang);
            updateLanguage();
        };
    }
    if (btnVoiceS) {
        btnVoiceS.classList.toggle('muted', !voiceEnabled);
        btnVoiceS.onclick = () => {
            voiceEnabled = !voiceEnabled;
            localStorage.setItem('tanksync_voice', voiceEnabled);
            btnVoiceS.classList.toggle('muted', !voiceEnabled);
            if (voiceEnabled) announce('dashboard');
        };
    }

    // ── 6. Maintenance Timers ──
    setInterval(checkHeartbeats, 30000);
    setInterval(fetchWeather, 1800000);

    // Profile Lightbox
    const trigger = document.getElementById('profilePicTrigger');
    const mbPic = document.getElementById('mobileProfileBtn');
    const closeBtn = document.querySelector('.close-lightbox');
    const lb = document.getElementById('lightbox');
    if (trigger) trigger.onclick = openLightbox;
    if (mbPic) mbPic.onclick = openLightbox;
    if (closeBtn) closeBtn.onclick = closeLightbox;
    if (lb) lb.onclick = (e) => { if (e.target === lb) closeLightbox(); };
};
