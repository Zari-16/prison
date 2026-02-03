#app new version
import os
import time
from flask import Flask, render_template, jsonify, session, redirect, url_for, request
from flask_socketio import SocketIO
from influxdb_client import InfluxDBClient
from dotenv import load_dotenv

load_dotenv()
app = Flask(__name__)
app.config['SECRET_KEY'] = os.urandom(24)
socketio = SocketIO(app, cors_allowed_origins="*")

# InfluxDB Configuration (SET THESE IN .env FILE)
INFLUX_URL = os.getenv("INFLUX_URL", "http://10.197.56.165:8086")
INFLUX_TOKEN = os.getenv("INFLUX_TOKEN", "")
INFLUX_ORG = os.getenv("INFLUX_ORG", "MDX")
INFLUX_BUCKET = os.getenv("INFLUX_BUCKET", "Prison_Data")
DEMO_MODE = os.getenv("DEMO_MODE", "False").lower() == "true"

client = None
query_api = None

if not DEMO_MODE and INFLUX_TOKEN:
    try:
        client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
        query_api = client.query_api()
        print("✓ InfluxDB connected successfully")
    except Exception as e:
        print(f"⚠ InfluxDB connection failed: {e}. Falling back to DEMO_MODE")
        DEMO_MODE = True

# SECURITY: Password hashing (generate hash via werkzeug.security)
ADMIN_PASSWORD_HASH = os.getenv("ADMIN_PASSWORD_HASH", "pbkdf2:sha256:...") 

@app.route('/')
def landing():
    return redirect(url_for('login')) if 'user' in session else render_template('landing.html')

@app.route('/login', methods=['GET', 'POST'])
def login():
    if 'user' in session:
        return redirect(url_for('dashboard'))
    
    if request.method == 'POST':
        from werkzeug.security import check_password_hash
        if request.form.get('username') == "admin" and check_password_hash(ADMIN_PASSWORD_HASH, request.form.get('password')):
            session['user'] = "admin"
            return redirect(url_for('dashboard'))
        return render_template('login.html', error="Invalid security credentials")
    return render_template('login.html')

@app.route('/logout')
def logout():
    session.clear()
    return redirect(url_for('landing'))

@app.route('/dashboard')
def dashboard():
    return redirect(url_for('login')) if 'user' not in session else render_template('index.html', user=session['user'])

@app.route('/api/status')
def get_status():
    if 'user' not in session:
        return jsonify({"status": "error", "message": "Unauthorized"}), 401
    
    if DEMO_MODE:
        import random
        return jsonify({
            "status": "success",
            "control_room": {
                "people_count": random.randint(0, 15),
                "door_open": random.choice([0, 1]),
                "door_state": random.choice([-1, 0, 1]),
                "fence_alert": random.choice([0, 0, 0, 1]),
                "pir_value": random.randint(0, 1),
                "vib_value": random.randint(200, 400)
            },
            "sensors": {
                "temperature": round(22 + random.random() * 8, 1),
                "humidity": round(45 + random.random() * 25, 1),
                "gas": random.randint(100, 600),
                "water": random.randint(150, 300),
                "motion": random.choice([0, 1]),
                "alert_state": random.choice([0, 0, 0, 1, 2, 3])
            }
        })
    
    try:
        # FIXED FLUX QUERIES - NO SPACES IN SYNTAX
        cr_query = f'from(bucket:"{INFLUX_BUCKET}") |> range(start: -10m) |> filter(fn:(r) => r._measurement == "control_room") |> last()'
        pg_query = f'from(bucket:"{INFLUX_BUCKET}") |> range(start: -10m) |> filter(fn:(r) => r._measurement == "prison_sensors") |> last()'
        
        # Process Control Room data
        cr_data = {}
        for table in query_api.query(cr_query):
            for rec in table.records:
                cr_data[rec.get_field()] = rec.get_value()
        
        # Process Patrol Guard data
        pg_data = {}
        for table in query_api.query(pg_query):
            for rec in table.records:
                pg_data[rec.get_field()] = rec.get_value()
        
        return jsonify({
            "status": "success",
            "control_room": cr_data,
            "sensors": pg_data
        })
    except Exception as e:
        print(f"API Error: {str(e)}")
        return jsonify({"status": "error", "message": str(e)}), 500

@app.route('/api/history/<measurement>')
def get_history(measurement):
    if 'user' not in session:
        return jsonify({"status": "error", "message": "Unauthorized"}), 401
    
    if DEMO_MODE:
        import random, datetime
        now = datetime.datetime.utcnow()
        data = []
        for i in range(60):
            ts = (now - datetime.timedelta(minutes=60-i)).isoformat() + "Z"
            data.append({
                "time": ts,
                "field": "value",
                "value": random.randint(20, 30) if measurement == "prison_sensors" else random.randint(0, 10)
            })
        return jsonify(data)
    
    try:
        # FIXED QUERY - VALID FLUX SYNTAX
        query = f'''
        from(bucket:"{INFLUX_BUCKET}")
          |> range(start: -1h)
          |> filter(fn:(r) => r._measurement == "{measurement}")
          |> pivot(rowKey:["_time"], columnKey:["_field"], valueColumn:"_value")
        '''
        results = query_api.query(query)
        data = []
        for table in results:
            for rec in table.records:
                for field in ["people_count", "door_open", "fence_alert", "temperature", "humidity", "gas", "water", "motion", "alert_state"]:
                    if hasattr(rec, field):
                        data.append({
                            "time": rec.get_time().isoformat(),
                            "field": field,
                            "value": getattr(rec, field)
                        })
        return jsonify(data)
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

if __name__ == '__main__':
    socketio.run(app, host='0.0.0.0', port=5000, debug=False)