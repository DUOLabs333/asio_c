class server(BuildBase):
    SRC_FILES=["Server.cpp","util.cpp"]

    INCLUDE_PATHS=[get_dep_path("asio", "asio/include")]

    OUTPUT_NAME = "shmem_connect"