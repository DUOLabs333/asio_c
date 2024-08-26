COMMON_INCLUDE_PATHS=[get_dep_path("asio", "asio/include")]
COMMON_SRC_FILES=["utils.cpp"]

class library(BuildBase):
    
    INCLUDE_PATHS=COMMON_INCLUDE_PATHS+[get_dep_path("lz4","lib")]

    OUTPUT_TYPE=STATIC
    OUTPUT_NAME="asio_c"

    SRC_FILES=COMMON_SRC_FILES+["asio_c.cpp"]

    STATIC_LIBS=[get_dep_path("lz4", "lib/liblz4.a")]


class server(BuildBase):
    INCLUDE_PATHS = COMMON_INCLUDE_PATHS
    OUTPUT_TYPE=EXE
    OUTPUT_NAME = "server"
    SRC_FILES = COMMON_SRC_FILES + ["Server.cpp"]
    
class test_backend(BuildBase):
    OUTPUT_TYPE=EXE
    SRC_FILES=["test_backend.cpp"]
    STATIC_LIBS=[library]
    OUTPUT_NAME="test_backend"

class test_client(BuildBase):
    OUTPUT_TYPE=EXE
    SRC_FILES=["test_client.cpp"]
    STATIC_LIBS=[library]
    OUTPUT_NAME="test_client"
