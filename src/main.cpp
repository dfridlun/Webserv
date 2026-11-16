#include "Server.hpp"
#include "Listener.hpp"
#include <iostream>

int main(int argc, char **argv)
{
	(void)argv;
	(void)argc;
	ServerConfig conf;
	std::vector<ServerConfig> configvector;
	try
	{
		conf.port = 8080;
		conf.root = "./www";
		configvector.push_back(conf);
	}
	catch (const std::exception& e)
	{
		std::cerr << e.what() << '\n';
		return(1);
	}
	// if(conf.empty())
	// {
	// 	//error handling
	// }
	Server server(configvector);
	server.run();
}



// webserv
// │
// ├── 1. STARTUP (eenmalig, voor de loop begint)
// │   ├── Config parsen → lijst van ServerConfig (poort, root, routes, ...)
// │   ├── Voor elke poort: socket() → bind() → listen() → non-blocking (fcntl)
// │   └── Alle listening sockets bewaren in één lijst
// │
// ├── 2. EVENT LOOP (draait oneindig, hart van de server)
// │   ├── Bouw readfds/writefds op basis van de STATE van elke fd (zie 3)
// │   ├── select()/poll() — wacht tot er iets klaar is
// │   ├── Is het een listening socket? → accept() → nieuwe Connection aanmaken, state = READING
// │   └── Is het een client socket?
// │       ├── klaar om te lezen → ga naar 3a
// │       └── klaar om te schrijven → ga naar 3d
// │
// ├── 3. CONNECTION STATE MACHINE (per client, dit is de kern)
// │   │
// │   ├── 3a. READING_REQUEST
// │   │   ├── recv() een stukje binnen
// │   │   ├── Parser: is de request compleet? (headers klaar? body compleet o.b.v. Content-Length/chunked?)
// │   │   └── nee → blijf in READING (wacht op volgende select-hit)
// │   │       ja  → ga naar 3b, state = PROCESSING
// │   │
// │   ├── 3b. PROCESSING
// │   │   ├── Match request-path tegen de juiste Location uit de config
// │   │   ├── Is methode toegestaan op deze route? nee → Error 405
// │   │   ├── Is dit een CGI-extensie? → ga naar 4 (CGI), state = WAITING_CGI
// │   │   └── Anders → Handlers (GET/POST/DELETE, resolvePath, file lezen/schrijven)
// │   │       → bouw Response, state = WRITING_RESPONSE
// │   │
// │   ├── 3c. WAITING_CGI (alleen als CGI)
// │   │   ├── pipe/fork/execve is al gestart, deze fd's zitten OOK in de select-set
// │   │   ├── als CGI-output klaar is → lees het, bouw Response
// │   │   └── state = WRITING_RESPONSE
// │   │
// │   ├── 3d. WRITING_RESPONSE
// │   │   ├── send() een stukje (kan ook gedeeltelijk zijn — grote files!)
// │   │   ├── alles verstuurd? nee → blijf in WRITING
// │   │   └── ja → Keep-Alive? → state = READING_REQUEST (reset buffers)
// │   │       anders → state = CLOSING
// │   │
// │   └── 3e. CLOSING
// │       └── close(fd), fd + Connection object opruimen uit alle lijsten
// │
// ├── 4. CGI (aparte sub-flow, hangt aan 3b/3c)
// │   ├── pipe() aanmaken (in + out)
// │   ├── fork()
// │   ├── child: dup2 pipes → chdir → execve met env vars (REQUEST_METHOD, CONTENT_LENGTH, etc.)
// │   └── parent: pipe-fd's toevoegen aan select-set, NIET blocking wachten op child
// │
// └── 5. ERROR HANDLING (kan vanaf overal getriggerd worden)
// 	├── Config zonder error-pages? → defaults gebruiken
// 	└── Bij elke fout (400/403/404/405/413/500...) → direct naar state WRITING_RESPONSE

// Marc: Core server + HTTP request parsing

// Listener class (socket/bind/listen/non-blocking setup, RAII, non-copyable)
// Server class (owns listeners + connections, poll-loop construction/destruction)
// The event loop itself (single poll(), dispatch to listening vs client fds, accept() handling)
// HTTP request parsing (recv() into buffer, HttpRequest: method/path/version/headers, body via Content-Length and chunked)
// Contract between parser, handlers and response builder (HttpRequest in, response data out)
// README skeleton, cleanup, test route for the team

// Mees — Config + routing/handlers

// Config file parsing (server blocks, locations, all the settings listed in the subject: root, error_pages, client_max_body_size, allowed methods, redirects, autoindex, index, upload location, cgi extension mapping)
// Route/Location matching (resolving a request path against the right config block, per the /kapouet example)
// Handlers: GET/POST/DELETE logic, static file serving, directory listing, file uploads

// Daniel: CGI + responses + errors + connection lifecycle

// CGI (pipe/fork/execve, environment variables, un-chunking request body for CGI, handling CGI output without Content-Length, CGI pipes in the poll loop)
// HTTP response building (status line, headers like Content-Length/Content-Type/Connection, body assembly from what Handlers/CGI hand back)
// Error handling (default error pages, error_page mapping from config, all required status codes: 400/403/404/405/413/500, etc.)
// Limits (client_max_body_size -> 413, maximum header size)
// Connection lifecycle (keep-alive: reset buffers/state after a response, CLOSING, closeConnection extended with CGI pipe/child cleanup)
// Timeouts (a request must never hang forever: poll timeout + last-activity time per connection)
// POLLHUP/POLLERR refinement and error handling in run() (the TODOs in the event loop)
// Stress test script (Python/Go: many simultaneous clients, server must stay available)