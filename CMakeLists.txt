cmake_minimum_required(VERSION 4.3)
project(uconnect_desktop VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

option(UCD_DEBUG_UI "Build the Dear ImGui debug window" ON)
option(UCD_BUILD_TESTS "Build the test suite" ON)

# --- Qt ----------------------------------------------------------------------
# CMAKE_PREFIX_PATH / Qt6_DIR win if set; otherwise look where aqtinstall and
# the Qt online installer put MinGW builds on Windows.
if (MINGW)
    file(GLOB _ucd_qt_hints LIST_DIRECTORIES true "C:/Qt/6.*/mingw_64")
    list(SORT _ucd_qt_hints COMPARE NATURAL ORDER DESCENDING)
endif ()
if (APPLE)
    # Qt before 6.9 links AGL, whose link stub the macOS 26 SDK no longer
    # ships. Nothing here uses it; OpenGL stands in, harmless on every version.
    set(WrapOpenGL_AGL "-framework OpenGL" CACHE STRING "Framework Qt links in place of AGL")
endif ()
find_package(Qt6 6.5 REQUIRED
    COMPONENTS Core Gui Widgets Network Qml Quick QuickControls2 OpenGL Test
    HINTS ${_ucd_qt_hints})
qt_standard_project_setup(REQUIRES 6.5)
set(_ucd_deploy_qmldir "${CMAKE_SOURCE_DIR}/qml")

# --- uconnect ----------------------------------------------------------------
include(FetchContent)
include(cmake/ScreenDependencies.cmake)
set(UCONNECT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# The rendezvous server, uconnect-rendezvous, ships in the packages so anyone
# can run their own. On Linux it is linked statically: one binary for any distro.
option(UCD_BUILD_SERVER "Build the uConnect rendezvous server (uconnect-rendezvous) too" ON)
set(UCONNECT_BUILD_SERVER ${UCD_BUILD_SERVER} CACHE BOOL "" FORCE)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(UCONNECT_STATIC_SERVER ${UCD_BUILD_SERVER} CACHE BOOL "" FORCE)
endif ()
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
if (TARGET uconnect-rendezvous)
    # Next to the app, with its example configuration and systemd unit, where
    # the packaging scripts pick them up.
    set_target_properties(uconnect-rendezvous PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    configure_file("${uconnect_SOURCE_DIR}/server/rendezvous.example.yaml" "${CMAKE_BINARY_DIR}/rendezvous.example.yaml" COPYONLY)
    configure_file("${uconnect_SOURCE_DIR}/deploy/uconnect-rendezvous.service" "${CMAKE_BINARY_DIR}/uconnect-rendezvous.service" COPYONLY)
endif ()

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
    src/core/FsUtil.cpp
    src/core/FsUtil.hpp
    src/core/Hello.cpp
    src/core/Hello.hpp
    src/core/Identity.cpp
    src/core/Identity.hpp
    src/core/OffThread.hpp
    src/core/Permissions.cpp
    src/core/Permissions.hpp
    src/core/Session.cpp
    src/core/Session.hpp
    src/core/screen/DatagramChannel.cpp
    src/core/screen/DatagramChannel.hpp
    src/core/screen/MediaTransport.cpp
    src/core/screen/MediaTransport.hpp
    src/core/StorageService.cpp
    src/core/StorageService.hpp
    src/core/sync/SyncFolder.cpp
    src/core/sync/SyncFolder.hpp
    src/core/sync/SyncIgnore.cpp
    src/core/sync/SyncIgnore.hpp
    src/core/sync/SyncModel.cpp
    src/core/sync/SyncModel.hpp
    src/core/sync/SyncScanner.cpp
    src/core/sync/SyncScanner.hpp
    src/core/sync/SyncService.cpp
    src/core/sync/SyncService.hpp
)
target_include_directories(uconnect_desktop_core PUBLIC src)
target_link_libraries(uconnect_desktop_core PUBLIC Qt6::Core Qt6::Network uconnect PRIVATE monocypher)
if (UCD_SCREEN_AVAILABLE)
    target_sources(uconnect_desktop_core PRIVATE
        src/core/screen/Codec.cpp src/core/screen/Codec.hpp
        src/core/screen/ScreenService.cpp src/core/screen/ScreenService.hpp
        src/core/screen/DesktopEncoder.cpp src/core/screen/DesktopEncoder.hpp)
    target_sources(uconnect_desktop_core PRIVATE src/core/screen/Input.cpp src/core/screen/Input.hpp src/core/screen/NativeInput.cpp)
    target_sources(uconnect_desktop_core PRIVATE src/core/screen/Cursor.cpp src/core/screen/Cursor.hpp)
    # Extended display: the handshake and virtual monitors.
    target_sources(uconnect_desktop_core PRIVATE
        src/core/display/DisplayService.cpp src/core/display/DisplayService.hpp
        src/core/display/VirtualDisplay.cpp src/core/display/VirtualDisplay.hpp
        src/core/display/DriverSetup.hpp)
    target_compile_definitions(uconnect_desktop_core PUBLIC UCD_HAS_SCREEN)
    target_link_libraries(uconnect_desktop_core PUBLIC Qt6::Multimedia ucd_ffmpeg_shared)
    if (WIN32)
        target_sources(uconnect_desktop_core PRIVATE src/core/screen/DesktopEncoder_win.cpp)
        target_sources(uconnect_desktop_core PRIVATE src/core/display/VirtualDisplay_win.cpp
            src/core/display/DriverSetup_win.cpp src/core/display/uconnect-vdd.h)
        target_link_libraries(uconnect_desktop_core PRIVATE d3d11 dxgi setupapi newdev)
    elseif (APPLE)
        enable_language(OBJCXX)
        set_source_files_properties(src/core/screen/DesktopEncoder_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_sources(uconnect_desktop_core PRIVATE src/core/screen/DesktopEncoder_mac.mm)
        set_source_files_properties(src/core/display/VirtualDisplay_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_sources(uconnect_desktop_core PRIVATE src/core/display/VirtualDisplay_mac.mm)
        target_link_libraries(uconnect_desktop_core PRIVATE "-framework Foundation" "-framework CoreGraphics")
        target_link_libraries(uconnect_desktop_core PRIVATE "-framework ScreenCaptureKit" "-framework CoreMedia" "-framework CoreVideo")
        target_link_libraries(uconnect_desktop_core PRIVATE "-framework ApplicationServices")
    elseif (UNIX)
        find_package(X11 QUIET)
        if (TARGET X11::Xtst)
            target_compile_definitions(uconnect_desktop_core PRIVATE UCD_HAS_XTEST)
            target_link_libraries(uconnect_desktop_core PRIVATE X11::X11 X11::Xtst)
        endif ()
    endif ()
endif ()
if (NOT UCD_SCREEN_AVAILABLE)
    # Do not ask deployment tools to import Qt Multimedia from unused views.
    set(_ucd_deploy_qmldir "${CMAKE_BINARY_DIR}/deploy-qml")
    file(MAKE_DIRECTORY "${_ucd_deploy_qmldir}")
    file(GLOB _ucd_base_qml "${CMAKE_SOURCE_DIR}/qml/*.qml")
    list(REMOVE_ITEM _ucd_base_qml "${CMAKE_SOURCE_DIR}/qml/ScreenView.qml" "${CMAKE_SOURCE_DIR}/qml/ScreenSafety.qml"
        "${CMAKE_SOURCE_DIR}/qml/DisplaySetup.qml" "${CMAKE_SOURCE_DIR}/qml/DisplaySafety.qml" "${CMAKE_SOURCE_DIR}/qml/DisplayView.qml")
    file(COPY ${_ucd_base_qml} DESTINATION "${_ucd_deploy_qmldir}")
endif ()

add_library(uconnect_desktop_screen_ui STATIC
    src/app/ScreenController.cpp src/app/ScreenController.hpp
    src/app/DisplayController.cpp src/app/DisplayController.hpp
    src/app/KeepAwake.cpp src/app/KeepAwake.hpp
    src/core/screen/StopShortcut.cpp src/core/screen/StopShortcut.hpp)
if (UCD_SCREEN_AVAILABLE)
    target_sources(uconnect_desktop_screen_ui PRIVATE src/app/InputCapture.cpp src/app/InputCapture.hpp)
    if (APPLE)
        enable_language(OBJCXX)
        target_sources(uconnect_desktop_screen_ui PRIVATE src/core/screen/NativeCursor_mac.mm)
        target_link_libraries(uconnect_desktop_screen_ui PRIVATE "-framework AppKit")
    else ()
        target_sources(uconnect_desktop_screen_ui PRIVATE src/core/screen/NativeCursor.cpp)
    endif ()
endif ()
target_link_libraries(uconnect_desktop_screen_ui PUBLIC uconnect_desktop_core Qt6::Gui)
if (APPLE)
    target_link_libraries(uconnect_desktop_screen_ui PRIVATE "-framework Carbon" "-framework IOKit")
elseif (UNIX)
    # Keeps a screen showing an extended display awake (DisplayController).
    find_package(Qt6 QUIET COMPONENTS DBus HINTS ${_ucd_qt_hints})
    if (TARGET Qt6::DBus)
        target_compile_definitions(uconnect_desktop_screen_ui PRIVATE UCD_HAS_DBUS)
        target_link_libraries(uconnect_desktop_screen_ui PRIVATE Qt6::DBus)
    endif ()
    find_package(X11 QUIET)
    if (X11_FOUND)
        target_compile_definitions(uconnect_desktop_screen_ui PRIVATE UCD_HAS_X11)
        target_link_libraries(uconnect_desktop_screen_ui PRIVATE X11::X11)
        if (TARGET X11::Xfixes)
            target_compile_definitions(uconnect_desktop_screen_ui PRIVATE UCD_HAS_XFIXES)
            target_link_libraries(uconnect_desktop_screen_ui PRIVATE X11::Xfixes)
        endif ()
    endif ()
endif ()
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
    src/app/LoginItem.cpp
    src/app/LoginItem.hpp
    src/app/SingleInstance.cpp
    src/app/SingleInstance.hpp
    src/app/TrayIcon.cpp
    src/app/TrayIcon.hpp
    src/app/QrImage.cpp
    src/app/QrImage.hpp
    src/third_party/qrcodegen/qrcodegen.cpp
    src/third_party/qrcodegen/qrcodegen.hpp
)

if (QT_KNOWN_POLICY_QTP0004)
    qt_policy(SET QTP0004 NEW)
endif ()
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
if (UCD_SCREEN_AVAILABLE)
    set(_ucd_screen_qml qml/ScreenView.qml qml/ScreenSafety.qml qml/DisplaySetup.qml qml/DisplaySafety.qml qml/DisplayView.qml)
endif ()
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
        qml/PromptDialog.qml
        qml/StorageActivityWindow.qml
        qml/PreviewPopup.qml
        qml/SyncWindow.qml
        qml/TransfersPopup.qml
        ${_ucd_screen_qml}
        # controls
        qml/UButton.qml
        qml/UToolButton.qml
        qml/UTextField.qml
        qml/USpinBox.qml
        qml/UComboBox.qml
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

if (APPLE)
    target_sources(uconnect_desktop PRIVATE src/app/DockReopen_mac.mm)
    target_link_libraries(uconnect_desktop PRIVATE "-framework AppKit")
endif ()
target_link_libraries(uconnect_desktop PRIVATE
    uconnect_desktop_core
    uconnect_desktop_screen_ui
    Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::QuickControls2)
ucd_deploy_screen(uconnect_desktop)

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
# The app icon (packaging/icons, drawn by scripts/make-icons.py): the window
# icon at runtime, the exe's icon on Windows, the bundle's on macOS.
qt_add_resources(uconnect_desktop "app_icons" PREFIX "/icons" BASE "packaging/icons" FILES
    packaging/icons/uconnect-16.png packaging/icons/uconnect-24.png packaging/icons/uconnect-32.png
    packaging/icons/uconnect-48.png packaging/icons/uconnect-64.png packaging/icons/uconnect-128.png
    packaging/icons/uconnect-256.png
    packaging/icons/tray-black-16.png packaging/icons/tray-black-20.png packaging/icons/tray-black-24.png
    packaging/icons/tray-black-32.png packaging/icons/tray-black-48.png packaging/icons/tray-black-64.png
    packaging/icons/tray-white-16.png packaging/icons/tray-white-20.png packaging/icons/tray-white-24.png
    packaging/icons/tray-white-32.png packaging/icons/tray-white-48.png packaging/icons/tray-white-64.png)
if (WIN32)
    enable_language(RC)
    set(UCD_ICON_FILE "${CMAKE_SOURCE_DIR}/packaging/icons/uconnect.ico")
    configure_file(packaging/icons/uconnect.rc.in "${CMAKE_BINARY_DIR}/uconnect.rc" @ONLY)
    target_sources(uconnect_desktop PRIVATE "${CMAKE_BINARY_DIR}/uconnect.rc")
    # Rebuilt when the icon is redrawn.
    set_source_files_properties("${CMAKE_BINARY_DIR}/uconnect.rc" PROPERTIES OBJECT_DEPENDS "${UCD_ICON_FILE}")
elseif (APPLE)
    set_source_files_properties(packaging/icons/uconnect.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(uconnect_desktop PRIVATE packaging/icons/uconnect.icns)
    set_target_properties(uconnect_desktop PROPERTIES MACOSX_BUNDLE_ICON_FILE uconnect.icns)
endif ()
if (APPLE)
    set_target_properties(uconnect_desktop PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.uconnect.desktop"
        MACOSX_BUNDLE_BUNDLE_NAME "uConnect"
        # uConnect.app: the Dock and Finder name an app by its bundle's name.
        OUTPUT_NAME "uConnect"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/Info.plist.in"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
    find_program(UCD_MACDEPLOYQT macdeployqt
        HINTS "${QT6_INSTALL_PREFIX}/bin" "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}"
        NO_DEFAULT_PATH REQUIRED)
    add_custom_command(TARGET uconnect_desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:uconnect_desktop>/Contents/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/docs/third-party-screen.md"
                "${CMAKE_SOURCE_DIR}/docs/OpenH264-BINARY-LICENSE.txt"
                "${CMAKE_SOURCE_DIR}/docs/licenses/LGPL-2.1.txt"
                "${CMAKE_SOURCE_DIR}/docs/licenses/GPL-3.0.txt"
                "$<TARGET_BUNDLE_DIR:uconnect_desktop>/Contents/Resources"
        COMMAND "${UCD_MACDEPLOYQT}" "$<TARGET_BUNDLE_DIR:uconnect_desktop>"
                "-qmldir=${_ucd_deploy_qmldir}" -always-overwrite -codesign=-
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
                --qmldir "${_ucd_deploy_qmldir}" "$<TARGET_FILE:uconnect_desktop>"
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
        add_test(NAME ${name} COMMAND ${CMAKE_COMMAND}
            "-DUCD_TEST_EXE=$<TARGET_FILE:${name}>" "-DUCD_TEST_LOG=${CMAKE_BINARY_DIR}/${name}.txt"
            -P "${CMAKE_SOURCE_DIR}/cmake/RunTest.cmake")
        set_tests_properties(${name} PROPERTIES TIMEOUT 120)
        ucd_deploy_screen(${name})
        if (WIN32)
            set_tests_properties(${name} PROPERTIES ENVIRONMENT_MODIFICATION "${_ucd_test_path}")
        endif ()
    endfunction()

    ucd_add_test(IdentityTest)
    ucd_add_test(ScreenFoundationTest)
    ucd_add_test(ScreenTransportTest)
    if (NOT TARGET uconnect_rendezvous)  # uconnect adds it itself when building the server
        add_subdirectory(${uconnect_SOURCE_DIR}/server ${uconnect_BINARY_DIR}/server)
    endif ()
    target_link_libraries(ScreenFoundationTest PRIVATE uconnect_rendezvous)
    if (UCD_SCREEN_AVAILABLE)
        set(UCD_TEST_OPENH264 "" CACHE FILEPATH "Separately installed Cisco library for codec and integration tests")
        ucd_add_test(ScreenCodecTest)
        ucd_add_test(ScreenSessionTest)
        ucd_add_test(ScreenCaptureTest)
        ucd_add_test(ScreenInputTest)
        ucd_add_test(ScreenSpreadTest)
        ucd_add_test(DisplayServiceTest)
        ucd_add_test(VirtualDisplayTest)  # opt in: UCD_TEST_VIRTUAL_DISPLAY=1
        ucd_add_test(KeepAwakeTest)
        target_link_libraries(KeepAwakeTest PRIVATE uconnect_desktop_screen_ui)
        if (TARGET Qt6::DBus AND UNIX AND NOT APPLE)  # as for the screen library
            target_compile_definitions(KeepAwakeTest PRIVATE UCD_HAS_DBUS)
            target_link_libraries(KeepAwakeTest PRIVATE Qt6::DBus)
        endif ()
        target_link_libraries(ScreenInputTest PRIVATE uconnect_desktop_screen_ui)
        ucd_add_test(NativeCursorTest)
        target_link_libraries(NativeCursorTest PRIVATE uconnect_desktop_screen_ui Qt6::Gui)
        target_link_libraries(ScreenSessionTest PRIVATE uconnect_rendezvous)
        set_tests_properties(ScreenCodecTest ScreenSessionTest ScreenCaptureTest ScreenSpreadTest PROPERTIES
            ENVIRONMENT "UCD_TEST_OPENH264=${UCD_TEST_OPENH264}")
        if (WIN32)
            set_property(TEST ScreenCaptureTest APPEND PROPERTY ENVIRONMENT "QT_PLUGIN_PATH=${QT6_INSTALL_PREFIX}/plugins")
        endif ()
    endif ()
    ucd_add_test(StorageServiceTest)
    ucd_add_test(SyncLogicTest)
    ucd_add_test(SyncServiceTest)

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
        src/app/TransferListModel.cpp src/app/TransferListModel.hpp
        src/app/TrayIcon.cpp src/app/TrayIcon.hpp
        src/app/LoginItem.cpp src/app/LoginItem.hpp)
    if (APPLE)
        target_sources(SettingsTest PRIVATE src/app/DockReopen_mac.mm)
        target_link_libraries(SettingsTest PRIVATE "-framework AppKit")
    endif ()
    target_link_libraries(SettingsTest PRIVATE Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::QuickControls2)
    target_link_libraries(SettingsTest PRIVATE uconnect_desktop_screen_ui)
    target_compile_definitions(SettingsTest PRIVATE UCD_QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    get_property(_ucd_gui_test_environment TEST StorageViewTest PROPERTY ENVIRONMENT)
    set_tests_properties(SettingsTest PROPERTIES ENVIRONMENT "${_ucd_gui_test_environment}")
endif ()
