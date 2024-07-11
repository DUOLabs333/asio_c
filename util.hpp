#include <asio.hpp>
#include <cstdint>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

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
typedef std::unique_ptr<socket_type> socket_ptr;

#define TCP ip::tcp::v4().protocol()
#define UNIX local::stream_protocol().protocol()

#define TCP_DEFAULT_ADDRESS "192.168.64.1"
#define TCP_DEFAULT_PORT 8222
#define UNIX_DEFAULT_SOCKET "/tmp/shmem.sock"
#define SHMEM_DEFAULT_HOST_DEVICE "/dev/disk4"
#define SHMEM_DEFAULT_GUEST_DEVICE "/dev/vdb"

uint32_t deserializeInt(uint8_t* buf, int i);
void serializeInt(uint8_t* buf, int i, uint32_t val);

std::tuple <MessageType, uint32_t, uint32_t> readFromConn(socket_type& socket, std::array<uint8_t, 12> buf);
void writeToConn(socket_type& socket, std::array<uint8_t, 12> buf, MessageType msg_type, uint32_t arg1, uint32_t arg2);

uint32_t open_disk(int fd, char** buf);
uint32_t open_disk(std::string path, char** buf, int* fd);

std::string getEnv(const char* _key, std::string _default);
int getEnv(const char* _key, int _default);
