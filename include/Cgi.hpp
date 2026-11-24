#pragma once

#include <string>
#include <chrono>
#include <sys/types.h>
#include "ServerConfig.hpp"

// State of one running CGI script. Lives inside the Connection that started it.
//
//   server --(inFd)--> script stdin      we write the request body here
//   server <-(outFd)-- script stdout     we read the script's output here
//
// Both fds are non-blocking and go into the same poll() as everything else.
// The Server maps each pipe fd back to its client fd in _cgiFds.
struct CgiProcess
{
	pid_t		pid = -1;		// -1 = no child running for this connection
	int			inFd = -1;		// -1 = closed (body fully written, or never needed)
	int			outFd = -1;		// -1 = closed
	size_t		bodySent = 0;	// how much of the request body we already wrote
	std::string	output;			// everything the script printed so far
	std::chrono::steady_clock::time_point start;	// for the CGI timeout
};

// Interpreter for this path (".py" -> "/usr/bin/python3"), or "" if it is not CGI.
std::string findCgiInterpreter(const std::string& path, const ServerConfig& config);
