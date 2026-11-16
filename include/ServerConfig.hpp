#pragma once

#include <string>

struct ServerConfig
{
	int port;
	std::string root;
	// error_pages
	//locations

	//use reserve with vector so we can use a pointer
	// have to use reserve to secure .. amount of space in configs, and otherwise we can't point or reference to it.
};