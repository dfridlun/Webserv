#pragma once

#include <string>
#include "ServerConfig.hpp"
#include "Http.hpp"

enum State { READING, PROCESSING, WAITING_CGI, WRITING, CLOSING };

class Connection
{
	private:
		int 				_fd;
		std::string			_buffer;
		State 				_state;
		const ServerConfig& _config;
		size_t				_bytesSent;
		std::string			_response;
		HttpRequest			_request;
		
	public:
		Connection(int fd, const ServerConfig& config);
		~Connection() = default;
		Connection(const Connection &obj) = default;
		Connection& operator=(const Connection&) = delete;
		int getFd() const;
		const std::string& getBuffer() const;
		State getState();
		void setState(State s);
		const std::string& getResponse();
		size_t getBytessend();
		void setResponse(const std::string& r);
		const ServerConfig& getConfig() const;
		void addtoBuffer(const char* data, size_t len);
		bool hasCompleteHeaders() const;
		void addBytes(size_t n);
		HttpRequest& getRequest();
};