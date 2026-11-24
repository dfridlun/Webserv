#include "Http.hpp"
#include <sstream>
#include <algorithm>
#include "Http.hpp"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdlib>


// ---------- kleine helpers ----------

static ParseResult fail(HttpRequest& req, int status)
{
	req.status = status;
	return PARSE_ERROR;
}

static std::string trim(const std::string& s)
{
	size_t start = s.find_first_not_of(" \t\r\n");
	if (start == std::string::npos)
		return "";
	size_t end = s.find_last_not_of(" \t\r\n");
	return s.substr(start, end - start + 1);
}

static std::string toLower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(),
		[](unsigned char c) { return std::tolower(c); });
	return s;
}

static bool isDigits(const std::string& s)
{
	return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
}

static bool isHex(const std::string& s)
{
	return !s.empty() && s.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}

// ---------- request line en headers ----------

static ParseResult parseRequestLine(const std::string& line, HttpRequest& req)
{
	std::istringstream iss(line);
	iss >> req.method >> req.path >> req.version;
	if (req.method.empty() || req.path.empty() || req.version.empty())
		return fail(req, 400);

	size_t q = req.path.find('?');
	if (q != std::string::npos)
	{
		req.query = req.path.substr(q + 1);
		req.path.resize(q);
	}
	return PARSE_DONE;
}

static ParseResult parseHeaders(std::istringstream& stream, HttpRequest& req)
{
	std::string line;
	while (std::getline(stream, line))
	{
		if (trim(line).empty())
			continue;
		size_t colon = line.find(':');
		if (colon == std::string::npos)
			return fail(req, 400);
		std::string key = toLower(trim(line.substr(0, colon)));
		if (key.empty())
			return fail(req, 400);
		req.headers[key] = trim(line.substr(colon + 1));
	}
	return PARSE_DONE;
}

// ---------- chunked ----------

// leest de hex-grootte op de regel raw[pos, lineEnd)
static ParseResult parseChunkSize(const std::string& raw, size_t pos,
	size_t lineEnd, size_t& size, HttpRequest& req)
{
	std::string s = raw.substr(pos, lineEnd - pos);
	size_t semi = s.find(';');			// chunk-extensies negeren
	if (semi != std::string::npos)
		s.resize(semi);
	s = trim(s);
	if (!isHex(s) || s.size() > 8)
		return fail(req, 400);
	size = std::strtoul(s.c_str(), NULL, 16);
	return PARSE_DONE;
}

// na de 0-chunk: ofwel meteen "\r\n", ofwel trailers tot "\r\n\r\n"
// Returns where the request ends, or npos if it is not complete yet.
static size_t lastChunkEnd(const std::string& raw, size_t pos)
{
	if (raw.size() < pos + 2)
		return std::string::npos;
	if (raw.compare(pos, 2, "\r\n") == 0)
		return pos + 2;
	size_t trailersEnd = raw.find("\r\n\r\n", pos);
	if (trailersEnd == std::string::npos)
		return std::string::npos;
	return trailersEnd + 4;
}

static ParseResult decodeChunked(const std::string& raw, size_t pos, HttpRequest& req)
{
	std::string body;
	while (true)
	{
		size_t lineEnd = raw.find("\r\n", pos);
		if (lineEnd == std::string::npos)
			return PARSE_INCOMPLETE;

		size_t size = 0;
		if (parseChunkSize(raw, pos, lineEnd, size, req) == PARSE_ERROR)
			return PARSE_ERROR;
		pos = lineEnd + 2;

		if (size == 0)
		{
			size_t end = lastChunkEnd(raw, pos);
			if (end == std::string::npos)
				return PARSE_INCOMPLETE;
			req.body = body;
			req.length = end;
			return PARSE_DONE;
		}
		if (raw.size() < pos + size + 2)
			return PARSE_INCOMPLETE;
		if (raw.compare(pos + size, 2, "\r\n") != 0)
			return fail(req, 400);

		body.append(raw, pos, size);
		pos += size + 2;
		// TODO: if (body.size() > max_body_size) return fail(req, 413);
	}
}

// ---------- body ----------

static ParseResult parseContentLength(const std::string& value, size_t& len, HttpRequest& req)
{
	if (!isDigits(value) || value.size() > 12)
		return fail(req, 400);
	len = std::strtoul(value.c_str(), NULL, 10);
	// TODO: if (len > max_body_size) return fail(req, 413);
	return PARSE_DONE;
}

static ParseResult parseBody(const std::string& raw, size_t bodyStart, HttpRequest& req)
{
	auto te = req.headers.find("transfer-encoding");
	auto cl = req.headers.find("content-length");
	req.length = bodyStart;		// no body: the request ends after the headers

	if (te != req.headers.end())
	{
		if (cl != req.headers.end())		// beide tegelijk is verdacht
			return fail(req, 400);
		if (toLower(te->second) != "chunked")
			return fail(req, 501);
		return decodeChunked(raw, bodyStart, req);
	}
	if (cl != req.headers.end())
	{
		size_t len = 0;
		if (parseContentLength(cl->second, len, req) == PARSE_ERROR)
			return PARSE_ERROR;
		if (raw.size() - bodyStart < len)
			return PARSE_INCOMPLETE;
		req.body = raw.substr(bodyStart, len);
		req.length = bodyStart + len;
	}
	return PARSE_DONE;
}

// ---------- publiek ----------

ParseResult parseRequest(const std::string& raw, HttpRequest& req)
{
	size_t headerEnd = raw.find("\r\n\r\n");
	if (headerEnd == std::string::npos)
		return PARSE_INCOMPLETE;

	req = HttpRequest();			// schone start, we parsen bij elke recv opnieuw

	std::istringstream stream(raw.substr(0, headerEnd));
	std::string line;
	if (!std::getline(stream, line))
		return fail(req, 400);

	if (parseRequestLine(line, req) == PARSE_ERROR)
		return PARSE_ERROR;
	if (parseHeaders(stream, req) == PARSE_ERROR)
		return PARSE_ERROR;
	return parseBody(raw, headerEnd + 4, req);
}
