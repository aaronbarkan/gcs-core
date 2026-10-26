#!/usr/bin/env python3
"""Exercise native Core, a native simulator, and host grpcurl on default ports."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--grpcurl', default='grpcurl')
parser.add_argument('--binary', default=str(root / 'build-mac/gcs-core'))
parser.add_argument('--simulator-dir', type=Path, default=root.parent / 'uav-simulator')
args = parser.parse_args()
rpc_args = [args.grpcurl, '-plaintext', '-emit-defaults', '-import-path', str(root / 'proto'),
            '-proto', 'telemetry.proto', '-d', '{}', 'localhost:50051']
processes = []

def rpc():
    return subprocess.run(rpc_args + ['gcs.v1.TelemetryService/GetTelemetry'],
                          capture_output=True, text=True, timeout=3)

def snapshot():
    response = rpc()
    assert response.returncode == 0, response.stderr
    return json.loads(response.stdout)

def wait_for(check):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.1)
    raise AssertionError('Timed out waiting for expected native state')

def read(file):
    return os.pread(file.fileno(), 1_000_000, 0).decode()

with tempfile.TemporaryFile() as logs, tempfile.TemporaryFile() as events:
    try:
        core = subprocess.Popen([args.binary], stdout=logs, stderr=logs)
        processes.append(core)
        wait_for(lambda: 'LINK WAITING' in read(logs) or core.poll() is not None)
        assert core.poll() is None, read(logs)
        assert 'NotFound' in rpc().stderr
        stream = subprocess.Popen(rpc_args + ['gcs.v1.TelemetryService/SubscribeTelemetry'],
                                  stdout=events, stderr=logs)
        processes.append(stream)
        wait_for(lambda: 'WAITING' in read(events))
        sim = subprocess.Popen([str(args.simulator_dir / '.venv/bin/python'),
            str(args.simulator_dir / 'simulator.py'), '--host', '127.0.0.1',
            '--scenario', 'straight', '--speed-mps', '5', '--heading-deg', '90'],
            stdin=subprocess.PIPE, stdout=logs, stderr=logs, text=True)
        processes.append(sim)
        wait_for(lambda: rpc().returncode == 0)
        first = snapshot()
        time.sleep(0.6)
        later = snapshot()
        assert later['status'] == 'LIVE'
        assert later['telemetry']['speedMps'] == 5
        assert later['telemetry']['longitudeDeg'] > first['telemetry']['longitudeDeg']
        sim.stdin.write('pause\n')
        sim.stdin.flush()
        wait_for(lambda: snapshot()['status'] == 'STALE')
        wait_for(lambda: 'STALE' in read(events))
        wait_for(lambda: 'LINK STALE' in read(logs))
        stale = snapshot()
        assert stale['telemetry']['speedMps'] == 5
        sim.stdin.write('resume\n')
        sim.stdin.flush()
        wait_for(lambda: int(snapshot()['revision']) > int(stale['revision']))
        assert snapshot()['status'] == 'LIVE'
        assert read(logs).count('LINK LIVE') == 2
        core.terminate()
        assert core.wait(timeout=5) == 0
        stream.wait(timeout=3)
        assert 'GCS Core stopped' in read(logs)
        print('PASS native startup, unary NOT_FOUND, WAITING stream, straight telemetry,')
        print('     LIVE/STALE/resume over UDP and gRPC, shutdown with active stream')
    except Exception:
        print(read(logs))
        raise
    finally:
        for proc in reversed(processes):
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
