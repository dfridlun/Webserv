#!/usr/bin/python3
# Sets its own status code with the CGI "Status:" header.
print("Status: 418\nContent-Type: text/plain\n\nshort and stout", end="")
