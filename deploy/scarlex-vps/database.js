import Database from 'better-sqlite3';
import { readFileSync } from 'node:fs';

export const iso = () => new Date().toISOString();
export function openDatabase(path = process.env.SCARLEX_DB_PATH || '/var/lib/scarlex-server/licenses.db') {
  const db = new Database(path);
  db.pragma('busy_timeout = 5000');
  db.pragma('journal_mode = WAL');
  const schema = readFileSync(new URL('./schema.sql', import.meta.url), 'utf8').replace(/^\uFEFF/, '');
  db.transaction(() => {
    const columns = db.pragma('table_info(licenses)');
    if (columns.some(c => c.name === 'expires_at' && c.notnull)) {
      if (columns.length !== 5) throw new Error('Unexpected legacy license schema; manual migration required');
      db.exec('ALTER TABLE licenses RENAME TO licenses_legacy');
      db.exec(schema);
      const insert = db.prepare(`INSERT INTO licenses(id,key_hash,hwid,expires_at,is_banned,duration_days)
        VALUES(@id,@key_hash,@hwid,@expires_at,@is_banned,@duration_days)`);
      for (const license of db.prepare('SELECT * FROM licenses_legacy').all()) {
        const remaining = Math.ceil((Date.parse(license.expires_at) - Date.now()) / 86400000);
        insert.run({ ...license, duration_days: Math.min(36500, Math.max(1, Number.isFinite(remaining) ? remaining : 30)) });
      }
      db.exec('DROP TABLE licenses_legacy');
    } else db.exec(schema);
    const add=(table,fields)=>{
      const existing=new Set(db.pragma(`table_info(${table})`).map(c=>c.name));
      for(const [name,type] of Object.entries(fields))if(!existing.has(name))db.exec(`ALTER TABLE ${table} ADD COLUMN ${name} ${type}`);
    };
    add('licenses',{key_cipher:'TEXT'});
    add('license_logs',{client_session:'TEXT',client_seq:'INTEGER',client_time:'TEXT',client_version:'TEXT',
      stage:'TEXT',code:'TEXT',level:'TEXT',os_error:'INTEGER',process_exit:'INTEGER'});
    db.exec(`CREATE UNIQUE INDEX IF NOT EXISTS license_logs_client_dedup ON license_logs(license_id,client_session,client_seq)
      WHERE client_session IS NOT NULL`);
    db.pragma('user_version = 3');
  }).immediate();
  db.pragma('foreign_keys = ON');
  return db;
}
export function logLicense(db, id, event, message, hwid = null, ip = null, version = null) {
  db.prepare(`INSERT INTO license_logs(license_id,event_type,message,hwid,ip,version,created_at)
    VALUES(?,?,?,?,?,?,?)`).run(id ?? null, event, message, hwid, ip, version, iso());
}
export function audit(db, adminId, action, targetId, details = {}) {
  db.prepare('INSERT INTO admin_audit(admin_id,action,target_id,details,created_at) VALUES(?,?,?,?,?)')
    .run(adminId ?? null, action, targetId ?? null, JSON.stringify(details), iso());
}
