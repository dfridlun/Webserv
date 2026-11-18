#include "Server.hpp"
#include "Connection.hpp"
#include "Response.hpp"

#include <sys/socket.h>
#include <iostream>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <iostream>
#include <fstream>
#include <unistd.h>

void Server::handleNewConnection(Listener& listener)
{
	int clientFd = accept(listener.getFd(), NULL, NULL);
	if(clientFd == -1)
		return;
	if (fcntl(clientFd, F_SETFL, O_NONBLOCK) < 0)
		{
			std::cerr << "Getting blocked" << std::endl;
			//throw error
		}
	Connection connect(clientFd, *listener.getConfig());
	_connections.insert({clientFd, connect});
}

void Server::handleReadable(int fd)
{
		auto it = _connections.find(fd);
		if(it == _connections.end())
			return;
		Connection& conn = it->second;
		char buf[4096];
		ssize_t bytesrecv = recv(fd, buf, 4096, 0);
		if(bytesrecv > 0)
		{
			std::cout << "Received " << bytesrecv << " Bytes on fd" << fd << std::endl;
			conn.touch();
			conn.addtoBuffer(buf, bytesrecv);
			// Still busy with the previous request (e.g. WAITING_CGI): only buffer.
			// Re-parsing now would reset the request whose body the CGI is reading.
			if (conn.getState() != READING)
				return;
			ParseResult res = parseRequest(conn.getBuffer(), conn.getRequest());
			// No default: on purpose, so -Wswitch (-Werror) breaks the build
			// if a new ParseResult value is added and not handled here.
			switch (res)
			{
				case PARSE_INCOMPLETE:
					break;		// wait for the next POLLIN
				case PARSE_DONE:
				{
					processRequest(fd, conn);
					break;
				}
				case PARSE_ERROR:
				{
					std::cout << "Parse error, status: " << conn.getRequest().status << std::endl;
					int status = conn.getRequest().status;
					if (status < 400)
						status = 400;
					conn.setState(WRITING);
					conn.setResponse(buildResponse(makeErrorResponse(status, conn.getConfig()), false));
					break;
				}
			}
		}
		else if(bytesrecv <= 0)
		{
			if(bytesrecv == 0)
				std::cout << "Client disconnected" << std::endl;
			else if(bytesrecv < 0)
				std::cout << "Connection issue" << std::endl;
			closeConnection(fd);
		}
	}

// PROCESSING: decide what to do with a complete request.
// Either we have a response right away (state WRITING), or a CGI script
// was started and the response comes later (state WAITING_CGI).
void Server::processRequest(int fd, Connection& conn)
{
	const HttpRequest& req = conn.getRequest();
	std::cout << "Method: " << req.method << std::endl;
	std::cout << "Path: " << req.path << std::endl;
	std::cout << "Query: " << req.query << std::endl;
	std::cout << "Body: " << req.body << std::endl;

	// TODO (Mees): routing first (location, allowed methods -> 405)
	std::string interpreter = findCgiInterpreter(req.path, conn.getConfig());
	if (!interpreter.empty())
	{
		startCgi(fd, interpreter);
		return;
	}
	// TODO (Mees): static file / upload / delete handlers fill this HttpResponse
	HttpResponse response;
	response.headers["Content-Type"] = "text/html";
	response.body = "<h1>Home</h1>";
	conn.setState(WRITING);
	conn.setResponse(buildResponse(response, false));
}

void Server::handleWritable(int fd)
{
	auto it = _connections.find(fd);
	if(it == _connections.end())
		return;
	Connection& conn = it->second;
	size_t offset = conn.getBytessend();
	std::cout << "Length of response  " << conn.getResponse() << " lengte van offset " << offset << std::endl;
	ssize_t sent = send(fd, conn.getResponse().c_str() + offset, conn.getResponse().size() - offset,  0);
	std::cout << "Length of response  " << conn.getResponse() << " lengte van offset " << offset << std::endl;
	if(sent < 0)
	{
		closeConnection(fd);
		return;
	}
	conn.addBytes(sent);
	conn.touch();
	if(conn.getBytessend() == conn.getResponse().size())
		closeConnection(fd);
}

