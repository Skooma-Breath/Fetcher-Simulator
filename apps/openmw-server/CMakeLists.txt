set(OPENMW_SERVER_SOURCES
    main.cpp
    Server.cpp
)

set(OPENMW_SERVER_HEADERS
    Server.hpp
)

source_group("apps\\openmw-server" FILES
    ${OPENMW_SERVER_SOURCES}
    ${OPENMW_SERVER_HEADERS}
)

add_executable(openmw-server
    ${OPENMW_SERVER_SOURCES}
)

target_include_directories(openmw-server PRIVATE
    ${CMAKE_SOURCE_DIR}          # so <components/...> includes resolve
    ${GNS_INCLUDE_DIR}
)

target_link_libraries(openmw-server
    components
    GameNetworkingSockets::GameNetworkingSockets
)

target_compile_features(openmw-server PRIVATE cxx_std_20)

install(TARGETS openmw-server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
