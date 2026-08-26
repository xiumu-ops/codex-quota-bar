cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 NEW)
project(Codex-Quota-Bar VERSION 2.5.2 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "Codex-Quota-Bar supports Windows 11 x64 with MSVC only.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Codex-Quota-Bar supports x64 builds only; use an x64 MSVC generator.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CORE_HEADERS
    src/Core/Constants.h
    src/Core/DpiHelper.h
    src/Core/Models.h
    src/Core/ConfigStore.h
    src/Core/SimpleJson.h
    src/Core/Logger.h
)

set(CORE_SOURCES
    src/Core/ConfigStore.cpp
    src/Core/Logger.cpp
)

set(GRAPHICS_HEADERS
    src/Graphics/D2DCommon.h
    src/Graphics/Theme.h
    src/Graphics/Direct2DRenderer.h
)

set(GRAPHICS_SOURCES
    src/Graphics/Theme.cpp
    src/Graphics/Direct2DRenderer.cpp
)

set(UI_HEADERS
    src/UI/MainWindow.h
    src/UI/CustomMenu.h
)

set(UI_SOURCES
    src/UI/MainWindow.cpp
    src/UI/CustomMenu.cpp
)

set(SERVICES_HEADERS
    src/Services/CodexClient.h
    src/Services/CodexAppServerClient.h
    src/Services/CompanionMode.h
    src/Services/PipeServer.h
)

set(SERVICES_SOURCES
    src/Services/CodexClient.cpp
    src/Services/CodexAppServerClient.cpp
    src/Services/CompanionMode.cpp
    src/Services/PipeServer.cpp
)

set(RESOURCE_HEADERS
    src/resources/resource.h
)

set(SOURCES
    src/main.cpp
    ${CORE_SOURCES}
    ${CORE_HEADERS}
    ${GRAPHICS_SOURCES}
    ${GRAPHICS_HEADERS}
    ${UI_SOURCES}
    ${UI_HEADERS}
    ${SERVICES_SOURCES}
    ${SERVICES_HEADERS}
    ${RESOURCE_HEADERS}
    src/resources/app.rc
)

source_group("Header Files\\Core" FILES ${CORE_HEADERS})
source_group("Source Files\\Core" FILES ${CORE_SOURCES})
source_group("Header Files\\Graphics" FILES ${GRAPHICS_HEADERS})
source_group("Source Files\\Graphics" FILES ${GRAPHICS_SOURCES})
source_group("Header Files\\UI" FILES ${UI_HEADERS})
source_group("Source Files\\UI" FILES ${UI_SOURCES})
source_group("Header Files\\Services" FILES ${SERVICES_HEADERS})
source_group("Source Files\\Services" FILES ${SERVICES_SOURCES})
source_group("Header Files\\Resource" FILES ${RESOURCE_HEADERS})
source_group("Resource Files" FILES ${RESOURCE_HEADERS} src/resources/app.rc src/resources/app.manifest)

add_executable(CodexQuotaBarApp WIN32 ${SOURCES})

function(cqb_configure_target target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
            $<$<COMPILE_LANGUAGE:CXX>:/W4>
            $<$<COMPILE_LANGUAGE:CXX>:/WX>
            $<$<COMPILE_LANGUAGE:CXX>:/permissive->
            $<$<COMPILE_LANGUAGE:CXX>:/Zc:preprocessor>
        )
    endif()
endfunction()

cqb_configure_target(CodexQuotaBarApp)

if(MSVC)
    # app.rc 已内嵌应用清单；禁止链接器再生成默认清单，避免 RT_MANIFEST/1 冲突。
    target_link_options(CodexQuotaBarApp PRIVATE /MANIFEST:NO)
endif()

target_link_libraries(CodexQuotaBarApp
    user32
    shell32
    ole32
    d2d1
    dwrite
    dwmapi
    advapi32
)

include(CTest)
if(BUILD_TESTING)
    add_executable(FakeCodexAppServer tests/FakeCodexAppServer.cpp)
    cqb_configure_target(FakeCodexAppServer)

    add_executable(CodexAppServerIntegrationTests
        tests/CodexAppServerIntegrationTests.cpp
        src/Services/CodexClient.cpp
        src/Services/CodexAppServerClient.cpp
        src/Services/CompanionMode.cpp
        src/Core/ConfigStore.cpp
        src/Core/Logger.cpp
    )
    cqb_configure_target(CodexAppServerIntegrationTests)
    target_link_libraries(CodexAppServerIntegrationTests advapi32 shell32 ole32)

    add_executable(HookConfigTests
        tests/HookConfigTests.cpp
        installer/HookConfig.cpp
    )
    cqb_configure_target(HookConfigTests)
    target_include_directories(HookConfigTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/installer"
    )
    target_link_libraries(HookConfigTests
        shell32
        ole32
    )

    add_executable(SimpleJsonTests tests/SimpleJsonTests.cpp)
    cqb_configure_target(SimpleJsonTests)

    add_executable(PipeServerTests
        tests/PipeServerTests.cpp
        src/Services/PipeServer.cpp
        src/Core/ConfigStore.cpp
        src/Core/Logger.cpp
    )
    cqb_configure_target(PipeServerTests)
    target_link_libraries(PipeServerTests advapi32 shell32 ole32)

    add_test(
        NAME CodexAppServerIntegration
        COMMAND CodexAppServerIntegrationTests $<TARGET_FILE:FakeCodexAppServer>
    )
    add_test(NAME HookConfig COMMAND HookConfigTests)
    add_test(NAME SimpleJson COMMAND SimpleJsonTests)
    add_test(NAME PipeServer COMMAND PipeServerTests)
    set_tests_properties(CodexAppServerIntegration PROPERTIES TIMEOUT 30)
    set_tests_properties(HookConfig PROPERTIES TIMEOUT 10)
    set_tests_properties(SimpleJson PROPERTIES TIMEOUT 10)
    set_tests_properties(PipeServer PROPERTIES TIMEOUT 10)
endif()

set_target_properties(CodexQuotaBarApp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.build/output"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}/.build/output/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/.build/output/Release"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_CURRENT_SOURCE_DIR}/.build/output/RelWithDebInfo"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_CURRENT_SOURCE_DIR}/.build/output/MinSizeRel"
    OUTPUT_NAME "Codex-Quota-Bar"
)
