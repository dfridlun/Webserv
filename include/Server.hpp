#pragma once

#include "Connection.hpp"
#include "Listener.hpp"
#include "ServerConfig.hpp"
#include <map>
#include <vector>
#include <memory>

class Server
{
	private:
		std::vector<std::unique_ptr<Listener>>  _listeners;
		std::map<int, Connection> _connections;
	
	public:
		Server(const std::vector<ServerConfig>& configs);
		~Server();

		void run();
		void handleNewConnection(Listener & listener);
		void handleReadable(int fd);
		void handleWritable(int fd);
		void closeConnection(int fd);
};