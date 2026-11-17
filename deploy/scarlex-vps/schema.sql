CREATE TABLE IF NOT EXISTS licenses (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  key_hash TEXT NOT NULL UNIQUE,
  hwid TEXT,
  expires_at TEXT,
  is_banned INTEGER NOT NULL DEFAULT 0 CHECK (is_banned IN (0, 1)),
  key_prefix TEXT,
  key_cipher TEXT,
  activated_at TEXT,
  duration_days INTEGER NOT NULL DEFAULT 30 CHECK (duration_days BETWEEN 1 AND 36500),
  note TEXT NOT NULL DEFAULT '',
  last_version TEXT,
  last_ip TEXT,
  last_seen_at TEXT
);
CREATE INDEX IF NOT EXISTS licenses_prefix ON licenses(key_prefix);
CREATE INDEX IF NOT EXISTS licenses_hwid ON licenses(hwid);
CREATE INDEX IF NOT EXISTS licenses_ip ON licenses(last_ip);
CREATE INDEX IF NOT EXISTS licenses_seen ON licenses(last_seen_at);
CREATE TABLE IF NOT EXISTS license_logs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  license_id INTEGER REFERENCES licenses(id) ON DELETE SET NULL,
  event_type TEXT NOT NULL,
  message TEXT NOT NULL DEFAULT '',
  hwid TEXT, ip TEXT, version TEXT,
  created_at TEXT NOT NULL,
  client_session TEXT, client_seq INTEGER, client_time TEXT, client_version TEXT,
  stage TEXT, code TEXT, level TEXT, os_error INTEGER, process_exit INTEGER
);
CREATE INDEX IF NOT EXISTS license_logs_license_date ON license_logs(license_id, id DESC);
CREATE INDEX IF NOT EXISTS license_logs_date_type ON license_logs(created_at, event_type);
CREATE TABLE IF NOT EXISTS admin_users (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  username TEXT NOT NULL UNIQUE,
  password_hash TEXT NOT NULL,
  totp_secret TEXT,
  token_version INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS admin_sessions (
  id TEXT PRIMARY KEY,
  admin_id INTEGER NOT NULL REFERENCES admin_users(id) ON DELETE CASCADE,
  expires_at INTEGER NOT NULL,
  created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS admin_sessions_expiry ON admin_sessions(expires_at);
CREATE TABLE IF NOT EXISTS releases (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  version TEXT NOT NULL UNIQUE,
  min_required TEXT NOT NULL,
  payload_path TEXT NOT NULL,
  sha256 TEXT NOT NULL,
  signature TEXT NOT NULL,
  changelog TEXT NOT NULL DEFAULT '',
  published_at TEXT NOT NULL,
  is_forced INTEGER NOT NULL DEFAULT 0 CHECK (is_forced IN (0,1))
);
CREATE TABLE IF NOT EXISTS admin_audit (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  admin_id INTEGER REFERENCES admin_users(id) ON DELETE SET NULL,
  action TEXT NOT NULL,
  target_id INTEGER,
  details TEXT NOT NULL DEFAULT '{}',
  created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS admin_audit_date ON admin_audit(id DESC);
