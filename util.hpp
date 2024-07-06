#include <asio.hpp>
#include <cstdint>
#include <asio/buffer.hpp>


using namespace asio;
using namespace ip;

enum MessageType{
	CONNECT = 0,
	ESTABLISH = 1,
	READ = 2,
	WRITE = 3,
	INIT = 4,
	SEGMENT = 5
};

typedef asio::generic::stream_protocol::socket socket_type;
typedef std::shared_ptr<socket_type> socket_ptr;

uint32_t deserializeInt(uint8_t* buf, int i);
void serializeInt(uint8_t* buf, int i, uint32_t val);
std::tuple <MessageType, uint32_t, uint32_t> readFromConn(socket_ptr socket, std::array<uint8_t, 12> buf);
void writeToConn(socket_ptr socket, std::array<uint8_t, 12> buf, MessageType msg_type, uint8_t arg1, uint8_t arg2);
