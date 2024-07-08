
class common(BuildBase):
    SRC_FILES=["util.cpp"]

    INCLUDE_PATHS=[get_dep_path("asio", "asio/include")]

    OUTPUT_NAME="shmem_connect"

class server(BuildBase):

    OUTPUT_TYPE=EXE 
    
    def __init__(self):
        self.SRC_FILES.append("Server.cpp")

class library(BuildBase):

     OUTPUT_TYPE=STATIC
    
    def __init__(self):
        self.SRC_FILES.append("Library.cpp")
        