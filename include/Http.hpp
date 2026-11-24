#pragma once 

#include <string>
#include <map>

struct HttpRequest
{
	std::string method;
	std::string	path;
	std::string	version;
	std::map<std::string, std::string> headers;
	std::string body;
	std::string query;
	int status = 0;
	size_t length = 0;	// bytes of the raw buffer this request used (keep-alive:
						// anything after it is already the next request)
};

enum ParseResult { PARSE_INCOMPLETE, PARSE_DONE, PARSE_ERROR};


ParseResult parseRequest(const std::string& raw, HttpRequest& req);
