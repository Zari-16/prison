# app API with role-based auth, Influx proxies, alerts, email and Socket.IO live updates.
import os
import time
from datetime import datetime, timedelta
from flask import Flask, render_template, jsonify, session, redirect, url_for, request, send_from_directory
from flask_socketio import SocketIO
from influxdb_client import InfluxDBClient
from flask_sqlalchemy import SQLAlchemy
from flask_mail import Mail, Message
from werkzeug.security import generate_password_hash, check_password_hash
from dotenv import load_dotenv

load_dotenv()

# Flask app
app = Flask(__name__, template_folder="templates", static_folder="static")
app.config["SECRET_KEY"] = os.getenv("SECRET_KEY", os.urandom(24))
# SQLite DB (file data.db)
app.config["SQLALCHEMY_DATABASE_URI"] = os.getenv("DATABASE_URL", "sqlite:///data.db")
app.config["SQLALCHEMY_TRACK_MODIFICATIONS"] = False

# Mail configuration (optional)
app.config["MAIL_SERVER"] = os.getenv("MAIL_SERVER", "")
app.config["MAIL_PORT"] = int(os.getenv("MAIL_PORT", "587"))
app.config["MAIL_USERNAME"] = os.getenv("MAIL_USERNAME", "")
app.config["MAIL_PASSWORD"] = os.getenv("MAIL_PASSWORD", "")
app.config["MAIL_USE_TLS"] = os.getenv("MAIL_USE_TLS", "true").lower() in ("1", "true", "yes")
app.config["MAIL_DEFAULT_SENDER"] = os.getenv("MAIL_DEFAULT_SENDER", app.config["MAIL_USERNAME"])

# Influx config: we expect separate tokens for control and patrol
INFLUX_URL = os.getenv("INFLUX_URL", "http://10.197.56.165:8086")
INFLUX_ORG = os.getenv("INFLUX_ORG", "MDX")
INFLUX_BUCKET = os.getenv("INFLUX_BUCKET", "Prison_Data")
INFLUX_CONTROL_TOKEN = os.getenv("INFLUX_CONTROL_TOKEN", "")
INFLUX_PATROL_TOKEN = os.getenv("INFLUX_PATROL_TOKEN", "")

# Demo mode fallback (if Influx tokens are missing)
DEMO_MODE = os.getenv("DEMO_MODE", "False").lower() == "true" or (not INFLUX_CONTROL_TOKEN and not INFLUX_PATROL_TOKEN)

# Socket.IO
socketio = SocketIO(app, cors_allowed_origins="*", async_mode="eventlet")

# DB
db = SQLAlchemy(app)
mail = Mail(app)

# Models
class User(db.Model):
    id = db.Column(db.Integer, primary_key=True)
    username = db.Column(db.String(80), unique=True, nullable=False)
    password_hash = db.Column(db.String(256), nullable=False)
    role = db.Column(db.String(20), nullable=False, default="level1")  # 'level1' or 'level2'
    email = db.Column(db.String(200), nullable=True)

class Alert(db.Model):
    id = db.Column(db.Integer, primary_key=True)
    kind = db.Column(db.String(80), nullable=False)
    message = db.Column(db.Text, nullable=False)
    created_at = db.Column(db.DateTime, default=datetime.utcnow)
    sent_to = db.Column(db.Text, nullable=True)

# Initialize DB and seed minimal users
@app.before_first_request
def init_db():
    db.create_all()
    # Seed default users if none exist
    if not User.query.first():
        admin = User(username="admin", password_hash=generate_password_hash("adminpass"), role="level2", email=os.getenv("ADMIN_EMAIL",""))
        guard = User(username="guard", password_hash=generate_password_hash("guardpass"), role="level1", email=os.getenv("GUARD_EMAIL",""))
        db.session.add_all([admin, guard])
        db.session.commit()
        app.logger.info("Seeded default users: admin (level2), guard (level1)")

# Helpers
def login_required(fn):
    def wrapper(*args, **kwargs):
        if "user" not in session:
            return jsonify({"error": "Unauthorized"}), 401
        return fn(*args, **kwargs)
    wrapper.__name__ = fn.__name__
    return wrapper

