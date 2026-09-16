"""Example regression tests and a small, repeatable local benchmark."""
import argparse
from contextlib import contextmanager
import datetime
import json
import os
from pathlib import Path
import platform
import re
import shutil
import socket
import statistics
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parent

def run(args, cwd):
    result = subprocess.run(list(map(str, args)), cwd=cwd, capture_output=True, text=True, timeout=120)
    if result.returncode:
        raise RuntimeError(f'{args}\n{result.stdout}\n{result.stderr}')
    return result.stdout

@contextmanager
def worker(binary, directory):
    directory.mkdir()
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    with (directory / 'worker.log').open('w') as log:
        process = subprocess.Popen([str(binary), '--port', str(port)], cwd=directory,
                                   stdout=log, stderr=log)
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError((directory / 'worker.log').read_text())
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=0.1):
                        pass
                    break
                except OSError:
                    time.sleep(0.05)
            else:
                raise RuntimeError('Worker did not start')
            yield port
        finally:
            process.terminate()
            process.wait(timeout=5)

def counts(output):
    match = re.search(r'compiled=(\d+), cache hits=(\d+), failures=(\d+)', output)
    assert match, output
    return tuple(map(int, match.groups()))

def examples(client, worker_binary, root):
    with worker(worker_binary, root / 'example-worker') as port:
        command = [client, '--port', str(port), '-j', '4', '-o', 'example-app']
        def build(files, expected, flags=None, hits=None):
            output = run(command + (flags or ['-std=c++20']) + ['--'] + files, root)
            if hits is not None:
                assert counts(output) == hits, output
            assert run([root / 'example-app'], root) == expected
        build(['main.cpp', 'math.cpp', 'strings.cpp'], 'Hello, Forge\n10 + 20 = 30\n6 * 7 = 42\n')
        dependency = ['dependency-test/main.cpp', 'dependency-test/engine.cpp']
        build(dependency, '150\n', hits=(2, 0, 0))
        build(dependency, '150\n', hits=(0, 2, 0))
        header = root / 'dependency-test/config.h'
        header.write_text(header.read_text().replace('30', '31'))
        build(dependency, '155\n', hits=(2, 0, 0))
        for factor in (7, 8):
            build(['flag-test/src/main.cpp'], f'{6 * factor}\n',
                  ['-std=c++20', '-Iflag-test/include', f'-DFACTOR={factor}'], (1, 0, 0))
        files = sorted(str(p.relative_to(root)) for p in (root / 'complex-test/src').rglob('*.cpp'))
        build(files, 'engine parser: 10\nutils parser: 20\nhttp client: 30\nhttp server: 40\n'
                     'auth handler: 50\npayments handler: 60\ntotal: 210\n')
        (root / 'path-main.cpp').write_text('int engine_parser(); int utils_parser(); '
                                         'int main() { return engine_parser()+utils_parser()==3 ? 0 : 1; }')
        build(['path-main.cpp', 'path-test/engine/parser.cpp', 'path-test/utils/parser.cpp'], '')
        build(pool_sources(root), '210\n')
    print('PASS: basic, nested paths, duplicate basenames, transitive headers, flag changes, 20-file pool')

def pool_sources(root):
    files = [f'pool-test/file_{i}.cpp' for i in range(1, 21)]
    declarations = '\n'.join(f'int function_{i}();' for i in range(1, 21))
    total = '+'.join(f'function_{i}()' for i in range(1, 21))
    (root / 'pool-main.cpp').write_text('#include <iostream>\n' + declarations
                                      + '\nint main() { std::cout << (' + total + ') << "\\n"; }')
    return files + ['pool-main.cpp']

def benchmark(client, worker_binary, root, repeats, jobs):
    files = pool_sources(root)
    samples = {'local_sequential': [], 'forge_cold': [], 'forge_warm': []}
    for trial in range(repeats):
        objects = []
        started = time.perf_counter()
        for i, source in enumerate(files):
            obj = f'baseline-{i}.o'
            run(['g++', '-std=c++20', '-O2', '-c', source, '-o', obj], root)
            objects.append(obj)
        run(['g++', '-std=c++20', '-O2'] + objects + ['-o', 'baseline-app'], root)
        samples['local_sequential'].append(time.perf_counter() - started)
        assert run([root / 'baseline-app'], root) == '210\n'
        # A fresh worker directory starts with no Forge object cache.
        with worker(worker_binary, root / f'benchmark-worker-{trial}') as port:
            command = [client, '--port', str(port), '-j', str(jobs), '-o', 'benchmark-app',
                       '-std=c++20', '-O2', '--'] + files
            for mode, expected in [('forge_cold', (21, 0, 0)), ('forge_warm', (0, 21, 0))]:
                started = time.perf_counter()
                output = run(command, root)
                samples[mode].append(time.perf_counter() - started)
                assert counts(output) == expected, output
                assert run([root / 'benchmark-app'], root) == '210\n'
    return {
        'measured_at_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'platform': platform.platform(), 'logical_cpus': os.cpu_count(),
        'compiler': run(['g++', '--version'], root).splitlines()[0],
        'jobs': jobs, 'repeats': repeats, 'source_files': len(files),
        'flags': ['-std=c++20', '-O2'],
        'method': 'Client and worker on the same machine over loopback. 20 tiny pool files plus '
                  'a generated main. Local baseline compiles serially. Each trial runs local, '
                  'cold Forge, then warm Forge. Timings include linking, exclude tool builds, '
                  'worker startup, and executable verification. OS filesystem cache is not cleared.',
        'seconds': samples,
        'median_seconds': {key: statistics.median(value) for key, value in samples.items()}}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--benchmark', action='store_true')
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.repeats < 1 or not 1 <= args.jobs <= 256:
        parser.error('repeats must be positive; jobs must be between 1 and 256')
    with tempfile.TemporaryDirectory(prefix='forge-examples-') as directory:
        root = Path(directory)
        for name in ('main.cpp', 'math.cpp', 'math.h', 'strings.cpp', 'strings.h'):
            shutil.copy2(REPO / name, root / name)
        for name in ('dependency-test', 'flag-test', 'pool-test', 'path-test', 'complex-test'):
            shutil.copytree(REPO / name, root / name)
        for name in ('forge-client', 'forge-worker'):
            run(['g++', '-std=c++20', '-O2', '-pthread', '-Wall', '-Wextra',
                 REPO / f'{name}.cpp', '-o', root / name], root)
        if args.benchmark:
            result = benchmark(root / 'forge-client', root / 'forge-worker', root, args.repeats, args.jobs)
            text = json.dumps(result, indent=2)
            print(text)
            if args.output:
                args.output.write_text(text + '\n')
        else:
            examples(root / 'forge-client', root / 'forge-worker', root)

if __name__ == '__main__':
    main()
