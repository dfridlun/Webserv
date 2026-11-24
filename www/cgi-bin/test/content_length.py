#!/usr/bin/python3
# Sends a Content-Length chosen by ?mode=exact|short|long|bad
import os, sys
mode = os.environ.get("QUERY_STRING", "").replace("mode=", "")
body = "hello EXTRA"
length = {"exact": "11", "short": "5", "long": "50", "bad": "abc"}.get(mode, "11")
sys.stdout.write("Content-Type: text/plain\r\nContent-Length: " + length + "\r\n\r\n" + body)
