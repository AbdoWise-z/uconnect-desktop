cmake_minimum_required(VERSION 4.3)
project(uconnect_desktop VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(UCD_DEBUG_UI "Build the Dear ImGui debug window" ON)
option(UCD_BUILD_TESTS "Build the test suite" ON)

# --- Qt ----------------------------------------------------------------------
# CMAKE_PREFIX_PATH / Qt6_DIR win if set; otherwise look where aqtinstall and
# the Qt online installer put MinGW builds on Windows.
if (MINGW)
    file(GLOB _ucd_qt_hints LIST_DIRECTORIES true "C:/Qt/6.*/mingw_64")
    list(SORT _ucd_qt_hints COMPARE NATURAL ORDER DESCENDING)
endif ()
find_package(Qt6 6.5 REQUIRED
    COMPONENTS Core Gui Network Qml Quick QuickControls2 OpenGL Test
    HINTS ${_ucd_qt_hints})
qt_standard_project_setup(REQUIRES 6.5)

# --- uconnect ----------------------------------------------------------------
include(FetchContent)
set(UCONNECT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UCONNECT_BUILD_SERVER OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    uconnect
    GIT_REPOSITORY https://github.com/AbdoWise-z/uconnect.git
    GIT_TAG 7fc35921de5ba67acb860defb246e65be68e3fa3
)
FetchContent_MakeAvailable(uconnect)
# uconnect's targets use ${CMAKE_SOURCE_DIR}/include, which only points at its
# own headers when it is the top-level project. Here it resolves to this
# project's root, so mirror the public headers there.
file(COPY ${uconnect_SOURCE_DIR}/include/ DESTINATION ${CMAKE_SOURCE_DIR}/include)

# --- Monocypher (Ed25519 device identities) ------------------------------------
# Small, audited, dependency-free. The optional ed25519 module is RFC 8032
# Ed25519 (SHA-512), which other implementations (e.g. Dart's) interoperate with.
FetchContent_Declare(
    monocypher
    GIT_REPOSITORY https://github.com/LoupVaillant/Monocypher.git
    GIT_TAG 4.0.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(monocypher)
add_library(monocypher STATIC
    ${monocypher_SOURCE_DIR}/src/monocypher.c
    ${monocypher_SOURCE_DIR}/src/optional/monocypher-ed25519.c
)
target_include_directories(monocypher PUBLIC
    ${monocypher_SOURCE_DIR}/src
    ${monocypher_SOURCE_DIR}/src/optional)

# --- Dear ImGui (debug window) -----------------------------------------------
if (UCD_DEBUG_UI)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.9
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(imgui)

    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR})
    # The OpenGL3 backend loads GL itself (dlopen on Linux/macOS).
    target_link_libraries(imgui PRIVATE ${CMAKE_DL_LIBS})
endif ()

# --- core: networking and services, no UI --------------------------------------
add_library(uconnect_desktop_core STATIC
    src/core/Hello.cpp
    src/core/Hello.hpp
    src/core/Identity.cpp
    src/core/Identity.hpp
    src/core/Permissions.cpp
    src/core/Permissions.hpp
    src/core/Session.cpp
    src/core/Session.hpp
    src/core/StorageService.cpp
    src/core/StorageService.hpp
)
target_include_directories(uconnect_desktop_core PUBLIC src)
target_link_libraries(uconnect_desktop_core PUBLIC Qt6::Core Qt6::Network uconnect PRIVATE monocypher)
if (NOT MSVC)
    target_compile_options(uconnect_desktop_core PRIVATE -Wall -Wextra)
endif ()

# --- app ---------------------------------------------------------------------
qt_add_executable(uconnect_desktop
    main.cpp
    src/app/AppController.cpp
    src/app/AppController.hpp
    src/app/PeerListModel.cpp
    src/app/PeerListModel.hpp
    src/app/TransferListModel.cpp
    src/app/TransferListModel.hpp
)

if (QT_KNOWN_POLICY_QTP0004)
    qt_policy(SET QTP0004 NEW)
endif ()
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(uconnect_desktop
    URI UConnectDesktop
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        # pages and windows
        qml/FirstRunPage.qml
        qml/ConnectPage.qml
        qml/WorkspacePage.qml
        qml/DevicesPanel.qml
        qml/DeviceView.qml
        qml/StatusBar.qml
        qml/ServiceMenu.qml
        qml/SettingsWindow.qml
        qml/ServiceWindow.qml
        qml/StorageView.qml
        qml/PermissionEditor.qml
        qml/TrustDialog.qml
        # controls
        qml/UButton.qml
        qml/UToolButton.qml
        qml/UTextField.qml
        qml/UCheckBox.qml
        qml/ULink.qml
        qml/UMenu.qml
        qml/UMenuItem.qml
        qml/UMenuSeparator.qml
        qml/UMenuBar.qml
        qml/Avatar.qml
        qml/StatusDot.qml
        qml/TitledSeparator.qml
        qml/Banner.qml
)

target_link_libraries(uconnect_desktop PRIVATE
    uconnect_desktop_core
    Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)

