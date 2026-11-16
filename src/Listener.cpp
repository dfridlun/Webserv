#include "Listener.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <iostream>
#include <fstream>

Listener::Listener(int poort, const ServerConfig* config)
:_fd(-1), _config(config)
{
	int listening = socket(AF_INET, SOCK_STREAM, 0);
	if(listening == -1)
	{
		std::cerr << "Can't create a socket" << std::endl;
			//throw (Error can't make a socket);
	}
	sockaddr_in hint;
	hint.sin_family = AF_INET;
	hint.sin_port = htons(poort);
	inet_pton(AF_INET, "0.0.0.0", &hint.sin_addr);
	if(bind(listening, (sockaddr*)&hint, sizeof(hint)) == -1)
	{
		std::cerr << "Can't bind to IP/port" << std::endl;
		//throw error
	}

	if (listen(listening, SOMAXCONN) == -1)
	{
		std::cerr << "Can't listen" << std::endl;
		//throw error
	}

	if (fcntl(listening, F_SETFL, O_NONBLOCK) < 0)
		{
			std::cerr << "Getting blocked" << std::endl;
			//throw error
		}
	std::cout << poort << std::endl;
	_fd = listening;
}

Listener::~Listener()
{
	// if(_fd != -1)
	// 	close(_fd);
}

int Listener::getFd() const
{
	return (_fd);
}

const ServerConfig* Listener::getConfig() const
{
	return(_config);
}