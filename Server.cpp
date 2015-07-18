#include <iostream>
#include <string>
#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

#include "dns-tcp2udp.hpp"

using namespace std;
using namespace boost::asio;
using boost::system::error_code;

Server::Server(io_service &io_, const string &source, ip::udp::endpoint dest_)
		: io(io_), acceptor(io), socket(io) {
	ip::tcp::resolver resolver(io);
	ip::tcp::resolver::query query(source, "domain",
			ip::resolver_query_base::address_configured
			| ip::resolver_query_base::numeric_host
			| ip::resolver_query_base::passive);

	dest = dest_;

	try {
		ip::tcp::endpoint endpoint = *resolver.resolve(query);

		acceptor.open(endpoint.protocol());
		acceptor.set_option(socket_base::reuse_address(true));
		if (endpoint.protocol() == ip::tcp::v6())
			acceptor.set_option(ip::v6_only(true));
		acceptor.bind(endpoint);
		acceptor.listen();
	} catch (boost::system::system_error &se) {
		cerr << source << ": " << se.what() << "\n";
		exit(EXIT_FAILURE);
	}
}

void Server::start() {
	auto self(shared_from_this());
	acceptor.async_accept(socket, [this, self](const error_code &ec){ this->newConnection(ec); });
}

void Server::newConnection(const error_code &ec) {
	if (!acceptor.is_open())
		return;

	if (!ec) {
		socket.set_option(socket_base::receive_buffer_size(BUFSZ));
		socket.set_option(socket_base::send_buffer_size(BUFSZ));
		try {
			auto client(make_shared<Client>(io, move(socket), dest));
			client->start();
		} catch (boost::system::system_error &se) {
			error_code ec2;
			socket.close(ec2);
		}
	}

	start();
}
