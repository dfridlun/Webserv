# Webserv: handoff notes (Marc -> Mees, Daniel)

Working notes, not the final README. The final `README.md` still needs the mandatory first line, Description, Instructions and Resources (including how AI was used).

## 1. Where we are

The network core and the HTTP request parser work end to end. You can start the server, send requests with `curl`/`telnet`/`nc`, and get a (hardcoded) response back. Everything runs on one `poll()`, all sockets are non-blocking, and clients that stay silent do not block other clients.

## 2. Build and run

```
make re          # the Makefile does not track headers, so use make re after header changes
./Webserv
```

- Flags: `-Wall -Wextra -Werror`. Warnings break the build (unused private fields, signed/unsigned comparisons, etc.).
- Modern C++ is allowed at our school (no C++98 restriction).
- `main()` currently hardcodes one `ServerConfig` (port 8080, root `./www`). The config file argument is not used yet.

## 3. Architecture

```
main -> Server(vector<ServerConfig>) -> run()

Server
 |- _listeners    vector<unique_ptr<Listener>>   one per config/port
 |- _connections  map<int fd, Connection>        all active clients, flat
 '- run(): the single poll() loop

Connection (one per client)
 |- _fd, _buffer (raw bytes received), _state, _config (const ref)
 |- _request (HttpRequest, filled by the parser)
 '- _response, _bytesSent (what still has to be sent)
```

Key decisions:
- **One poll() for everything.** `run()` rebuilds the `pollfd` vector every iteration from `_listeners` + `_connections`. Listeners always get `POLLIN`. A connection gets `POLLIN` in `READING` and `POLLOUT` otherwise.
- **Connections live on the Server, not on the Listener.** A Connection only holds a `const ServerConfig&` to know which config applies.
- **`Listener` is RAII.** The constructor does socket/bind/listen/non-blocking. It is non-copyable and closes its fd in the destructor.
- **`Connection` does NOT close its own fd.** Always use `Server::closeConnection(fd)` (close + erase). After calling it, `conn` and any iterator are invalid: `return` immediately.
- **`Connection` is non-assignable** (it has a reference member, `operator=` is deleted).
- **`ServerConfig` pointers:** each `Listener` keeps a pointer into the `vector<ServerConfig>`. That vector must be fully filled and `reserve()`d before the Server is created, and must never grow afterwards.

## 4. Flow of one request

1. `poll()` reports `POLLIN` on a listener -> `handleNewConnection` calls `accept()`, sets the fd non-blocking, adds a `Connection` (state `READING`).
2. `poll()` reports `POLLIN` on a client -> `handleReadable` does ONE `recv()`, appends to `_buffer`, calls `parseRequest(buffer, request)`.
   - `PARSE_INCOMPLETE`: do nothing, wait for the next `POLLIN`.
   - `PARSE_DONE`: set state `WRITING` and the response.
   - `PARSE_ERROR`: set state `WRITING` with a 400 response.
3. State is `WRITING` -> next round the fd gets `POLLOUT` -> `handleWritable` does ONE `send()` of the remaining bytes (`_response` from offset `_bytesSent`), and closes when everything is sent.

## 5. The HTTP request contract

`HttpRequest` (plain struct, filled by `parseRequest`):

| Field | Meaning |
|---|---|
| `method` | `GET`, `POST`, ... (not validated against allowed methods) |
| `path` | URL path without the query, e.g. `/test` |
| `query` | raw string after `?`, e.g. `a=1&b=2` (empty if none) |
| `version` | e.g. `HTTP/1.1` |
| `headers` | `map<string,string>`, **names are lowercase** (`host`, `content-length`) |
| `body` | the full body; chunked bodies are already un-chunked |
| `status` | `0` = no problem. `>= 400` = the parser found a problem (400, 501, ...) |

`parseRequest` returns `PARSE_INCOMPLETE`, `PARSE_DONE` or `PARSE_ERROR`.

The parser **marks** errors by setting `req.status`. It does **not** build the error response. The caller (Daniel) turns `req.status` into a real response.

The parser detects: missing/ill-formed request line, header without `:`, invalid `Content-Length` (non-digits, empty, too long), invalid chunk size, `Transfer-Encoding` + `Content-Length` together (400), unsupported transfer encoding (501).

## 6. Known gaps and temporary shortcuts

