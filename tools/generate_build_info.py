"""Generate immutable C build metadata, without shell-escaping flag strings."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def c_string(text):
    """Encode UTF-8 bytes as a C literal, independent of JSON/C Unicode escapes."""
    escaped = []
    for byte in text.encode('utf-8'):
        if byte in (ord('"'), ord('\\'), ord('?')):
            escaped.append('\\' + chr(byte))
        elif 32 <= byte < 127:
            escaped.append(chr(byte))
        else:
            # Exactly three octal digits prevent following digits joining the escape.
            escaped.append('\\%03o' % byte)
    return '"' + ''.join(escaped) + '"'


def generate(path, kind):
    flags = os.environ['MEASUREMENT_' + kind.upper() + '_FLAGS']
    compiler = subprocess.run([os.environ['MEASUREMENT_CC'], '--version'],
                              capture_output=True, text=True, check=True).stdout.splitlines()[0]
    levels = re.findall(r'(?:^|\s)(-O(?:[0-9]+|[sgz]|fast)?)(?=\s|$)', flags)
    metadata = dict(compiler=compiler, flags=flags,
                    optimization=levels[-1] if levels else 'compiler default (no -O flag)',
                    source=os.environ['MEASUREMENT_' + kind.upper() + '_SOURCE'])
    path = Path(path)
    old = path.read_text(encoding='utf-8') if path.exists() else ''
    # Preserve timestamp and mtime when all compilation inputs are unchanged.
    signature = hashlib.sha256(('compile-date-v3-c-escaping ' + json.dumps(
        metadata, sort_keys=True, ensure_ascii=True)).encode()).hexdigest()
    if old.startswith('/* ' + signature + ' */\n'):
        return
    encoded = json.dumps(metadata, ensure_ascii=True, separators=(',', ':'))
    content = ('/* ' + signature + ' */\n#ifndef GENERATED_BUILD_INFO_H\n'
               '#define GENERATED_BUILD_INFO_H\n#define BUILD_INFO_JSON '
               + c_string(encoded[:-1] + ',"build_timestamp":"')
               + ' __DATE__ " " __TIME__ ' + c_string('"}')
               + '\n#define BENCH_BUILD_FLAGS ' + c_string(flags)
               + '\n#define BUILD_TIMESTAMP __DATE__ " " __TIME__\n#endif\n')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding='utf-8')


if __name__ == '__main__':
    generate(sys.argv[1], sys.argv[2])
