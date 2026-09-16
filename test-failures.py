"""Linux regression test: python3 test-failures.py (requires g++)."""
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import time

sources = pathlib.Path(__file__).resolve().parent

def field(value):
    data = value.encode()
    return struct.pack('!I', len(data)) + data

with tempfile.TemporaryDirectory(prefix='forge-test-') as root:
    root = pathlib.Path(root)
    # Use a spare port so an existing Forge worker cannot affect this test.
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    for name in ('forge-worker', 'forge-client'):
        source = (sources / (name + '.cpp')).read_text()
        path = root / (name + '.cpp')
        path.write_text(source)
        subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-pthread',
                        '-I', str(sources), str(path), '-o', str(root / name)], check=True)
    worker, client = str(root / 'forge-worker'), str(root / 'forge-client')
    client_command = [client, '--host', '127.0.0.1', '--port', str(port)]
    (root / 'worker-jobs/job-1').mkdir(parents=True)
    (root / 'worker-jobs/job-1/stale.h').write_text('old data')
    (root / 'main.cpp').write_text('int main() { return 0; }\n')
    with (root / 'worker.log').open('w') as log:
        server = subprocess.Popen([worker, '--port', str(port)], cwd=root, stdout=log, stderr=log)
        try:
            for _ in range(50):
                if server.poll() is not None:
                    raise AssertionError('Worker exited; check whether port 9000 is occupied')
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=1):
                        break
                except OSError:
                    time.sleep(0.1)
            else:
                raise AssertionError('Worker did not start')

            # A path collision raises a filesystem exception inside one job.
            with socket.create_connection(('127.0.0.1', port)) as connection:
                connection.sendall(field('main.cpp') + struct.pack('!II', 0, 2)
                                   + field('dir') + struct.pack('!I', 1) + b'x'
                                   + field('dir/main.cpp') + struct.pack('!I', 1) + b'x')

            # Close while the worker is about to send a compilation result.
            source = 'int value() { return 3; }'
            for _ in range(5):
                connection = socket.create_connection(('127.0.0.1', port))
                connection.sendall(field('value.cpp') + struct.pack('!II', 0, 1)
                                   + field('value.cpp') + struct.pack('!I', len(source))
                                   + source.encode())
                connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                connection.close()

            (root / 'bad.cpp').write_text('this is invalid C++')
            failed = subprocess.run(client_command + ['bad.cpp'], cwd=root, capture_output=True, timeout=30)
            assert failed.returncode != 0
            assert b'bad.cpp:1:' in failed.stderr, failed.stderr
            assert b'compiler exit status: 1' in failed.stderr, failed.stderr
            mixed = subprocess.run(client_command + ['bad.cpp', 'main.cpp'], cwd=root,
                                   capture_output=True, timeout=30)
            assert mixed.returncode != 0 and b'bad.cpp:1:' in mixed.stderr
            assert (root / 'returned/main.o').is_file()
            (root / 'warning.cpp').write_text('int f() { int unused; return 0; }')
            warning = subprocess.run(client_command + ['-Wall', '--', 'warning.cpp'],
                                     cwd=root, capture_output=True, timeout=30)
            assert warning.returncode == 0, warning.stderr
            assert b'warning.cpp:' in warning.stderr and b'warning:' in warning.stderr
            for options in (['--host', 'invalid'], ['--port', '0'], ['--port', '65536'],
                            ['--port', '12x'], ['--host'], ['--port']):
                invalid = subprocess.run([client] + options, cwd=root, capture_output=True, timeout=5)
                assert invalid.returncode != 0
            for _ in range(2):
                result = subprocess.run(client_command + ['main.cpp'], cwd=root, capture_output=True, timeout=30)
                assert result.returncode == 0, result.stderr.decode()
                assert (root / 'returned/main.o').stat().st_size > 0
            subprocess.run(['g++', 'returned/main.o', '-o', 'app'], cwd=root, check=True)
            subprocess.run([str(root / 'app')], check=True)
            # Linux /dev/full opens normally but fails when data is written.
            (root / 'returned/main.o').unlink()
            (root / 'returned/main.o').symlink_to('/dev/full')
            failed_write = subprocess.run(client_command + ['main.cpp'], cwd=root,
                                          capture_output=True, timeout=30)
            assert failed_write.returncode != 0
            assert b'Could not write returned object' in failed_write.stderr
            time.sleep(0.5)
            assert server.poll() is None
            assert list((root / 'worker-jobs').iterdir()) == [root / 'worker-jobs/job-1']
            print('PASS: diagnostics, warnings, concurrent failure/success, host/port options, invalid arguments, disconnects, exception recovery, cached builds, linking, failed writes, cleanup')
        finally:
            server.terminate()
            server.wait(timeout=5)
            if sys.exc_info()[0]:
                print((root / 'worker.log').read_text())
