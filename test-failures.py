"""Linux regression test: python3 test-failures.py (requires g++)."""
import pathlib
import socket
import socketserver
import struct
import subprocess
import sys
import tempfile
import time
import threading

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
    build_command = [client, '--host', '127.0.0.1', '--port', str(port)]
    client_command = build_command + ['--compile-only']
    # A deliberately slow worker records actual overlapping requests.
    class MeasuredWorker(socketserver.ThreadingTCPServer):
        allow_reuse_address = True
        active = peak = completed = 0
        lock = threading.Lock()

    class RejectJob(socketserver.BaseRequestHandler):
        def handle(self):
            self.request.settimeout(5)
            def receive(size):
                data = b''
                while len(data) < size:
                    chunk = self.request.recv(size - len(data))
                    if not chunk:
                        raise RuntimeError('Unexpected disconnect')
                    data += chunk
                return data
            def number():
                return struct.unpack('!I', receive(4))[0]
            def blob():
                return receive(number())
            with self.server.lock:
                self.server.active += 1
                self.server.peak = max(self.server.peak, self.server.active)
            try:
                blob()  # source
                for _ in range(number()):
                    blob()  # flag
                for _ in range(number()):
                    blob()  # path
                    blob()  # contents
                time.sleep(0.3)
                self.request.sendall(struct.pack('!I', 0))
            finally:
                with self.server.lock:
                    self.server.active -= 1
                    self.server.completed += 1

    pool_sources = [f'parallel_{i}.cpp' for i in range(9)]
    for name in pool_sources:
        (root / name).write_text('int f() { return 0; }')
    for options, limit in ((['-j', '1'], 1), (['-j3'], 3), ([], 4)):
        with MeasuredWorker(('127.0.0.1', 0), RejectJob) as measured:
            thread = threading.Thread(target=measured.serve_forever)
            thread.start()
            try:
                result = subprocess.run([client, '--port', str(measured.server_address[1]),
                                         '--compile-only'] + options + pool_sources,
                                        cwd=root, capture_output=True, timeout=30)
                assert result.returncode != 0
                assert b'failures=9' in result.stdout, result.stdout
                assert measured.completed == 9 and measured.peak == limit, (measured.completed, measured.peak)
            finally:
                measured.shutdown()
                thread.join()
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
                            ['--port', '12x'], ['--host'], ['--port'], ['-j'],
                            ['-j0'], ['-j-1'], ['-j257'], ['-j', 'abc']):
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
            (root / 'build.cpp').write_text('int helper(); int main() { return helper() == 7 ? 0 : 1; }')
            (root / 'helper.cpp').write_text('int helper() { return 7; }')
            build_args = build_command + ['-j', '1', '-o', 'build app', 'build.cpp', 'helper.cpp']
            for expected in (b'compiled=2, cache hits=0', b'compiled=0, cache hits=2'):
                build = subprocess.run(build_args, cwd=root, capture_output=True, timeout=30)
                assert build.returncode == 0, build.stderr
                assert expected in build.stdout and b'link=succeeded' in build.stdout, build.stdout
                subprocess.run([str(root / 'build app')], check=True)
            previous = (root / 'build app').read_bytes()
            (root / 'helper.cpp').write_text('int helper() { invalid syntax; }')
            build = subprocess.run(build_args, cwd=root, capture_output=True, timeout=30)
            assert build.returncode != 0 and b'failures=1, link=skipped' in build.stdout
            assert (root / 'build app').read_bytes() == previous
            # Existing objects in returned/ must not supply the missing helper.
            build = subprocess.run(build_command + ['-o', 'build app', 'build.cpp'],
                                   cwd=root, capture_output=True, timeout=30)
            assert build.returncode != 0 and b'link=failed' in build.stdout
            assert (root / 'build app').read_bytes() == previous
            duplicate = subprocess.run(build_command + ['main.cpp', './main.cpp'],
                                       cwd=root, capture_output=True, timeout=5)
            assert duplicate.returncode != 0 and b'Duplicate object' in duplicate.stderr
            time.sleep(0.5)
            assert server.poll() is None
            assert list((root / 'worker-jobs').iterdir()) == [root / 'worker-jobs/job-1']
            print('PASS: measured -j1/-j3/default concurrency, queue progress after failures, complete builds, statistics, diagnostics, options, failure recovery, cleanup')
        finally:
            server.terminate()
            server.wait(timeout=5)
            if sys.exc_info()[0]:
                print((root / 'worker.log').read_text())
