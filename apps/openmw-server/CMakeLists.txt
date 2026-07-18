set(OPENMW_SERVER_SOURCES
    AdminHttpServer.cpp
    main.cpp
    Server.cpp
    LuaServerContext.cpp
    MasterServerClient.cpp
    PlayerDatabase.cpp
    bindings/PlayerBindings.cpp
    bindings/ServerBindings.cpp
    bindings/TypesStubBindings.cpp
    ${CMAKE_SOURCE_DIR}/extern/bcrypt/bcrypt_impl.cpp
)

set(OPENMW_SERVER_HEADERS
    AdminHttpServer.hpp
    Server.hpp
    LuaServerContext.hpp
    MpEventQueue.hpp
    OutboundQueue.hpp
    MasterServerClient.hpp
    PlayerDatabase.hpp
    bindings/PlayerBindings.hpp
    bindings/ServerBindings.hpp
    bindings/TypesStubBindings.hpp
)

source_group("apps\\openmw-server" FILES
    ${OPENMW_SERVER_SOURCES}
    ${OPENMW_SERVER_HEADERS}
)

add_executable(openmw-server
    ${OPENMW_SERVER_SOURCES}
)

target_include_directories(openmw-server PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${GNS_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/extern/sol3
    ${CMAKE_SOURCE_DIR}/extern/httplib
    ${CMAKE_SOURCE_DIR}/extern/bcrypt
    ${CMAKE_SOURCE_DIR}/extern/GameNetworkingSockets/src/common  # crypto_25519.h, keypair.h
    ${CMAKE_SOURCE_DIR}/extern/GameNetworkingSockets/src/public  # tier0/*, minbase/*
    ${LUA_INCLUDE_DIR}
)

target_compile_definitions(openmw-server PRIVATE
    VALVE_CRYPTO_ENABLE_25519   # exposes CCrypto::GenerateSigningKeyPair, CECSigning* etc.
    OPENMW_SERVER_SOURCE_SCRIPTS_DIR="${CMAKE_SOURCE_DIR}/apps/openmw-server/scripts"
    OPENMW_SERVER_SOURCE_LUA_LIBS_DIR="${CMAKE_SOURCE_DIR}/components/lua"
)

target_link_libraries(openmw-server
    components
    GameNetworkingSockets::GameNetworkingSockets_s
    SQLite3::SQLite3
    ${LUA_LIBRARIES}
)

if (UNIX)
    target_link_libraries(openmw-server crypt)
endif()

target_compile_features(openmw-server PRIVATE cxx_std_20)

install(TARGETS openmw-server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_custom_command(TARGET openmw-server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/apps/openmw-server/scripts"
        "$<TARGET_FILE_DIR:openmw-server>/server-scripts"
)
