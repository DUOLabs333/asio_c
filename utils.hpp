#include <asio.hpp>

#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

namespace ip = asio::ip;
namespace local = asio::local;


enum MessageType{
	INIT = 0, //Handled specially
	CONFIRM, //Acts only as denoting when confirmation occurs
	CONNECT_LOCAL,
	CONNECT_REMOTE,
	ESTABLISH,
	WRITE_LOCAL,
	WRITE_REMOTE,
	SEGMENT_READ
};

typedef asio::generic::stream_protocol::socket socket_type;

#define TCP ip::tcp::v4().protocol()
#define UNIX local::stream_protocol().protocol()

extern std::string SERVER_SOCKET;

uint32_t deserializeInt(uint8_t* buf, int i);
void serializeInt(uint8_t* buf, int i, uint32_t val);

std::tuple <MessageType, uint32_t, uint32_t> readFromConn(socket_type& socket, std::array<uint8_t, 12> buf);
void writeToConn(socket_type& socket, std::array<uint8_t, 12> buf, MessageType msg_type, uint32_t arg1, uint32_t arg2);
MessageType peekFromConn(socket_type& socket);


std::string getEnv(std::string _key, std::string _default);
int getEnv(std::string _key, int _default);

template<typename T> class buffer { //Since we can't guarentee that vector.reserve will make the data at (data()+size(), data()+capacity()] usable (On GCC at least, this seems to be true though).
	private:
		std::unique_ptr<T> buf = NULL;
		uint32_t cap = 0;
	public:
		uint32_t capacity(){
			return cap;
		}

		T* data() {
			return buf.get();
		}

		void reserve(uint32_t new_cap){
			if (cap >= new_cap){
				return;
			}else{
				#if 1 //Bit-fiddling --- see https://stackoverflow.com/questions/466204/rounding-up-to-next-power-of-2
					new_cap--;
					new_cap |= new_cap >> 1;
					new_cap |= new_cap >> 2;
					new_cap |= new_cap >> 4;
					new_cap |= new_cap >> 8;
					new_cap |= new_cap >> 16;
				#else //In C++20
					new_cap=std::bit_ceil(new_cap);	
				#endif
				auto old_ptr = buf.release();
				buf.reset((T*)realloc(old_ptr, new_cap));
				cap=new_cap;
				return;
				
			}
		}
};
