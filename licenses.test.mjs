import assert from 'node:assert/strict';
import test from 'node:test';
import { createLicenseDemo } from './licenses.mjs';

test('activation, device binding, renewal, signature rejection, and expiration', t => {
  let now = 1_000_000;
  const demo = createLicenseDemo(() => now);
  t.after(demo.close);
  const key = 'SCARLEX-PORTFOLIO-DEMO';
  const device = 'a'.repeat(64);
  assert.equal(demo.activate({ key: 'wrong', device }).status, 'invalid');
  const session = demo.activate({ key, device });
  assert.equal(session.status, 'ok');
  assert.equal(demo.activate({ key, device: 'b'.repeat(64) }).status, 'device_mismatch');
  now += 60;
  assert.ok(demo.refresh({ ticket: session.ticket }).expiresAt > session.expiresAt);
  const [payload, signature] = session.ticket.split('.');
  const corrupt = (signature.startsWith('A') ? 'B' : 'A') + signature.slice(1);
  assert.equal(demo.refresh({ ticket: `${payload}.${corrupt}` }).status, 'invalid');
  now = session.expiresAt;
  assert.equal(demo.refresh({ ticket: session.ticket }).status, 'expired');
});
