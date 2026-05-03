include(ExternalProject)

ExternalProject_Add(jpeg
    GIT_REPOSITORY https://github.com/libjpeg-turbo/libjpeg-turbo.git
    GIT_TAG        3.1.4
    GIT_SHALLOW    TRUE

    CMAKE_ARGS
        ${ANDROID_CMAKE_ARGS}
        -DCMAKE_INSTALL_PREFIX:PATH=${DEPS_INSTALL_DIR}
        -DCMAKE_INSTALL_LIBDIR:STRING=lib
        -DENABLE_SHARED:BOOL=OFF
        -DWITH_TURBOJPEG:BOOL=OFF
        -DENABLE_TESTING:BOOL=OFF
        -DWITH_TOOLS:BOOL=OFF

)
