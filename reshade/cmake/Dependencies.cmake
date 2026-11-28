include(FetchContent)

set(NATIVE_MOTION_DOWNLOAD_CACHE "" CACHE PATH "Directory with pre-downloaded, checksum-verified dependencies")
set(native_motion_downloads "${CMAKE_CURRENT_BINARY_DIR}/_deps/downloads")

function(native_motion_fetch_file relative_path url sha256)
    set(destination "${native_motion_downloads}/${relative_path}")
    get_filename_component(directory "${destination}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")

    if(EXISTS "${destination}")
        file(SHA256 "${destination}" digest)
        if(digest STREQUAL sha256)
            return()
        endif()
    endif()

    if(NATIVE_MOTION_DOWNLOAD_CACHE)
        set(cached_file "${NATIVE_MOTION_DOWNLOAD_CACHE}/${relative_path}")
        if(NOT EXISTS "${cached_file}")
            message(FATAL_ERROR "Dependency download cache is missing ${relative_path}.")
        endif()
        file(SHA256 "${cached_file}" digest)
        if(NOT digest STREQUAL sha256)
            message(FATAL_ERROR "Checksum mismatch for cached dependency ${relative_path}.")
        endif()
        file(COPY_FILE "${cached_file}" "${destination}")
    else()
        file(DOWNLOAD "${url}" "${destination}"
            EXPECTED_HASH "SHA256=${sha256}" TLS_VERIFY ON TIMEOUT 120 INACTIVITY_TIMEOUT 30
        )
    endif()
endfunction()

# ReShade 6.8.0 and MinHook 1.3.4, pinned to the release commits.
set(reshade_sha256 582184d6b278ed670a7b6811b5cb99541e3cf1de996998b8b9fed0de9991ef76)
set(minhook_sha256 31757833475be039c16359d4e276e718db6ba2ba61a6b870053a112106015822)
native_motion_fetch_file(reshade.tar.gz
    https://codeload.github.com/crosire/reshade/tar.gz/18deaa52de0c425a78b329e9cb3c497281cd00ec
    "${reshade_sha256}"
)
native_motion_fetch_file(minhook.tar.gz
    https://codeload.github.com/TsudaKageyu/minhook/tar.gz/c3fcafdc10146beb5919319d0683e44e3c30d537
    "${minhook_sha256}"
)

FetchContent_Declare(reshade
    URL "${native_motion_downloads}/reshade.tar.gz"
    URL_HASH "SHA256=${reshade_sha256}"
    SOURCE_SUBDIR include
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(minhook
    URL "${native_motion_downloads}/minhook.tar.gz"
    URL_HASH "SHA256=${minhook_sha256}"
    SOURCE_SUBDIR include
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(reshade minhook)

# Public API headers from AMD FidelityFX SDK 1.1.4. The game supplies the DLL.
set(fidelityfx_headers https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/ffx-api/include/ffx_api)
native_motion_fetch_file(fidelityfx/ffx_api/ffx_api.h
    "${fidelityfx_headers}/ffx_api.h"
    408b4203545e8299ed188fbb1d9ddfe0932b73ea6e8b14fe10edcf1f45771561
)
native_motion_fetch_file(fidelityfx/ffx_api/ffx_api_types.h
    "${fidelityfx_headers}/ffx_api_types.h"
    dee2782813d22047c068e0fb27d1347324f8ce79d280a4672848b3b294cb12fe
)
native_motion_fetch_file(fidelityfx/ffx_api/ffx_upscale.h
    "${fidelityfx_headers}/ffx_upscale.h"
    1155bcdcea9c10e6a95507bc9422ca062a853ac84250f0b09a637a66836b3ac1
)
native_motion_fetch_file(fidelityfx/ffx_api/dx12/ffx_api_dx12.h
    "${fidelityfx_headers}/dx12/ffx_api_dx12.h"
    215804fba316acb81d2c463b6bbce36c53cc63465c15b8c9858554c8111fae24
)
native_motion_fetch_file(fidelityfx/LICENSE.txt
    https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/LICENSE.txt
    82cf74fc23885107c7f69249ac3f1dac55898ff68320eb2c753984cae6b2d78a
)
set(native_motion_fidelityfx_include "${native_motion_downloads}/fidelityfx")
