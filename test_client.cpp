#include "Library.h"
#include <cstdint>
#include <cstdio>
#include <memory.h>
#include <cstdlib>

auto buf=new uint8_t[1024];
bool err;
int main(int argc, char** argv){
	auto client=asio_connect(1);

	for(int i=0; i < 256; i++){
		memset(buf, i, 1024);
		asio_write(client, (char*)buf, 1024, &err);
	}


}
