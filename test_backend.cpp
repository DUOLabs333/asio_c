#include "asio_c.h"
#include <cstdint>
#include <cstdio>
#include <memory.h>
#include <cstdlib>
auto test_buf=new uint8_t[1024];
char* actual_buf;
bool err;
int main(int argc, char** argv){
	auto acceptor=asio_server_init(0);

	auto client=asio_server_accept(acceptor);

	for(int i=0; i < 256; i++){
		memset(test_buf, i, 1024);
		int dummy;
		asio_read(client, &actual_buf, &dummy, &err);
		if(memcmp(test_buf, actual_buf, 1024)){
			printf("Buffers don't match!");
			exit(1);
		}
	}


}
