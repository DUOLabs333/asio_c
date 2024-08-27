#include "utils.hpp"
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

std::tuple <MessageType, uint32_t, uint32_t> readFromConn(socket_type& socket, std::array<uint8_t, 12> buf){
	asio::read(socket, asio::buffer(buf));

	return {static_cast<MessageType>(deserializeInt(buf.data(), 0)), deserializeInt(buf.data(), 4), deserializeInt(buf.data(), 8)};
}

void writeToConn(socket_type& socket, std::array<uint8_t, 12> buf, MessageType msg_type, uint32_t arg1, uint32_t arg2){
	serializeInt(buf.data(), 0, static_cast<uint32_t>(msg_type));
	serializeInt(buf.data(), 4, arg1);
	serializeInt(buf.data(), 8, arg2);

	asio::write(socket, asio::buffer(buf));
}

MessageType peekFromConn(socket_type& socket){
	std::array<uint8_t,4> buf;
	int bytes_read=0;

	while(bytes_read < buf.max_size()){
		bytes_read+=socket.receive(asio::buffer(buf), asio::socket_base::message_peek);
	}

	return static_cast<MessageType>(deserializeInt(buf.data(),0));
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
