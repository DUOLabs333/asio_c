typedef struct ShmemConn ShmemConn;

ShmemConn* shmem_connect(int id);

ShmemConn* shmem_acceptor_init(int id);
ShmemConn* shmem_acceptor_accept(ShmemConn* acceptor);

void shmem_close(ShmemConn* result);

void shmem_read(ShmemConn* result, char* buf, int len, bool* err);
void shmem_write(ShmemConn* result, char* buf, int len, bool* err);

