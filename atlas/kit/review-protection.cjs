// Executable local protection checks. These detect drift and reject unsafe reviewer profiles;
// they do not provide an OS sandbox or prevent an authorized shell from writing.
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const READ_ONLY_TOOLS = Object.freeze(['read','grep','find','ls','contact_supervisor']);
const DANGEROUS = new Set(['edit','write','create','patch','bash','shell','exec','run','kill','stop','Start-Process','Stop-Process','process.kill']);
function validateProfile(profile) {
  if (!profile || typeof profile !== 'object' || Array.isArray(profile) || !Array.isArray(profile.tools))
    throw new Error('profile.tools must be an array');
  const unsupported = Object.keys(profile).filter(key => key !== 'tools');
  if (unsupported.length) throw new Error('read-only profile contains unsupported capability fields: ' + unsupported.join(', '));
  const tools = new Set(profile.tools);
  const unknown = profile.tools.filter(t => !READ_ONLY_TOOLS.includes(t));
  if (unknown.length) throw new Error('read-only profile contains denied/ambient capabilities: ' + unknown.join(', '));
  if (tools.size !== profile.tools.length) throw new Error('profile tools must be unique');
  return true;
}
function digest(file) { return crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex'); }
function verifyManifest(manifest, root) {
  if (!manifest || manifest.schema !== 'atlas-source-manifest-v1' || !Array.isArray(manifest.files)) throw new Error('invalid manifest');
  const base = fs.realpathSync(root); const expected = new Map();
  for (const e of manifest.files) {
    if (!e || typeof e.path !== 'string' || path.isAbsolute(e.path) || e.path.split(/[\\/]/).some(s=>s==='..'||s==='')) throw new Error('unsafe manifest path: '+(e && e.path));
    if (!/^[a-f0-9]{64}$/.test(e.sha256)) throw new Error('invalid manifest digest: '+e.path);
    expected.set(e.path.replace(/\\/g,'/'), e.sha256);
  }
  const actual = new Map();
  function walk(dir) { for (const e of fs.readdirSync(dir,{withFileTypes:true})) { if (['.git','node_modules','.pi','.agent-gate'].includes(e.name)) continue; const full=path.join(dir,e.name); if(e.isDirectory()) walk(full); else if(e.isFile()){const real=fs.realpathSync(full);if(!real.toLowerCase().startsWith(base.toLowerCase()+path.sep.toLowerCase())) throw new Error('path escapes project: '+full);actual.set(path.relative(base,full).split(path.sep).join('/'),digest(full));} else throw new Error('unreadable/non-regular entry: '+full); } }
  walk(base);
  const drift=[]; for(const [p,h] of expected) {if(!actual.has(p)) drift.push('deleted:'+p); else if(actual.get(p)!==h) drift.push('changed:'+p);} for(const p of actual.keys()) if(!expected.has(p)) drift.push('added:'+p);
  if (drift.length) throw new Error('manifest drift: '+drift.join(', '));
  return {ok:true,fileCount:actual.size};
}
module.exports={READ_ONLY_TOOLS,validateProfile,verifyManifest,digest};
