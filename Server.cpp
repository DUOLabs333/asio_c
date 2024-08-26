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


std::string H2G_FILE=getEnv("CONN_SERVER_H2G_FILE", "/dev/rdisk4");
std::string G2H_FILE=getEnv("CONN_SERVER_G2H_FILE", "/dev/rdisk5");
//TODO: Make one for guest to
std::string ADDRESS=getEnv("CONN_SERVER_ADDRESS", "192.168.64.1");
int PORT=getEnv("CONN_SERVER_PORT", 4000);
bool is_guest = getEnv("CONN_SERVER_IS_GUEST", false); 

int NUM_SEGMENTS=100;

int h2g_fd = -1;
int g2h_fd = -1;

typedef struct {
	uint32_t offset;
	uint32_t size;
} SegmentInfo;
std::vector<SegmentInfo> segment_to_info;

std::queue<int> available_segments;
std::mutex as_mutex;
std::condition_variable as_cv;

std::atomic<int> thread_counter = 0;

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

auto& acquireSegment(int& segment){
	std::unique_lock lk(as_mutex);
	as_cv.wait(lk, []{return !available_segments.empty();});
	
	segment=available_segments.front();
	available_segments.pop();
	return segment_to_info[segment];
	
}

void releaseSegment(int& segment){
	if (segment == -1){
		return;
	}
	std::unique_lock<std::mutex> lk(as_mutex);
	available_segments.push(segment);
	as_cv.notify_all();

	segment=-1;
}

void HandleConn(socket_ptr socket){
	std::array<uint8_t, 12> message_buf;
	socket_ptr to = NULL;
	try {
		for (;;){
			auto [ msg_type, arg1, arg2 ] = readFromConn(*socket, message_buf);
			switch (msg_type){

				case (CONNECTTO): //Guest wants to connect to host. Therefore, this will only ever be run by the guest.
				{
					to = std::make_unique<socket_type>(context, TCP);
					asio::error_code ec;
					while(true){
						to->connect(ip::tcp::endpoint(ip::make_address(ADDRESS), PORT), ec);
						if (!ec){
							break;
						}
					}

					writeToConn(*to, message_buf, CONNECTFROM, arg1, arg2); //Essentially forwarding the message.

					readFromConn(*to, message_buf); //Wait for confirmation
						
				}
				case (CONNECTFROM): //Guest wants to connect to host. Therefore, this will only be run by the host.
					{
					b2u_mutex.lock();
					backend_to_unconnected_clients[arg1].push(std::move(socket));
					b2u_mutex.unlock();

					writeToBackend(arg1, message_buf, ESTABLISH, 0, 0); //Tell backend to create a new connection

					//We don't have to do anything else, since the backend will pick it up from here
					return;
					break;	
					}
				case (ESTABLISH): //New connection spawned from the backend. This serves to simulate connecting directly to a port.
					{
						b2u_mutex.lock();
						to = std::move(backend_to_unconnected_clients[arg1].front()); //This is safe, as the only reason why an ESTABLISH would be sent is if there's a new connection in the first place
						backend_to_unconnected_clients[arg1].pop();
						b2u_mutex.unlock();
						
						writeToConn(*to, message_buf, CONNECTFROM, 1, 0); //Send confirmation back, indicating that the setup is finished
						break;
					}

				case (WRITETO): //When one side initiates a write 
					{

					writeToConn(*to, WRITEFROM, arg1, arg2);
					auto& segment = acquireSegment(info.segment);
					ssize_t size=std::min(segment.size, arg1);
					
					writeToConn(*info.conn, message_buf, SEGMENT, segment.offset, size);
					
					readFromConn(*info.conn, message_buf);
					
					ssize_t bytes_written=0;

					t2i_mutex.lock_shared();

					while(bytes_written < size){
						bytes_written+=write(thread_to_info[info.to].pipes[1], device_mmap+segment.offset+bytes_written, size - bytes_written);
					}

					t2i_mutex.unlock_shared();

					writeToConn(*info.conn, message_buf, WRITE, 1, 0);
					
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
            auto socket=std::make_unique<socket_type>(context, TCP);
            
            acceptor.accept(*socket);

	    socket->set_option(ip::tcp::no_delay( true)); //Should just be placed in a wrapper functionvfor making new tcp sockets (so I don't forget about the no_delay


            std::thread(HandleConn, std::move(socket)).detach();
        }
}

MessageType peekFromConn(socket_type& socket){
	std::array<uint8_t,4> buf;
	int bytes_read=0;

	while(bytes_read < buf.max_size()){
		bytes_read+=socket.receive(asio::buffer(buf), socket_base::message_peek);
	}

	return static_cast<MessageType>(deserializeInt(buf.data(),0));
}
void HandleBackend(socket_ptr socket){
	auto msg_type = peekFromConn(*socket);
	if (msg_type == ESTABLISH){
		std::thread(HandleConn, std::move(socket)).detach();
	}else{
		std::array<uint8_t, 12> message_buf;
		auto [msg_type, arg1, arg2] = readFromConn(*socket, message_buf);
		{
			std::unique_lock<std::shared_mutex> lk(b2i_mutex);
			if (backend_to_info.contains(arg1)){
				socket->close();
				return;
			}
			backend_to_info[arg1].conn=std::move(socket);
		}

	}
}
void BackendServer(){
	unlink(SOCKET.c_str());
	local::stream_protocol::acceptor acceptor(context, local::stream_protocol::endpoint(SOCKET));
	for (;;){
		auto socket=std::make_unique<socket_type>(context, UNIX);
	    	acceptor.accept(*socket);
	    	std::thread(HandleBackend, std::move(socket)).detach();
	}

}

int main(int argc, char** argv){

	auto device_size=open_disk(DEVICE, &device_mmap, NULL);
	
	segment_to_info.resize(NUM_SEGMENTS);
	
	auto segment_size=device_size/NUM_SEGMENTS; //Later, can use "fair allocation" to use all of the space available
	for(int i =0; i< NUM_SEGMENTS; i++){
		available_segments.push(i);
		segment_to_info[i]={.offset=i*segment_size,.size=segment_size };
	}

	std::thread(BackendServer).detach();
	std::thread(FrontendServer).detach();
	
	std::promise<void>().get_future().wait();
}

