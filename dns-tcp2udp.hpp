#include <cstdint>
#include <list>
#include <string>
#include <system_error>
#include <asio.hpp>
#include <asio/steady_timer.hpp>

class Server;

class Proxy {
public:
	Proxy(std::string name_, const std::list<std::string> &sources, const std::string &dest, bool background_ = true);
	void mainLoop();

private:
	void terminateSignal(const std::error_code &ec, int signum);
	asio::ip::udp::endpoint resolveDest(const std::string &dest);
	void createServers(const std::list<std::string> &sources, asio::ip::udp::endpoint dest);
	void dropRoot() const;
	void background();
	void acceptConnections();

	std::string name;
	asio::io_service io;
	asio::signal_set signals;
	std::list<std::shared_ptr<Server>> servers;
};

class Server: public std::enable_shared_from_this<Server> {
public:
	Server(asio::io_service &io_, const std::string &source, asio::ip::udp::endpoint dest_);
	void start();

private:
	void newConnection(const std::error_code &ec);

	asio::io_service &io;
	asio::ip::tcp::acceptor acceptor;
	asio::ip::tcp::socket socket;
	asio::ip::udp::endpoint dest;
};

class Client: public std::enable_shared_from_this<Client> {
public:
	Client(asio::io_service &io_, asio::ip::tcp::socket incoming_, const asio::ip::udp::endpoint &outgoing_);
	void start();

private:
	void readRequest(const std::error_code &ec, size_t count);
	void writeRequest(const std::error_code &ec);
	void readResponse(const std::error_code &ec, size_t count, asio::mutable_buffers_1 bufHeader);
	void writeResponse(const std::error_code &ec);
	uint16_t getRequestMessageSize();
	static void setResponseMessageSize(asio::mutable_buffers_1 buf, uint16_t len);
	void activity();
	void timeout(const std::error_code& ec);
	void stop();

	asio::io_service &io;
	asio::io_service::strand strand;
	asio::ip::tcp::socket incoming;
	asio::ip::udp::socket outgoing;
	asio::steady_timer idle;
	asio::streambuf request;
	asio::streambuf response;
};
