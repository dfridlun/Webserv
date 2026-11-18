#!/usr/bin/python3
# Lists the fds this script has open. Should be only stdin/stdout/stderr
# (+ the one listdir opens): the server must not leak its own fds to us.
import os
fds = sorted(int(f) for f in os.listdir("/proc/self/fd"))
print("Content-Type: text/plain\n\n" + " ".join(map(str, fds)), end="")
