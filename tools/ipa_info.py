"""Print the app's Info.plist identity from an IPA as JSON.

Usage: ipa_info.py <file.ipa>
"""
import json
import plistlib
import sys
import zipfile

KEYS = ['CFBundleDisplayName', 'CFBundleIdentifier', 'CFBundleShortVersionString',
        'CFBundleVersion', 'PortDataFolder', 'MinimumOSVersion']

with zipfile.ZipFile(sys.argv[1]) as z:
    name = next(n for n in z.namelist() if n.count('/') == 2 and n.endswith('.app/Info.plist'))
    info = plistlib.loads(z.read(name))
print(json.dumps({k: info.get(k) for k in KEYS}, ensure_ascii=False))
