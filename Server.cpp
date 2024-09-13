#include "utils.hpp"
#include <asio/error_code.hpp>
#include <asio/local/connect_pair.hpp>
#include <asio/local/stream_protocol.hpp>
#include <asio/system_error.hpp>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <memory>
#include <unordered_map>
#include <vector>
#include <array>
#include <future>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>

typedef std::shared_ptr<std::atomic<int>> lock_ptr;
#ifdef __APPLE__
	#include <sys/disk.h>
#endif

std::string ADDRESS=getEnv("CONN_SERVER_ADDRESS", "192.168.64.1");
int PORT=getEnv("CONN_SERVER_PORT", 4000);
bool is_guest; 

int NUM_SEGMENTS=100;

typedef std::shared_ptr<socket_type> socket_ptr;

typedef struct {
	uint32_t offset;
	uint32_t size;
} SegmentInfo;

typedef struct {
	std::string file;
	int fd = -1;
	std::mutex mu;
	std::condition_variable cv;
	bool is_write; //Whether we will be writing to it (otherwise, we will be reading from it)
	std::vector<SegmentInfo> segment_to_info; //You pop off structures, then add them back (using std::move for pop and emplace)
	std::queue<int> available_segments;

	int size = 0;

	char* mmap = NULL;
} DriveInfo;

DriveInfo H2G, G2H; //Fill in path information in main()

typedef struct {
	 socket_ptr conn = NULL;
	std::mutex mu;
	std::condition_variable cv; //Allows the server to wait for a backend
	std::queue<socket_ptr> unconnected_clients;
	std::mutex uc_mutex;
	bool exists = false;
} BackendInfo;

//TODO: Create function that returns the BackendInfo& for a given id (should do the locking automatically for the caller)
std::unordered_map<int, BackendInfo> backend_to_info;
std::shared_mutex b2i_mutex;

asio::io_context context;

auto SocketClose(socket_ptr& socket){
	if (!socket){
		return;
	}
	
	asio::error_code ec;
	//TODO: Maybe also do a graceful shutdown with ->shutdown()?
	socket->close(ec);
}

void writeToBackend(int key, std::array<uint8_t, 12> buf, MessageType msg_type, uint8_t arg1, uint8_t arg2){
	b2i_mutex.lock();
	auto& info=backend_to_info[key]; //This could be creating the dictionary, not merely accessing it
	b2i_mutex.unlock();

	std::unique_lock lk(info.mu); //From this point forward, only one thread can run this at a time

	while(true){
		info.cv.wait(lk, [&]{return info.exists;}); //Wait for backend to exist

		try{
			writeToConn(*info.conn, buf, msg_type, arg1, arg2);
		}
		catch (asio::system_error& e){
			SocketClose(info.conn);
			info.exists = false;
		}

		if (info.exists){ //Backend exists and message was sent
			break;
		}
	}
}

auto& acquireSegment(DriveInfo& info, int& segment){

	std::unique_lock lk(info.mu);
	info.cv.wait(lk, [&]{ return !info.available_segments.empty();});

	segment=info.available_segments.front();
	info.available_segments.pop();
	return info.segment_to_info[segment];
	
}

void releaseSegment(DriveInfo& info, int& segment){
	if (segment == -1){
		return;
	}
	std::unique_lock lk(info.mu);
	info.available_segments.push(segment);
	info.cv.notify_all();

	segment=-1;
}
/*
You can't use ramdisks on macOS because mmap and pread/pwrite will fail --- since they depend on the size being non-zero. However, on macOS all disks has zero size (this is not true on Linux) --- so any offset argument has to be <= 0, which is a problem. You will take a performance hit (and may wear out the drive faster). Will try ivshmem to see if it minimizes memory copies (will need to patch QEMU, and will use mmap with msync --- can just memcpy)

default tcp (even with macOS vmnet.framework specifically designed for VMs) is too slow. vhost-user, but want to minimize out-of-tree patches to QEMU (there is a patchset, but progress on it is moving very slowly), and more importantly, I have no idea how to use it (I asked a question in the mailing list and IRC, and have not recieved to either yet, at the time of writing this). Only supports Linux guests (macOS can not work with drives, and even ivshmem wouldn't work either)
*/
void CreateOppositeThread(socket_ptr& from_sock, socket_ptr& to_sock, lock_ptr& from_lock);

