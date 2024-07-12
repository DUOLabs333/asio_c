#include "utils.hpp"
#include <asio/buffer.hpp>
#include <asio/error_code.hpp>
#include <asio/system_error.hpp>
#include <cstdint>
#include <optional>
#include "Library.h"
#include <lz4.h>

struct AsioConn {
	std::optional<tcp::acceptor> acceptor;
	socket_ptr socket;
	uint8_t size_buf[8];
	std::vector<uint8_t> compressed_buf;
	std::vector<uint8_t> uncompressed_buf;
};


typedef struct {
	std::string prefix;
	std::string address = "192.168.64.1";
	int port;
	bool compress = false;
} BackendInfo;

static int COMPRESSION_CUTOFF= 1000000/4;

std::vector<BackendInfo> backends = { {.prefix="STREAM", .port = 9000, .compress=true} };


char* device_mmap;
asio::io_context context;
tcp::resolver resolver(context);


std::tuple<std::string, int> get_backend(int id){
	auto& backend=backends.at(id);

	auto address_key=std::format("{}_ADDRESS", backend.prefix);
	auto port_key=std::format("{}_PORT", backend.prefix);

	return {getEnv(address_key, backend.address), getEnv(port_key, backend.port)};
}

AsioConn* asio_connect(int id){ //For clients
	auto conn=new AsioConn();
	
	auto [address, port] = get_backend(id);

	auto resolver_conns=resolver.resolve(address, std::to_string(port));

	std::vector<ip::tcp::resolver::endpoint_type> endpoints;

	for(auto& conn: resolver_conns){
		endpoints.push_back(conn.endpoint());
	}
	
	conn->socket=std::make_unique<socket_type>(context, TCP);
	
	asio::error_code ec;
	while (true){
		asio::connect(*conn->socket, endpoints, ec);
		if (!ec){
		    break;
		}
	}
	conn->socket->set_option( asio::ip::tcp::no_delay( true) );

	return conn;

}

AsioConn* asio_server_init(int id){ //For backends
	auto server=new AsioConn();
	
	auto [address, port] = get_backend(id);
	
	server->acceptor=tcp::acceptor(context,tcp::endpoint(asio::ip::make_address(address), port));

	return server;
}

AsioConn* asio_server_accept(AsioConn* server){ //For backends
	auto conn=new AsioConn();

	conn->socket=std::make_unique<socket_type>(context, TCP);
	server->acceptor->accept(*conn->socket);
	conn->socket->set_option( asio::ip::tcp::no_delay( true) );

	return conn;

}

void asio_close(AsioConn* conn){
	if(conn->acceptor){
		conn->acceptor->close();
	}

	if (conn->socket){
		conn->socket->close();
	}

	delete conn;
}

void asio_read(AsioConn* conn, char** buf, int* len, bool* err){
	*err=false;

	try{
		uint8_t is_compressed=0;
		asio::read(*conn->socket, std::vector<asio::mutable_buffer>{asio::buffer(&is_compressed,1),asio::buffer(conn->size_buf)});
		auto compressed_size=deserializeInt(conn->size_buf, 0);
    		auto uncompressed_size=deserializeInt(conn->size_buf, 4);
	
		conn->compressed_buf.reserve(compressed_size);
		conn->uncompressed_buf.reserve(uncompressed_size);

		asio::read(*(conn->socket), asio::buffer(conn->compressed_buf.data(), compressed_size));
		
		char* compressed_buf=reinterpret_cast<char*>(conn->compressed_buf.data());
		char* uncompressed_buf=reinterpret_cast<char*>(conn->uncompressed_buf.data());

		if (is_compressed){
			*buf=uncompressed_buf;
			*len=uncompressed_size;
			LZ4_decompress_safe(compressed_buf, uncompressed_buf, compressed_size, uncompressed_size);
			
		}else{
			*buf=compressed_buf;
			*len=compressed_size;
		}

	}
	catch(asio::system_error& e){
		*err=true;
	}

}

void asio_write(AsioConn* conn, char* buf, int len, bool* err){
	*err=0;
	
	try{
		uint8_t is_compressed;
		
		auto max_compressed_size=LZ4_compressBound(len);
		conn->compressed_buf.reserve(max_compressed_size);
		char* compressed_buf=reinterpret_cast<char*>(conn->compressed_buf.data());

		const char* input;
		uint32_t size;

		if (len>=COMPRESSION_CUTOFF){
			is_compressed=1;
			size=LZ4_compress_default(buf, compressed_buf, len, max_compressed_size);
			input=compressed_buf;
		}else{
			is_compressed=0;
			input=buf;
			size=len;
		}
		
		serializeInt(conn->size_buf, 0, size);
		serializeInt(conn->size_buf, 4, len);


		asio::write(*(conn->socket), std::vector<asio::const_buffer>{asio::buffer(&is_compressed, 1), asio::buffer(conn->size_buf), asio::buffer(input, size)});
	}
	catch(asio::system_error& e){
		*err=1;
	}
}