def role_required(allowed_roles):
    def decorator(fn):
        def wrapper(*args, **kwargs):
            if "user" not in session:
                return jsonify({"error": "Unauthorized"}), 401
            role = session.get("role")
            if role not in allowed_roles:
                return jsonify({"error": "Forbidden"}), 403
            return fn(*args, **kwargs)
        wrapper.__name__ = fn.__name__
        return wrapper
    return decorator

def get_influx_client(token):
    if DEMO_MODE or not token:
        return None
    return InfluxDBClient(url=INFLUX_URL, token=token, org=INFLUX_ORG)

def query_last(token, measurement, limit=100, range_minutes=5):
    if DEMO_MODE:
        return []
    client = get_influx_client(token)
    if not client:
        return []
    qapi = client.query_api()
    flux = f'from(bucket: "{INFLUX_BUCKET}") |> range(start: -{range_minutes}m) |> filter(fn: (r) => r._measurement == "{measurement}") |> last()'
    out = []
    try:
        tables = qapi.query(flux)
        for table in tables:
            for rec in table.records:
                out.append({
                    "time": rec.get_time().isoformat() if rec.get_time() else None,
                    "measurement": rec.get_measurement(),
                    "field": rec.get_field(),
                    "value": rec.get_value()
                })
    except Exception as e:
        app.logger.exception("Influx query error: %s", e)
    return out

# Routes: pages
@app.route("/")
def landing():
    if "user" in session:
        return redirect(url_for("dashboard"))
    return render_template("landing.html")

@app.route("/login", methods=["GET", "POST"])
def login():
    # DB-backed login
    if "user" in session:
        return redirect(url_for("dashboard"))
    error = None
    if request.method == "POST":
        username = request.form.get("username", "")
        password = request.form.get("password", "")
        user = User.query.filter_by(username=username).first()
        if user and check_password_hash(user.password_hash, password):
            session["user"] = user.username
            session["role"] = user.role
            return redirect(url_for("dashboard"))
        error = "Invalid credentials"
    return render_template("login.html", error=error)

@app.route("/logout")
def logout():
    session.clear()
    return redirect(url_for("landing"))

@app.route("/dashboard")
def dashboard():
    if "user" not in session:
        return redirect(url_for("login"))
    return render_template("index.html", user=session.get("user"))

@app.route("/control_room")
def control_room_page():
    # page served; data endpoints enforce role
    if "user" not in session:
        return redirect(url_for("login"))
    return render_template("control_room.html", user=session.get("user"), role=session.get("role"))

@app.route("/patrol_guard")
def patrol_guard_page():
    if "user" not in session:
        return redirect(url_for("login"))
    return render_template("patrol_guard.html", user=session.get("user"), role=session.get("role"))

# API: whoami
@app.route("/api/whoami")
def whoami():
    if "user" not in session:
        return jsonify({"user": None})
    return jsonify({"user": session.get("user"), "role": session.get("role")})

# API: create user (only level2)
@app.route("/api/users", methods=["POST"])
@role_required(["level2"])
def create_user():
    data = request.get_json() or {}
    username = data.get("username")
    password = data.get("password")
    role = data.get("role", "level1")
    email = data.get("email", "")
    if not username or not password:
        return jsonify({"error": "username and password required"}), 400
    if User.query.filter_by(username=username).first():
        return jsonify({"error": "user exists"}), 400
    h = generate_password_hash(password)
    u = User(username=username, password_hash=h, role=role, email=email)
    db.session.add(u)
    db.session.commit()
    return jsonify({"msg": "created"}), 201

# API: data endpoints (role-protected)
@app.route("/api/data/controlroom")
@role_required(["level2"])
def api_controlroom():
    # measurement name from query or default
    measurement = request.args.get("measurement", "control_room")
    if DEMO_MODE:
        import random
        return jsonify([{"time": datetime.utcnow().isoformat(), "field": "people_count", "value": random.randint(0, 10)}])
    points = query_last(INFLUX_CONTROL_TOKEN, measurement, range_minutes=5)
    return jsonify(points)

@app.route("/api/data/patrol")
@role_required(["level1","level2"])
def api_patrol():
    measurement = request.args.get("measurement", "prison_sensors")
    if DEMO_MODE:
        import random
        return jsonify([{"time": datetime.utcnow().isoformat(), "field": "temperature", "value": round(22+random.random()*8,1)}])
    points = query_last(INFLUX_PATROL_TOKEN, measurement, range_minutes=5)
    return jsonify(points)

