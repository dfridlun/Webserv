#include "Server.hpp"
#include <unistd.h>

Server::Server(const std::vector<ServerConfig>& configs)
{
	for(size_t i = 0; i < configs.size(); i++)
	{
		_listeners.push_back(std::make_unique<Listener>(configs[i].port, &configs[i]));
	}
}

Server::~Server() {}

void Server::closeConnection(int fd)
{
	close(fd);
	_connections.erase(fd);
}