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
};

enum ParseResult { PARSE_INCOMPLETE, PARSE_DONE, PARSE_ERROR};


ParseResult parseRequest(const std::string& raw, HttpRequest& req);
