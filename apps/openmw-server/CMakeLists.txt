set(OPENMW_SERVER_SOURCES
    main.cpp
    Server.cpp
    ScriptEngine.cpp
    MasterServerClient.cpp
    PlayerDatabase.cpp
    bindings/PlayerBindings.cpp
    bindings/ServerBindings.cpp
    ${CMAKE_SOURCE_DIR}/extern/bcrypt/bcrypt_impl.cpp
)

set(OPENMW_SERVER_HEADERS
    Server.hpp
    ScriptEngine.hpp
    MasterServerClient.hpp
    PlayerDatabase.hpp
    bindings/PlayerBindings.hpp
    bindings/ServerBindings.hpp
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
)

target_link_libraries(openmw-server
    components
    GameNetworkingSockets::GameNetworkingSockets_s
    SQLite::SQLite3
    ${LUA_LIBRARIES}
)

target_compile_features(openmw-server PRIVATE cxx_std_20)

install(TARGETS openmw-server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
