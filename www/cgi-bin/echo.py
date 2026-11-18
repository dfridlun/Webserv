#!/usr/bin/python3
# POST test: sends the request body back.
import os
import sys

length = int(os.environ.get("CONTENT_LENGTH", "0") or 0)
body = sys.stdin.buffer.read(length)

sys.stdout.write("Content-Type: text/plain\r\n\r\n")
sys.stdout.flush()
sys.stdout.buffer.write(body)
