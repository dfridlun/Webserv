#!/usr/bin/python3
# Takes 2 seconds. Other clients must still be served meanwhile.
import time
time.sleep(2)
print("Content-Type: text/plain\n\nslow done", end="")
