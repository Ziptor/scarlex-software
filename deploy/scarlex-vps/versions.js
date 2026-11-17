import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

export const releasePath = name => resolve(process.env.SCARLEX_RELEASE_DIR || '.', name);

export const validVersion = value => typeof value === 'string' &&
  /^(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})$/.test(value) &&
  value.split('.').every(part => Number(part) <= 65535);

export function compareVersion(left, right) {
  const a = left.split('.').map(Number), b = right.split('.').map(Number);
  return a[0] - b[0] || a[1] - b[1] || a[2] - b[2];
}

export function readManifest() {
  try {
    return JSON.parse(readFileSync(releasePath('version.json'), 'utf8'));
  } catch (error) {
    if (error.code === 'ENOENT') return null;
    throw error;
  }
}
