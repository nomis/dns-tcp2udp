#include <cstdint>
#include <chrono>
#include <memory>
#include <string>
#include <system_error>
#include <asio.hpp>
#include <asio/steady_timer.hpp>

#include "dns-tcp2udp.hpp"

using namespace std;
using namespace asio;
using asio::error::operation_aborted;

static const size_t LENSZ = 2;
static const size_t MAXLEN = (1 << 16) - 1;
static const size_t BUFSZ = LENSZ + MAXLEN;
static const size_t READAHEADLEN = 512;
static const auto TIMEOUT = chrono::seconds(30);
static const error_code SUCCESS;

Client::Client(io_service &io_, ip::tcp::socket incoming_, const ip::udp::endpoint &outgoing_)
		: io(io_), strand(io), incoming(move(incoming_)), outgoing(io, ip::udp::endpoint()), idle(io), request(BUFSZ), response(BUFSZ) {
	incoming.set_option(socket_base::receive_buffer_size(BUFSZ));
	incoming.set_option(socket_base::send_buffer_size(BUFSZ));
	outgoing.connect(outgoing_);
}

void Client::start() {
	activity();
	readRequest(SUCCESS, 0);
}

void Client::readRequest(const error_code &ec, size_t count) {
	if (ec) {
		if (ec != operation_aborted)
			stop();
		return;
	}

	auto self(shared_from_this());
	size_t required = LENSZ;
	size_t available;

	request.commit(count);
	available = request.size();

	if (available >= required) {
		size_t len = getRequestMessageSize();

		required += len;

		if (available >= required) {
			request.consume(LENSZ);
			if (len > 0) {
				activity();
				outgoing.async_send(buffer(request.data(), len), strand.wrap([this, self](const error_code &ec2, size_t){ this->writeRequest(ec2); }));
			} else {
				io.post([this, self]{ this->readRequest(SUCCESS, 0); });
			}
			return;
		}
	} else {
		required += READAHEADLEN;
	}

	incoming.async_receive(request.prepare(required - available), strand.wrap([this, self](const error_code &ec2, size_t count2){ this->readRequest(ec2, count2); }));
}

uint16_t Client::getRequestMessageSize() {
	const uint8_t *data = buffer_cast<const uint8_t*>(request.data());
	return (data[0] << 8) | data[1];
}

void Client::writeRequest(const error_code &ec) {
	if (ec) {
		if (ec != operation_aborted)
			stop();
		return;
	}

	auto self(shared_from_this());
	uint8_t *buf = buffer_cast<uint8_t*>(response.prepare(BUFSZ));
	mutable_buffers_1 bufHeader = buffer(buf, LENSZ);
	mutable_buffers_1 bufMessage = buffer(buf + LENSZ, BUFSZ - LENSZ);

	request.consume(getRequestMessageSize());
	outgoing.async_receive(bufMessage, strand.wrap([this, self, bufHeader](const error_code &ec2, size_t count2){ this->readResponse(ec2, count2, bufHeader); }));
}

void Client::setResponseMessageSize(mutable_buffers_1 buf, uint16_t len) {
	uint8_t *data = buffer_cast<uint8_t*>(buf);
	data[0] = (len >> 8) & 0xFF;
	data[1] = len & 0xFF;
}

void Client::readResponse(const error_code &ec, size_t count, mutable_buffers_1 bufHeader) {
	if (ec) {
		if (ec != operation_aborted)
			stop();
		return;
	}

	auto self(shared_from_this());

	setResponseMessageSize(bufHeader, count);
	response.commit(LENSZ + count);
	activity();
	incoming.async_send(response.data(), strand.wrap([this, self](const error_code &ec2, size_t){ this->writeResponse(ec2); }));
}

void Client::writeResponse(const error_code &ec) {
	if (ec) {
		if (ec != operation_aborted)
			stop();
		return;
	}

	response.consume(response.size());
	readRequest(SUCCESS, 0);
}

void Client::activity() {
	auto self(shared_from_this());

	idle.expires_from_now(TIMEOUT);
	idle.async_wait(strand.wrap([this, self](const error_code &ec){ this->timeout(ec); }));
}

void Client::timeout(const error_code &ec)
{
  if (!ec)
	  stop();
}

void Client::stop() {
	error_code ec;

	idle.cancel(ec);
	incoming.close(ec);
	outgoing.close(ec);
}
