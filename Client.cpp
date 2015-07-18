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
		: io(io_), incoming(move(incoming_)), outgoing(io, ip::udp::endpoint()), idle(io), request(BUFSZ), response(BUFSZ) {
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
		} else {
			required = READAHEADLEN;
		}
		incoming.async_receive(request.prepare(required - available), boost::bind(&Client::readIncoming, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
	} else if (ec != boost::system::errc::operation_canceled) {
		close();
	}
}

uint16_t Client::getRequestMessageSize() {
	const uint8_t *data = buffer_cast<const uint8_t*>(request.data());
	return (data[0] << 8) | data[1];
}

void Client::writeOutgoing(const boost::system::error_code &ec, size_t count __attribute__((unused))) {
	if (!ec) {
		request.consume(getRequestMessageSize());
		uint8_t *buf = buffer_cast<uint8_t*>(response.prepare(BUFSZ));
		mutable_buffers_1 bufHeader = buffer(buf, LENSZ);
		mutable_buffers_1 bufMessage = buffer(buf + LENSZ, BUFSZ - LENSZ);
		outgoing.async_receive(bufMessage, boost::bind(&Client::readOutgoing, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred, bufHeader));
	} else if (ec != boost::system::errc::operation_canceled) {
		close();
	}
}

void Client::setResponseMessageSize(mutable_buffers_1 buf, uint16_t len) {
	uint8_t *data = buffer_cast<uint8_t*>(buf);
	data[0] = (len >> 8) & 0xFF;
	data[1] = len & 0xFF;
}

void Client::readOutgoing(const boost::system::error_code &ec, size_t count, mutable_buffers_1 bufHeader) {
	if (!ec) {
		setResponseMessageSize(bufHeader, count);
		response.commit(LENSZ + count);

		try {
			activity();
			incoming.async_send(response.data(), boost::bind(&Client::writeIncoming, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
		} catch (const boost::system::system_error &se) {
			close();
		}
	} else if (ec != boost::system::errc::operation_canceled) {
		close();
	}
}

void Client::writeIncoming(const boost::system::error_code &ec, size_t count __attribute__((unused))) {
	if (!ec) {
		response.consume(response.size());
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
