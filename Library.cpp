#include "util.hpp"
#include <asio/error_code.hpp>
#include <optional>
#include "Library.h"

struct AsioConn {
	std::optional<tcp::acceptor> acceptor;
	socket_ptr conn;
};


typedef struct {
	std::string prefix;
	std::string address = "192.168.64.1";
	int port;
} BackendInfo;

std::vector<BackendInfo> backends = { {.prefix="STREAM", .port = 9000} };


char* device_mmap;
asio::io_context context;
tcp::resolver resolver(context);


std::tuple<std::string, int> get_backend(int id){
	auto& backend=backends[id];

	auto address_key=std::format("{}_ADDRESS", backend.prefix);
	auto port_key=std::format("{}_PORT", backend.prefix);

	return {getEnv(address_key, backend.address), getEnv(port_key, backend.port)};
}

AsioConn* asio_connect(int id){ //For clients
	auto result=new AsioConn();
	
	auto [address, port] = get_backend(id);

	auto resolver_results=resolver.resolve(address, std::to_string(port));

	std::vector<ip::tcp::resolver::endpoint_type> endpoints;

	for(auto& result: resolver_results){
		endpoints.push_back(result.endpoint());
	}
	
	result->conn=std::make_unique<socket_type>(context, TCP);
	
	asio::error_code ec;
	while (true){
		asio::connect(*result->conn, endpoints, ec);
		if (!ec){
		    break;
		}
	}
	result->conn->set_option( asio::ip::tcp::no_delay( true) );

	return result;

}

AsioConn* asio_acceptor_init(int id){ //For backends
	auto result=new AsioConn();

	auto [address, port] = get_backend(id);
	
	result->acceptor=tcp::acceptor(context,tcp::endpoint(asio::ip::make_address(address), port));

	return result;
}

AsioConn* asio_acceptor_accept(AsioConn* acceptor){ //For backends
	auto result=new AsioConn();

	result->conn=std::make_unique<socket_type>(context, TCP);
	acceptor->acceptor->accept(*result->conn);
	result->conn->set_option( asio::ip::tcp::no_delay( true) );

	return result;

}

void asio_close(AsioConn* result){
	if(result->acceptor){
		result->acceptor->close();
	}

	if (result->conn){
		result->conn->close();
	}

	delete result;
}

void asio_read(AsioConn* result, char* buf, int len, bool* err){
	asio::error_code ec;
	asio::read(*result->conn, asio::buffer(buf, len), ec);

	*err=!!ec;

}

void asio_write(AsioConn* result, char* buf, int len, bool* err){
	*err=0;

	asio::error_code ec;
	asio::write(*result->conn, asio::buffer(buf, len), ec);

	*err=!!ec;
}
