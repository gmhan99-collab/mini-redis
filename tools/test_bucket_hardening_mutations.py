"""Kill narrow mutants using isolated source copies; never edits production."""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', default='build/hardening-mutants')
parser.add_argument('--mutant', choices=['root_black', 'black_height', 'rehash_count', 'rehash_duration'])
args = parser.parse_args()
directory = (ROOT / args.build_dir).resolve()
directory.mkdir(parents=True, exist_ok=True)
source = (ROOT / 'src/hash_table/hash_table.c').read_text(encoding='utf-8-sig')
test = (ROOT / 'tests/test_bucket_hardening.c').read_text(encoding='utf-8-sig')
mutants = {
    'root_black': ('is_red(table->buckets[i]) || !validate_tree', '!validate_tree'),
    'black_height': ('!left || left != right', '!left || !right'),
    'rehash_count': ('table->rehash_count++;', '/* mutation: count update removed */'),
    'rehash_duration': ('table->rehash_ms += REHASH_MS(start, REHASH_TICKS());',
                        '(void)REHASH_MS(start, REHASH_TICKS());'),
}
results = []
for name, (original, replacement) in mutants.items():
    if args.mutant and name != args.mutant:
        continue
    assert source.count(original) == 1, name
    changed = directory / f'{name}.c'
    changed.write_text(source.replace(original, replacement), encoding='utf-8')
    driver = directory / f'test_{name}.c'
    driver.write_text(test.replace('../src/hash_table/hash_table.c', changed.name), encoding='utf-8')
    executable = directory / (f'test_{name}.exe' if os.name == 'nt' else f'test_{name}')
    command = ['gcc', '-std=c11', '-Wall', '-Wextra', '-Iinclude', '-Isrc/hash_table',
               '-Ithird_party/siphash', '-Itests', str(driver.relative_to(ROOT)), 'tests/test_allocator.c',
               'src/hash_table/key_hash.c', 'src/hash_table/random_seed.c', 'third_party/siphash/siphash.c']
    if os.name == 'nt':
        command += ['-lbcrypt']
    command += ['-o', str(executable.relative_to(ROOT))]
    subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True, errors="replace")
    result = subprocess.run([str(executable)], cwd=ROOT, capture_output=True, text=True, errors="replace", timeout=10)
    # An assertion is required: a crash/timeout alone does not count as killing it.
    killed = result.returncode != 0 and 'Assertion failed' in result.stderr
    if os.name != 'nt':
        killed = result.returncode != 0 and 'Assertion' in result.stderr
    results.append({'mutant': name, 'killed': killed, 'returncode': result.returncode,
                    'stdout': result.stdout, 'stderr': result.stderr, 'compile_command': command})
    print(name, 'killed by assertion' if killed else 'NOT KILLED')
(directory / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
assert all(result['killed'] for result in results)
