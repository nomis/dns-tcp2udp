#include <iostream>
#include <list>
#include <string>
#include <boost/asio.hpp>
#include <boost/bind.hpp>

#include "dns-tcp2udp.hpp"

using namespace std;
using namespace boost::asio;

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

void Server::acceptConnection() {
	acceptor.async_accept(socket, boost::bind(&Server::newConnection, this, boost::asio::placeholders::error));
}

void Server::newConnection(const boost::system::error_code &ec) {
	if (!acceptor.is_open())
		return;

	if (!ec) {
		socket.set_option(socket_base::receive_buffer_size(BUFSZ));
		socket.set_option(socket_base::send_buffer_size(BUFSZ));
		try {
			clients.push_back(make_shared<Client>(this, io, move(socket), dest));
		} catch (boost::system::system_error &se) {
			boost::system::error_code ec2;
			socket.close(ec2);
		}
	}

	acceptConnection();
}

void Server::clientFinished(Client *client) {
	io.post(boost::bind(&Server::removeClient, this, client));
}

void Server::removeClient(Client *client) {
	clients.remove(client->shared_from_this());
}
