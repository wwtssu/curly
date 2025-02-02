#!/bin/bash
#Based on https://github.com/sinofool/build-libcurl-ios

VERSION="7.47.0"
CURRENTPATH=`pwd`

export DEVROOT=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain
DFT_DIST_DIR=${CURRENTPATH}
DIST_DIR=${DIST_DIR:-$DFT_DIST_DIR}

rm -rf ${CURRENTPATH}/include
rm -rf ${CURRENTPATH}/lib

curl -O -L http://curl.haxx.se/download/curl-${VERSION}.tar.gz
tar xf curl-${VERSION}.tar.gz
cd curl-${VERSION}

function check_curl_ver() {
echo "#include \"include/curl/curlver.h\"
#if LIBCURL_VERSION_MAJOR < 7 || LIBCURL_VERSION_MINOR < 40
#error Required curl 7.40.0+; See http://curl.haxx.se/docs/adv_20150108A.html
#endif"|gcc -c -o /dev/null -xc -||exit 9
}

function build_for_arch() {
  ARCH=$1
  HOST=$2
  SYSROOT=$3
  PREFIX=$4
  IPHONEOS_DEPLOYMENT_TARGET="6.0"
  export PATH="${DEVROOT}/usr/bin/:${PATH}"
  #For now, turn off bitcode and change -Os to -Og
  #export CFLAGS="-arch ${ARCH} -pipe -Os -gdwarf-2 -isysroot ${SYSROOT} -miphoneos-version-min=${IPHONEOS_DEPLOYMENT_TARGET} -fembed-bitcode"
  export CFLAGS="-arch ${ARCH} -pipe -O0 -gdwarf-2 -isysroot ${SYSROOT} -miphoneos-version-min=${IPHONEOS_DEPLOYMENT_TARGET}"
  export LDFLAGS="-arch ${ARCH} -isysroot ${SYSROOT}"
  ./configure --disable-shared --enable-static ${SSL_FLAG} --host="${HOST}" --prefix=${PREFIX} && make -j8 && make install
}

if [ "$1" == "openssl" ]
then
  if [ ! -d ${HOME}/Desktop/openssl-ios-dist ]
  then
    echo "Please use https://github.com/sinofool/build-openssl-ios/ to build OpenSSL for iOS first"
    exit 8
  fi
  export SSL_FLAG=--with-ssl=${HOME}/Desktop/openssl-ios-dist
else
  check_curl_ver
  export SSL_FLAG=--with-darwinssl
fi

TMP_DIR=/tmp/build_libcurl_$$

build_for_arch i386 i386-apple-darwin /Applications/Xcode.app/Contents/Developer/Platforms/iPhoneSimulator.platform/Developer/SDKs/iPhoneSimulator.sdk ${TMP_DIR}/i386 || exit 1
build_for_arch x86_64 x86_64-apple-darwin /Applications/Xcode.app/Contents/Developer/Platforms/iPhoneSimulator.platform/Developer/SDKs/iPhoneSimulator.sdk ${TMP_DIR}/x86_64 || exit 2
build_for_arch arm64 arm-apple-darwin /Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk ${TMP_DIR}/arm64 || exit 3
build_for_arch armv7s armv7s-apple-darwin /Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk ${TMP_DIR}/armv7s || exit 4
build_for_arch armv7 armv7-apple-darwin /Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk ${TMP_DIR}/armv7 || exit 5

mkdir -p ${TMP_DIR}/lib/
${DEVROOT}/usr/bin/lipo \
	-arch i386 ${TMP_DIR}/i386/lib/libcurl.a \
	-arch x86_64 ${TMP_DIR}/x86_64/lib/libcurl.a \
	-arch armv7 ${TMP_DIR}/armv7/lib/libcurl.a \
	-arch armv7s ${TMP_DIR}/armv7s/lib/libcurl.a \
	-arch arm64 ${TMP_DIR}/arm64/lib/libcurl.a \
	-output ${TMP_DIR}/lib/libcurl.a -create

cp -r ${TMP_DIR}/armv7s/include ${TMP_DIR}/
curl -O https://raw.githubusercontent.com/sinofool/build-libcurl-ios/master/patch-include.patch
patch ${TMP_DIR}/include/curl/curlbuild.h < patch-include.patch

mkdir -p ${DIST_DIR}
cp -r ${TMP_DIR}/include ${TMP_DIR}/lib ${DIST_DIR}

cd ${CURRENTPATH}
rm curl-${VERSION}.tar.gz
rm -rf curl-${VERSION}
