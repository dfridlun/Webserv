#include "Server.hpp"
#include "Cgi.hpp"
#include "Response.hpp"

#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <vector>

// ---------- helpers ----------

std::string findCgiInterpreter(const std::string& path, const ServerConfig& config)
{
	size_t slash = path.rfind('/');
	size_t dot = path.rfind('.');
	if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
		return "";
	auto it = config.cgi.find(path.substr(dot));
	if (it == config.cgi.end())
		return "";
	return it->second;
}

static void respondError(Connection& conn, int status)
{
	conn.respond(makeErrorResponse(status, conn.getConfig()));
}

// "content-type" -> "HTTP_CONTENT_TYPE"
static std::string toCgiHeaderName(const std::string& name)
{
	std::string out = "HTTP_";
	for (char c : name)
		out += (c == '-') ? '_' : std::toupper(static_cast<unsigned char>(c));
	return out;
}

// The environment the script sees (RFC 3875). scriptFile is the file name
// as seen from the script's own directory, because the child chdir()s there.
static std::vector<std::string> buildEnv(const HttpRequest& req,
	const ServerConfig& config, const std::string& scriptFile)
{
	std::vector<std::string> env;
	env.push_back("GATEWAY_INTERFACE=CGI/1.1");
	env.push_back("SERVER_PROTOCOL=" + req.version);
	env.push_back("SERVER_SOFTWARE=webserv");
	env.push_back("SERVER_PORT=" + std::to_string(config.port));
	env.push_back("REQUEST_METHOD=" + req.method);
	env.push_back("QUERY_STRING=" + req.query);
	env.push_back("SCRIPT_NAME=" + req.path);
	env.push_back("SCRIPT_FILENAME=" + scriptFile);
	env.push_back("PATH_INFO=" + req.path);
	env.push_back("REDIRECT_STATUS=200");			// php-cgi refuses to run without it
	env.push_back("CONTENT_LENGTH=" + std::to_string(req.body.size()));	// body is already un-chunked

	for (const auto& h : req.headers)				// header names are lowercase
	{
		if (h.first == "content-type")
			env.push_back("CONTENT_TYPE=" + h.second);
		else if (h.first == "host")
		{
			env.push_back("SERVER_NAME=" + h.second.substr(0, h.second.find(':')));
			env.push_back("HTTP_HOST=" + h.second);
		}
		else if (h.first != "content-length")
			env.push_back(toCgiHeaderName(h.first) + "=" + h.second);
	}
	return env;
}

// execve wants a NULL-terminated char* array. The strings must outlive it.
static std::vector<char*> toCharArray(std::vector<std::string>& strings)
{
	std::vector<char*> out;
	for (std::string& s : strings)
		out.push_back(&s[0]);
	out.push_back(NULL);
	return out;
}

// ---------- start ----------

