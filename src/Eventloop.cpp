#include "Server.hpp"

#include <poll.h>
#include <iostream>
#include <unistd.h>


void Server::run()
{
	while(true)
	{
		std::vector<pollfd> pollFds;
		for(std::unique_ptr<Listener>& listen : _listeners)
		{
			pollfd entry;
			entry.revents = 0;
			entry.fd = listen->getFd();
			entry.events = POLLIN;
			pollFds.push_back(entry);
		}
		for(std::pair<const int, Connection>& connects : _connections)
		{
			pollfd entrycon;
			entrycon.fd = connects.first;
			entrycon.revents = 0;
			if(connects.second.getState() == READING)
				entrycon.events = POLLIN;
			else
				entrycon.events = POLLOUT; // Should be changed by makeing CGI, but it works now becuase only READING And WRITING
			pollFds.push_back(entrycon);
		}
		int amountsockets = poll(pollFds.data(), pollFds.size(), -1);
		if(amountsockets == -1)
		{
			std::cerr << "Do error handling" << std::endl;
			//get actual error handling
		}
		for(pollfd& con : pollFds)
		{
			if(_connections.find(con.fd) != _connections.end())
			{
				if(con.revents & (POLLHUP | POLLERR))
					closeConnection(con.fd);
				else if(con.revents & POLLIN)
					handleReadable(con.fd);
				else if(con.revents & POLLOUT)
					handleWritable(con.fd);
			}
			else
			{
				for(std::unique_ptr<Listener>& listen : _listeners)
				{
					if (listen->getFd() == con.fd && (con.revents & POLLIN))
						handleNewConnection(*listen);
				}
			}
		}
	}
}