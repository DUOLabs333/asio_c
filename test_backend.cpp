#include "Library.h"
#include <cstdint>
#include <cstdio>
#include <memory.h>
#include <cstdlib>
auto test_buf=new uint8_t[1024];
auto actual_buf=new uint8_t[1024];
bool err;
int main(int argc, char** argv){
	auto acceptor=asio_acceptor_init(1);

	auto client=asio_acceptor_accept(acceptor);

	for(int i=0; i < 256; i++){
		memset(test_buf, i, 1024);
		asio_read(client, (char*)actual_buf, 1024, &err);
		if(memcmp(test_buf, actual_buf, 1024)){
			printf("Buffers don't match!");
			exit(1);
		}
	}


}
