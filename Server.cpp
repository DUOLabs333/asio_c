#include "utils.hpp"
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <queue>
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

#ifdef __APPLE__
	#include <sys/disk.h>
#endif

std::string ADDRESS=getEnv("CONN_SERVER_ADDRESS", "192.168.64.1");
int PORT=getEnv("CONN_SERVER_PORT", 4000);
bool is_guest = getEnv("CONN_SERVER_IS_GUEST", false); 

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
} DriveInfo;

DriveInfo H2G, G2H; //Fill in path information in main()

typedef struct {
	 socket_ptr conn = NULL;
	std::mutex mu;
} BackendInfo;

std::unordered_map<int, BackendInfo> backend_to_info;
std::shared_mutex b2i_mutex;

std::unordered_map<int, std::queue<socket_ptr>> backend_to_unconnected_clients;
std::mutex b2u_mutex;

asio::io_context context;

void writeToBackend(int key, std::array<uint8_t, 12> buf, MessageType msg_type, uint8_t arg1, uint8_t arg2){
	b2i_mutex.lock_shared();
	auto& info=backend_to_info[key];
	b2i_mutex.unlock_shared();

	std::unique_lock lk(info.mu);
	try{
		writeToConn(*info.conn, buf, msg_type, arg1, arg2);
	}
	catch (asio::system_error& e){
		std::unique_lock lk(b2i_mutex);
		info.conn->close();
		backend_to_info.erase(key);

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

void HandleConn(socket_ptr from, socket_ptr to){ //Sending messages from -> to
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
			switch (msg_type){

				case (CONNECT_LOCAL): //Guest wants to connect to host. Therefore, this will only ever be run by the guest.
				{
					to = std::make_shared<socket_type>(context, TCP);
					asio::error_code ec;
					while(true){
						to->connect(ip::tcp::endpoint(ip::make_address(ADDRESS), PORT), ec);
						if (!ec){
							break;
						}
					}

					writeToConn(*to, message_buf, CONNECT_REMOTE, arg1, arg2); //Essentially forwarding the message to remote.

					readFromConn(*to, message_buf); //Wait for confirmation
					writeToConn(*from, message_buf, CONFIRM, 0, 0); //Send confirmation back
					std::thread(HandleConn, to, from).detach();
					break;		
				}
				case (CONNECT_REMOTE): //Host recieved notification that a guest is trying to connect. Therefore, this will only run on the host
					{
					b2u_mutex.lock();

					backend_to_unconnected_clients[arg1].push(from);
					b2u_mutex.unlock();

					writeToBackend(arg1, message_buf, ESTABLISH, 0, 0); //Tell backend to create a new connection

					//We don't have to do anything else, since the backend will pick it up from here
					return;
					break;	
					}
				case (ESTABLISH): //Server tells backend to make a new connection. This serves to simulate connecting directly to a port.
					{
						b2u_mutex.lock();
						to = std::move(backend_to_unconnected_clients[arg1].front()); //This is safe, as the only reason why an ESTABLISH would be sent is if there's a new connection in the first place
						backend_to_unconnected_clients[arg1].pop();
						b2u_mutex.unlock();
						
						writeToConn(*from, message_buf, CONFIRM, 0, 0);
						writeToConn(*to, message_buf, CONFIRM, 0, 0);

						std::thread(HandleConn, to, from).detach();
						break;
					}
				
				case (WRITE_LOCAL): //When one side initiates a write 
					{

					writeToConn(*to, message_buf, WRITE_REMOTE, arg1, arg2);

					auto len = arg1;

					auto& segment = acquireSegment(write, segment_id);

					while (len>0){
						auto size=std::min(segment.size, len);
						buf.reserve(size);
						asio::read(*from, asio::buffer(buf.data(), size));
						pwrite(write.get().fd, buf.data(), size, segment.offset);
						#ifdef __linux__
							sync_file_range(write.get().fd, segment.offset, size, SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE);
						#elif defined(__APPLE__)
							//fsync_range(write.get().fd,  FFILESYNC, segment.offset, size);
							fcntl(write.get().fd, F_FULLFSYNC);
						#else
							fsync(write.get().fd);
						#endif

						writeToConn(*to, message_buf, SEGMENT_READ, segment.offset, size);
						readFromConn(*to, message_buf);
						len-=size;

					}

					releaseSegment(write.get(), segment_id);
					writeToConn(*from, message_buf, CONFIRM, 0,0);
					break;

					}
				case (WRITE_REMOTE): 
					{
					writeToConn(*to, message_buf, WRITE_LOCAL, arg1, arg2);
					break;
					}
				case (SEGMENT_READ):
					{
					auto offset = arg1;
					auto size = arg2;
					buf.reserve(size);
					pread(read.get().fd, buf.data(), size, offset);
					asio::write(*to, asio::buffer(buf.data(), size));
					writeToConn(*from, message_buf, CONFIRM, 0, 0);
					}
				default:
					{
						printf("This is not supposed to happen!\n");
						continue;
					}

			}
		}
	}

	catch (asio::system_error&){
		releaseSegment(write.get(), segment_id);
		
		if (from){
			from->close();
		}

		if (to){
			to->close();
		}


	}


}
void FrontendServer(){
	ip::tcp::acceptor acceptor(context, ip::tcp::endpoint(asio::ip::make_address(ADDRESS), PORT));
       
        for (;;){
            auto socket=std::make_shared<socket_type>(context, TCP);
            
            acceptor.accept(*socket);

	    socket->set_option(ip::tcp::no_delay( true)); //Should probably be placed in a wrapper function for making new tcp sockets (so I don't forget about the no_delay)


            std::thread(HandleConn, std::move(socket), std::shared_ptr<socket_type>(NULL)).detach();
        }
}

