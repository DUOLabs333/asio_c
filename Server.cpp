#include "util.hpp"
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <queue>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <array>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>


std::string ADDRESS="0.0.0.0";
int PORT=8222;
std::string SOCKET="/tmp/shmem.sock";
std::string DEVICE_PATH="/dev/disk4";
int NUM_SEGMENTS=100;

char* device_mmap;

typedef struct {
	int offset;
	int size;
} SegmentInfo;
std::vector<SegmentInfo> segment_to_info;

std::unordered_set<int> available_segments;
std::mutex as_mutex;

std::atomic<int> thread_counter;

typedef struct {
	 socket_ptr conn = NULL;
	std::mutex mu;
} BackendInfo;

std::unordered_map<int, BackendInfo> backend_to_info;
std::shared_mutex b2i_mutex;

std::unordered_map<int, std::queue<int>> backend_to_unconnected_clients;
std::mutex b2u_mutex;

typedef struct {
	int segment = -1;
	std::array<int, 2> pipes;
	std::atomic<int> to = -1;
	socket_ptr conn = NULL;
} ThreadInfo;

std::unordered_map<int, ThreadInfo> thread_to_info;
std::shared_mutex t2i_mutex;

asio::io_context context;

void writeToBackend(int key, std::array<uint8_t, 12> buf, MessageType msg_type, uint8_t arg1, uint8_t arg2){
	b2i_mutex.lock_shared();
	auto& info=backend_to_info[key];
	b2i_mutex.unlock_shared();

	{
		std::unique_lock<std::mutex> lk(info.mu);
		writeToConn(info.conn, buf, msg_type, arg1, arg2);
	}
}

auto& acquireSegment(int& segment){
	std::unique_lock<std::mutex> lk(as_mutex);
	
}
void HandleConn(socket_ptr socket){
	auto key=thread_counter.fetch_add(1);
	t2i_mutex.lock_shared();
	auto& info=thread_to_info[key];
	t2i_mutex.unlock_shared();

	pipe(info.pipes.data());
	info.conn=socket;
	std::array<uint8_t, 12> message_buf;
	
	try {
		for (;;){
			auto [ msg_type, arg1, arg2 ] = readFromConn(socket, message_buf);
			switch (msg_type){
				case (CONNECT):
					{
					b2u_mutex.lock();
					backend_to_unconnected_clients[arg1].push(key);
					b2u_mutex.unlock();

					writeToBackend(arg1, message_buf, ESTABLISH, 0, 0); //Locks, then calls writeToConn

					info.to.wait(-1);
					writeToConn(socket, message_buf, CONNECT, 1, 0);
					break;	
					}
				case (ESTABLISH):
					{
						b2u_mutex.lock();
						int to = backend_to_unconnected_clients[arg1].pop();
						b2u_mutex.unlock();
						
						info.to=to;

						t2i_mutex.lock();
						thread_to_info[to].to=key;
						t2i_mutex.unlock();

						writeToConn(socket, message_buf, CONNECT, 1, 0);
						break;
					}
				case (READ):
					{
					auto& segment = acquireSegment(info.segment); //Should be a reference, and set it. Should also return a reference to the proper key.

					ssize_t bytes_read=0;
					ssize_t size=std::min(segment.size, arg1);
					while(bytes_read < size){
						bytes_read+=read(info.pipes[0],device_mmap+segment.offset+bytes_read, size - bytes_read);
					}
					msync(device_mmap+segment.offset, size, MS_SYNC);
					writeToConn(socket, message_buf, SEGMENT, segment.offset, size);

					readFromConn(socket, message_buf);
					
					releaseSegment(info.segment); //Should also be a reference, and should set to -1
					break;
					}


				case (WRITE):
					{
					auto& segment = acquireSegment(info.segment);
					ssize_t size=std::min(segment.size, arg1);
					
					writeToConn(socket, message_buf, SEGMENT, segment.offset, size);
					
					readFromConn(socket, message_buf);
					
					ssize_t bytes_written=0;

					t2i_mutex.lock_shared();

					while(bytes_written < size){
						write(thread_to_info[info.to].pipes[1], device_mmap+segment.offset+bytes_written, size - bytes_written);
					}

					t2i_mutex.unlock_shared();

					writeToConn(socket, message_buf, WRITE, 1, 0);
					
					releaseSegment(info.segment);
					break;

					}
				default:
					{
						continue;
					}

			}
		}
	}

	catch (asio::system_error& e){
	

		releaseSegment(info.segment);
		close(info.pipes[0]);
		close(info.pipes[1]);
		
		t2i_mutex.lock();

		int to=info.to;
		thread_to_info.erase(key);

		t2i_mutex.unlock();

		t2i_mutex.lock_shared();
		if (thread_to_info.contains(to)){
			info.conn->close();
		}
		t2i_mutex.unlock_shared();
	}


}
void FrontendServer(){
	tcp::acceptor acceptor(context, tcp::endpoint(asio::ip::make_address(ADDRESS), PORT));
       
        for (;;){
            auto socket=std::make_shared<socket_type>(context, ip::tcp::v4);
            
            acceptor.accept(*socket);

	    socket->set_option(ip::tcp::no_delay( true));

            std::thread(HandleConn, socket).detach();
        }
}

MessageType peekFromConn(socket_ptr socket){
	std::array<uint8_t,4> buf;
	asio::read(*socket, asio::buffer(buf), socket::local::message_peek);

	return static_cast<MessageType>(deserializeInt(buf.data(),0));
}
void HandleBackend(socket_ptr socket){
	auto msg_type = peekFromConn(socket);
	if (msg_type == ESTABLISH){
		std::thread(HandleConn, socket).detach();
	}else{
		std::array<uint8_t, 12> message_buf;
		auto [msg_type, arg1, arg2] = readFromConn(socket, message_buf);
		{
			std::unique_lock<std::shared_mutex> lk(b2i_mutex);
			if (backend_to_info.contains(arg1)){
				socket->close();
				return;
			}
			backend_to_info[arg1].conn=socket;
		}

	}
}
void BackendServer(){
	unlink(SOCKET.c_str());
	local::stream_protocol::acceptor acceptor(context, local::stream_protocol::endpoint(SOCKET));
	for (;;){
            auto socket=std::make_shared<socket_type>(context);
	    acceptor.accept(*socket);
	    std::thread(HandleBackend, socket).detach();
	}

}

int main(int argc, char** argv){
	int device_fd=open(DEVICE_PATH.c_str(), O_RDWR);

	int device_size=lseek(device_fd, 0, SEEK_END);

	lseek(device_size, 0, SEEK_SET);

	device_mmap=(char*)mmap(NULL, device_size, PROT_WRITE| PROT_READ, MAP_SHARED, device_fd, 0);
	
	segment_to_info.resize(NUM_SEGMENTS);
	
	int segment_size=device_size/NUM_SEGMENTS; //Later, can use "fair allocation" to use all of the space available
	for(int i =0; i< NUM_SEGMENTS; i++){
		available_segments.insert(i);
		segment_to_info[i]={.offset=i*segment_size,.size=segment_size };
	}

	std::thread(BackendServer).detach();
	std::thread(FrontendServer).detach();
	
	std::promise<void>().get_future().wait();
}

