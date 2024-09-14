#include "utils.hpp"
#include <algorithm>
#include <asio/error_code.hpp>
#include <asio/local/connect_pair.hpp>
#include <asio/local/stream_protocol.hpp>
#include <asio/system_error.hpp>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <array>
#include <future>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>

#ifdef __APPLE__
	#include <sys/disk.h>
#endif

std::string ADDRESS=getEnv("CONN_SERVER_ADDRESS", "192.168.64.1");
int PORT=getEnv("CONN_SERVER_PORT", 4000);

bool is_guest; 

#define NUM_SEGMENTS 256 //Must be no bigger than 256 (since the information is stored in a single byte
typedef std::shared_ptr<socket_type> socket_ptr;

socket_ptr server_connection = NULL; //TODO: Read constantly, if I disconnect, reexec (use argv for that)

typedef struct {
	uint32_t offset;
	uint32_t size;
} SegmentInfo;

typedef struct {
	std::string file;
	int fd = -1;
	bool is_write; //Whether we will be writing to it (otherwise, we will be reading from it)
	std::vector<SegmentInfo> segment_to_info;
	uint8_t* mmap = NULL;
	size_t size;
} DriveInfo;

DriveInfo H2G, G2H; //Fill in path information in main()

std::reference_wrapper<DriveInfo> Read = G2H, Write = H2G;

auto SocketClose(socket_ptr& socket){
	if (!socket){
		return;
	}
	
	asio::error_code ec;
	//TODO: Maybe also do a graceful shutdown with ->shutdown()?
	socket->close(ec);
}

typedef struct ThreadInfo{
	socket_ptr conn = NULL;
	uint32_t thread;
	std::atomic<bool> connected = false;
	std::mutex mu;
} ThreadInfo;

std::unordered_map<uint32_t, ThreadInfo> thread_to_info; //Map the client thread id to the corresponding thread. We use the client server thread number globally for both server and client (works as long as there is a 1-to-1 relationship between a client and server, and that upon disconnection on either side, we completely restart (ie, reexec

std::shared_mutex t2i_mutex;

std::atomic<uint32_t> thread_counter = 0;

asio::io_context context;

std::mutex ring_mutex;
/*
You can't use ramdisks on macOS because mmap and pread/pwrite will fail --- since they depend on the size being non-zero. However, on macOS all disks has zero size (this is not true on Linux) --- so any offset argument has to be <= 0, which is a problem. You will take a performance hit (and may wear out the drive faster). Will try ivshmem to see if it minimizes memory copies (will need to patch QEMU, and will use mmap with msync --- can just memcpy)

default tcp (even with the macOS-specific vmnet.framework specifically designed for VMs) is too slow. There is vhost-user, but want to minimize out-of-tree patches to QEMU (there is a patchset, but progress on it is moving very slowly), and more importantly, I have no idea how to use it (I asked a question in the mailing list and IRC, and have not received on either platform yet, at the time of writing this). Only supports Linux guests (macOS can not work with drives, and even ivshmem wouldn't work either on macOS)
*/

void flushDrive(std::reference_wrapper<DriveInfo>& info){
}

//0|1|2|3|4|5
//T| |H|
auto WaitForChange(uint8_t* mem, std::function<bool(uint8_t,uint8_t)> f){
	for(;;){
		auto head = mem[0];
		auto tail = mem[1];
		if(f(head,tail)){
			return std::make_tuple(head, tail, f);
		}

		usleep(10);
	}
}


