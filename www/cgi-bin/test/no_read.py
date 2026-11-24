#!/usr/bin/python3
# Ignores the request body. The server must not crash (SIGPIPE) when the pipe closes.
print("Content-Type: text/plain\n\nignored your body", end="")
