CMAKE_MINIMUM_REQUIRED( VERSION 2.6 )

PROJECT( fcgicc CXX )
SET( PROJECT_VERSION 0.1.3 )
SET( CMAKE_BUILD_TYPE RELEASE )
SET( CMAKE_INSTALL_PREFIX ${PREFIX} )

FIND_PATH( FCGI_INCLUDE_DIR fastcgi.h )
IF( NOT FCGI_INCLUDE_DIR )
    FIND_PATH( FCGI_INCLUDE_DIR fastcgi.h ${PROJECT_SOURCE_DIR}/fastcgi_devkit )
ENDIF()
INCLUDE_DIRECTORIES( ${FCGI_INCLUDE_DIR} )

ADD_SUBDIRECTORY( src )
ADD_SUBDIRECTORY( test EXCLUDE_FROM_ALL )

INSTALL( FILES LICENSE.txt README.txt DESTINATION share/doc/${PROJECT_NAME} )

SET( DIST_FILE ${PROJECT_NAME}-${PROJECT_VERSION} )
ADD_CUSTOM_TARGET( dist ln -sf ${PROJECT_SOURCE_DIR} ${DIST_FILE} &&
    tar cjf ${DIST_FILE}.tar.bz2
        ${DIST_FILE}/LICENSE.txt
        ${DIST_FILE}/README.txt
        ${DIST_FILE}/CHANGES.txt
        ${DIST_FILE}/CMakeLists.txt
        ${DIST_FILE}/fastcgi_devkit/LICENSE.TERMS
        ${DIST_FILE}/fastcgi_devkit/fastcgi.h
        ${DIST_FILE}/src/fcgicc.cc
        ${DIST_FILE}/src/fcgicc.h
        ${DIST_FILE}/src/CMakeLists.txt
        ${DIST_FILE}/test/test1.cc
        ${DIST_FILE}/test/test2.cc
        ${DIST_FILE}/test/lighttpd.conf
        ${DIST_FILE}/test/CMakeLists.txt )
