import { createPublicKey } from 'node:crypto';
import { readFileSync } from 'node:fs';

if (process.argv.length !== 3) {
  console.error('Usage: node print_public_key.mjs public.pem');
  process.exit(1);
}
const key = createPublicKey(readFileSync(process.argv[2]));
if (key.asymmetricKeyType !== 'ed25519') throw new Error('Expected an Ed25519 public key.');
const der = key.export({ type: 'spki', format: 'der' });
const prefix = Buffer.from('302a300506032b6570032100', 'hex');
if (der.length !== prefix.length + 32 || !der.subarray(0, prefix.length).equals(prefix)) {
  throw new Error('Unexpected SPKI prefix or length.');
}
const bytes = Array.from(der.subarray(prefix.length), byte => byte.toString(16).padStart(2, '0'));
console.log('{ ' + bytes.map(byte => '0x' + byte).join(', ') + ' }');
console.log('"' + bytes.map(byte => '\\x' + byte).join('') + '"');
