#!/usr/bin/python3
# GET test: shows the query string and the CGI environment.
import os

print("Content-Type: text/html")
print()
print("<h1>Hello from CGI</h1>")
print("<p>Query: " + os.environ.get("QUERY_STRING", "") + "</p>")
print("<pre>")
for key in sorted(os.environ):
    print(key + "=" + os.environ[key])
print("</pre>")
