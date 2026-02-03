let mainChart, overviewChart;
let chartData = {
    labels: [],
    temp: [],
    humidity: []
};

document.addEventListener('DOMContentLoaded', () => {
    initCharts();
    updateDashboard();
    setInterval(updateDashboard, 5000);
    setInterval(updateTime, 1000);

    // Lockdown button listener
    const lockdownBtn = document.getElementById('btn-lockdown');
    if (lockdownBtn) {
        lockdownBtn.addEventListener('click', toggleLockdown);
    }
});

function updateTime() {
    const now = new Date();
    document.getElementById('current-time').innerText = now.toLocaleString();
}

function initCharts() {
    // Overview Chart (Mini)
    const ovCtx = document.getElementById('overviewChart').getContext('2d');
    overviewChart = createChart(ovCtx, ['#00d2ff'], ['System Activity']);

    // Main Analytics Chart
    const mainCtx = document.getElementById('mainChart').getContext('2d');
    mainChart = createChart(mainCtx, ['#ff4b2b', '#00d2ff'], ['Temperature (°C)', 'Humidity (%)']);
}

function createChart(ctx, colors, datasets) {
    return new Chart(ctx, {
        type: 'line',
        data: {
            labels: chartData.labels,
            datasets: datasets.map((label, i) => ({
                label: label,
                data: i === 0 ? chartData.temp : chartData.humidity,
                borderColor: colors[i],
                backgroundColor: `${colors[i]}1A`,
                fill: true,
                tension: 0.4
            }))
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            scales: {
                y: { grid: { color: 'rgba(255, 255, 255, 0.05)' }, ticks: { color: '#a0a0a0' } },
                x: { grid: { display: false }, ticks: { color: '#a0a0a0' } }
            },
            plugins: { legend: { labels: { color: '#ffffff' } } }
        }
    });
}

async function updateDashboard() {
    try {
        const response = await fetch('/api/status');
        const data = await response.json();

        if (data.status === 'success') {
            const cr = data.control_room;
            const pg = data.sensors;

            // Overview Tab Updates
            if (document.getElementById('val-people-count-ov')) {
                document.getElementById('val-people-count-ov').innerText = cr.people_count || 0;
            }

            // Control Room Tab Updates
            const doorOpen = cr.door_open === 1;
            const doorVal = document.getElementById('val-door-status');
            const doorBadge = document.getElementById('badge-door-status');
            if (doorVal) doorVal.innerText = doorOpen ? 'OPENED' : 'LOCKED';
            if (doorBadge) {
                doorBadge.innerText = doorOpen ? 'Unsecured' : 'Secure';
                doorBadge.className = doorOpen ? 'badge badge-danger' : 'badge badge-success';
            }

            const fenceAlert = cr.fence_alert === 1;
            const fenceVal = document.getElementById('val-fence-status');
            const fenceBadge = document.getElementById('badge-fence-status');
            if (fenceVal) fenceVal.innerText = fenceAlert ? 'BREACH' : 'CLEAR';
            if (fenceBadge) {
                fenceBadge.innerText = fenceAlert ? 'Intrusion Detected' : 'No Activity';
                fenceBadge.className = fenceAlert ? 'badge badge-danger' : 'badge badge-success';
            }

            if (fenceAlert) addLog('PERIMETER BREACH DETECTED!', 'danger');

            // Patrol Guard Tab Updates
            const temp = pg.temperature || 0;
            const hum = pg.humidity || 0;
            const tempVal = document.getElementById('val-temp');
            const humVal = document.getElementById('val-hum');

            if (tempVal) tempVal.innerText = `${temp.toFixed(1)}°C`;
            if (humVal) humVal.innerText = `${hum.toFixed(1)}%`;

            // Chart Updates
            updateChartData(temp, hum);
        }
    } catch (error) {
        console.error('Error fetching status:', error);
    }
}

function updateChartData(temp, humidity) {
    const now = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });

    chartData.labels.push(now);
    chartData.temp.push(temp);
    chartData.humidity.push(humidity);

    if (chartData.labels.length > 10) {
        chartData.labels.shift();
        chartData.temp.shift();
        chartData.humidity.shift();
    }

    if (mainChart) mainChart.update();
    if (overviewChart) overviewChart.update();
}

function toggleLockdown() {
    const btn = document.getElementById('btn-lockdown');
    const isLockdown = btn.innerText === 'INITIATE LOCKDOWN';

    if (isLockdown) {
        btn.innerText = 'RELEASE LOCKDOWN';
        btn.style.background = 'var(--success)';
        addLog('FACILITY LOCKDOWN INITIATED', 'danger');
    } else {
        btn.innerText = 'INITIATE LOCKDOWN';
        btn.style.background = 'var(--danger)';
        addLog('FACILITY LOCKDOWN RELEASED', 'success');
    }
}

function addLog(msg, type) {
    const logs = document.getElementById('event-logs');
    if (!logs) return;

    const time = new Date().toLocaleTimeString();
    const logEntry = document.createElement('div');
    logEntry.className = 'log-entry entry-animation';
    logEntry.style.background = type === 'danger' ? 'rgba(255, 75, 43, 0.1)' : 'rgba(255, 255, 255, 0.05)';
    logEntry.innerHTML = `<span style="color: var(--text-secondary)">[${time}]</span> ${msg}`;

    logs.prepend(logEntry);
    if (logs.children.length > 10) logs.lastChild.remove();
}

function switchTab(tab) {
    // Update Nav UI
    const items = document.querySelectorAll('.nav-item');
    items.forEach(i => i.classList.remove('active'));
    event.currentTarget.classList.add('active');

    // Update Page Header
    document.getElementById('page-title').innerText = tab.replace('_', ' ').toUpperCase();

    // Toggle View Visibility
    const views = document.querySelectorAll('.tab-view');
    views.forEach(v => v.classList.remove('active'));

    const targetView = document.getElementById(`${tab}-view`);
    if (targetView) targetView.classList.add('active');
}
