#include "Response.hpp"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdlib>

// ---------- helpers ----------

static std::string toLower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(),
		[](unsigned char c) { return std::tolower(c); });
	return s;
}

static std::string trim(const std::string& s)
{
	size_t start = s.find_first_not_of(" \t\r\n");
	if (start == std::string::npos)
		return "";
	size_t end = s.find_last_not_of(" \t\r\n");
	return s.substr(start, end - start + 1);
}

// true if the map already has this header, ignoring case
static bool hasHeader(const std::map<std::string, std::string>& headers, const std::string& name)
{
	std::string lower = toLower(name);
	for (const auto& h : headers)
		if (toLower(h.first) == lower)
			return true;
	return false;
}

// ---------- public ----------

bool wantsKeepAlive(const HttpRequest& req)
{
	std::string connection;
	auto it = req.headers.find("connection");		// header names are lowercase
	if (it != req.headers.end())
		connection = toLower(it->second);
	if (connection.find("close") != std::string::npos)
		return false;
	if (req.version == "HTTP/1.0")
		return connection.find("keep-alive") != std::string::npos;
	return true;
}

std::string reasonPhrase(int status)
{
	switch (status)
	{
		case 200: return "OK";
		case 201: return "Created";
		case 204: return "No Content";
		case 301: return "Moved Permanently";
		case 302: return "Found";
		case 303: return "See Other";
		case 307: return "Temporary Redirect";
		case 308: return "Permanent Redirect";
		case 400: return "Bad Request";
		case 403: return "Forbidden";
		case 404: return "Not Found";
		case 405: return "Method Not Allowed";
		case 408: return "Request Timeout";
		case 411: return "Length Required";
		case 413: return "Payload Too Large";
		case 414: return "URI Too Long";
		case 418: return "I'm a teapot";
		case 431: return "Request Header Fields Too Large";
		case 500: return "Internal Server Error";
		case 501: return "Not Implemented";
		case 502: return "Bad Gateway";
		case 504: return "Gateway Timeout";
		case 505: return "HTTP Version Not Supported";
		default:  return "Unknown";
	}
}

std::string buildResponse(const HttpResponse& res, bool keepAlive)
{
	std::ostringstream out;
	out << "HTTP/1.1 " << res.status << " " << reasonPhrase(res.status) << "\r\n";

	// headers from the handler / CGI, except the ones we always control ourselves
	for (const auto& h : res.headers)
	{
		std::string lower = toLower(h.first);
		if (lower == "content-length" || lower == "connection")
			continue;
		out << h.first << ": " << h.second << "\r\n";
	}
	if (!hasHeader(res.headers, "Content-Type") && !res.body.empty())
		out << "Content-Type: text/html\r\n";
	out << "Content-Length: " << res.body.size() << "\r\n";
	out << "Connection: " << (keepAlive ? "keep-alive" : "close") << "\r\n";
	out << "\r\n";
	out << res.body;
	return out.str();
}

HttpResponse makeErrorResponse(int status, const ServerConfig& config)
{
	(void)config;	// TODO: if config has error_page for status, read that file instead
	HttpResponse res;
	res.status = status;
	res.headers["Content-Type"] = "text/html";

	std::ostringstream code;
	code << status << " " << reasonPhrase(status);
	res.body = "<!DOCTYPE html>\n<html><head><title>" + code.str() + "</title></head>\n"
		"<body><h1>" + code.str() + "</h1><hr><p>webserv</p></body></html>\n";
	return res;
}

bool parseCgiOutput(const std::string& raw, HttpResponse& res)
{
	// CGI scripts may use "\n\n" instead of "\r\n\r\n" between headers and body
	size_t headerEnd = raw.find("\r\n\r\n");
	size_t sepLen = 4;
	size_t lfEnd = raw.find("\n\n");
	if (headerEnd == std::string::npos || (lfEnd != std::string::npos && lfEnd < headerEnd))
	{
		headerEnd = lfEnd;
		sepLen = 2;
	}
	if (headerEnd == std::string::npos)
		return false;

	res = HttpResponse();
	std::istringstream stream(raw.substr(0, headerEnd));
	std::string line;
	while (std::getline(stream, line))
	{
		if (trim(line).empty())
			continue;
		size_t colon = line.find(':');
		if (colon == std::string::npos)
			return false;
		std::string key = trim(line.substr(0, colon));
		std::string value = trim(line.substr(colon + 1));
		if (key.empty())
			return false;

		if (toLower(key) == "status")			// "Status: 404 Not Found"
		{
			int code = std::atoi(value.c_str());
			if (code < 100 || code > 599)
				return false;
			res.status = code;
		}
		else
			res.headers[key] = value;
	}
	// CGI spec: a Location header without Status means a redirect
	if (hasHeader(res.headers, "Location") && res.status == 200)
		res.status = 302;

	res.body = raw.substr(headerEnd + sepLen);

	// Without Content-Length the body runs until EOF (what we already have).
	// With it, the script decides the length:
	//   more output than announced -> cut off the extra bytes
	//   less output than announced -> script died halfway -> false (502)
	// buildResponse() always writes its own Content-Length from body.size().
	for (auto it = res.headers.begin(); it != res.headers.end(); ++it)
	{
		if (toLower(it->first) != "content-length")
			continue;
		const std::string& value = it->second;
		if (value.empty() || value.size() > 12
			|| value.find_first_not_of("0123456789") != std::string::npos)
			return false;
		size_t len = std::strtoul(value.c_str(), NULL, 10);
		if (len > res.body.size())
			return false;
		res.body.resize(len);
		break;
	}
	return true;
}
