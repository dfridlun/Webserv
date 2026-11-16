#include "Server.hpp"
#include "Connection.hpp"

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
			conn.addtoBuffer(buf, bytesrecv);
			ParseResult res = parseRequest(conn.getBuffer(), conn.getRequest());
		if (res == PARSE_DONE)
		{
			const HttpRequest& req = conn.getRequest();
			std::cout << "Method: " << req.method << std::endl;
			std::cout << "Path: " << req.path << std::endl;
			std::cout << "Query: " << req.query << std::endl;
			std::cout << "Body: " << req.body << std::endl;
			conn.setState(WRITING);
			conn.setResponse("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n<h1>Home</h1>");
		}
		else if (res == PARSE_ERROR)
		{
			std::cout << "Parse error, status: " << conn.getRequest().status << std::endl;
			conn.setState(WRITING);
			conn.setResponse("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
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
	else
		conn.addBytes(sent);
	if(conn.getBytessend() == conn.getResponse().size())
		closeConnection(fd);
}

