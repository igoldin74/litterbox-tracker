import sqlite3, os
from flask import Flask, request, jsonify, render_template
from datetime import datetime, timezone, timedelta

app = Flask(__name__)
DB = os.path.join(os.path.dirname(__file__), "data", "visits.db")
os.makedirs(os.path.dirname(DB), exist_ok=True)

def db():
    con = sqlite3.connect(DB)
    con.row_factory = sqlite3.Row
    return con

def init_db():
    con = db()
    con.execute("""CREATE TABLE IF NOT EXISTS visits (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        device_id TEXT,
        start_time TEXT,
        end_time TEXT,
        duration_sec REAL,
        peak_weight_kg REAL,
        baseline_kg REAL,
        created_at TEXT DEFAULT (datetime('now'))
    )""")
    con.execute("""CREATE TABLE IF NOT EXISTS maintenance (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        active INTEGER,
        until_ts TEXT,
        source TEXT,
        updated_at TEXT DEFAULT (datetime('now'))
    )""")
    con.commit(); con.close()
init_db()

def is_maintenance():
    con = db()
    row = con.execute("SELECT * FROM maintenance ORDER BY id DESC LIMIT 1").fetchone()
    con.close()
    if not row: return False
    if not row["active"]: return False
    if row["until_ts"]:
        try:
            until = datetime.fromisoformat(row["until_ts"])
            if datetime.now(timezone.utc).replace(tzinfo=None) > until and "Z" not in row["until_ts"]:
                pass
        except: pass
    # Simple check: if until_ts is in past (stored as ISO in local time), treat as expired
    # We store until as datetime('now', '+N minutes') in SQLite, so compare in SQLite
    con2 = db()
    expired = con2.execute("SELECT datetime('now') > until_ts as expired FROM maintenance ORDER BY id DESC LIMIT 1").fetchone()
    con2.close()
    if expired and expired["expired"]:
        return False
    return True

@app.route("/")
def index():
    return render_template("index.html")

@app.route("/api/maintenance", methods=["GET"])
def get_maint():
    return jsonify({"active": is_maintenance()})

@app.route("/api/maintenance", methods=["POST"])
def set_maint():
    d = request.get_json(force=True)
    active = 1 if d.get("active") else 0
    minutes = int(d.get("minutes", 10))
    con = db()
    if active:
        con.execute("INSERT INTO maintenance (active, until_ts, source) VALUES (1, datetime('now', ?), ?)", (f"+{minutes} minutes", d.get("source","dashboard")))
    else:
        con.execute("INSERT INTO maintenance (active, until_ts, source) VALUES (0, datetime('now'), ?)", (d.get("source","dashboard"),))
    con.commit(); con.close()
    return jsonify({"ok": True, "active": bool(active)})

@app.route("/api/visit", methods=["POST"])
def add_visit():
    if is_maintenance():
        return jsonify({"ok": False, "reason": "maintenance - ignored"}), 200
    d = request.get_json(force=True)
    con = db()
    con.execute("INSERT INTO visits (device_id, start_time, end_time, duration_sec, peak_weight_kg, baseline_kg) VALUES (?,?,?,?,?,?)",
                (d.get("device_id","litter-01"), d.get("start_ts"), d.get("end_ts"), d.get("duration_sec"), d.get("peak_weight_kg"), d.get("baseline_kg")))
    con.commit(); con.close()
    return jsonify({"ok": True}), 201

@app.route("/api/heartbeat", methods=["POST"])
def heartbeat():
    d = request.get_json(force=True)
    con = db()
    con.execute("INSERT INTO heartbeats (device_id, weight_kg, ts) VALUES (?,?,?)",
                (d.get("device_id","litter-01"), d.get("weight_kg"), d.get("ts")))
    con.commit(); con.close()
    return jsonify({"ok": True})

@app.route("/api/visits")
def get_visits():
    days = int(request.args.get("days", 30))
    con = db()
    rows = con.execute("SELECT * FROM visits WHERE start_time >= datetime('now', ?) ORDER BY start_time DESC", (f"-{days} days",)).fetchall()
    con.close()
    return jsonify([dict(r) for r in rows])

@app.route("/api/stats/daily")
def daily():
    days = int(request.args.get("days", 30))
    con = db()
    rows = con.execute("SELECT date(start_time) as day, COUNT(*) as count, ROUND(AVG(duration_sec),1) as avg_duration FROM visits WHERE start_time >= datetime('now', ?) GROUP BY day ORDER BY day", (f"-{days} days",)).fetchall()
    con.close()
    return jsonify([dict(r) for r in rows])

@app.route("/api/visits/<int:vid>", methods=["DELETE"])
def delete_visit(vid):
    con = db(); con.execute("DELETE FROM visits WHERE id=?", (vid,)); con.commit(); con.close()
    return jsonify({"ok": True})

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8000)
