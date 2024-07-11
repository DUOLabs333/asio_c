COMMON_SRC_FILES=["util.cpp"]

class common(BuildBase):
    INCLUDE_PATHS=[get_dep_path("asio", "asio/include")]

    OUTPUT_NAME="shmem_connect"

class server(common):

    OUTPUT_TYPE=EXE 
    SRC_FILES=COMMON_SRC_FILES+["Server.cpp"]

class library(common):

     OUTPUT_TYPE=STATIC

     SRC_FILES=COMMON_SRC_FILES+["Library.cpp"]
        
class test_backend(common):
    OUTPUT_TYPE=EXE
    SRC_FILES=["test_backend.cpp"]
    STATIC_LIBS=[library]
    OUTPUT_NAME="test_backend"

class test_client(common):
    OUTPUT_TYPE=EXE
    SRC_FILES=["test_client.cpp"]
    STATIC_LIBS=[library]
    OUTPUT_NAME="test_client"
