# MAVLink headers

`c_library_v2/` is a vendored, unmodified checkout of the generated MAVLink C
headers from https://github.com/mavlink/c_library_v2 at commit:

`6b4243f0e54b42ddb1e65df7a3a5e8c99d15ec1f`

The same revision is recorded in `MAVLINK_COMMIT`. The build includes the
`common` dialect; the upstream generated dialects and support headers are
retained together. No host mount or network fetch is needed for MAVLink during
the Docker build. Upstream Git metadata is omitted from this vendored copy.