void writeToRing(uint32_t thread, MessageType msg_type, uint32_t arg1, socket_ptr conn = NULL, uint32_t length = 0){
	std::unique_lock lk(ring_mutex);
	
	auto mem = Write.get().mmap;
	auto& segments = Write.get().segment_to_info;

	for(;;){
		auto [ head, tail, f ] = WaitForChange(mem, [](uint8_t a, uint8_t b){ return b!=(a-1);}); //Wait until the tail (which points to the index after the last filled element) is one place before the head.

		while(f(head,tail)){
			auto offset = segments[tail-1].offset; //Has to be tail-1 since tail points to the index after the last actual element
			auto size = segments[tail-1].size;

			auto written_length = std::min(size - 12, length); //How much data to write
			if(length > 0){ //There's data to write --- any call to write data should be separate from the rest of the message

				msg_type = DATA;
				arg1 = written_length;
			}

			packMessage(mem+offset, thread, msg_type, arg1);


			asio::read(*conn, asio::buffer(mem+offset+12, written_length));

			length -= written_length;
			
			mem[1]=(++tail);
			flushDrive(Write);
			
			
			if(size == 0){
				return;
			}
		}
	}
}
void HandleConn(int key, ThreadInfo& info){ //Read from socket and write to ring
//When quitting, remove from dictionary
	std::array<uint8_t, 12> message_buf;
	
	auto read = std::ref(G2H);
	auto write = std::ref(H2G);

	if(is_guest){ //By default, read and write are set up for host, not guest
		std::swap(read, write);
	}

	try {
		for (;;){
			auto [ msg_type, arg1, arg2 ] = readFromConn(*info.conn, message_buf);
			//printf("Message type: %i\n", msg_type);
			switch (msg_type){

				case (CONNECT): //Guest wants to connect to host. Therefore, this will only ever be run by the guest.
				{
					auto backend = arg1;
					writeToRing(info.thread, CONNECT, backend); //TODO: Locks mutex, waits until the tail!=head, serialize (thread, msg, arg1) to memory directly, and if ((conn != NULL) and (size > 0 )), write it out to segment (according to the size --- another thing, the size available to the 256 segments is shrunk by 2 for the head and tails), repeating across multiple segments as needed. After each write to segment update, if APPLE, then flush. After each write, make sure that there's space, then continue
						info.connected.wait(false); //Atomic variable, waiting for an update
						writeToConn(*info.conn, message_buf, CONFIRM, 0, 0); //Tell UNIX socket that we've connected
					break;		
				}
				case(WRITE):
				{
					auto size = arg1;
					writeToRing(info.thread, WRITE, size);
					writeToRing(info.thread, DUMMY, 0, info.conn, size); //Write the data
					break;
				}

				default:
					{
						printf("This is not supposed to happen!\n");
						break;
					}

			}
		}
	}

	catch (asio::system_error&){
		writeToRing(info.thread, DISCONNECT, 0);
		SocketClose(info.conn);

		t2i_mutex.lock();
		thread_to_info.erase(key);
		t2i_mutex.unlock();

		thread_counter--;

	}


}

