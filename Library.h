typedef struct AsioConn AsioConn;

AsioConn* asio_connect(int id);

AsioConn* asio_acceptor_init(int id);
AsioConn* asio_acceptor_accept(AsioConn* acceptor);

void asio_close(AsioConn* result);

void asio_read(AsioConn* result, char* buf, int len, bool* err);
void asio_write(AsioConn* result, char* buf, int len, bool* err);