- The response is **hardcoded** in `handleReadable` (`200 OK`, `<h1>Home</h1>`, no `Content-Length`). Replace it with real response building.
- Every `PARSE_ERROR` currently gets the same hardcoded `400` response, even if `req.status` is 501.
- No `PROCESSING` step yet: after `PARSE_DONE` we jump straight to `WRITING`. This is where routing/handlers/CGI hook in.
- Connection closes after every response (no keep-alive). After a response a keep-alive connection needs `_buffer`, `_request`, `_response` and `_bytesSent` reset.
- In `run()`, a connection that is not `READING` gets `POLLOUT`. This is wrong once `WAITING_CGI` exists.
- `POLLHUP`/`POLLERR` are handled by closing the connection. Refine if needed.
- `poll()` failure (`-1`) is only a `// TODO` print.
- No timeouts: `poll()` runs with `-1`, so an incomplete request waits forever.
- No `client_max_body_size` check. The parser has `// TODO` markers where a 413 belongs.
- The parser re-parses the whole buffer on every `recv`. Fine for now, slow for huge uploads. Large bodies also exist several times in memory (buffer -> request -> handler), so pass them by `const&` / `std::move`.
- Helper functions in the parser return `PARSE_DONE` to mean "this part is fine".

## 7. Rules from the subject to keep in mind

- Never `recv`/`send`/`read`/`write` on a socket or pipe without `poll()` saying it is ready (grade 0). Regular disk files are exempt.
- Never check `errno` after a read/write operation to change behaviour.
- One `poll()` for all I/O (listeners, clients, CGI pipes).
- `fork` only for CGI. Never `execve` another web server.
- A request must never hang forever. The server must stay available under stress.
- Everyone must be able to explain all of the code at the evaluation.

## 8. Tasks

### Mees: config + routing + handlers
- Config file parser (`./webserv config.conf`): server blocks, `listen`, `root`, `error_page`, `client_max_body_size`, locations with allowed methods, redirect, root, autoindex, index, upload store, CGI extension -> interpreter. Fill a `vector<ServerConfig>` and `reserve()` it before use.
- Extend `ServerConfig` / add `LocationConfig` (agree on field names with Daniel: he reads `client_max_body_size` and `error_page`).
- Route matching: pick the location for `req.path` (the `/kapouet` -> `/tmp/www` example from the subject), check allowed methods (405).
- Handlers: static files, directory listing / index file, redirects, GET, POST (uploads), DELETE.
- Handler output: agree with Daniel on one structure (status, headers, body) that the response builder consumes.

### Daniel: CGI + responses + errors + lifecycle
- CGI: pipe/fork/execve, env vars (`REQUEST_METHOD`, `QUERY_STRING`, `CONTENT_LENGTH`, ...), run in the right directory, un-chunked body in, EOF-terminated output if there is no `Content-Length`. CGI pipe fds go into the same `poll()` vector (state `WAITING_CGI`; map pipe fd -> client fd).
- Response building: status line, `Content-Length`, `Content-Type`, `Connection`, body.
- Error handling: default error pages, `error_page` from config, turn `req.status` / handler errors into responses (400/403/404/405/413/500/501).
- Limits: `client_max_body_size` (413) and a maximum header size.
- Keep-alive, `CLOSING`, and extend `closeConnection` (CGI pipes, child processes).
- Timeouts: `poll()` timeout plus last-activity time per connection.
- Proper `poll()` error handling and `POLLHUP`/`POLLERR` refinement in `run()`.
- Stress test script (Python/Go): many simultaneous clients, the server must stay up.

### Marc: core + parser (done, remaining)
- Remove debug output, commit.
- Add the `PROCESSING` hook so handlers/CGI have a clear place to plug in.
- README skeleton with the mandatory sections.

## 9. Testing

```
curl -i "localhost:8080/test?a=1"
curl -i -X POST -d "hallo" localhost:8080
curl -i -X POST -H "Transfer-Encoding: chunked" -d "hallo" localhost:8080
printf 'POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhallo\r\n7\r\n wereld\r\n0\r\n\r\n' | nc localhost 8080
printf 'GET /\r\n\r\n' | nc localhost 8080                     # expect a 400 response
curl -i -X POST -H "Content-Length: 99" -d "hallo" localhost:8080   # hangs on purpose (incomplete)
```

Also test with `telnet localhost 8080` (type a request line by line), keep two idle telnets open while running `curl` (must still answer), and kill a telnet with Ctrl+C while watching `top` (the server must not spin at 100% CPU).

Chunk sizes are hexadecimal and must match the data length exactly.
