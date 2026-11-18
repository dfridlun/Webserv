#include "Connection.hpp"

Connection::Connection(int fd, const ServerConfig& config)
: _fd(fd), _state(READING), _config(config), _bytesSent(0),
  _lastActivity(std::chrono::steady_clock::now())
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
