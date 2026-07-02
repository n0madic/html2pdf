cmake_minimum_required(VERSION 3.16)
project(html2pdf LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(FetchContent)

# Non-system dependencies are linked statically: litehtml plus its bundled
# gumbo are built as static archives and compiled into the executable.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(litehtml
    GIT_REPOSITORY https://github.com/litehtml/litehtml.git
    GIT_TAG        v0.10)
set(LITEHTML_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(litehtml)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
pkg_check_modules(CURL  REQUIRED IMPORTED_TARGET libcurl)

# Text backend selection:
#   auto  (default) - use Pango if pangocairo is found, otherwise the toy backend
#   pango           - require Pango (fatal error if pangocairo is missing)
#   toy             - always use the Cairo toy text API
set(HTML2PDF_TEXT_BACKEND "auto" CACHE STRING "Text backend: auto, pango or toy")
set_property(CACHE HTML2PDF_TEXT_BACKEND PROPERTY STRINGS auto pango toy)

# Dependency linkage mode:
#   mixed  (default) - link each external dependency statically when a static
#                      archive (.a) is available, dynamically otherwise. System
#                      libraries without a .a (libc, libz, ...) stay dynamic.
#   shared           - link all external dependencies dynamically.
#   static           - link everything statically (-static). Requires a full
#                      static toolchain (e.g. Linux/musl); fails on macOS.
set(HTML2PDF_DEP_LINKAGE "mixed" CACHE STRING "Dependency linkage: mixed, shared or static")
set_property(CACHE HTML2PDF_DEP_LINKAGE PROPERTY STRINGS mixed shared static)
# Back-compat: HTML2PDF_STATIC_DEPS=ON selects the fully-static mode.
if(HTML2PDF_STATIC_DEPS)
    set(HTML2PDF_DEP_LINKAGE "static")
endif()

# Rewrite a pkg-config --static link line for "mixed" linkage: every -lfoo for
# which a static archive exists in the search dirs is replaced by the absolute
# path to that .a (forcing static link), while system libs without a .a,
# frameworks and linker flags pass through unchanged (staying dynamic).
# Duplicates are collapsed while preserving -framework pairing.
function(h2pdf_mixed_links in_ldflags out_var)
    set(_dirs "")
    foreach(tok IN LISTS in_ldflags)
        if(tok MATCHES "^-L(.+)$")
            list(APPEND _dirs "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    list(APPEND _dirs /opt/homebrew/lib /usr/local/lib /usr/lib /lib)
    list(REMOVE_DUPLICATES _dirs)
    # A library is linked statically only when its .a sits in a directory the
    # dependency's own pkg-config advertises (or a standard system dir). This
    # deliberately leaves keg-only libs whose static deps are not declared
    # (e.g. Homebrew libcurl, whose .pc omits brotli/openssl) linked dynamically.

    set(_out "")
    set(_seen "")
    set(_fw FALSE)
    foreach(tok IN LISTS in_ldflags)
        if(_fw)
            set(_fw FALSE)
            if(NOT "fw:${tok}" IN_LIST _seen)
                # Single token so CMake/clang forward the pair to ld intact.
                list(APPEND _out "-Wl,-framework,${tok}")
                list(APPEND _seen "fw:${tok}")
            endif()
        elseif(tok STREQUAL "-framework")
            set(_fw TRUE)
        elseif(tok MATCHES "^-L(.+)$")
            # Drop -L: resolved archives use absolute paths and dynamic -l fall
            # back to the linker's default search (which includes these dirs via
            # the imported targets' settings) -- but keep them for safety.
            if(NOT "${tok}" IN_LIST _seen)
                list(APPEND _out "${tok}")
                list(APPEND _seen "${tok}")
            endif()
        elseif(tok MATCHES "^-l(.+)$")
            set(_name "${CMAKE_MATCH_1}")
            set(_a "")
            if(NOT _name IN_LIST H2PDF_MIXED_FORCE_DYNAMIC_LIBS)
                foreach(d IN LISTS _dirs)
                    if(EXISTS "${d}/lib${_name}.a")
                        set(_a "${d}/lib${_name}.a")
                        break()
                    endif()
                endforeach()
            endif()
            if(_a)
                if(NOT "${_a}" IN_LIST _seen)
                    list(APPEND _out "${_a}")
                    list(APPEND _seen "${_a}")
                endif()
            else()
                if(NOT "-l${_name}" IN_LIST _seen)
                    list(APPEND _out "-l${_name}")
                    list(APPEND _seen "-l${_name}")
                endif()
            endif()
        elseif(tok STREQUAL "")
            # skip empty tokens
        else()
            if(NOT "${tok}" IN_LIST _seen)
                list(APPEND _out "${tok}")
                list(APPEND _seen "${tok}")
            endif()
        endif()
    endforeach()
    set(${out_var} "${_out}" PARENT_SCOPE)
endfunction()

set(HTML2PDF_USE_PANGO OFF)
if(NOT HTML2PDF_TEXT_BACKEND STREQUAL "toy")
    pkg_check_modules(PANGOCAIRO IMPORTED_TARGET pangocairo)
    if(PANGOCAIRO_FOUND)
        set(HTML2PDF_USE_PANGO ON)
        pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
        # zlib: WOFF1 web fonts store each SFNT table DEFLATE-compressed.
        pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)
    elseif(HTML2PDF_TEXT_BACKEND STREQUAL "pango")
        message(FATAL_ERROR
            "HTML2PDF_TEXT_BACKEND=pango but 'pangocairo' was not found via pkg-config")
    endif()
endif()

if(HTML2PDF_USE_PANGO)
    FetchContent_Declare(woff2
        GIT_REPOSITORY https://github.com/google/woff2.git
        GIT_TAG        1bccf208bca986e53a647dfe4811322adb06ecf8) # v1.0.2
    if(HTML2PDF_DEP_LINKAGE STREQUAL "static")
        find_library(H2PDF_BROTLIDEC_STATIC NAMES libbrotlidec.a REQUIRED)
        find_library(H2PDF_BROTLIENC_STATIC NAMES libbrotlienc.a REQUIRED)
        find_library(H2PDF_BROTLICOMMON_STATIC NAMES libbrotlicommon.a REQUIRED)
        set(BROTLIDEC_LIBRARIES
            "${H2PDF_BROTLIDEC_STATIC};${H2PDF_BROTLICOMMON_STATIC}"
            CACHE STRING "Static Brotli decoder libraries for google/woff2" FORCE)
        set(BROTLIENC_LIBRARIES
            "${H2PDF_BROTLIENC_STATIC};${H2PDF_BROTLICOMMON_STATIC}"
            CACHE STRING "Static Brotli encoder libraries for google/woff2" FORCE)
    endif()
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    FetchContent_MakeAvailable(woff2)
    foreach(extra_woff2_target
            woff2enc woff2_decompress woff2_compress woff2_info
            convert_woff2ttf_fuzzer convert_woff2ttf_fuzzer_new_entry)
        if(TARGET ${extra_woff2_target})
            set_target_properties(${extra_woff2_target} PROPERTIES EXCLUDE_FROM_ALL TRUE)
        endif()
    endforeach()
endif()

# Reference cairo container shipped inside the litehtml source tree.
set(CONTAINER_SOURCES
    ${litehtml_SOURCE_DIR}/containers/cairo/container_cairo.cpp
    ${litehtml_SOURCE_DIR}/containers/cairo/cairo_borders.cpp
    ${litehtml_SOURCE_DIR}/containers/cairo/conic_gradient.cpp)
if(HTML2PDF_USE_PANGO)
    # Pango text backend (proper shaping, BiDi, font fallback).
    list(APPEND CONTAINER_SOURCES
        ${litehtml_SOURCE_DIR}/containers/cairo/container_cairo_pango.cpp)
endif()

add_executable(html2pdf
    src/main.cpp
    src/cli.cpp
    src/resource_loader.cpp
    src/web_font_manager.cpp
    src/html_container.cpp
    src/renderer.cpp
    ${CONTAINER_SOURCES})

target_include_directories(html2pdf PRIVATE
    src
    third_party
    ${litehtml_SOURCE_DIR}/containers/cairo)

target_link_libraries(html2pdf PRIVATE litehtml)

if(HTML2PDF_USE_PANGO)
    target_compile_definitions(html2pdf PRIVATE HTML2PDF_USE_PANGO)
    target_include_directories(html2pdf PRIVATE ${woff2_SOURCE_DIR}/include)
    target_link_libraries(html2pdf PRIVATE woff2dec)
endif()

if(HTML2PDF_DEP_LINKAGE STREQUAL "shared")
    # All external dependencies dynamic.
    target_link_libraries(html2pdf PRIVATE PkgConfig::CAIRO PkgConfig::CURL)
    if(HTML2PDF_USE_PANGO)
        target_link_libraries(html2pdf PRIVATE
            PkgConfig::PANGOCAIRO PkgConfig::FONTCONFIG PkgConfig::ZLIB)
    endif()

elseif(HTML2PDF_DEP_LINKAGE STREQUAL "static")
    # Everything static. pkg-config's *_STATIC_* carries the full transitive
    # list; --start-group resolves circular references (glib/intl, harfbuzz/ft).
    target_include_directories(html2pdf PRIVATE
        ${CAIRO_STATIC_INCLUDE_DIRS} ${CURL_STATIC_INCLUDE_DIRS})
    target_link_directories(html2pdf PRIVATE
        ${CAIRO_STATIC_LIBRARY_DIRS} ${CURL_STATIC_LIBRARY_DIRS})
    set(STATIC_DEP_LIBS ${CAIRO_STATIC_LIBRARIES} ${CURL_STATIC_LIBRARIES})
    if(HTML2PDF_USE_PANGO)
        target_include_directories(html2pdf PRIVATE
            ${PANGOCAIRO_STATIC_INCLUDE_DIRS} ${FONTCONFIG_STATIC_INCLUDE_DIRS}
            ${ZLIB_STATIC_INCLUDE_DIRS})
        target_link_directories(html2pdf PRIVATE
            ${PANGOCAIRO_STATIC_LIBRARY_DIRS} ${FONTCONFIG_STATIC_LIBRARY_DIRS}
            ${ZLIB_STATIC_LIBRARY_DIRS})
        list(APPEND STATIC_DEP_LIBS
            ${PANGOCAIRO_STATIC_LIBRARIES} ${FONTCONFIG_STATIC_LIBRARIES}
            ${ZLIB_STATIC_LIBRARIES})
    endif()
    list(REMOVE_DUPLICATES STATIC_DEP_LIBS)
    target_link_options(html2pdf PRIVATE -static)
    target_link_libraries(html2pdf PRIVATE
        -Wl,--start-group ${STATIC_DEP_LIBS} -Wl,--end-group)

else()  # mixed (default): static where a .a exists, dynamic otherwise
    target_include_directories(html2pdf PRIVATE
        ${CAIRO_INCLUDE_DIRS} ${CURL_INCLUDE_DIRS})
    set(MIXED_LDFLAGS ${CAIRO_STATIC_LDFLAGS} ${CURL_STATIC_LDFLAGS})
    if(HTML2PDF_USE_PANGO)
        # Fontconfig has process-global initialization/config state. In mixed
        # builds, dynamic Cairo/Pango can also load libfontconfig.dylib/so; do
        # not embed a second static copy or FcInit() and app fonts affect the
        # wrong instance.
        list(APPEND H2PDF_MIXED_FORCE_DYNAMIC_LIBS fontconfig)
        target_include_directories(html2pdf PRIVATE
            ${PANGOCAIRO_INCLUDE_DIRS} ${FONTCONFIG_INCLUDE_DIRS} ${ZLIB_INCLUDE_DIRS})
        list(APPEND MIXED_LDFLAGS
            ${PANGOCAIRO_STATIC_LDFLAGS} ${FONTCONFIG_STATIC_LDFLAGS} ${ZLIB_STATIC_LDFLAGS})
    endif()
    h2pdf_mixed_links("${MIXED_LDFLAGS}" MIXED_LINKS)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # GNU ld: group resolves circular references among the static archives.
        target_link_libraries(html2pdf PRIVATE
            -Wl,--start-group ${MIXED_LINKS} -Wl,--end-group)
    else()
        # Apple ld resolves archive members without grouping (and rejects it).
        target_link_libraries(html2pdf PRIVATE ${MIXED_LINKS})
    endif()
endif()

if(HTML2PDF_USE_PANGO)
    message(STATUS "html2pdf text backend: Pango ${PANGOCAIRO_VERSION}")
else()
    message(STATUS "html2pdf text backend: Cairo toy text API")
endif()
message(STATUS "html2pdf dependency linkage: ${HTML2PDF_DEP_LINKAGE}")
