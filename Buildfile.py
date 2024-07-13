class library(BuildBase):
    
    INCLUDE_PATHS=[get_dep_path("asio", "asio/include"), get_dep_path("lz4","lib")]

    OUTPUT_TYPE=STATIC
    OUTPUT_NAME="asio_c"

    SRC_FILES=["utils.cpp", "asio_c.cpp"]

    STATIC_LIBS=[get_dep_path("lz4", "lib/liblz4.a")]
        
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
