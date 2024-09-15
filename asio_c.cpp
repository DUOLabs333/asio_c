#include "utils.hpp"
#include <asio/buffer.hpp>
#include <asio/error_code.hpp>
#include <asio/system_error.hpp>
#include <functional>
#include <optional>
#include "asio_c.h"
#include <lz4.h>
#include <mutex>
#include <array>
#include <tuple>

struct AsioConn {
	std::optional<ip::tcp::acceptor> acceptor;
	socket_ptr socket;
	uint8_t size_buf[8];
	std::array<uint8_t, 12> msg_buf;

	BackendInfo* backend = NULL;
	buffer<uint8_t> compressed_buf;
	buffer<uint8_t> uncompressed_buf;
	buffer<char> output_buf; //For applications that need to write the result of an operation to a buffer, then pass it to asio_write
};

static int COMPRESSION_CUTOFF= 1000000/4;
//static int COMPRESSION_CUTOFF= std::numeric_limits<int>::max(); //Effectively disable compression

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

AsioConn* asio_connect(int id){ //For clients
	auto conn=new AsioConn();
	
	auto backend = get_backend(id, &(conn->backend));

	if (backend->use_tcp){
		connectToBackend(backend, conn->socket, context);
	}else{
conn->socket=std::make_unique<socket_type>(context, UNIX);
		connect_to_server(*conn->socket);
		writeToConn(*conn->socket, conn->msg_buf, CONNECT, id, 0);
		readFromConn(*conn->socket, conn->msg_buf);

	}
	

	return conn;

}

AsioConn* asio_server_init(int id){ //For backends
	auto server=new AsioConn();
	
	auto backend = get_backend(id, &(server->backend));
	
	server->acceptor=ip::tcp::acceptor(context,ip::tcp::endpoint(asio::ip::make_address(backend->address), backend->port));

	return server;
}

AsioConn* asio_server_accept(AsioConn* server){ //For backends
	auto conn=new AsioConn();
	
	conn->socket=std::make_unique<socket_type>(context, TCP);
	server->acceptor->accept(*conn->socket);
	conn->socket->set_option( asio::ip::tcp::no_delay(true) );
	

	conn->backend=server->backend;

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
		if (conn->backend->use_tcp){
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
		if(conn->backend->use_tcp){
			uint8_t is_compressed;
			
			auto max_compressed_size=LZ4_compressBound(len);
			conn->compressed_buf.reserve(max_compressed_size);
			char* compressed_buf=reinterpret_cast<char*>(conn->compressed_buf.data());

			const char* input;
			uint32_t size;

			if ((conn->backend->compression) && (len>=COMPRESSION_CUTOFF)){
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
