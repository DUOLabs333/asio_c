#include "util.hpp"
#include <asio/io_context.hpp>
#include <optional>
#include "Library.h"

struct ShmemConn{
	std::optional<tcp::acceptor> acceptor;
	socket_ptr conn;
	std::array<uint8_t, 12> msg_buf;
	int id;
};

bool NET=getEnv("NET", false);

std::string ADDRESS=getEnv("ADDRESS", TCP_DEFAULT_ADDRESS);
int PORT=getEnv("PORT", TCP_DEFAULT_PORT);
std::string SOCKET=getEnv("SOCKET", UNIX_DEFAULT_SOCKET);

char* device_mmap;
asio::io_context context;

#ifdef CLIENT
	std::string DEVICE=getEnv("DEVICE", SHMEM_DEFAULT_GUEST_DEVICE);
	auto dummy=open_disk(DEVICE, &device_mmap, NULL);

	tcp::resolver resolver(context);
#else
	std::string DEVICE=getEnv("DEVICE", SHMEM_DEFAULT_HOST_DEVICE);
	auto dummy=open_disk(DEVICE, &device_mmap, NULL); //Temporary --- once I get fd passing working, this line will no longer be needed
#endif

//CLIENT : index =1. SERVER && TCP && acceptor = 0. SERVER && TCP && else = 1.


ShmemConn* shmem_connect(int id){ 
	auto result=new ShmemConn();

	auto endpoints=resolver.resolve(ADDRESS, PORT);
	
	result->conn=std::make_unique<socket_type>(context, TCP);
	
	asio::error_code ec;
        while (true){
        	asio::connect(*result->conn, endpoints, ec);
        	if (!ec){
                    break;
                }
        }
	result->conn->set_option( asio::ip::tcp::no_delay( true) );
	
	if(!NET){
		writeToConn(*result->conn, result->msg_buf, CONNECT, id, 0);
		readFromConn(*result->conn, result->msg_buf);
	}

	return result;

}

ShmemConn* shmem_acceptor_init(int id){
	auto result=new ShmemConn();
	if(NET){
		result->acceptor=tcp::acceptor(context,tcp::endpoint(asio::ip::make_address(ADDRESS), PORT));
	}else{
		result->conn=std::make_unique<socket_type>(context, UNIX);
		writeToConn(*result->conn, result->msg_buf, INIT, id, 0);
		result->id=id;
	}

	return result;
}

ShmemConn* shmem_acceptor_accept(ShmemConn* acceptor){
	auto result=new ShmemConn();
	if(NET){
		result->conn=std::make_unique<socket_type>(context, UNIX);
		acceptor->acceptor->accept(*result->conn);
		result->conn->set_option( asio::ip::tcp::no_delay( true) );
	}else{
		readFromConn(*acceptor->conn, result->msg_buf);

		result->conn=std::make_unique<socket_type>(context, UNIX);
		asio::connect(*result->conn, local::stream_protocol::endpoint(SOCKET));
		writeToConn(*result->conn, result->msg_buf, ESTABLISH, acceptor->id, 0);

		readFromConn(*acceptor->conn, result->msg_buf);
	}

	return result;

}

void shmem_close(ShmemConn* result){
	if(result->acceptor){
		result->acceptor->close();
	}

	if (result->conn){
		result->conn->close();
	}

	delete result;
}

void shmem_read(ShmemConn* result, char* buf, int len, bool* err){
	*err=0;
	try{
		if(NET){
			asio::read(*result->conn, asio::buffer(buf, len));
		}else{
			auto offset=0;
			while(len > 0){
				writeToConn(*result->conn, result->msg_buf, READ, len, 0);
				auto [ msg_type, arg1, arg2] = readFromConn( *result->conn, result->msg_buf);
				memcpy(buf+offset, device_mmap+arg1, arg2);
				offset+=arg2;
				len-=arg2;

				writeToConn(*result->conn, result->msg_buf, READ, 1, 0);
			}
			 
		}
	}
	catch (asio::system_error& e){
		*err=1;
	}


}

void shmem_write(ShmemConn* result, char* buf, int len, bool* err){
	*err=0;
	try{
		if(NET){
			asio::write(*result->conn, asio::buffer(buf, len));
		}else{
			auto offset=0;
			while(len > 0){
				writeToConn(*result->conn, result->msg_buf, WRITE, len, 0);
				auto [ msg_type, arg1, arg2] = readFromConn( *result->conn, result->msg_buf);
				memcpy(device_mmap+arg1, buf+offset, arg2);
				offset+=arg2;
				len-=arg2;

				writeToConn(*result->conn, result->msg_buf, WRITE, 1, 0);
			}
			 
		}
	}
	catch (asio::system_error& e){
		*err=1;
	}


}
