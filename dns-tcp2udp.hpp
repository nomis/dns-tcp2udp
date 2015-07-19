#include <cstdint>
#include <list>
#include <string>
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>

class Server;
class Client;

class Proxy {
public:
	Proxy(std::string name_, const std::list<std::string> &sources, const std::string &dest, bool background_ = true);
	void mainLoop();

private:
	void terminateSignal(const boost::system::error_code &ec, int signum);
	boost::asio::ip::udp::endpoint resolveDest(const std::string &dest);
	void createServers(const std::list<std::string> &sources, boost::asio::ip::udp::endpoint dest);
	void dropRoot() const;
	void background();
	void acceptConnections();

	std::string name;
	boost::asio::io_service io;
	boost::asio::signal_set signals;
	std::list<std::shared_ptr<Server>> servers;
};

class Server: public std::enable_shared_from_this<Server> {
public:
	Server(boost::asio::io_service &io_, const std::string &source, boost::asio::ip::udp::endpoint dest_);
	void start();

private:
	void newConnection(const boost::system::error_code &ec);

	boost::asio::io_service &io;
	boost::asio::ip::tcp::acceptor acceptor;
	boost::asio::ip::tcp::socket socket;
	boost::asio::ip::udp::endpoint dest;
};

class Client: public std::enable_shared_from_this<Client> {
public:
	Client(boost::asio::io_service &io_, boost::asio::ip::tcp::socket incoming_, const boost::asio::ip::udp::endpoint &outgoing_);
	void start();

private:
	void readRequest(const boost::system::error_code &ec, size_t count);
	void writeRequest(const boost::system::error_code &ec);
	void readResponse(const boost::system::error_code &ec, size_t count, boost::asio::mutable_buffers_1 bufHeader);
	void writeResponse(const boost::system::error_code &ec);
	uint16_t getRequestMessageSize();
	static void setResponseMessageSize(boost::asio::mutable_buffers_1 buf, uint16_t len);
	void activity();
	void timeout(const boost::system::error_code& ec);
	void stop();

	boost::asio::io_service &io;
	boost::asio::strand strand;
	boost::asio::ip::tcp::socket incoming;
	boost::asio::ip::udp::socket outgoing;
	boost::asio::steady_timer idle;
	boost::asio::streambuf request;
	boost::asio::streambuf response;
};
