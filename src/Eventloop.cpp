#include "Server.hpp"

#include <poll.h>
#include <iostream>
#include <unistd.h>
#include "Response.hpp"

// poll() wakes up at least this often, so timeouts are checked and finished
// CGI children are reaped even when no fd has any activity.
static const int	POLL_TIMEOUT_MS = 1000;
// No progress on the socket for this long (slow/silent client) -> 408 or close.
static const double	CLIENT_TIMEOUT_SEC = 30;
// A CGI script that runs longer than this is killed -> 504.
static const double	CGI_TIMEOUT_SEC = 10;


void Server::run()
{
	while(true)
	{
		reapChildren();
		checkTimeouts();
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
			State state = connects.second.getState();
			// WAITING_CGI also gets POLLIN: a client that disconnects only makes
			// its socket readable (recv() == 0), it does NOT raise POLLHUP.
			// Without POLLIN we would never notice and the script keeps running.
			if(state == READING || state == WAITING_CGI)
				entrycon.events = POLLIN;
			else
				entrycon.events = POLLOUT;
			pollFds.push_back(entrycon);
		}
		for(std::pair<const int, int>& cgi : _cgiFds)
		{
			pollfd entrycgi;
			entrycgi.fd = cgi.first;
			entrycgi.revents = 0;
			// the body pipe is written to, the output pipe is read from
			if(_connections.at(cgi.second).getCgi().inFd == cgi.first)
				entrycgi.events = POLLOUT;
			else
				entrycgi.events = POLLIN;
			pollFds.push_back(entrycgi);
		}
		int amountsockets = poll(pollFds.data(), pollFds.size(), POLL_TIMEOUT_MS);
		if(amountsockets == -1)
		{
			std::cerr << "Do error handling" << std::endl;
			//get actual error handling
		}
		for(pollfd& con : pollFds)
		{
			// look it up again every time: a handler earlier in this loop may have closed it
			if(_cgiFds.find(con.fd) != _cgiFds.end())
			{
				if(con.revents)
					handleCgiEvent(con.fd, con.revents);
			}
			else if(_connections.find(con.fd) != _connections.end())
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

// "A request to your server should never hang indefinitely."
void Server::checkTimeouts()
{
	std::vector<int> toClose;	// closing erases from _connections: not while iterating it
	for (std::pair<const int, Connection>& entry : _connections)
	{
		Connection& conn = entry.second;
		State state = conn.getState();

		if (state == WAITING_CGI)
		{
			std::chrono::duration<double> running = std::chrono::steady_clock::now() - conn.getCgi().start;
			if (running.count() > CGI_TIMEOUT_SEC)
			{
				std::cout << "CGI timeout on fd " << entry.first << std::endl;
				stopCgi(conn);	// kill the script, close its pipes
				conn.setResponse(buildResponse(makeErrorResponse(504, conn.getConfig()), false));
				conn.setState(WRITING);
			}
		}
		else if (conn.secondsIdle() > CLIENT_TIMEOUT_SEC)
		{
			// half a request and then silence: tell the client, then close.
			// Nothing received at all: nothing to answer, just close.
			if (state == READING && !conn.getBuffer().empty())
			{
				std::cout << "Request timeout on fd " << entry.first << std::endl;
				conn.setResponse(buildResponse(makeErrorResponse(408, conn.getConfig()), false));
				conn.setState(WRITING);
			}
			else
				toClose.push_back(entry.first);	// idle, or client stopped reading our response
		}
	}
	for (int fd : toClose)
		closeConnection(fd);
}
