#include "utils.hpp"
#include <asio/io_context.hpp>
#include <cstdint>
#include <format>

#ifdef __APPLE__
	#include <sys/disk.h>
#endif

std::string SERVER_SOCKET = getEnv("CONN_SERVER_SOCKET", "/tmp/conn_server.sock");

uint32_t deserializeInt(uint8_t* buf, int i){ //Deserialzes from little endian in endian-agnostic way
    return buf[i+0] | (buf[i+1] << 8) | (buf[i+2] << 16) | (buf[i+3] << 24);
}

void serializeInt(uint8_t* buf, int i, uint32_t val) { //Assumes that val is a 32-bit number (almost always true). Serializes in little endian in endian-agnostic way
    buf[i+0] = (val) & 0xFF;
    buf[i+1] = (val >> 8) & 0xFF;
    buf[i+2] = (val >> 16) & 0xFF;
    buf[i+3] = (val >> 24) & 0xFF;
}

void packMessage(uint8_t* buf, uint32_t a, uint32_t b, uint32_t c){
	serializeInt(buf, 0, a);
	serializeInt(buf, 4, b);
	serializeInt(buf, 8, c);
}

std::tuple<uint32_t, uint32_t, uint32_t> unpackMessage(uint8_t* buf){
	return {deserializeInt(buf, 0), deserializeInt(buf, 4), deserializeInt(buf, 8)};
}

std::tuple <MessageType, uint32_t, uint32_t> readFromConn(socket_type& socket, std::array<uint8_t, 12> buf){
	asio::read(socket, asio::buffer(buf));
	
	auto [msg_type, arg1, arg2] = unpackMessage(buf.data());
	return {static_cast<MessageType>(msg_type), arg1, arg2};

}

void writeToConn(socket_type& socket, std::array<uint8_t, 12> buf, MessageType msg_type, uint32_t arg1, uint32_t arg2){
	packMessage(buf.data(), static_cast<uint32_t>(msg_type), arg1, arg2);
	asio::write(socket, asio::buffer(buf));
}


std::string getEnv(std::string _key, std::string _default){
	auto result=std::getenv(_key.c_str());
	if (result == NULL){
		return _default;
	}else{
		return result;
	}
}

int getEnv(std::string _key, int _default){
	auto result=std::getenv(_key.c_str());
	if (result == NULL){
		return _default;
	}else{
		return atoi(result);
	}
}



BackendInfo backends[] = { {.prefix="STREAM", .port = 9000, .compression=true} , {.prefix="CLIP", .port= 9001}, {.prefix="AV", .port = 9002}};

BackendInfo* getBackend(int id, BackendInfo** ret = NULL){
	auto backend =&backends[id];

	backend->mu.lock();
	if (!backend->resolved){ //Cache environment variable lookup
		backend->address=getEnv("CONN_ADDRESS", getEnv(std::format("CONN_{}_ADDRESS", backend->prefix),backend->address));

		backend->port=getEnv("CONN_PORT", getEnv(std::format("CONN_{}_PORT", backend->prefix),backend->port));
		
		backend->use_tcp = getEnv("CONN_USE_TCP", getEnv(std::format("CONN_{}_USE_TCP", backend->prefix), backend->use_tcp));
		backend->resolved=true;
	}
	backend->mu.unlock();
	
	if(ret!=NULL){
		*ret = backend;
	}

	return backend;
}

void connectToBackend(BackendInfo* backend, socket_ptr& socket, asio::io_context& context){
	ip::tcp::resolver resolver(context);

	auto resolver_conns=resolver.resolve(backend->address, std::to_string(backend->port));

	std::vector<ip::tcp::resolver::endpoint_type> endpoints;

	for(auto& conn: resolver_conns){
		endpoints.push_back(conn.endpoint());
	}
	
	socket=std::make_unique<socket_type>(context, TCP);
	
	asio::error_code ec;
	while (true){
		asio::connect(*socket, endpoints, ec);
		if (!ec){
		    break;
		}
	}
	socket->set_option( asio::ip::tcp::no_delay(true) );	
}


void connectToBackend(int id, socket_ptr& socket, asio::io_context& context){
	return connectToBackend(getBackend(id), socket, context);
}
