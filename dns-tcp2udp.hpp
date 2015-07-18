#include <cstdint>
#include <list>
#include <string>
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>

static const size_t LENSZ = 2;
static const size_t MAXLEN = (1 << 16) - 1;
static const size_t BUFSZ = LENSZ + MAXLEN;
static const size_t READAHEADLEN = 512;

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
	void acceptConnection();
	void clientFinished(Client *client);

private:
	void newConnection(const boost::system::error_code &ec);
	void removeClient(Client *client);

	boost::asio::io_service &io;
	boost::asio::ip::tcp::acceptor acceptor;
	boost::asio::ip::tcp::socket socket;
	boost::asio::ip::udp::endpoint dest;
	std::list<std::shared_ptr<Client>> clients;
};

class Client: public std::enable_shared_from_this<Client> {
public:
	Client(Server *server, boost::asio::io_service &io_, boost::asio::ip::tcp::socket incoming_, const boost::asio::ip::udp::endpoint &outgoing_);

private:
	void readIncoming(const boost::system::error_code &ec, size_t count);
	void writeOutgoing(const boost::system::error_code &ec, size_t count);
	void readOutgoing(const boost::system::error_code &ec, size_t count, boost::asio::mutable_buffers_1 bufHeader);
	void writeIncoming(const boost::system::error_code &ec, size_t count);
	uint16_t getRequestMessageSize();
	static void setResponseMessageSize(boost::asio::mutable_buffers_1 buf, uint16_t len);
	void activity();
	void timeout(const boost::system::error_code& ec);
	void close();

	Server *server;
	boost::asio::io_service &io;
	boost::asio::ip::tcp::socket incoming;
	boost::asio::ip::udp::socket outgoing;
	boost::asio::steady_timer idle;
	boost::asio::streambuf request;
	boost::asio::streambuf response;
};
