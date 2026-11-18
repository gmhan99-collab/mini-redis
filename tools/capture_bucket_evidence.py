"""Capture a small, reproducible, alternated two-structure run without touching servers."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--exe', required=True)
parser.add_argument('--output-parent', required=True)
args = parser.parse_args()
exe = Path(args.exe).resolve()
parent = Path(args.output_parent).resolve()
parent.mkdir(parents=True, exist_ok=True)
directory = Path(tempfile.mkdtemp(prefix='bucket-evidence-', dir=parent))
evidence = {'version': 1, 'environment': {'platform': platform.platform(), 'processor': platform.processor(),
            'python': platform.python_version(), 'logical_cpus': os.cpu_count(),
            'gcc': subprocess.check_output(['gcc', '--version'], text=True).splitlines()[0]},
            'executable': str(exe), 'commands': [], 'datasets': {}, 'runs': []}
for distribution in ['normal', 'collision']:
    options = ['--count', '64', '--queries', '400', '--rounds', '1', '--seed', '7', '--hit-percent', '75',
               '--initial-buckets', '4', '--hash-seed', '000102030405060708090a0b0c0d0e0f',
               '--distribution', distribution]
    command = [str(exe), *options, '--prepare-only']
    evidence['commands'].append(command)
    prepared = json.loads(subprocess.check_output(command, text=True, timeout=30))
    fingerprint = hashlib.sha256(json.dumps([prepared[k] for k in ['keys', 'values', 'query_ids']],
                                            separators=(',', ':')).encode()).hexdigest()
    evidence['datasets'][distribution] = {'sha256': fingerprint, 'prepared': prepared}
    for round_index in range(3):
        order = ['list', 'rbtree'] if round_index % 2 == 0 else ['rbtree', 'list']
        for position, mode in enumerate(order):
            command = [str(exe), *options, '--bucket-structure', mode]
            evidence['commands'].append(command)
            result = json.loads(subprocess.check_output(command, text=True, timeout=30))
            row = result['rounds'][0]
            assert result['ok'] and row['verified'] and row['hits'] == 300 and row['misses'] == 100
            evidence['runs'].append({'distribution': distribution, 'external_round': round_index,
                                      'execution_position': position, 'dataset_sha256': fingerprint,
                                      'result': result})
path = directory / 'evidence.json'
path.write_text(json.dumps(evidence, ensure_ascii=False, indent=2), encoding='utf-8')
print(path)
