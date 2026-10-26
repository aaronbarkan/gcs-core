#!/usr/bin/env python3
"""Real Docker + unchanged simulator + host grpcurl acceptance checks.

Requires images gcs-core:dev and uav-sim:dev. Uses temporary container/network
names and localhost:50051. Does not touch any pre-existing simulator container.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--grpcurl', default='grpcurl')
parser.add_argument('--core-image', default='gcs-core:dev')
parser.add_argument('--sim-image', default='uav-sim:dev')
args = parser.parse_args()
NET = 'gcs-core-acceptance-net'
CORE = 'gcs-core-acceptance'
SIM = 'gcs-core-acceptance-sim'
RPC = [args.grpcurl, '-plaintext', '-emit-defaults', '-import-path', str(ROOT / 'proto'),
       '-proto', 'telemetry.proto', '-d', '{}', 'localhost:50051',
       'gcs.v1.TelemetryService/']
streams = []
simulator = None
created_core = created_network = False
sim_log = tempfile.TemporaryFile(mode='w+')

def run(*cmd, check=True, **kwargs):
    return subprocess.run(cmd, text=True, capture_output=True, check=check, timeout=20, **kwargs)

def rpc(method):
    return run(*(RPC[:-1] + [RPC[-1] + method]), check=False)

def snapshot():
    response = rpc('GetTelemetry')
    assert response.returncode == 0, response.stderr
    return json.loads(response.stdout)

def wait_for(predicate, description, timeout=8):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        result = predicate()
        if result:
            return result
        time.sleep(0.1)
    raise AssertionError('Timed out: ' + description)

def subscribe():
    output = tempfile.TemporaryFile(mode='w+')
    errors = tempfile.TemporaryFile(mode='w+')
    proc = subprocess.Popen(RPC[:-1] + [RPC[-1] + 'SubscribeTelemetry'],
                            stdout=output, stderr=errors, text=True)
    stream = (proc, output, errors)
    streams.append(stream)
    return stream

def events(stream):
    # Read from a separate file descriptor offset; never disturb the writer offset.
    import os
    text = os.pread(stream[1].fileno(), 10_000_000, 0).decode()
    result = []
    decoder = json.JSONDecoder()
    while text.strip():
        try:
            event, end = decoder.raw_decode(text.lstrip())
        except json.JSONDecodeError:
            break  # Last message is still being written.
        result.append(event)
        text = text.lstrip()[end:]
    return result

def cancel(stream):
    stream[0].terminate()
    stream[0].wait(timeout=5)

def start_sim(scenario):
    return subprocess.Popen(['docker', 'run', '--rm', '-i', '--name', SIM,
        '--network', NET, args.sim_image, '--host', 'gcs-core', '--scenario', scenario,
        '--rate-hz', '5', '--speed-mps', '5', '--heading-deg', '90'],
        stdin=subprocess.PIPE, stdout=sim_log, stderr=sim_log, text=True)

def command(value):
    simulator.stdin.write(value + '\n')
    simulator.stdin.flush()

def logs():
    return run('docker', 'logs', CORE).stdout

def inject(mode):
    run('docker', 'exec', '-i', SIM, 'python', '-', mode,
        input=(ROOT / 'tests/send_frames.py').read_text())

def passed(text):
    print('PASS ' + text, flush=True)

try:
    run('docker', 'network', 'create', NET)
    created_network = True
    run('docker', 'run', '-d', '--name', CORE, '--network', NET,
        '--network-alias', 'gcs-core', '-p', '127.0.0.1:50051:50051', args.core_image)
    created_core = True
    wait_for(lambda: 'NotFound' in rpc('GetTelemetry').stderr, 'unary NOT_FOUND')
    first = subscribe()
    wait_for(lambda: events(first), 'initial WAITING event')
    time.sleep(0.7)
    assert events(first) == [{'revision': '0', 'status': 'WAITING', 'telemetry': None}]
    assert first[0].poll() is None
    assert 'LINK WAITING' in logs()
    passed('WAITING emitted once; unary NOT_FOUND; subscription stays connected')

    simulator = start_sim('hover')
    wait_for(lambda: rpc('GetTelemetry').returncode == 0, 'hover telemetry')
    hover = snapshot()
    assert hover['status'] == 'LIVE'
    assert hover['telemetry'] == dict(latitudeDeg=32, longitudeDeg=34.8,
        altitudeMslM=100, relativeAltM=50, speedMps=0, headingDeg=90)
    wait_for(lambda: any(e['status'] == 'LIVE' for e in events(first)), 'stream LIVE')
    second = subscribe()
    wait_for(lambda: events(second), 'second subscriber')
    assert events(second)[0]['status'] == 'LIVE'
    passed('hover units and two independent LIVE subscriptions')

    command('pause')
    wait_for(lambda: snapshot()['status'] == 'STALE', 'pause STALE')
    stale = snapshot()
    wait_for(lambda: events(first)[-1]['status'] == 'STALE', 'stream STALE')
    count = len(events(first))
    time.sleep(0.7)
    assert len(events(first)) == count
    assert events(first)[-1] == stale
    assert logs().count('LINK STALE') == 1
    passed('pause emits one STALE transition and retains last sample')

    inject('invalid')
    time.sleep(0.3)
    assert snapshot() == stale
    inject('valid')
    wait_for(lambda: int(snapshot()['revision']) == int(stale['revision']) + 3,
             'three valid frames')
    valid = snapshot()
    assert valid['telemetry']['speedMps'] == 5
    inject('heartbeat')
    assert snapshot()['status'] == 'STALE'
    assert snapshot()['revision'] == valid['revision']
    passed('corrupt CRC, garbage, invalid coordinates/heading and foreign IDs ignored; '
           'coalesced/split valid frames accepted; heartbeat/attitude cannot mask staleness')

    command('resume')
    wait_for(lambda: snapshot()['status'] == 'LIVE', 'resume LIVE')
    wait_for(lambda: events(first)[-1]['status'] == 'LIVE', 'stream resume')
    assert logs().count('LINK LIVE') == 3  # Initial, injected valid frame, resume.
    cancel(first)
    before = int(snapshot()['revision'])
    wait_for(lambda: int(events(second)[-1]['revision']) > before, 'other stream continues')
    cancel(second)
    before_logs = logs().count('TELEMETRY')
    time.sleep(0.6)
    assert logs().count('TELEMETRY') > before_logs
    third = subscribe()
    wait_for(lambda: events(third), 'reconnect snapshot')
    assert events(third)[0]['status'] == 'LIVE'
    passed('resume, cancellation, zero-subscriber logging and reconnect')

    command('quit')
    simulator.wait(timeout=5)
    simulator = None
    wait_for(lambda: snapshot()['status'] == 'STALE', 'simulator quit STALE')
    simulator = start_sim('straight')
    wait_for(lambda: snapshot()['status'] == 'LIVE', 'simulator restart')
    straight = snapshot()
    time.sleep(0.6)
    later = snapshot()
    assert later['telemetry']['longitudeDeg'] > straight['telemetry']['longitudeDeg']
    assert later['telemetry']['speedMps'] == 5
    assert int(later['revision']) > int(straight['revision'])
    passed('simulator quit/restart and straight-flight motion at 5 m/s')

    for stream in streams:
        pairs = [(e['revision'], e['status']) for e in events(stream)]
        assert all(a != b for a, b in zip(pairs, pairs[1:])), 'duplicate stream event'
    started = time.monotonic()
    run('docker', 'stop', '--time', '5', CORE)
    duration = time.monotonic() - started
    assert run('docker', 'inspect', '--format', '{{.State.ExitCode}}', CORE).stdout.strip() == '0'
    assert duration < 5
    third[0].wait(timeout=3)
    assert 'GCS Core stopped' in logs()
    passed(f'clean shutdown with active stream in {duration:.2f}s, exit code 0')
    print('All Docker/gRPC integration scenarios passed.', flush=True)
finally:
    if simulator is not None:
        if simulator.poll() is None:
            try:
                command('quit')
                simulator.wait(timeout=5)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                run('docker', 'stop', '--time', '2', SIM, check=False)
                simulator.wait(timeout=5)
    for proc, output, errors in streams:
        if proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=5)
        output.close()
        errors.close()
    if created_core:
        run('docker', 'rm', '-f', CORE, check=False)
    if created_network:
        run('docker', 'network', 'rm', NET, check=False)
    sim_log.close()
