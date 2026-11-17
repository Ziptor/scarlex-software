import { randomBytes, createCipheriv, createDecipheriv, createHash } from 'node:crypto';
import { readFileSync, writeFileSync, chmodSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';

export function createLicenseVault() {
  const path=process.env.SCARLEX_LICENSE_VAULT_KEY || join(dirname(process.env.SCARLEX_DB_PATH || '/var/lib/scarlex-server/licenses.db'),'license-vault.key');
  mkdirSync(dirname(path),{recursive:true,mode:0o700});
  try { writeFileSync(path,randomBytes(32),{flag:'wx',mode:0o600}); }
  catch(error) { if(error.code!=='EEXIST')throw error; }
  chmodSync(path,0o600);
  const secret=readFileSync(path);
  if(secret.length!==32)throw new Error('Invalid license vault key');
  const seal=(key,hash=createHash('sha256').update(key).digest('hex'))=>{
    const nonce=randomBytes(12),cipher=createCipheriv('aes-256-gcm',secret,nonce);
    cipher.setAAD(Buffer.from(hash));
    const ciphertext=Buffer.concat([cipher.update(key,'utf8'),cipher.final()]);
    return Buffer.concat([Buffer.from([1]),nonce,cipher.getAuthTag(),ciphertext]).toString('base64');
  };
  const reveal=row=>{
    if(!row.key_cipher)return null;
    const bytes=Buffer.from(row.key_cipher,'base64');
    if(bytes.length<30||bytes[0]!==1)throw new Error('Invalid encrypted license record');
    const cipher=createDecipheriv('aes-256-gcm',secret,bytes.subarray(1,13));
    cipher.setAAD(Buffer.from(row.key_hash));cipher.setAuthTag(bytes.subarray(13,29));
    const key=Buffer.concat([cipher.update(bytes.subarray(29)),cipher.final()]).toString('utf8');
    if(createHash('sha256').update(key).digest('hex')!==row.key_hash)throw new Error('License vault integrity failure');
    return key;
  };
  return {seal,reveal};
}
