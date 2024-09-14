#include "utils.hpp"
#include <asio/buffer.hpp>
#include <asio/error_code.hpp>
#include <asio/system_error.hpp>
#include <optional>
#include "asio_c.h"
#include <lz4.h>
#include <mutex>
#include <array>
#include <tuple>

typedef std::unique_ptr<socket_type> socket_ptr;

struct AsioConn {
	std::optional<ip::tcp::acceptor> acceptor;
	socket_ptr socket;
	uint8_t size_buf[8];
	std::array<uint8_t, 12> msg_buf;
	int id;
	bool use_tcp;
	buffer<uint8_t> compressed_buf;
	buffer<uint8_t> uncompressed_buf;
	buffer<char> output_buf; //For applications that need to write the result of an operation to a buffer, then pass it to asio_write
};


typedef struct {
	std::string prefix;
	std::string address = "192.168.64.1";
	int port;

	bool use_tcp = true; //However, hopefully, at some point, we can either fully depreciate using TCP, or gate it behind some more conditions so only a few people actually need it enabled.


	bool compression = false;

	bool resolved = false;

	std::mutex mu;
} BackendInfo;

static int COMPRESSION_CUTOFF= 1000000/4;
//static int COMPRESSION_CUTOFF= std::numeric_limits<int>::max(); //Effectively disable compression

BackendInfo backends[] = { {.prefix="STREAM", .port = 9000, .compression=true} , {.prefix="CLIP", .port= 9001}, {.prefix="AV", .port = 9002}};


asio::io_context context;
ip::tcp::resolver resolver(context);

void connect_to_server(socket_type& socket){
	asio::error_code ec;
	while(true){
		asio::connect(socket, std::vector<asio::local::stream_protocol::endpoint>({asio::local::stream_protocol::endpoint(SERVER_SOCKET)}), ec);
		if (!ec){
			break;
		}
	}
}

auto& get_backend(int id){
	auto& backend =backends[id];

	backend.mu.lock();
	if (!backend.resolved){ //Cache environment variable lookup
		backend.address=getEnv("CONN_ADDRESS", getEnv(std::format("CONN_{}_ADDRESS", backend.prefix),backend.address));

		backend.port=getEnv("CONN_PORT", getEnv(std::format("CONN_{}_PORT", backend.prefix),backend.port));
		
		backend.use_tcp = getEnv("CONN_USE_TCP", getEnv(std::format("CONN_{}_USE_TCP", backend.prefix), backend.use_tcp));
		backend.resolved=true;
	}
	backend.mu.unlock();

	return backend;
}

AsioConn* asio_connect(int id){ //For clients
	auto conn=new AsioConn();
	
	auto& backend = get_backend(id);
	
	if (backend.use_tcp){
		auto resolver_conns=resolver.resolve(backend.address, std::to_string(backend.port));

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
		conn->socket->set_option( asio::ip::tcp::no_delay(true) );
	}else{
		conn->socket=std::make_unique<socket_type>(context, UNIX);
		connect_to_server(*conn->socket);
		writeToConn(*conn->socket, conn->msg_buf, CONNECT, id, 0);
		readFromConn(*conn->socket, conn->msg_buf);

	}
	
	conn->id=id;
	conn->use_tcp=backend.use_tcp;

	return conn;

}

AsioConn* asio_server_init(int id){ //For backends
	auto server=new AsioConn();
	
	auto& backend = get_backend(id);
	
	if(true){
		server->acceptor=ip::tcp::acceptor(context,ip::tcp::endpoint(asio::ip::make_address(backend.address), backend.port));
	}
	server->id=id;
	server->use_tcp=true;

	return server;
}

AsioConn* asio_server_accept(AsioConn* server){ //For backends
	auto conn=new AsioConn();
	
	conn->socket=std::make_unique<socket_type>(context, TCP);
	server->acceptor->accept(*conn->socket);
	conn->socket->set_option( asio::ip::tcp::no_delay(true) );
	

	conn->id=server->id;
	conn->use_tcp=server->use_tcp;

	return conn;

}

void asio_close(AsioConn* conn){
	if (conn==NULL){
		return;
	}
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
	 
	 if (conn==NULL){
	 	*err=true;
		return;
	}
	try{
		if (conn->use_tcp){
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
		}else{
			uint32_t size;
			std::tie(std::ignore, size, std::ignore) = readFromConn(*conn->socket, conn->msg_buf); //Recieve WRITE response from server
			conn->uncompressed_buf.reserve(size);
			asio::read(*conn->socket, asio::buffer(conn->uncompressed_buf.data(), size));

			*buf=reinterpret_cast<char*>(conn->uncompressed_buf.data());
			*len=size;	
		}

	}
	catch(asio::system_error& e){
		*err=true;
	}

}

void asio_write(AsioConn* conn, char* buf, int len, bool* err){
	*err=false;
	if (conn==NULL){
		*err=true;
		return;
	}
	try{
		if(conn->use_tcp){
			uint8_t is_compressed;
			
			auto max_compressed_size=LZ4_compressBound(len);
			conn->compressed_buf.reserve(max_compressed_size);
			char* compressed_buf=reinterpret_cast<char*>(conn->compressed_buf.data());

			const char* input;
			uint32_t size;

			if ((backends[conn->id].compression) && (len>=COMPRESSION_CUTOFF)){
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
		}else{
			writeToConn(*conn->socket, conn->msg_buf, WRITE, len, 0);
			asio::write(*conn->socket, asio::buffer(buf, len));

			//readFromConn(*conn->socket, conn->msg_buf);
		}
	}
	catch(asio::system_error& e){
		*err=1;
	}
}

char* asio_get_buf(AsioConn* conn, uint32_t* cap){
	conn->output_buf.reserve(*cap);
	*cap=conn->output_buf.capacity();

	return conn->output_buf.data();
}
