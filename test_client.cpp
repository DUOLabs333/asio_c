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
	auto client=asio_connect(0);
	
	auto start = std::chrono::steady_clock::now();

	for(int i=0; i < 256; i++){
		memset(test_buf, i, SIZE);
		asio_write(client, (char*)test_buf, SIZE, &err);
		int dummy;
		asio_read(client, &actual_buf, &dummy, &err);
		
		#if 0
		for (int j =0; j < dummy; j++){
			printf("%u ", (uint8_t)actual_buf[j]);
		}
		printf("\n");
		#endif

		#if 1 //Verification of transmission integrity
		if ((dummy!=SIZE) || memcmp(test_buf, actual_buf, SIZE)){
			printf("Buffers don't match!\n");
			printf("Offending: %i\n", i);
			exit(1);
	}
		#endif

	}

	auto end = std::chrono::steady_clock::now();
	const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
	printf("Time to complete: %lu\n", duration.count());


}
