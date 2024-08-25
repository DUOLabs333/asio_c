#include <asio.hpp>

#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

namespace ip = asio::ip;

using namespace ip;

enum MessageType{
	CONNECT = 0,
	INIT = 4,
	ESTABLISH = 1,
	WRITE = 3,
	SEGMENT = 5
};

typedef asio::generic::stream_protocol::socket socket_type;
typedef std::unique_ptr<socket_type> socket_ptr;

#define TCP ip::tcp::v4().protocol()
#define UNIX asio::local::stream_protocol().protocol()

uint32_t deserializeInt(uint8_t* buf, int i);
void serializeInt(uint8_t* buf, int i, uint32_t val);

std::tuple <MessageType, uint32_t, uint32_t> readFromConn(socket_type& socket, std::array<uint8_t, 12> buf);
void writeToConn(socket_type& socket, std::array<uint8_t, 12> buf, MessageType msg_type, uint32_t arg1, uint32_t arg2);

uint32_t open_disk(std::string path, int* fd, size_t* size);

std::string getEnv(std::string _key, std::string _default);
int getEnv(std::string _key, int _default);
