#include "Connection.hpp"

Connection::Connection(int fd, const ServerConfig& config)
: _fd(fd), _state(READING), _config(config), _bytesSent(0),
  _lastActivity(std::chrono::steady_clock::now()), _keepAlive(false)
{}


int Connection::getFd() const
{
	return(_fd);
}

const std::string& Connection::getBuffer() const
{
	return(_buffer);
}
	
State Connection::getState()
{
	return(_state);
}
void Connection::setState(State s)
{
	_state = s;
	touch();	// every new state gets a fresh timeout window
}
void Connection::setResponse(const std::string& r)
{
	_response = r;
}
const std::string& Connection::getResponse()
{
	return _response;
}

const ServerConfig& Connection::getConfig() const
{
	return(_config);
}

size_t Connection::getBytessend()
{
	return(_bytesSent);
}

void Connection::addtoBuffer(const char* data, size_t len)
{
	_buffer.append(data, len);
}

bool Connection::hasCompleteHeaders() const
{
	return(_buffer.find(("\r\n\r\n")) != std::string::npos);
}

void Connection::addBytes(size_t n)
{
	_bytesSent += n;
}

HttpRequest& Connection::getRequest()
{
	return _request;
}

CgiProcess& Connection::getCgi()
{
	return _cgi;
}

void Connection::touch()
{
	_lastActivity = std::chrono::steady_clock::now();
}

double Connection::secondsIdle() const
{
	std::chrono::duration<double> idle = std::chrono::steady_clock::now() - _lastActivity;
	return idle.count();
}

void Connection::setKeepAlive(bool keepAlive)
{
	_keepAlive = keepAlive;
}

bool Connection::getKeepAlive() const
{
	return _keepAlive;
}

// The one way to answer a request: serialize with the right Connection
// header (keep-alive or close) and switch to WRITING.
void Connection::respond(const HttpResponse& res)
{
	_response = buildResponse(res, _keepAlive);
	setState(WRITING);
}

// Keep-alive: the response is fully sent, get ready for the next request
// on the same socket. Only the bytes of the request we just answered are
// removed from the buffer: whatever comes after them is already (part of)
// the next request (pipelining).
void Connection::resetForNextRequest()
{
	_buffer.erase(0, _request.length);
	_request = HttpRequest();
	_response.clear();
	_bytesSent = 0;
	_cgi = CgiProcess();
	_keepAlive = false;
	setState(READING);
}
