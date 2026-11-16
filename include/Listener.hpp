#pragma once
#include "ServerConfig.hpp"

class Listener
{
	private:
		int _fd;
		const ServerConfig* _config;
	
	public:

		Listener(int poort, const ServerConfig* config);
		~Listener();
		Listener(const Listener& obj) = delete;
		Listener& operator=(const Listener&) = delete;
		int getFd() const;
		const ServerConfig* getConfig() const;
};