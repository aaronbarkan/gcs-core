"""Integration fixture: run via docker exec -i in the paused simulator container."""
import socket
import sys
import time
from pymavlink.dialects.v20 import common as mavlink

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
encoder = mavlink.MAVLink(None, srcSystem=1, srcComponent=1)
def frame(lat=320000000, lon=348000000, hdg=9000):
    return mavlink.MAVLink_global_position_int_message(
        0, lat, lon, 100000, 50000, -300, 400, 0, hdg).pack(encoder)
def send(data):
    sock.sendto(data, ('gcs-core', 14550))

mode = sys.argv[1]
if mode == 'invalid':
    bad = bytearray(frame())
    bad[-1] ^= 0x80
    send(bytes(bad))
    send(b'not mavlink\x00\x01')
    send(frame(lat=900000001))
    send(frame(lon=-1800000001))
    send(frame(hdg=65535))
    send(frame(hdg=36000))
    encoder.srcSystem = 2
    send(frame())
    encoder.srcSystem = 1
    encoder.srcComponent = 2
    send(frame())
elif mode == 'heartbeat':
    # Valid traffic must not refresh position liveness.
    for _ in range(30):
        send(mavlink.MAVLink_heartbeat_message(2, 0, 128, 0, 4, 3).pack(encoder))
        send(mavlink.MAVLink_attitude_message(0, 0, 0, 1.57, 0, 0, 0).pack(encoder))
        time.sleep(0.1)
elif mode == 'valid':
    # Both coalesced frames and a frame split across datagrams.
    send(frame() + frame())
    packet = frame()
    send(packet[:12])
    send(packet[12:])
else:
    raise ValueError(mode)
