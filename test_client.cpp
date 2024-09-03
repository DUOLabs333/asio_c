#include "asio_c.h"
#include <cstdint>
#include <cstdio>
#include <memory.h>
#include <cstdlib>

auto test_buf=new uint8_t[1024];
char* actual_buf;
bool err;
int main(int argc, char** argv){
	auto client=asio_connect(0);

	for(int i=0; i < 256; i++){
		memset(test_buf, i, 1024);
		asio_write(client, (char*)test_buf, 1024, &err);
	}

	for(int i=0; i < 256; i++){
		memset(test_buf, i, 1024);
		int dummy;
		asio_read(client, &actual_buf, &dummy, &err);
		printf("Actual length: %i\n", dummy);
		for (int j =0; j < dummy; j++){
			printf("%i \n", actual_buf[j]);
		}
		if(memcmp(test_buf, actual_buf, 1024)){
			printf("Buffers don't match!\n");
			printf("Offending: %i\n", i);
			exit(1);
		}
	}



}
