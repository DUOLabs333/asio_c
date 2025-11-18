#include "asio_c.h"
#include <cstdint>
#include <cstdio>
#include <memory.h>
#include <cstdlib>
#include <chrono>

#define SIZE 4*1024*1024

auto test_buf=new uint8_t[SIZE];
char* actual_buf;
bool err;
int main(int argc, char** argv){
	auto acceptor=asio_server_init(0);

	auto client=asio_server_accept(acceptor);

	auto start = std::chrono::steady_clock::now();

	for(int i=0; i < 256; i++){
		memset(test_buf, i, SIZE);
		int dummy;
		asio_read(client, &actual_buf, &dummy, &err);

		#if 1 //Verification of transmission integrity
		if ((dummy!=SIZE) || memcmp(test_buf, actual_buf, SIZE)){
			printf("Buffers don't match!\n");
			printf("Offending: %i\n", i);
			exit(1);
		}
		#endif
		asio_write(client, actual_buf, dummy, &err);

	}

	auto end = std::chrono::steady_clock::now();
	const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
	printf("Time to complete: %lu\n", duration.count());

}
