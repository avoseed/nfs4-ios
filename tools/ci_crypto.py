"""Authenticated encryption for owner-only CI inputs, app packages and diagnostics.

Only port source is public. Keep secrets/build.key private and configure the same
base64 AES-256 key as the repository secret NFS4_BUILD_KEY. No key is printed.
"""
from pathlib import Path
import argparse, base64, hashlib, io, json, os, secrets, sys, zipfile
ROOT = Path(__file__).resolve().parents[1]
if (ROOT/'.python-deps').is_dir():
    sys.path.insert(0, str(ROOT/'.python-deps'))
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
MAGIC = b'NFS4-AESGCM-v1\0'
NAMES = ('nfs4.exe', 'voodoo2a.dll', 'eacsnd.dll')


def key(path=None):
    raw = os.environ.get('NFS4_BUILD_KEY')
    if not raw:
        p = Path(path) if path else ROOT/'secrets/build.key'
        raw = p.read_text(encoding='ascii').strip()
    value = base64.b64decode(raw, validate=True)
    if len(value) != 32:
        raise ValueError('Expected a base64 AES-256 build key')
    return value


def encrypt(data, value):
    nonce = secrets.token_bytes(12)
    return MAGIC + nonce + AESGCM(value).encrypt(nonce, data, MAGIC)


def decrypt(data, value):
    if not data.startswith(MAGIC) or len(data) < len(MAGIC)+28:
        raise ValueError('Invalid encrypted artifact header')
    start = len(MAGIC)
    return AESGCM(value).decrypt(data[start:start+12], data[start+12:], MAGIC)


def write(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def bundle_inputs(value):
    buf = io.BytesIO()
    metadata = {}
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name in NAMES:
            data = (ROOT/'binaries'/name).read_bytes()
            z.writestr(name, data)
            metadata[name] = {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    write(ROOT/'ci/inputs.aes', encrypt(buf.getvalue(), value))
    write(ROOT/'ci/input-manifest.json', (json.dumps(metadata, indent=2)+'\n').encode('utf-8'))
    print('Encrypted build inputs prepared (3 files)')


def restore_inputs(value, destination):
    metadata = json.loads((ROOT/'ci/input-manifest.json').read_text(encoding='utf-8'))
    data = decrypt((ROOT/'ci/inputs.aes').read_bytes(), value)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        if sorted(z.namelist()) != sorted(NAMES):
            raise ValueError('Unexpected input archive members')
        for name in NAMES:
            item = z.getinfo(name)
            if item.file_size != metadata[name]['size'] or item.file_size > 10_000_000:
                raise ValueError('Input size mismatch: '+name)
            content = z.read(name)
            if hashlib.sha256(content).hexdigest() != metadata[name]['sha256']:
                raise ValueError('Input checksum mismatch: '+name)
            write(Path(destination)/name, content)
    print('Build inputs verified and restored')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('operation', choices=['bundle-inputs','restore-inputs','encrypt','decrypt','diagnostics'])
    p.add_argument('input', nargs='?')
    p.add_argument('output', nargs='?')
    p.add_argument('--key-file')
    a = p.parse_args()
    value = key(a.key_file)
    if a.operation == 'bundle-inputs':
        bundle_inputs(value)
    elif a.operation == 'restore-inputs':
        restore_inputs(value, ROOT/'binaries')
    elif a.operation == 'diagnostics':
        out = ROOT/'ci-output'; out.mkdir(exist_ok=True)
        for path in [ROOT/'recompilation.log', ROOT/'build.log', ROOT/'build/NFS4-LinkMap.txt']:
            if path.is_file():
                write(out/(path.name+'.aes'), encrypt(path.read_bytes(), value))
        print('Available diagnostics encrypted')
    else:
        if not a.input or not a.output:
            p.error('input and output paths required')
        data = Path(a.input).read_bytes()
        write(a.output, encrypt(data, value) if a.operation == 'encrypt' else decrypt(data, value))
        print('Artifact '+('encrypted' if a.operation == 'encrypt' else 'decrypted'))


if __name__ == '__main__':
    main()