void readFromRing(){
	auto mem = Read.get().mmap;
	//By the time the server accepts, and the client connects, the respective memory has been set to 0
	auto& segments = Read.get().segment_to_info;

	for(;;){
		auto [head, tail, f ] = WaitForChange(mem, [](uint8_t a, uint8_t b){ return a!=b;}); //Wait until ring buffer is not empty (as denoted by a!=b)
		
		while(f(head, tail)){
			
			auto offset=segments[head].offset;
			auto size = segments[head].size;

			auto [thread, msg_type, arg1] = unpackMessage(mem+offset); //TODO: Should deserialize into three uint8_t
	
			t2i_mutex.lock_shared(); //Makes sure that checking + retrieving object is one atomic operation

			std::optional<std::unique_lock<std::mutex>> lk; //Holds a lock to the thread's mutex if the thread exists 
			auto exists = thread_to_info.contains(thread);
			std::optional<std::reference_wrapper<ThreadInfo>> info_ref;
			if (exists){
				info_ref = std::ref(thread_to_info[thread]);
				lk.emplace(info_ref->get().mu);
			}
			t2i_mutex.unlock_shared();

			if (exists){
				auto& info = info_ref->get();
				switch(msg_type){
					case(CONNECT): //Received request from client
						{
							info.thread = thread;
							auto& backend = get_backend(arg1);
							connectToBackend(backend, info.conn); //TODO: Should move code from asio_c.cpp to utils 

							writeToRing(thread, CONFIRM, 0);

							break;
						}

					case(WRITE):
						{
						writeToConn(*info.conn, WRITE, arg1, 0);
						break;
						}
					case(DATA):
						{
						auto size = arg1;
						asio::write(*info.conn, asio::buffer(mem+offset+12, size));
						break;
						}
					case(DISCONNECT):
						{
							SocketClose(info.conn); //Trigger the thread's shutdown sequence	
						}

					case(CONFIRM): //Received confirmation of connection by server
						{
						info.connected = true;
						}
				}

				mem[0]=(++head);
				flushDrive(
			}
		}
	}
}


void HandleBackend(socket_ptr socket){
	auto key = thread_counter++;

	t2i_mutex.lock();
	auto& info = thread_to_info[key];
	t2i_mutex.unlock();

	std::thread(HandleConn, key, std::ref(info)).detach();
}
void Server(){ //Only for client
	local::stream_protocol::endpoint socket_endpoint(SERVER_SOCKET);
	{
	local::stream_protocol::socket socket(context);
	asio::error_code ec;
	socket.connect(socket_endpoint, ec);
	if (!ec){ //If you can connect to it
		throw std::runtime_error(std::format("There's a server already running on the socket {}!", SERVER_SOCKET));
	}else{
		unlink(SERVER_SOCKET.c_str());
	}
	}
	local::stream_protocol::acceptor acceptor(context, socket_endpoint);
	for (;;){
		auto socket=std::make_shared<socket_type>(context, UNIX);
	    	acceptor.accept(*socket);

	
	    	std::thread(HandleBackend, std::move(socket)).detach();
	}

}

int main(int argc, char** argv){	
	std::string H2G_DEFAULT_FILE = "";
	std::string G2H_DEFAULT_FILE = "";
	bool IS_GUEST_DEFAULT = false;
	#ifdef __APPLE__
		H2G_DEFAULT_FILE="/Volumes/disk4/h2g";
		G2H_DEFAULT_FILE="/Volumes/disk4/g2h";
		IS_GUEST_DEFAULT = false;
	#elif defined(__linux__)
		H2G_DEFAULT_FILE = "/sys/devices/platform/3f000000.pcie/pci0000:00/0000:00:02.0/resource2_wc";
		G2H_DEFAULT_FILE = "/sys/devices/platform/3f000000.pcie/pci0000:00/0000:00:03.0/resource2_wc";
		IS_GUEST_DEFAULT = true;
	#endif
	
	is_guest = getEnv("CONN_SERVER_IS_GUEST", IS_GUEST_DEFAULT);
	
	if(is_guest){ //By default, we assumed we are on the host
		std::swap(Read, Write);
	}
	H2G.file=getEnv("CONN_SERVER_H2G_FILE", H2G_DEFAULT_FILE);
	G2H.file=getEnv("CONN_SERVER_G2H_FILE", G2H_DEFAULT_FILE);

	H2G.is_write = !is_guest;
	G2H.is_write = is_guest;

	std::array<DriveInfo*, 2> drives = {&H2G, &G2H};
	for(auto& info: drives){
		//auto flags = (info->is_write ? O_WRONLY: O_RDONLY);
		auto flags = O_RDWR;

		int fd = -1;
		while(fd == -1){
			fd = open(info->file.c_str(), flags);
			int error = errno;
			if (fd == -1){
				fprintf(stderr, "Error opening the disk %s due to error: %s\n", info->file.c_str(), strerror(error));
			}
		}
		info->fd = fd;
		
		auto size=lseek(info->fd, 0, SEEK_END);
		lseek(info->fd, 0, SEEK_SET);

		info->mmap=static_cast<uint8_t*>(mmap(NULL, size, PROT_WRITE, MAP_SHARED, info->fd, 0));
	
		#ifdef __APPLE__
			fcntl(fd, F_FULLFSYNC); //TODO: Replace with FlushFd
		#endif

		size -= 2;
		info->size = size; //To account for <head> and <tail>
		
		info->segment_to_info.reserve(NUM_SEGMENTS);
		
		uint32_t segment_size=size/NUM_SEGMENTS; //Later, we can use "fair allocation" to use all of the space available
		
		for(int i =0; i< NUM_SEGMENTS; i++){ //Has to start from 2 since the first two bytes are taken
			info->segment_to_info.push_back({.offset=2+(i*segment_size),.size=segment_size });
		}

		
	}
	
	memset(Write.get().mmap,0, 2);
	
	char buf[2] = "1";
	ip::tcp::socket socket(context);
	ip::tcp::endpoint endpoint(ip::address::from_string(ADDRESS), PORT);
	ip::tcp::acceptor acceptor(context, endpoint);
	asio::error_code ec;

	while(true){
		if(is_guest){
			socket.connect(endpoint, ec);
		}else{
			acceptor.accept(socket, ec);
		}
		if(!ec){
			break;
		}

	}

	if (is_guest){
		std::thread(Server).detach();
	}
		
	std::thread(readFromRing).detach();

	asio::read(socket, asio::buffer(buf)); //As long as the client/server is alive, this should never return...
	
	execv(argv[0], argv); //...however, if it does, you should restart the whole program.
}