# Alerts: create and list
@app.route("/api/alerts", methods=["GET", "POST"])
@login_required
def api_alerts():
    if request.method == "GET":
        items = Alert.query.order_by(Alert.created_at.desc()).limit(100).all()
        return jsonify([{"id": a.id, "kind": a.kind, "message": a.message, "created_at": a.created_at.isoformat(), "sent_to": a.sent_to} for a in items])
    data = request.get_json() or {}
    kind = data.get("kind", "generic")
    message = data.get("message", "")
    if not message:
        return jsonify({"error": "message required"}), 400
    # store
    a = Alert(kind=kind, message=message, created_at=datetime.utcnow())
    db.session.add(a)
    db.session.commit()
    recipients = data.get("recipients")
    sent = []
    if recipients:
        to_list = recipients
    else:
        # default: all users with email
        to_list = [u.email for u in User.query.all() if u.email]
    # send mail
    if app.config["MAIL_SERVER"] and to_list:
        for r in to_list:
            try:
                msg = Message(subject=f"[Alert] {kind}", recipients=[r], body=message)
                mail.send(msg)
                sent.append(r)
            except Exception as e:
                app.logger.exception("Mail send failed: %s", e)
    a.sent_to = ",".join(sent)
    db.session.commit()
    # push via socketio
    socketio.emit("alert", {"id": a.id, "kind": a.kind, "message": a.message, "sent_to": sent}, namespace="/live")
    return jsonify({"id": a.id, "sent_to": sent}), 201

@app.route("/api/health")
def health():
    return jsonify({"status": "ok", "time": datetime.utcnow().isoformat(), "demo": DEMO_MODE})

# Static files (if you put control_room/patrol_guard in templates, they will be used)
@app.route("/static/<path:p>")
def static_proxy(p):
    return send_from_directory("static", p)

# Background task: poll Influx and emit new points via socketio
def poll_influx_and_emit():
    last_emitted = {"control": None, "patrol": None}
    while True:
        try:
            # control
            if not DEMO_MODE and INFLUX_CONTROL_TOKEN:
                pts = query_last(INFLUX_CONTROL_TOKEN, "control_room", range_minutes=1)
                for p in pts:
                    # emit each point; clients will dedupe/display
                    socketio.emit("control-data", p, namespace="/live", room="control")
            else:
                # demo emit
                import random
                p = {"time": datetime.utcnow().isoformat(), "measurement": "control_room", "field": "people_count", "value": random.randint(0, 10)}
                socketio.emit("control-data", p, namespace="/live", room="control")
            # patrol
            if not DEMO_MODE and INFLUX_PATROL_TOKEN:
                pts = query_last(INFLUX_PATROL_TOKEN, "prison_sensors", range_minutes=1)
                for p in pts:
                    socketio.emit("patrol-data", p, namespace="/live", room="patrol")
            else:
                import random
                p = {"time": datetime.utcnow().isoformat(), "measurement": "prison_sensors", "field": "temperature", "value": round(22+random.random()*8,1)}
                socketio.emit("patrol-data", p, namespace="/live", room="patrol")
        except Exception as e:
            app.logger.exception("Background poll error: %s", e)
        time.sleep(int(os.getenv("POLL_INTERVAL_S","3")))

# SocketIO events (clients should join rooms)
@socketio.on("subscribe", namespace="/live")
def handle_subscribe(data):
    room = data.get("room")
    if room not in ("control","patrol"):
        return
    # enforce role server-side: if user in session then check
    role = session.get("role")
    if room == "control" and role != "level2":
        # do not join
        return
    # join room
    from flask_socketio import join_room
    join_room(room)
    app.logger.info("socket joined room %s", room)

@socketio.on("unsubscribe", namespace="/live")
def handle_unsubscribe(data):
    room = data.get("room")
    from flask_socketio import leave_room
    leave_room(room)

# Start background poll thread after server starts
@socketio.on("connect")
def on_connect():
    # ensure background task started once
    if not getattr(app, "_poll_task_started", False):
        socketio.start_background_task(poll_influx_and_emit)
        app._poll_task_started = True

if __name__ == "__main__":
    # use eventlet in production; the server will run socketio with eventlet automatically when eventlet installed
    socketio.run(app, host="0.0.0.0", port=int(os.getenv("PORT", "5000")), debug=os.getenv("FLASK_DEBUG","False").lower()=="true")