MessageType peekFromConn(socket_type& socket){
	std::array<uint8_t,4> buf;
	int bytes_read=0;

	while(bytes_read < buf.max_size()){
		bytes_read+=socket.receive(asio::buffer(buf), asio::socket_base::message_peek);
	}

	return static_cast<MessageType>(deserializeInt(buf.data(),0));
}
void HandleBackend(socket_ptr socket){
	auto msg_type = peekFromConn(*socket);
	if (msg_type == ESTABLISH){
		std::thread(HandleConn, std::move(socket), std::shared_ptr<socket_type>(NULL)).detach();
	}else{
		std::array<uint8_t, 12> message_buf;
		int id;
		try {
		std::tie(std::ignore, id, std::ignore) = readFromConn(*socket, message_buf);
		}catch (asio::system_error&){
			return; //socket no longer exists, so we have to exit now
		}

		std::unique_lock<std::shared_mutex> lk(b2i_mutex);
		if (backend_to_info.contains(id)){
			socket->close();
			return;
		}
		backend_to_info[id].conn=std::move(socket);

	}
}
void BackendServer(){
	unlink(SERVER_SOCKET.c_str());
	local::stream_protocol::acceptor acceptor(context, local::stream_protocol::endpoint(SERVER_SOCKET));
	for (;;){
		auto socket=std::make_shared<socket_type>(context, UNIX);
	    	acceptor.accept(*socket);
	    	std::thread(HandleBackend, std::move(socket)).detach();
	}

}

int main(int argc, char** argv){	
	std::string H2G_DEFAULT_FILE = "";
	std::string G2H_DEFAULT_FILE = "";
	#ifdef __APPLE__
		H2G_DEFAULT_FILE="/dev/rdisk4";
		G2H_DEFAULT_FILE="/dev/rdisk5";
	#elif defined(__linux__)
		H2G_DEFAULT_FILE = "/dev/disk/by-id/conn-h2g";
		G2H_DEFAULT_FILE = "/dev/disk/by-id/conn-g2h";
	#endif

	H2G.file=getEnv("CONN_SERVER_H2G_FILE", H2G_DEFAULT_FILE);
	G2H.file=getEnv("CONN_SERVER_G2H_FILE", G2H_DEFAULT_FILE);

	H2G.is_write = !is_guest;
	G2H.is_write = is_guest;

	std::array<DriveInfo*, 2> drives = {&H2G, &G2H};
	for(auto& info: drives){
		auto flags = (info->is_write ? O_WRONLY: O_RDONLY);
		while(info->fd == -1){
			info->fd = open(info->file.c_str(), flags);
			int error = errno;
			if (error > 0){
				fprintf(stderr, "Error opening the disk %s due to error: %s\n", info->file.c_str(), strerror(error));
			}
		}
		
		auto size=lseek(info->fd, 0, SEEK_END);
		#ifdef __APPLE__ //Because lseek doesn't work on block devices on MacOS
			uint32_t bcount;
			auto ret1=ioctl(info->fd, DKIOCGETBLOCKCOUNT, &bcount);

			uint32_t bsize;
			auto ret2= ioctl(info->fd, DKIOCGETBLOCKSIZE, &bsize);

			if ((ret1 < 0) || (ret2 < 0)){
				fprintf(stderr, "Error getting size of disk %s", info->file.c_str());
				exit(2);
			}

			size=bcount*bsize;
		#endif
		lseek(info->fd, 0, SEEK_SET);

		info->segment_to_info.reserve(NUM_SEGMENTS);
		
		uint32_t segment_size=size/NUM_SEGMENTS; //Later, we can use "fair allocation" to use all of the space available
		
		for(int i =0; i< NUM_SEGMENTS; i++){
			info->available_segments.emplace(i);
			info->segment_to_info.push_back({.offset=i*segment_size,.size=segment_size });
		}

		
	}
	
	std::thread(BackendServer).detach();
	std::thread(FrontendServer).detach();
	
	std::promise<void>().get_future().wait();
}

