#include <cstdint>
#include <chrono>
#include <list>
#include <string>
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/bind.hpp>

#include "dns-tcp2udp.hpp"

using namespace std;
using namespace boost::asio;

static const std::chrono::seconds TIMEOUT(30);
static const boost::system::error_code SUCCESS = boost::system::errc::make_error_code(boost::system::errc::success);

Client::Client(Server *server_, io_service &io_, ip::tcp::socket incoming_, const ip::udp::endpoint &outgoing_)
		: io(io_), incoming(move(incoming_)), outgoing(io, ip::udp::endpoint()), idle(io), request(BUFSZ), responseHeader(LENSZ), responseMessage(MAXLEN) {
	server = server_;
	outgoing.connect(outgoing_);
	activity();
	readIncoming(SUCCESS, 0);
}

void Client::readIncoming(const boost::system::error_code &ec, size_t count) {
	if (!ec) {
		size_t required = LENSZ;
		size_t available;

		request.commit(count);
		available = request.size();

		if (available >= required) {
			size_t len = getRequestMessageSize();

			required += len;
			if (available >= required) {
				request.consume(LENSZ);
				if (request.size() > 0) {
					try {
						activity();
						outgoing.async_send(request.data(), boost::bind(&Client::writeOutgoing, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
					} catch (const boost::system::system_error &se) {
						close();
					}
				} else {
					readIncoming(SUCCESS, 0);
				}
				return;
			}
		}
		incoming.async_receive(request.prepare(required - available), boost::bind(&Client::readIncoming, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
	} else if (ec != boost::system::errc::operation_canceled) {
		close();
	}
}

size_t Client::getRequestMessageSize() {
	const uint8_t *data = buffer_cast<const uint8_t*>(request.data());
	uint16_t len = (data[0] << 8) | data[1];
	return len;
}

void Client::writeOutgoing(const boost::system::error_code &ec, size_t count __attribute__((unused))) {
	if (!ec) {
		request.consume(getRequestMessageSize());
		outgoing.async_receive(responseMessage.prepare(MAXLEN), boost::bind(&Client::readOutgoing, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
	} else if (ec != boost::system::errc::operation_canceled) {
		close();
	}
}

void Client::setResponseMessageSize(size_t len) {
	uint8_t *data = buffer_cast<uint8_t*>(responseHeader.prepare(LENSZ));
	data[0] = (len >> 8) & 0xFF;
	data[1] = len & 0xFF;
	responseHeader.commit(LENSZ);
}

void Client::readOutgoing(const boost::system::error_code &ec, size_t count) {
	if (!ec) {
		size_t len;

		responseMessage.commit(count);
		len = responseMessage.size();
		setResponseMessageSize(len);

		try {
			array<const_buffer, 2> bufs = {{ responseHeader.data(), responseMessage.data() }};
			activity();
			incoming.async_send(bufs, boost::bind(&Client::writeIncoming, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
		} catch (const boost::system::system_error &se) {
			close();
		}
	} else if (ec != boost::system::errc::operation_canceled) {
		close();
	}
}

void Client::writeIncoming(const boost::system::error_code &ec, size_t count __attribute__((unused))) {
	if (!ec) {
		responseHeader.consume(responseHeader.size());
		responseMessage.consume(responseMessage.size());
		readIncoming(SUCCESS, 0);
	} else if (ec != boost::system::errc::operation_canceled) {
		close();
	}
}

void Client::activity() {
	idle.expires_from_now(TIMEOUT);
	idle.async_wait(boost::bind(&Client::timeout, this, boost::asio::placeholders::error));
}

void Client::timeout(const boost::system::error_code &ec)
{
  if (!ec)
	  close();
}

void Client::close() {
	shared_ptr<Client> self = this->shared_from_this();
	boost::system::error_code ec;

	idle.cancel(ec);
	incoming.close(ec);
	outgoing.close(ec);
	server->clientFinished(this);
}
