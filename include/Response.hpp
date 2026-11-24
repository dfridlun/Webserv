#pragma once

#include <string>
#include <map>
#include "ServerConfig.hpp"

// What handlers (Mees) and CGI (Daniel) hand back.
// buildResponse() turns this into the raw bytes we send().
struct HttpResponse
{
	int status = 200;
	std::map<std::string, std::string> headers;	// e.g. "Content-Type" -> "text/html"
	std::string body;
};

// "OK", "Not Found", ... for a status code.
std::string reasonPhrase(int status);

// Serialize: status line + headers + "\r\n" + body.
// Always sets Content-Length and Connection, and a default Content-Type if missing.
std::string buildResponse(const HttpResponse& res, bool keepAlive);

// A response with a simple HTML error page for this status.
// TODO: use error_page from config once ServerConfig has it.
HttpResponse makeErrorResponse(int status, const ServerConfig& config);

// Parse what a CGI script wrote to stdout (headers, blank line, body)
// into an HttpResponse. Handles the CGI "Status:" header.
// Returns false if the output has no valid header block (-> caller sends 502).
bool parseCgiOutput(const std::string& raw, HttpResponse& res);
