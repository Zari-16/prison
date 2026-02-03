import os
import time
from flask import Flask, render_template, jsonify, session, redirect, url_for, request
from flask_socketio import SocketIO, emit
from influxdb_client import InfluxDBClient, Point
from influxdb_client.client.write_api import SYNCHRONOUS
from dotenv import load_dotenv

load_dotenv()

app = Flask(__name__)
app.config['SECRET_KEY'] = 'secret!'
socketio = SocketIO(app, cors_allowed_origins="*")

# InfluxDB Configuration 
INFLUX_URL = ""
INFLUX_TOKEN = ""
INFLUX_ORG = ""
INFLUX_BUCKET = ""

# Set to True if InfluxDB is unreachable
DEMO_MODE = True 

client = None
query_api = None

if not DEMO_MODE:
    try:
        client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
        query_api = client.query_api()
    except Exception as e:
        print(f"Failed to connect to InfluxDB: {e}")
        DEMO_MODE = True

@app.route('/')
def landing():
    """Landing page route."""
    if 'user' in session:
        return redirect(url_for('dashboard'))
    return render_template('landing.html')

@app.route('/login', methods=['GET', 'POST'])
def login():
    """Login page and logic."""
    if 'user' in session:
        return redirect(url_for('dashboard'))
        
    error = None
    if request.method == 'POST':
        username = request.form.get('username')
        password = request.form.get('password')
        
        # Simple hardcoded check for demo
        if username == "admin" and password == "security123":
            session['user'] = username
            return redirect(url_for('dashboard'))
        else:
            error = "Invalid security credentials"
            
    return render_template('login.html', error=error)

@app.route('/logout')
def logout():
    """Logout route."""
    session.pop('user', None)
    return redirect(url_for('landing'))

@app.route('/dashboard')
def dashboard():
    """Main dashboard route (protected)."""
    if 'user' not in session:
        return redirect(url_for('login'))
    return render_template('index.html', user=session['user'])

@app.route('/api/status')
def get_status():
    """Fetch latest status from InfluxDB or return demo data."""
    if 'user' not in session:
        return jsonify({"status": "error", "message": "Unauthorized"}), 401
    if DEMO_MODE:
        import random
        return jsonify({
            "status": "success",
            "control_room": {
                "people_count": random.randint(0, 10),
                "door_open": random.choice([0, 1]),
                "fence_alert": random.choice([0, 0, 0, 1]) # Low chance of alert
            },
            "sensors": {
                "temperature": 24 + random.random() * 5,
                "humidity": 45 + random.random() * 10
            }
        })

    try:
        # Query for Control Room
        cr_query = f'from(bucket: "{INFLUX_BUCKET}") |> range(start: -10m) |> filter(fn: (r) => r._measurement == "control_room") |> last()'
        cr_result = query_api.query(cr_query)
        
        control_room = {}
        for table in cr_result:
            for record in table.records:
                control_room[record.get_field()] = record.get_value()

        # Query for Patrol Guard Sensors
        pg_query = f'from(bucket: "{INFLUX_BUCKET}") |> range(start: -10m) |> filter(fn: (r) => r._measurement == "prison_sensors") |> last()'
        pg_result = query_api.query(pg_query)
        
        sensors = {}
        for table in pg_result:
            for record in table.records:
                sensors[record.get_field()] = record.get_value()

        return jsonify({
            "status": "success",
            "control_room": control_room,
            "sensors": sensors
        })
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

@app.route('/api/history/<measurement>')
def get_history(measurement):
    """Fetch last 1 hour of history for charts."""
    if 'user' not in session:
        return jsonify({"status": "error", "message": "Unauthorized"}), 401
    try:
        history_query = f'from(bucket: "{INFLUX_BUCKET}") |> range(start: -1h) |> filter(fn: (r) => r._measurement == "{measurement}")'
        result = query_api.query(history_query)
        
        data = []
        for table in result:
            for record in table.records:
                data.append({
                    "time": record.get_time().isoformat(),
                    "field": record.get_field(),
                    "value": record.get_value()
                })
        return jsonify(data)
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

def background_thread():
    """Example thread to push updates via SocketIO if needed."""
    while True:
        # In a real scenario, you might poll InfluxDB here and emit if data changed
        # For now, we'll let the frontend poll the API for simplicity OR implement a more complex reactive logic
        time.sleep(5)

if __name__ == '__main__':
    socketio.run(app, debug=True, port=5000)
