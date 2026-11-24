# CGI, responses and timeouts

How webserv turns a request into a response, runs CGI scripts, and makes sure no request hangs forever.
This covers Daniel's part (see `HANDOFF.md` §8): `Response.*`, `Cgi.*`, the timeout code in `Eventloop.cpp`, and `tests/test_cgi.py`.

**Contents**

1. [The big picture](#1-the-big-picture)
2. [Which file does what](#2-which-file-does-what)
3. [New concepts](#3-new-concepts) (read this first if pipes/fork/CGI are new to you)
4. [Workflow: one CGI request, step by step](#4-workflow-one-cgi-request-step-by-step)
5. [Building responses](#5-building-responses)
6. [Timeouts](#6-timeouts)
7. [Status codes: who sends what](#7-status-codes-who-sends-what)
8. [Subject rules and how we follow them](#8-subject-rules-and-how-we-follow-them)
9. [Testing](#9-testing)
10. [Evaluation prep: questions and quick modifications](#10-evaluation-prep-questions-and-quick-modifications)
11. [Known limitations / TODO](#11-known-limitations--todo)

---

## 1. The big picture

Every client is a `Connection` with a **state**. The single `poll()` loop in `Server::run()` looks at the state to decide what to wait for, and the handlers move the connection to the next state.

```
            accept()
               |
               v
        +-------------+   recv() + parseRequest()
        |   READING   |<----------------------------+
        +-------------+                             |
               | PARSE_DONE          PARSE_ERROR    |
               v                         |          |
        processRequest()                 |    (keep-alive: TODO)
          |          |                   |          |
   not CGI|          |CGI: startCgi()    |          |
          |          v                   |          |
          |   +-------------+            |          |
          |   | WAITING_CGI |  pipes in poll()      |
          |   +-------------+            |          |
          |          | script closes stdout -> finishCgi()
          |          |  (or 10s CGI timeout -> 504)  |
          v          v                   v          |
        +-----------------------------------+       |
        |              WRITING              |  send() until done
        +-----------------------------------+       |
                         |                          |
                         v                          |
                 closeConnection(fd) ---------------+
```

Three things can end a connection early, from any state:
- the client disconnects (`recv()` returns 0, or `POLLHUP`/`POLLERR`)
- a timeout (see [§6](#6-timeouts))
- a send/recv error

`closeConnection()` always cleans up a running CGI script first (`stopCgi()`).

---

## 2. Which file does what

| File | What's in it |
|---|---|
| `include/Response.hpp`, `src/Response.cpp` | `HttpResponse` struct, `buildResponse()`, `reasonPhrase()`, `makeErrorResponse()`, `parseCgiOutput()`. No I/O at all, just strings in → strings out. |
| `include/Cgi.hpp` | `CgiProcess` struct (the state of one running script), `findCgiInterpreter()` |
| `src/Cgi.cpp` | Everything that touches a CGI process: `startCgi()`, `closeServerFds()`, `handleCgiEvent()`, `finishCgi()`, `stopCgi()`, `closeCgiFd()`, `reapChildren()`, plus the env var builder |
| `src/HandleClient.cpp` | `handleReadable()` (switch on `ParseResult`), `processRequest()` (the PROCESSING step: CGI or not), `handleWritable()` |
| `src/Eventloop.cpp` | `run()` (builds the poll list incl. CGI pipes, dispatches events), `checkTimeouts()`, the timeout constants |
| `src/Server.cpp` | `closeConnection()` (now also calls `stopCgi()`) |
| `src/Connection.cpp` | `touch()` / `secondsIdle()` for timeouts; `setState()` also resets the timer |
| `src/main.cpp` | `signal(SIGPIPE, SIG_IGN)`, the temporary `.py` → `/usr/bin/python3` mapping |
| `tests/test_cgi.py` | 30 automated checks |
| `www/cgi-bin/` | `hello.py`, `echo.py` (demo), `test/*.py` (one script per edge case) |

Important data on the `Server`:

| Member | Meaning |
|---|---|
| `_connections` | `map<client fd, Connection>`, as before |
| `_cgiFds` | `map<pipe fd, client fd>`. Every open CGI pipe and which client it belongs to. |
| `_children` | PIDs of scripts that ended (or were killed) but haven't been reaped yet |

---

## 3. New concepts

### 3.1 What an HTTP response looks like

```
HTTP/1.1 404 Not Found\r\n          <- status line: version, code, reason phrase
Content-Type: text/html\r\n         <- headers, one per line
Content-Length: 129\r\n             <- exact number of body bytes
Connection: close\r\n
\r\n                                <- empty line = end of headers
<!DOCTYPE html>...                  <- body (exactly Content-Length bytes)
```

`Content-Length` is how the client knows where the body ends. If it's wrong, the browser waits forever (too big) or cuts the page off (too small). That's why `buildResponse()` always computes it itself from `body.size()` and never trusts a value from a handler or script.

### 3.2 What CGI is

CGI (Common Gateway Interface, RFC 3875) is an old, simple agreement between a web server and an external program:

| Direction | How |
|---|---|
| Request info → script | **Environment variables** (`REQUEST_METHOD`, `QUERY_STRING`, `CONTENT_LENGTH`, headers as `HTTP_*`, ...) |
| Request body → script | The script's **stdin**. The script reads until EOF (or `CONTENT_LENGTH` bytes). |
| Response → server | The script's **stdout**: CGI headers, an empty line, then the body |

So a CGI script is any program that reads env vars + stdin and prints to stdout. Our server's job is to start it, feed it, collect the output, and turn that into a real HTTP response.

What a script prints is *not* a full HTTP response. There's no status line:

```
Status: 404 Not Found          <- optional, default 200
Content-Type: text/plain       <- normal headers
                               <- empty line (\n\n is allowed, not only \r\n\r\n)
the body
```

`parseCgiOutput()` turns that into an `HttpResponse`, and `buildResponse()` adds the status line, `Content-Length` and `Connection`.

### 3.3 File descriptors (fds)

An fd is just a number that points to something open: a file, a socket, a pipe. `0` = stdin, `1` = stdout, `2` = stderr. Everything else (listening socket, client sockets, pipes) gets the next free number.

Two facts that matter a lot for CGI:
1. **`fork()` copies all fds.** The child has the same numbers pointing to the same sockets/pipes.
2. **`execve()` keeps all fds open** (unless marked close-on-exec). The script inherits everything the server had open.

### 3.4 `pipe()`

```
int p[2];
pipe(p);      // p[1] = write end  --->  p[0] = read end
```

A one-way channel in the kernel. Whatever is written to `p[1]` can be read from `p[0]`.

**EOF rule:** `read()` on the read end returns `0` (EOF) only when **every** copy of the write end is closed, in every process. One forgotten copy anywhere and the reader waits forever. This rule is the reason for most of the careful closing in `startCgi()`.

We use two pipes per script:

```
                 toChild                         fromChild
server  [1] ------------------> [0]  script  [1] ------------------> [0]  server
        cgi.inFd           stdin (dup2)     stdout (dup2)         cgi.outFd
     (writes the body)                                         (reads the output)
```

### 3.5 `fork()`, `dup2()`, `chdir()`, `execve()`

| Call | What it does here |
|---|---|
| `fork()` | Creates a copy of the server process. Returns `0` in the child, the child's PID in the parent, `-1` on failure. |
| `dup2(old, new)` | Makes fd `new` point to the same thing as `old`. `dup2(toChild[0], 0)` makes the pipe the script's stdin. |
| `chdir(dir)` | Changes the working directory. Subject: *"the CGI should be run in the correct directory for relative path file access"*. |
| `execve(path, argv, envp)` | Replaces the child process with the interpreter. On success it never returns. `path` must be a full path (no `$PATH` search), e.g. `/usr/bin/python3`. |

We run `execve("/usr/bin/python3", {"/usr/bin/python3", "hello.py", NULL}, env)` after `chdir("./www/cgi-bin")`, so the script is found by its plain file name.

### 3.6 Why the child closes the server's fds

Because of §3.3, a freshly forked child holds copies of:
- the listening socket,
- every client socket,
- the pipes of **other** running scripts.

That causes real bugs:
- **Another script hangs.** Script A is still waiting for EOF on its stdin. Script B holds a copy of A's write end → EOF never comes (the §3.4 rule).
- **Clients stay connected.** The server closes a client socket, but the script still has a copy, so the TCP connection doesn't actually close.

The usual fix is `FD_CLOEXEC`, but setting it needs `fcntl(fd, F_SETFD, ...)`, and the subject only allows `F_SETFL`, `O_NONBLOCK` and `FD_CLOEXEC`. So instead the child calls `closeServerFds()` right after `fork()`: it walks `_listeners`, `_connections` and `_cgiFds` and closes every one. Then it closes its four original pipe ends (the copies on 0 and 1 stay).

`tests/test_cgi.py` checks this. `www/cgi-bin/test/fds.py` lists its own fds and must see only `0 1 2 3` (3 is the fd Python uses for the listing itself).

### 3.7 Non-blocking pipes in the same `poll()`

The subject says **one** `poll()` for all I/O and **never** read/write without `poll()` saying the fd is ready. Pipes count. So:
- our two pipe ends are set to `O_NONBLOCK` (only ours; the script's ends stay blocking, scripts expect that),
- they're added to the poll list every loop: `inFd` with `POLLOUT`, `outFd` with `POLLIN`,
- `handleCgiEvent()` does at most **one** `write()` or `read()` per event.

**Why write and read at the same time?** A pipe holds only ~64 KB. Picture a script that prints a lot before reading its input, or one that echoes each piece of input as it reads it. If we first wrote the whole body and only then started reading:

```
server: blocked writing body, pipe to script is full, waiting for the script to read
script: blocked writing output, pipe to server is full, waiting for the server to read
-> deadlock
```

Because both pipes are in `poll()` at the same time, we drain the output while still feeding input.

The 2 MB echo test shows a body much bigger than the pipe gets written correctly over many `poll()` rounds. It does *not* trigger the deadlock case itself, because `echo.py` reads everything before it prints. A script that interleaves reading and printing would be the test for that.

### 3.8 `SIGPIPE`

Writing to a pipe or socket whose reader is gone sends the process `SIGPIPE`, and the **default action kills the process**. Without protection, a script that exits without reading its body (`test/no_read.py`) would kill the whole server.

`main()` does `signal(SIGPIPE, SIG_IGN)`. Now that `write()`/`send()` just returns `-1`, and we close that fd. No `errno` check is needed (which is forbidden anyway).

### 3.9 Zombies and `waitpid(..., WNOHANG)`

When a child process exits, the kernel keeps a small entry (its exit status) until the parent collects it with `waitpid()`. Until then it's a **zombie** (`<defunct>` in `ps`). Many zombies eventually fill the process table.

A plain `waitpid(pid, ...)` **blocks** until the child exits. That's forbidden for us (the server must stay non-blocking). So:
- `finishCgi()` tries `waitpid(pid, &status, WNOHANG)`. If the child already exited, it's reaped right away.
- If not (returns `0`), the PID goes into `_children`.
- `reapChildren()` runs at the top of every loop iteration and retries `WNOHANG` for everything in `_children`.
- Killed scripts (`stopCgi()`) also go into `_children`.

Because `poll()` wakes up at least every second (§6), nothing stays a zombie longer than ~1s.

### 3.10 Detecting a client that left: `POLLHUP` is not enough

A lesson from testing. While a script runs, the obvious idea is to poll the client socket with `events = 0` and wait for `POLLHUP`. That **doesn't work** on Linux. A client closing its side only makes the socket *readable* (`recv()` returns 0). `POLLHUP` only comes when both directions are shut down.

So in `WAITING_CGI` the client socket is polled for `POLLIN`:
- `recv()` returns 0 → `closeConnection()` → `stopCgi()` kills the script.
- `recv()` returns data (client already sent the next request) → it's buffered but **not parsed**, because `parseRequest()` resets the `HttpRequest` whose body the script is still reading.

### 3.11 Time: `std::chrono::steady_clock`

`time()`/`gettimeofday()` aren't on the allowed function list. `std::chrono` is part of the C++ standard library (like `std::string`), not a system call. We use `steady_clock` rather than `system_clock` because it never jumps (e.g. when the system clock is adjusted), so a timeout can't fire early or late because of a clock change.

---

## 4. Workflow: one CGI request, step by step

Example: `curl -X POST -d "hallo" localhost:8080/cgi-bin/echo.py`

**1. Reading** (`handleReadable`)
- `recv()` → `addtoBuffer()` → `parseRequest()` → `PARSE_DONE`.
- The switch calls `processRequest(fd, conn)`.

**2. Deciding** (`processRequest`)
- `findCgiInterpreter("/cgi-bin/echo.py", config)` looks at the extension after the last `/`: `.py` → `/usr/bin/python3`.
- Not empty → `startCgi(fd, "/usr/bin/python3")`.

**3. Starting** (`startCgi`)
1. Checks: path contains `..` → **403**. `stat()` fails or not a regular file → **404**. `access(R_OK)` fails → **403**.
2. Splits `./www/cgi-bin/echo.py` into dir `./www/cgi-bin` and file `echo.py`.
3. Builds `envp` and `argv` **before** `fork()`, so the child does as little as possible.
4. `pipe(toChild)`, `pipe(fromChild)`. On failure: close what's open → **500**.
5. `fork()`. On failure: close all four → **500**.
6. **Child:** `dup2` pipes onto 0/1 → `closeServerFds()` → close the four originals → `chdir(dir)` → `execve(...)`. If `chdir` or `execve` fails: `exit(1)` → empty output → the server answers **502**.
7. **Parent:** close the child's ends (`toChild[0]`, `fromChild[1]`), set ours to `O_NONBLOCK`, fill `conn.getCgi()` (`pid`, `start` time, `inFd`, `outFd`), register both fds in `_cgiFds`.
   - Empty body → close `toChild[1]` right away (the script sees EOF on stdin immediately) and there's no `inFd`.
8. State → `WAITING_CGI`.

**4. Running** (`run()` + `handleCgiEvent`)
- Next loop iteration: `inFd` is polled for `POLLOUT`, `outFd` for `POLLIN`, the client socket for `POLLIN` (§3.10).
- `inFd` writable → `write()` the next part of the body. All written (or error) → `closeCgiFd(inFd)` → the script sees EOF.
- `inFd` reports `POLLHUP`/`POLLERR` → the script closed its stdin → stop writing, close `inFd`.
- `outFd` readable → `read()` up to 4 KB, append to `cgi.output`.
- `read()` returns 0 → the script closed stdout → `finishCgi()`.

**5. Finishing** (`finishCgi`)
- Close any pipe still open, `waitpid(WNOHANG)` (or put the PID in `_children`).
- `parseCgiOutput(cgi.output, res)`: OK → `buildResponse(res, false)`. Invalid → **502**.
- State → `WRITING`.

**6. Writing** (`handleWritable`): `send()` until everything is sent → `closeConnection()`.

**What can interrupt this:**

| Event | Handled in | Result |
|---|---|---|
| Client disconnects while the script runs | `handleReadable` (recv 0) → `closeConnection` → `stopCgi` | `kill(SIGKILL)`, pipes closed, PID to `_children` |
| Script runs > 10 s | `checkTimeouts` → `stopCgi` | **504 Gateway Timeout** |
| Script ignores its body | `POLLHUP`/`write -1` on `inFd` | `inFd` closed, output still used |

---

## 5. Building responses

### 5.1 The contract: `HttpResponse`

```cpp
struct HttpResponse {
    int status = 200;
    std::map<std::string, std::string> headers;   // "Content-Type" -> "text/html"
    std::string body;
};
```

Anything that produces a response (Mees's handlers, CGI, error handling) fills one of these. Only `buildResponse()` turns it into bytes. That keeps the HTTP formatting rules in one place.

### 5.2 `buildResponse(res, keepAlive)`

1. Status line: `HTTP/1.1 <code> <reasonPhrase(code)>`
2. All headers from `res.headers`, **except** `Content-Length` and `Connection` (case-insensitive). The server always controls those two.
3. `Content-Type: text/html` if there is a body and no `Content-Type` was set.
4. `Content-Length: <body.size()>`
5. `Connection: keep-alive` or `close` (currently always `close`).
6. Empty line, body.

### 5.3 `makeErrorResponse(status, config)`

Builds a small HTML page like `<h1>404 Not Found</h1>` (the subject requires default error pages). `config` is already a parameter so that `error_page` from the config file can be plugged in later without changing any callers.

### 5.4 `parseCgiOutput(raw, res)` rules

| Script output | Result |
|---|---|
| No empty line between headers and body (`\r\n\r\n` or `\n\n`) | `false` → **502** |
| A header line without `:` | `false` → **502** |
| `Status: 404 Not Found` | `res.status = 404` (code must be 100–599, else 502) |
| `Location: /x` without `Status` | `res.status = 302` (CGI spec) |
| No `Content-Length` | Body = everything until EOF |
| `Content-Length` = output length | Body as is |
| `Content-Length` < output length | Body cut to that length |
| `Content-Length` > output length | `false` → **502** (script probably died halfway) |
| `Content-Length` not a number | `false` → **502** |

---

## 6. Timeouts

Subject: *"A request to your server should never hang indefinitely."*

Constants at the top of `src/Eventloop.cpp`:

| Constant | Value | Meaning |
|---|---|---|
| `POLL_TIMEOUT_MS` | 1000 | `poll()` returns after at most 1 s even with no events, so timeouts get checked and zombies reaped |
| `CLIENT_TIMEOUT_SEC` | 30 | Max time without progress on a client socket |
| `CGI_TIMEOUT_SEC` | 10 | Max total run time of a script |

**What counts as progress:** `Connection::touch()` runs on every `recv()` with data, every `send()`, and every `setState()`. So each new state gets a fresh 30 s.

`checkTimeouts()` (top of every loop iteration):

| State | Condition | Action |
|---|---|---|
| `WAITING_CGI` | script running > 10 s | `stopCgi()` (kill), **504**, → `WRITING` |
| `READING`, buffer not empty | no data for 30 s | **408 Request Timeout**, → `WRITING` |
| `READING`, buffer empty | nothing received for 30 s | close (nothing to answer) |
| `WRITING` | client not reading for 30 s | close |

Connections to close are collected in a vector first and closed after the loop, because `closeConnection()` erases from `_connections`, and erasing while iterating would break the iterator.

If you change a value, also change `CGI_TIMEOUT` / `CLIENT_TIMEOUT` at the top of `tests/test_cgi.py`.

---

## 7. Status codes: who sends what

| Code | When | Where |
|---|---|---|
| 200 | Normal response, CGI without `Status:` | `processRequest`, `parseCgiOutput` |
| 302 | CGI sent `Location` without `Status` | `parseCgiOutput` |
| 400 | Malformed request | parser sets `req.status`, `handleReadable` answers |
| 403 | Script path contains `..`, or script not readable | `startCgi` |
| 404 | Script doesn't exist / isn't a regular file | `startCgi` |
| 408 | Half a request, then 30 s of silence | `checkTimeouts` |
| 500 | `pipe()` or `fork()` failed | `startCgi` |
| 501 | Unsupported `Transfer-Encoding` | parser sets `req.status` |
| 502 | Invalid CGI output, or `execve`/`chdir` failed in the child | `finishCgi` |
| 504 | Script ran longer than 10 s | `checkTimeouts` |
| any | The script's own `Status:` header | `parseCgiOutput` |

Still to do (others / later): 405 (routing), 413 (`client_max_body_size`), `error_page` from config.

---

## 8. Subject rules and how we follow them

| Subject rule | How |
|---|---|
| One `poll()` for all I/O, listen included | CGI pipes are added to the same `pollFds` vector in `run()` |
| Never read/write without `poll()` readiness (grade 0!) | `handleCgiEvent` only does one `read`/`write` after `poll()` reported that fd |
| Never check `errno` after read/write | Every `-1` is handled by closing / finishing, never by inspecting `errno` |
| `fork` only for CGI | `startCgi` is the only caller |
| `fcntl` only with `F_SETFL`, `O_NONBLOCK`, `FD_CLOEXEC` | Only `fcntl(fd, F_SETFL, O_NONBLOCK)`. No `F_SETFD`; the child closes fds itself (§3.6). |
| Un-chunk chunked bodies; CGI expects EOF as end of body | Parser un-chunks; we close `inFd` after the last byte |
| No `content_length` from CGI → EOF marks the end | We read until `read()` returns 0 |
| Run CGI in the correct directory | `chdir(dir)` in the child |
| Full request available to the CGI | Env vars + every header as `HTTP_*` + body on stdin |
| A request must never hang indefinitely | Timeouts (§6) |
| Default error pages | `makeErrorResponse` |
| Accurate status codes | §7 |
| Resilience: server must stay up | `SIGPIPE` ignored, scripts killed on disconnect/timeout, zombies reaped |

---

## 9. Testing

```bash
make re && python3 tests/test_cgi.py            # everything, ~40 s
python3 tests/test_cgi.py --quick               # skip the timeout tests
python3 tests/test_cgi.py --no-start            # use an already running server
```

The script starts `./Webserv` itself, sends **raw bytes** over sockets (so it can send half requests, stay silent, or hang up mid-script), prints PASS/FAIL, and exits 1 on any failure.

| Group | What it proves |
|---|---|
| Basic CGI | GET + query string, headers → `HTTP_*`, POST body, chunked body, 2 MB body arrives identical (written over many `poll()` rounds, §3.7) |
| Output handling | 404, 502 (no headers), `Status:` → 418, all `Content-Length` cases (§5.4) |
| Parser errors | 400, 501, error page with a correct length |
| Robustness | Script ignoring a 2 MB body (SIGPIPE, §3.8), no leaked fds (§3.6), normal request served during a slow CGI, 30 parallel CGIs, disconnect kills the script (§3.10) |
| Timeouts | 504 after 10 s, 408 after 30 s (twice), silent connection closed, no zombies left |

Test scripts in `www/cgi-bin/test/` (each has a comment explaining what it demonstrates; handy for the evaluation demo):

| Script | Demonstrates |
|---|---|
| `slow.py` | Takes 2 s; other clients are still served |
| `hang.py` | Never finishes → 504 |
| `no_headers.py` | Invalid output → 502 |
| `teapot.py` | Own status via `Status: 418` |
| `no_read.py` | Ignores the body → no crash |
| `fds.py` | Shows which fds a script inherits |
| `content_length.py?mode=exact\|short\|long\|bad` | Script `Content-Length` handling |

Manual checks:

```bash
curl -i "localhost:8080/cgi-bin/hello.py?name=dan"
curl -i -X POST -d "hallo" localhost:8080/cgi-bin/echo.py
curl -i -X POST -H "Transfer-Encoding: chunked" -d "chunky" localhost:8080/cgi-bin/echo.py
ps --ppid $(pgrep -x Webserv) -o pid,stat,cmd   # running scripts / zombies (Z)
```

**"Can't bind to IP/port" after a restart:** the old connections are in `TIME_WAIT` for ~60 s and `Listener` doesn't set `SO_REUSEADDR` yet. Wait a minute or add the `setsockopt`.

**Always `make re` after changing a header.** The Makefile doesn't track headers, and mixing objects built with old and new class layouts crashes in strange ways.

---

## 10. Evaluation prep: questions and quick modifications

### Questions you can expect

**"How does your server know a request is CGI?"**
By file extension. `findCgiInterpreter()` takes the part after the last `.` (only if it comes after the last `/`) and looks it up in `config.cgi`.

**"Walk me through what happens between fork and execve."**
§4 step 3.6: `dup2` the pipes onto stdin/stdout, close all server fds, close the original pipe ends, `chdir` to the script's folder, `execve` the interpreter with the file name and the CGI env.

**"Why don't you use FD_CLOEXEC?"**
Setting it needs `fcntl(F_SETFD)`, which the subject doesn't allow. The child closes the fds itself, with the same effect (§3.6).

**"Is reading from the pipe blocking?"**
No. Our ends are `O_NONBLOCK`, and we only read/write after `poll()` reports them ready, one call per event.

**"What if the script prints a lot before reading its body?"**
No deadlock: both pipes are in `poll()` at the same time, so we keep reading output while writing input (§3.7).

**"What if the script runs forever?"**
`checkTimeouts()` kills it after 10 s and answers 504.

**"What if the client disconnects while the script runs?"**
The client socket is polled for `POLLIN`. `recv()` returns 0 → `closeConnection()` → `stopCgi()` sends `SIGKILL` (§3.10).

**"How do you avoid zombies without blocking?"**
`waitpid(WNOHANG)`, retried every loop iteration in `reapChildren()` (§3.9).

**"What happens if a script exits without reading its body?"**
Writing to its stdin fails. `SIGPIPE` is ignored, so `write()` returns -1 and we close that pipe. The output is still used (§3.8).

**"How does the CGI know the body length for a chunked request?"**
The parser un-chunks it, we set `CONTENT_LENGTH` to the decoded length, and we close stdin after the last byte, so EOF marks the end.

**"Why steady_clock?"**
It never jumps, unlike the wall clock, and it's standard C++, not a forbidden system call (§3.11).

### Quick modifications (where to change what)

| Request | Change |
|---|---|
| "Change the CGI timeout to 5 s" | `CGI_TIMEOUT_SEC` in `src/Eventloop.cpp` (+ `CGI_TIMEOUT` in the test) |
| "Support `.php`" | `conf.cgi[".php"] = "/usr/bin/php-cgi";` in `main.cpp` (later: config file). `REDIRECT_STATUS` is already set for php-cgi. |
| "Add an env var, e.g. `REMOTE_ADDR`" | One `env.push_back(...)` in `buildEnv()` in `src/Cgi.cpp` |
| "Add a status code" | One `case` in `reasonPhrase()` in `src/Response.cpp` |
| "Add a header to every response, e.g. `Server: webserv`" | One `out << ...` line in `buildResponse()` |
| "Return 500 instead of 502 for broken CGI output" | `respondError(conn, 502)` in `finishCgi()` |
| "Make the error page show something else" | `makeErrorResponse()` |
| "Show how long the script ran" | `cgi.start` is already stored; compute `steady_clock::now() - cgi.start` in `finishCgi()` |

---

## 11. Known limitations / TODO

| Item | Owner / note |
|---|---|
| CGI mapping is hardcoded in `main.cpp` | Mees: the config parser fills `ServerConfig::cgi`, probably per location |
| Path resolution is `root + path` with a simple `..` check | Mees: proper routing / normalization |
| `error_page` from config not used yet | Daniel: `makeErrorResponse()` already takes the config |
| No keep-alive (always `Connection: close`) | Daniel: reset buffer/request/response/`bytesSent` after a response, and handle data buffered during `WAITING_CGI` |
| No `client_max_body_size` (413) or max header size | Daniel |
| `poll()` returning -1 only prints a message | Daniel |
| No `SO_REUSEADDR` on the listener | Marc (one `setsockopt` before `bind`) |
| Timeout values are constants, not config | Optional |
| A script's stderr goes to the server's terminal | Fine for debugging; could be redirected to `/dev/null` in the child |
