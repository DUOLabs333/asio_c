#include <stdint.h>

typedef struct AsioConn AsioConn;

AsioConn* asio_connect(int id);

AsioConn* asio_server_init(int id);
AsioConn* asio_server_accept(AsioConn* server);

void asio_close(AsioConn* conn);

void asio_read(AsioConn* conn, char** buf, int* len, bool* err);
void asio_write(AsioConn* conn, char* buf, int len, bool* err);

char* asio_get_buf(AsioConn* conn, uint32_t* cap);