if (UCD_DEBUG_UI)
    target_sources(uconnect_desktop PRIVATE
        src/debug/ImGuiWindow.cpp
        src/debug/ImGuiWindow.hpp
        src/debug/DebugWindow.cpp
        src/debug/DebugWindow.hpp
    )
    target_compile_definitions(uconnect_desktop PRIVATE UCD_HAS_DEBUG_UI)
    target_link_libraries(uconnect_desktop PRIVATE imgui Qt6::OpenGL)
endif ()

if (NOT MSVC)
    target_compile_options(uconnect_desktop PRIVATE -Wall -Wextra)
endif ()

# Debug builds keep the console on Windows, for the session log.
set_target_properties(uconnect_desktop PROPERTIES
    WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>
    MACOSX_BUNDLE TRUE)
if (APPLE)
    set_target_properties(uconnect_desktop PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.uconnect.desktop"
        MACOSX_BUNDLE_BUNDLE_NAME "uConnect"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
    find_program(UCD_MACDEPLOYQT macdeployqt
        HINTS "${QT6_INSTALL_PREFIX}/bin" "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}"
        NO_DEFAULT_PATH REQUIRED)
    add_custom_command(TARGET uconnect_desktop POST_BUILD
        COMMAND "${UCD_MACDEPLOYQT}" "$<TARGET_BUNDLE_DIR:uconnect_desktop>"
                "-qmldir=${CMAKE_CURRENT_SOURCE_DIR}/qml" -always-overwrite -codesign=-
        COMMENT "Deploying Qt runtime inside the macOS app bundle"
        VERBATIM)
endif ()

# --- Windows: make the build directory runnable -------------------------------
# Copies the Qt DLLs, QML modules and plugins next to the executable, so it runs
# from CLion or Explorer without Qt on PATH.
if (WIN32)
    find_program(UCD_WINDEPLOYQT windeployqt
        HINTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}" NO_DEFAULT_PATH REQUIRED)
    add_custom_command(TARGET uconnect_desktop POST_BUILD
        COMMAND "${UCD_WINDEPLOYQT}" --no-translations --no-compiler-runtime --no-opengl-sw
                --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml" "$<TARGET_FILE:uconnect_desktop>"
        COMMENT "Deploying Qt runtime next to uconnect_desktop"
        VERBATIM)

    # The compiler's own runtime, not the older one Qt ships: this executable
    # needs the newer libstdc++, and it serves Qt's DLLs too.
    if (MINGW)
        get_filename_component(_ucd_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
        foreach (_dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
            if (EXISTS "${_ucd_mingw_bin}/${_dll}")
                add_custom_command(TARGET uconnect_desktop POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${_ucd_mingw_bin}/${_dll}" "$<TARGET_FILE_DIR:uconnect_desktop>"
                    VERBATIM)
            endif ()
        endforeach ()
    endif ()
endif ()

# --- tests ---------------------------------------------------------------------
if (UCD_BUILD_TESTS)
    enable_testing()
    # Qt's DLLs from its bin directory, but the compiler's (newer) C++ runtime
    # ahead of the older copy that directory also holds.
    if (WIN32)
        set(_ucd_test_path "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
        if (MINGW)
            list(APPEND _ucd_test_path "PATH=path_list_prepend:${_ucd_mingw_bin}")
        endif ()
    endif ()

    function(ucd_add_test name)
        qt_add_executable(${name} tests/${name}.cpp)
        set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
        target_link_libraries(${name} PRIVATE uconnect_desktop_core Qt6::Test)
        add_test(NAME ${name} COMMAND ${name})
        if (WIN32)
            set_tests_properties(${name} PROPERTIES ENVIRONMENT_MODIFICATION "${_ucd_test_path}")
        endif ()
    endfunction()

    ucd_add_test(IdentityTest)
    ucd_add_test(StorageServiceTest)

    ucd_add_test(StorageViewTest)
    target_sources(StorageViewTest PRIVATE
        src/app/PeerListModel.cpp src/app/PeerListModel.hpp
        src/app/TransferListModel.cpp src/app/TransferListModel.hpp)
    target_link_libraries(StorageViewTest PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
    target_compile_definitions(StorageViewTest PRIVATE UCD_QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    set_tests_properties(StorageViewTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
    if (WIN32)
        set_property(TEST StorageViewTest APPEND PROPERTY ENVIRONMENT
            "QT_PLUGIN_PATH=${QT6_INSTALL_PREFIX}/plugins"
            "QT_QPA_FONTDIR=$ENV{SystemRoot}/Fonts")
    endif ()

    ucd_add_test(SettingsTest)
    target_sources(SettingsTest PRIVATE
        src/app/AppController.cpp src/app/AppController.hpp
        src/app/PeerListModel.cpp src/app/PeerListModel.hpp
        src/app/TransferListModel.cpp src/app/TransferListModel.hpp)
    target_link_libraries(SettingsTest PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
    target_compile_definitions(SettingsTest PRIVATE UCD_QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    get_property(_ucd_gui_test_environment TEST StorageViewTest PROPERTY ENVIRONMENT)
    set_tests_properties(SettingsTest PROPERTIES ENVIRONMENT "${_ucd_gui_test_environment}")
endif ()
