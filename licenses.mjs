import { createHash, generateKeyPairSync, randomBytes, sign, verify } from 'node:crypto';
import { DatabaseSync } from 'node:sqlite';

const hash = value => createHash('sha256').update(value).digest('hex');
const isDevice = value => typeof value === 'string' && /^[a-f0-9]{64}$/.test(value);

export function createLicenseDemo(clock = () => Math.floor(Date.now() / 1000)) {
  const db = new DatabaseSync(':memory:');
  db.exec(`CREATE TABLE licenses (
    id INTEGER PRIMARY KEY,
    key_hash TEXT UNIQUE NOT NULL,
    device TEXT,
    expires_at INTEGER NOT NULL
  ) STRICT`);
  const { privateKey, publicKey } = generateKeyPairSync('ed25519');
  db.prepare('INSERT INTO licenses (key_hash, expires_at) VALUES (?, ?)')
    .run(hash('SCARLEX-PORTFOLIO-DEMO'), clock() + 86400);
  const byKey = db.prepare('SELECT * FROM licenses WHERE key_hash = ?');
  const byId = db.prepare('SELECT * FROM licenses WHERE id = ?');
  const bind = db.prepare('UPDATE licenses SET device = ? WHERE id = ? AND device IS NULL');

  function issue(license, now) {
    const expiresAt = Math.min(now + 1800, license.expires_at);
    const claims = Buffer.from(JSON.stringify({ aud: 'scarlex-portfolio', id: license.id,
      device: license.device, issuedAt: now, expiresAt, nonce: randomBytes(16).toString('hex') }));
    return { status: 'ok', expiresAt, ticket: `${claims.toString('base64url')}.${sign(null, claims, privateKey).toString('base64url')}` };
  }

  return {
    activate({ key, device } = {}) {
      if (typeof key !== 'string' || key.length > 128 || !isDevice(device)) return { status: 'invalid' };
      const license = byKey.get(hash(key));
      const now = clock();
      if (!license) return { status: 'invalid' };
      if (license.expires_at <= now) return { status: 'expired' };
      bind.run(device, license.id);
      const bound = byId.get(license.id);
      if (bound.device !== device) return { status: 'device_mismatch' };
      return issue(bound, now);
    },
    refresh({ ticket } = {}) {
      if (typeof ticket !== 'string' || ticket.length > 2048 || !/^[\w-]+\.[\w-]{86}$/.test(ticket)) return { status: 'invalid' };
      try {
        const [encoded, encodedSignature] = ticket.split('.');
        const claimsBytes = Buffer.from(encoded, 'base64url');
        const signature = Buffer.from(encodedSignature, 'base64url');
        if (claimsBytes.toString('base64url') !== encoded || signature.toString('base64url') !== encodedSignature ||
            !verify(null, claimsBytes, publicKey, signature)) return { status: 'invalid' };
        const claims = JSON.parse(claimsBytes.toString('utf8'));
        const now = clock();
        if (claims.aud !== 'scarlex-portfolio') return { status: 'invalid' };
        const license = byId.get(claims.id);
        if (!license || license.device !== claims.device) return { status: 'invalid' };
        if (claims.expiresAt <= now || license.expires_at <= now) return { status: 'expired' };
        return issue(license, now);
      } catch { return { status: 'invalid' }; }
    },
    close: () => db.close(),
  };
}
