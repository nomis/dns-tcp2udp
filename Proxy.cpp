#include <cerrno>
#include <cstring>
#include <iostream>
#include <list>
#include <string>
#include <system_error>
#include <asio.hpp>

#include <sys/types.h>
#include <unistd.h>

#include "dns-tcp2udp.hpp"

using namespace std;
using namespace asio;

static const uid_t UID = 65534;
static const gid_t GID = 65534;

Proxy::Proxy(string name_, const list<string> &sources, const string &dest, bool background_)
		: name(name_), signals(io, SIGINT, SIGTERM) {
	signals.async_wait([&](const error_code &, int){ io.stop(); });

	createServers(sources, resolveDest(dest));
	dropRoot();

	if (background_)
		background();

	acceptConnections();
}

void Proxy::mainLoop() {
	io.run();
}

ip::udp::endpoint Proxy::resolveDest(const string &dest) {
	ip::udp::resolver resolver(io);
	ip::udp::resolver::query query(dest, "domain",
			ip::resolver_query_base::address_configured
			| ip::resolver_query_base::numeric_host);

	try {
		return *resolver.resolve(query);
	} catch (system_error &se) {
		cerr << dest << ": " << se.what() << "\n";
		exit(EXIT_FAILURE);
	}
}

void Proxy::createServers(const list<string> &sources, ip::udp::endpoint dest) {
	for (string source : sources)
		servers.push_back(make_shared<Server>(io, source, dest));
}

void Proxy::dropRoot() const {
	int ret;

	ret = setregid(GID, GID);
	if (ret != 0) {
		cerr << name << ": unable to change GID: " << strerror(errno) << "\n";
		exit(EXIT_FAILURE);
	}

	ret = setreuid(UID, UID);
	if (ret != 0) {
		cerr << name << ": unable to change UID: " << strerror(errno) << "\n";
		exit(EXIT_FAILURE);
	}
}

void Proxy::background() {
	io.notify_fork(io_service::fork_prepare);

	pid_t pid = fork();
	if (pid == 0) {
		io.notify_fork(io_service::fork_child);
	} else {
		io.notify_fork(io_service::fork_parent);

		if (pid > 0) {
			cout << name << ": started successfully with PID " << pid << "\n";
			exit(EXIT_SUCCESS);
		} else {
			cerr << name << ": unable to fork: " << strerror(errno) << "\n";
			exit(EXIT_FAILURE);
		}
	}
}

void Proxy::acceptConnections() {
	for (shared_ptr<Server> server : servers)
		server->start();
}
