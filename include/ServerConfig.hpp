#pragma once

#include <string>
#include <map>

struct ServerConfig
{
	int port;
	std::string root;
	// extension -> interpreter, e.g. ".py" -> "/usr/bin/python3"
	// TODO (Mees): filled by the config parser, probably per location
	std::map<std::string, std::string> cgi;
	// error_pages
	//locations

	//use reserve with vector so we can use a pointer
	// have to use reserve to secure .. amount of space in configs, and otherwise we can't point or reference to it.
};