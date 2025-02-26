cmake_minimum_required(VERSION 3.5)
cmake_policy(SET CMP0048 NEW)
include(CheckCXXCompilerFlag)

project(FlashMQ VERSION 0.9.5 LANGUAGES CXX)

add_definitions(-DOPENSSL_API_COMPAT=0x10100000L)
add_definitions(-DFLASHMQ_VERSION=\"${PROJECT_VERSION}\")

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

SET(CMAKE_CXX_FLAGS "-msse4.2")

check_cxx_compiler_flag("-rdynamic" COMPILER_SUPPORTS_RDYNAMIC)
if (${COMPILER_SUPPORTS_RDYNAMIC})
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -rdynamic")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -rdynamic")
endif()

add_compile_options(-Wall)

add_executable(FlashMQ
    forward_declarations.h
    mainapp.h
    utils.h
    threaddata.h
    client.h
    session.h
    mqttpacket.h
    exceptions.h
    types.h
    subscriptionstore.h
    rwlockguard.h
    retainedmessage.h
    cirbuf.h
    logger.h
    authplugin.h
    configfileparser.h
    sslctxmanager.h
    timer.h
    iowrapper.h
    mosquittoauthoptcompatwrap.h
    settings.h
    listener.h
    unscopedlock.h
    scopedsocket.h
    bindaddr.h
    oneinstancelock.h
    evpencodectxmanager.h
    acltree.h
    enums.h
    threadlocalutils.h
    flashmq_plugin.h
    retainedmessagesdb.h
    persistencefile.h
    sessionsandsubscriptionsdb.h
    qospacketqueue.h
    threadauth.h

    mainapp.cpp
    main.cpp
    utils.cpp
    threaddata.cpp
    client.cpp
    session.cpp
    mqttpacket.cpp
    exceptions.cpp
    types.cpp
    subscriptionstore.cpp
    rwlockguard.cpp
    retainedmessage.cpp
    cirbuf.cpp
    logger.cpp
    authplugin.cpp
    configfileparser.cpp
    sslctxmanager.cpp
    timer.cpp
    iowrapper.cpp
    mosquittoauthoptcompatwrap.cpp
    settings.cpp
    listener.cpp
    unscopedlock.cpp
    scopedsocket.cpp
    bindaddr.cpp
    oneinstancelock.cpp
    evpencodectxmanager.cpp
    acltree.cpp
    threadlocalutils.cpp
    flashmq_plugin.cpp
    retainedmessagesdb.cpp
    persistencefile.cpp
    sessionsandsubscriptionsdb.cpp
    qospacketqueue.cpp
    threadauth.cpp

    )

target_link_libraries(FlashMQ pthread dl ssl crypto)

install(TARGETS FlashMQ
  RUNTIME DESTINATION "/usr/bin/")

install(DIRECTORY DESTINATION "/var/lib/flashmq")
install(DIRECTORY DESTINATION "/var/log/flashmq")
install(FILES flashmq.conf DESTINATION "/etc/flashmq")
install(FILES debian/flashmq.service DESTINATION "/lib/systemd/system")

SET(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_CURRENT_SOURCE_DIR}/debian/conffiles;${CMAKE_CURRENT_SOURCE_DIR}/debian/preinst;${CMAKE_CURRENT_SOURCE_DIR}/debian/postinst;${CMAKE_CURRENT_SOURCE_DIR}/debian/postrm;${CMAKE_CURRENT_SOURCE_DIR}/debian/prerm")

SET(CPACK_GENERATOR "DEB")
SET(CPACK_DEBIAN_PACKAGE_MAINTAINER "Wiebe Cazemier <wiebe@halfgaar.net>")
SET(CPACK_DEBIAN_PACKAGE_DESCRIPTION "Light-weight, high performance MQTT server capable of million+ messages per second.")
SET(CPACK_PACKAGE_HOMEPAGE_URL "https://www.flashmq.org/")
SET(CPACK_DEBIAN_PACKAGE_DEPENDS "libssl1.1 (>= 1.1.0)")
SET(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
SET(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
SET(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
INCLUDE(CPack)
