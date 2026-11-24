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
		std::map<int, int>		_cgiFds;		// CGI pipe fd -> client fd
		std::vector<pid_t>		_children;	// finished/killed CGI children not reaped yet

		void processRequest(int fd, Connection& conn);
		void startCgi(int clientFd, const std::string& interpreter);
		void handleCgiEvent(int pipeFd, short revents);
		void finishCgi(Connection& conn);
		void stopCgi(Connection& conn);
		void closeCgiFd(int& fd);
		void reapChildren();
		void closeServerFds();
		void checkTimeouts();
	
	public:
		Server(const std::vector<ServerConfig>& configs);
		~Server();

		void run();
		void handleNewConnection(Listener & listener);
		void handleReadable(int fd);
		void handleWritable(int fd);
		void closeConnection(int fd);
};