void HandleConn(socket_ptr from, socket_ptr to, lock_ptr lock){ //Sending messages from -> to
//After setup, writing to <unix> should only be of the form <WRITE_LOCAL><data...>
	std::array<uint8_t, 12> message_buf;
	auto read = std::ref(G2H);
	auto write = std::ref(H2G);

	buffer<uint8_t> buf;
	int segment_id = -1;

	if(is_guest){ //By default, read and write are set up for host, not guest
		std::swap(read, write);
	}

	try {
		for (;;){
			auto [ msg_type, arg1, arg2 ] = readFromConn(*from, message_buf);
			//printf("Message type: %i\n", msg_type);
			switch (msg_type){

				case (CONNECT_LOCAL): //Guest wants to connect to host. Therefore, this will only ever be run by the guest.
				{
					to = std::make_shared<socket_type>(context, TCP);
					asio::error_code ec;
					while(true){
						to->connect(ip::tcp::endpoint(ip::make_address(ADDRESS), PORT), ec);
						to->set_option(ip::tcp::no_delay( true));
						if (!ec){
							break;
						}
					}

					writeToConn(*to, message_buf, CONNECT_REMOTE, arg1, arg2); //Essentially forwarding the message to remote.

					readFromConn(*to, message_buf); //Wait for confirmation
					writeToConn(*from, message_buf, CONFIRM, 0, 0); //Send confirmation back
					CreateOppositeThread(from, to, lock);
					break;		
				}
				case (CONNECT_REMOTE): //Host received notification that a guest is trying to connect. Therefore, this will only run on the host
					{
					auto id = arg1;

					b2i_mutex.lock();
					auto& info = backend_to_info[id];
					b2i_mutex.unlock();


					info.uc_mutex.lock();
					info.unconnected_clients.push(std::move(from));
					info.uc_mutex.unlock();

					writeToBackend(id, message_buf, ESTABLISH, 0, 0); //Tell backend to create a new connection

					//We don't have to do anything else, since the backend will pick it up from here
					return;
					break;	
					}
				case (ESTABLISH): //Server tells backend to make a new connection. This serves to simulate connecting directly to a port. <unix> -> <tcp>
					{
						auto id = arg1;

						b2i_mutex.lock();
						auto& info = backend_to_info[id];
						b2i_mutex.unlock();

						info.uc_mutex.lock();
						to = std::move(info.unconnected_clients.front()); //This is safe, as the only reason why an ESTABLISH would be sent is if there's a new connection in the first place
						info.unconnected_clients.pop();
						info.uc_mutex.unlock();
						
						writeToConn(*from, message_buf, CONFIRM, 0, 0);
						writeToConn(*to, message_buf, CONFIRM, 0, 0);

						CreateOppositeThread(from, to, lock);
						break;
					}
				
				case (WRITE_LOCAL): //When one side initiates a write. <unix> -> <tcp>
					{

					writeToConn(*to, message_buf, WRITE_REMOTE, arg1, arg2);
					
					auto len = arg1;
					auto& segment = acquireSegment(write.get(), segment_id);

					while (len>0){
						auto size=std::min(segment.size, len);
						//buf.reserve(size);
						asio::read(*from, asio::buffer(write.get().mmap+segment.offset, size));
						
						#ifdef __linux__
							//sync_file_range(write.get().fd, segment.offset, size, SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER);
						#elif defined(__APPLE__)
							//fsync_range(write.get().fd,  FFILESYNC, segment.offset, size);
							fcntl(write.get().fd, F_FULLFSYNC);
						#else
							fsync(write.get().fd);
						#endif

						writeToConn(*to, message_buf, SEGMENT_READ, segment.offset, size);

						lock->wait(0);
						if(lock->load() == 2){
							throw asio::system_error();
						}
						*lock=0;
						//asio::read(pipe, asio::buffer(message_buf, 1)); //Wait for the signal that the confirm has been set without reading the <to> socket directly (as this could lead to a race condition).
						len-=size;

					}

					releaseSegment(write.get(), segment_id);
					//writeToConn(*from, message_buf, CONFIRM, 0,0);
					break;

					}
				case (WRITE_REMOTE): //<tcp> -> <unix>
					{
					writeToConn(*to, message_buf, WRITE_LOCAL, arg1, arg2);
					break;
					}
				case (SEGMENT_READ): //<tcp> -> <unix>
					{
					auto offset = arg1;
					auto size = arg2;
					//buf.reserve(size);
					#ifdef __linux__
						//posix_fadvise(read.get().fd, 0, read.get().size, POSIX_FADV_DONTNEED);
					#endif

					//pread(read.get().fd, buf.data(), size, offset);
					asio::write(*to, asio::buffer(read.get().mmap+offset, size));
					writeToConn(*from, message_buf, CONFIRM, 0, 0);
					break;
					}
				case (CONFIRM):
					{
					if(lock->load() == 2){
						throw asio::system_error();
					}
					*lock=1;
					lock->notify_all();
					//asio::write(pipe, asio::buffer("1", 1)); //Indicate to the other side that a confirm message has been sent, without having to read directly from from
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
		releaseSegment(write.get(), segment_id);

		SocketClose(from);
		SocketClose(to);
		*lock = 2; //Set *lock = 2 to indicate that the side has finished (this also means before waiting/checking, if *lock == 2, then the side throws an asio::system_error

	}


}

void CreateOppositeThread(socket_ptr& from_sock, socket_ptr& to_sock, lock_ptr& from_lock){ //This creates a thread that works <to> <-> <from>, and also has a to_pipe. The pipes are set up such that <from_pipe> <-> <to_pipe>
	std::thread(HandleConn, to_sock, from_sock, from_lock).detach();
}

auto HandleConn1(socket_ptr socket){
	return HandleConn(socket, std::shared_ptr<socket_type>(NULL), std::make_shared<std::atomic<bool>>(false));
}
void FrontendServer(){
	ip::tcp::acceptor acceptor(context, ip::tcp::endpoint(asio::ip::make_address(ADDRESS), PORT));
       
        for (;;){
            auto socket=std::make_shared<socket_type>(context, TCP);
            
            acceptor.accept(*socket);

	    socket->set_option(ip::tcp::no_delay( true)); //Should probably be placed in a wrapper function for making new tcp sockets (so I don't forget about the no_delay)


            std::thread(HandleConn1, std::move(socket)).detach();
        }
}

void HandleBackend(socket_ptr socket){
	auto msg_type = peekFromConn(*socket);
	if (msg_type == ESTABLISH){
		std::thread(HandleConn1, std::move(socket)).detach();
	}else{
		std::array<uint8_t, 12> message_buf;
		int id;
		try {
		std::tie(std::ignore, id, std::ignore) = readFromConn(*socket, message_buf);
		}catch (asio::system_error&){
			return; //socket no longer exists, so we have to exit now
		}

			
		b2i_mutex.lock();
		auto& info=backend_to_info[id];
		b2i_mutex.unlock();
		
		std::unique_lock lk(info.mu); //We do a unique_lock because we have to write to the connection (which technically modifies it), and may have to modify the connection directly, along with .exists


		if (info.exists){
			try{
				writeToConn(*info.conn, message_buf, CONFIRM, 0, 0);
				readFromConn(*info.conn, message_buf);
			}catch(asio::system_error&){
				SocketClose(info.conn);
				info.exists = false;
			}
			
			if (info.exists){ //Still alive
				SocketClose(socket);
				return;
			}
		}
		if(!info.exists){
			info.conn=std::move(socket);
			
			writeToConn(*info.conn, message_buf, CONFIRM, 0, 0);
			
			info.exists = true;
			info.cv.notify_all(); //Tell all threads that are waiting that this specific backend is available
		}

	}
}
void BackendServer(){
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
		if(is_guest){
			std::thread(HandleConn1, std::move(socket)).detach();
		}else{
	    		std::thread(HandleBackend, std::move(socket)).detach();
		}
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
	
	H2G.file=getEnv("CONN_SERVER_H2G_FILE", H2G_DEFAULT_FILE);
	G2H.file=getEnv("CONN_SERVER_G2H_FILE", G2H_DEFAULT_FILE);

	H2G.is_write = !is_guest;
	G2H.is_write = is_guest;

	std::array<DriveInfo*, 2> drives = {&H2G, &G2H};
	for(auto& info: drives){
		//auto flags = (info->is_write ? O_WRONLY: O_RDONLY);
		auto flags = O_RDWR;
		while(info->fd == -1){
			info->fd = open(info->file.c_str(), flags);
			int error = errno;
			if (info->fd == -1){
				fprintf(stderr, "Error opening the disk %s due to error: %s\n", info->file.c_str(), strerror(error));
			}
		}
		
		auto size=lseek(info->fd, 0, SEEK_END);
		info->size = size;
		lseek(info->fd, 0, SEEK_SET);

		info->mmap=static_cast<char*>(mmap(NULL, size, PROT_WRITE, MAP_SHARED, info->fd, 0));

		info->segment_to_info.reserve(NUM_SEGMENTS);
		
		uint32_t segment_size=size/NUM_SEGMENTS; //Later, we can use "fair allocation" to use all of the space available
		
		for(int i =0; i< NUM_SEGMENTS; i++){
			info->available_segments.emplace(i);
			info->segment_to_info.push_back({.offset=i*segment_size,.size=segment_size });
		}

		
	}
	
	std::thread(BackendServer).detach();

	if(!is_guest){
		std::thread(FrontendServer).detach();
	}
	
	std::promise<void>().get_future().wait();
}