void Server::startCgi(int clientFd, const std::string& interpreter)
{
	Connection& conn = _connections.at(clientFd);
	const HttpRequest& req = conn.getRequest();
	const ServerConfig& config = conn.getConfig();

	// TODO (Mees): real path resolution/normalization belongs in routing
	if (req.path.find("..") != std::string::npos)
		return respondError(conn, 403);
	std::string scriptPath = config.root + req.path;
	struct stat st;
	if (stat(scriptPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
		return respondError(conn, 404);
	if (access(scriptPath.c_str(), R_OK) != 0)
		return respondError(conn, 403);

	// "./www/cgi-bin/test.py" -> dir "./www/cgi-bin", file "test.py"
	size_t slash = scriptPath.rfind('/');
	std::string dir = scriptPath.substr(0, slash);
	std::string file = scriptPath.substr(slash + 1);

	// Build everything before fork(), so the child only has to dup2/chdir/execve.
	std::vector<std::string> envStrings = buildEnv(req, config, file);
	std::vector<std::string> argvStrings = { interpreter, file };
	std::vector<char*> envp = toCharArray(envStrings);
	std::vector<char*> argv = toCharArray(argvStrings);

	int toChild[2];		// [0] = script's stdin,  [1] = we write the body
	int fromChild[2];	// [0] = we read output,  [1] = script's stdout
	if (pipe(toChild) == -1)
		return respondError(conn, 500);
	if (pipe(fromChild) == -1)
	{
		close(toChild[0]);
		close(toChild[1]);
		return respondError(conn, 500);
	}
	int fds[4] = { toChild[0], toChild[1], fromChild[0], fromChild[1] };
	pid_t pid = fork();
	if (pid == -1)
	{
		for (int fd : fds)
			close(fd);
		return respondError(conn, 500);
	}
	if (pid == 0)
	{
		// child: pipes become stdin/stdout, then close every other fd we own
		dup2(toChild[0], STDIN_FILENO);
		dup2(fromChild[1], STDOUT_FILENO);
		closeServerFds();
		for (int fd : fds)
			close(fd);
		if (chdir(dir.c_str()) == -1)	// subject: run in the script's directory
			std::exit(1);
		execve(interpreter.c_str(), argv.data(), envp.data());
		std::exit(1);	// only reached if execve failed -> empty output -> 502
	}

	// parent: close the child's ends, keep ours non-blocking
	close(toChild[0]);
	close(fromChild[1]);
	fcntl(toChild[1], F_SETFL, O_NONBLOCK);
	fcntl(fromChild[0], F_SETFL, O_NONBLOCK);

	CgiProcess& cgi = conn.getCgi();
	cgi = CgiProcess();
	cgi.pid = pid;
	cgi.start = std::chrono::steady_clock::now();
	cgi.outFd = fromChild[0];
	_cgiFds[cgi.outFd] = clientFd;
	if (req.body.empty())
		close(toChild[1]);			// nothing to send: script sees EOF right away
	else
	{
		cgi.inFd = toChild[1];
		_cgiFds[cgi.inFd] = clientFd;
	}
	conn.setState(WAITING_CGI);
}

// Runs in the CHILD between fork() and execve(). The child got a copy of every
// fd the server has open. If we don't close them, the script holds them for
// as long as it runs:
//  - another script's body pipe -> that script never sees EOF on stdin, hangs
//  - a client socket            -> the client stays connected after we close it
//  - the listening socket       -> the port stays taken while the script runs
// (FD_CLOEXEC would do this automatically, but it needs fcntl(F_SETFD),
//  which the subject does not allow.)
void Server::closeServerFds()
{
	for (std::unique_ptr<Listener>& listener : _listeners)
		close(listener->getFd());
	for (std::pair<const int, Connection>& conn : _connections)
		close(conn.first);
	for (std::pair<const int, int>& cgi : _cgiFds)
		close(cgi.first);
}

// ---------- poll events on the pipes ----------

// Called from run() when poll() reports something on a CGI pipe fd.
// We read output and write the body in parallel: a script that prints a lot
// before reading stdin must not deadlock against us.
void Server::handleCgiEvent(int pipeFd, short revents)
{
	Connection& conn = _connections.at(_cgiFds.at(pipeFd));
	CgiProcess& cgi = conn.getCgi();

	if (pipeFd == cgi.inFd)
	{
		if (revents & (POLLERR | POLLHUP))	// script closed its stdin / exited
			return closeCgiFd(cgi.inFd);
		if (!(revents & POLLOUT))
			return;
		const std::string& body = conn.getRequest().body;
		ssize_t n = write(cgi.inFd, body.c_str() + cgi.bodySent, body.size() - cgi.bodySent);
		if (n > 0)
			cgi.bodySent += n;
		if (n <= 0 || cgi.bodySent == body.size())
			closeCgiFd(cgi.inFd);			// EOF on the script's stdin
	}
	else if (pipeFd == cgi.outFd)
	{
		// POLLHUP without POLLIN = script closed stdout, read() will return 0
		if (!(revents & (POLLIN | POLLHUP | POLLERR)))
			return;
		char buf[4096];
		ssize_t n = read(cgi.outFd, buf, sizeof(buf));
		if (n > 0)
			cgi.output.append(buf, n);
		else
			finishCgi(conn);				// 0 = EOF: the script is done
	}
}

// Script closed its stdout: turn its output into the HTTP response.
void Server::finishCgi(Connection& conn)
{
	CgiProcess& cgi = conn.getCgi();
	closeCgiFd(cgi.inFd);
	closeCgiFd(cgi.outFd);
	// Usually the child has exited by now. If not, reap it later (never block).
	int status;
	if (waitpid(cgi.pid, &status, WNOHANG) == 0)
		_children.push_back(cgi.pid);
	cgi.pid = -1;

	HttpResponse res;
	if (parseCgiOutput(cgi.output, res))
		conn.respond(res);
	else
		respondError(conn, 502);			// no valid header block from the script
	cgi.output.clear();
}

// ---------- cleanup ----------

// Client went away while its script was running: kill the script.
void Server::stopCgi(Connection& conn)
{
	CgiProcess& cgi = conn.getCgi();
	closeCgiFd(cgi.inFd);
	closeCgiFd(cgi.outFd);
	if (cgi.pid != -1)
	{
		kill(cgi.pid, SIGKILL);
		_children.push_back(cgi.pid);
		cgi.pid = -1;
	}
}

void Server::closeCgiFd(int& fd)
{
	if (fd == -1)
		return;
	_cgiFds.erase(fd);
	close(fd);
	fd = -1;
}

// Collect exit statuses of finished children so they don't stay zombies.
void Server::reapChildren()
{
	for (size_t i = 0; i < _children.size(); )
	{
		int status;
		if (waitpid(_children[i], &status, WNOHANG) != 0)	// exited (or error)
			_children.erase(_children.begin() + i);
		else
			i++;
	}
}
