# Copyright 2020 Major League Baseball
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


AC_INIT([tracemate], [], [] [https://github.mlbam.net/Infrastructure/tracemate])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_AUX_DIR([.])

node=`hostname | cut -f1 -d '.' | tr '[A-Z]' '[a-z]'`
if test -f ".configure.$node"; then
	echo "***********************************************************"
	echo "* Loading node specific configure settings for $node."
	echo "***********************************************************"
	. ./.configure.$node
fi

AC_CANONICAL_HOST
AC_CONFIG_HEADER(src/tm_config.h)

AC_PROG_CC
AC_PROG_CXX
AC_C_INLINE
AC_C_BIGENDIAN
AC_PROG_CPP
AC_PROG_RANLIB
AC_PROG_INSTALL
AC_PROG_LN_S
AC_PATH_PROG(AR, ar)
AC_PATH_PROG(NROFF, nroff)
AC_PATH_PROG(CTFCONVERT, ctfconvert)
AC_PATH_PROG(CTFMERGE, ctfmerge)
AC_SUBST(PERL)

MAPFLAGS=""
if test "x$GCC" = "xyes" ; then
	C99FLAG="-std=c11"
	CFLAGS="$CFLAGS -g $C99FLAG"
	DEPFLAGS="-MM"
else
	C99FLAG="-xc99=all"
	CFLAGS="$CFLAGS -g $C99FLAG"
	DEPFLAGS="-xM1"
fi
SHLDFLAGS="$LDFLAGS"

CPPFLAGS='-I$(top_srcdir)/src'" $CPPFLAGS -D_REENTRANT -D_XOPEN_SOURCE=600 -D_XOPEN_SOURCE_EXTENDED -D_POSIX_SOURCE -D_POSIX_C_SOURCE=200809L "
DOTSO=.so

MTEV_MODULES_DIR=`mtev-config --modules-dir 2>/dev/null`
if test "$?" -ne "0" ; then
    AC_MSG_ERROR([*** mtev-config not found ***])
fi
AC_SUBST(MTEV_MODULES_DIR)

MTEV_LIBDIR=`mtev-config --libdir 2>/dev/null`
if test "$?" -ne "0" ; then
    AC_MSG_ERROR([*** mtev-config not found ***])
fi
AC_SUBST(MTEV_LIBDIR)

MTEV_INCLUDEDIR=`mtev-config --includedir 2>/dev/null`
if test "$?" -ne "0" ; then
    AC_MSG_ERROR([*** mtev-config not found ***])
fi
AC_SUBST(MTEV_INCLUDEDIR)

case $host in
*-*-linux*)
	CAP_PLATFORM="linux"
	LDFLAGS="$LDFLAGS -Wl,-E"
	CPPFLAGS="$CPPFLAGS -D_SVID_SOURCE -D_BSD_SOURCE"
	PICFLAGS="-fpic"
	MODULELD="$CC -shared"
	SHLD="$CC -shared"
	MODULEEXT=so
	RLDFLAG="-Wl,--rpath="
	WHOLE_ARCHIVE="-Wl,--whole-archive"
	NOWHOLE_ARCHIVE="-Wl,--no-whole-archive"
	;;
*)
	PICFLAGS="-fpic"
	MODULELD="$CC -shared"
	MODULEEXT=so
	RLDFLAG="-Wl,--rpath="
	;;
esac
AC_SUBST(DOTSO)
AC_SUBST(DOTDYLIB)
AC_DEFINE_UNQUOTED(CAP_PLATFORM, "$CAP_PLATFORM", [capabilities platform])
if test "x$CAP_SUPPORTED" = "x1" ; then
	AC_DEFINE(CAP_SUPPORTED, [1], [capabilities supported])
fi
AC_SUBST(CLINKFLAGS)
AC_SUBST(SHLD)
AC_SUBST(RLDFLAG)
AC_SUBST(DEPFLAGS)
AC_SUBST(MODULELD)
AC_SUBST(MODULEEXT)
AC_SUBST(WHOLE_ARCHIVE)
AC_SUBST(NOWHOLE_ARCHIVE)
AC_SUBST(MAPFLAGS)
AC_DEFINE_UNQUOTED(MODULEEXT, "$MODULEEXT", [module extension])

# Checks for data types
AC_CHECK_SIZEOF(char, 1)
AC_CHECK_SIZEOF(short int, 2)
AC_CHECK_SIZEOF(int, 4)
AC_CHECK_SIZEOF(long int, 4)
AC_CHECK_SIZEOF(long long int, 8)
AC_CHECK_SIZEOF(void *, 1)

# Check for lua libraries
OLD_LIBS=$LIBS
LIBS=
if test "x$ENABLE_LUA" = "xLuaJIT"; then
	AC_CHECK_LIB(luajit-5.1, luaL_newstate, [], [AC_MSG_ERROR(*** libluajit-5.1 required ***)])
elif test "x$ENABLE_LUA" = "xlua52"; then
	AC_MSG_ERROR(*** liblua52 required ***)
fi
LUALIBS="$LUALIBS $LIBS"
AC_SUBST(LUALIBS)
LIBS=$OLD_LIBS

AC_CHECK_HEADER(mtev_config.h, [], [AC_MSG_ERROR(*** libmtev (headers) required ***)])
AC_CHECK_LIB(mtev, mtev_main, , [AC_MSG_ERROR(*** libmtev is required ***)])
AC_CHECK_LIB(circllhist, hist_alloc, , [AC_MSG_ERROR(*** libcircllhist is required ***)])
AC_CHECK_LIB(m, sqrt, , )
AC_CHECK_LIB(dl, dlsym, , )
AC_CHECK_LIB(ck, ck_ht_init, , [AC_MSG_ERROR(*** concurrencyckit (libck) is required ***)])
AC_CHECK_LIB(jlog, jlog_ctx_add_subscriber_copy_checkpoint, , [AC_MSG_ERROR(*** libjlog 2.0.2 or higher required github.com/omniti-labs/jlog ***)])
AC_CHECK_LIB(z, compress2, , [AC_MSG_ERROR(*** zlib is required ***)])
AC_CHECK_LIB(rt, sem_init, , [
  AC_CHECK_LIB(rt, clock_gettime, , )
])
AC_CHECK_LIB(posix4, sem_wait, , )
AC_CHECK_LIB(pthread, pthread_create, , )
AC_CHECK_HEADER(curl/curl.h, [], [AC_MSG_ERROR(*** curl headers required ***)])
AC_CHECK_LIB(curl, curl_easy_init, , [AC_MSG_ERROR(*** curl is required ***)])
AC_CHECK_LIB(rdkafka, rd_kafka_new, , [AC_MSG_ERROR(*** librdkafka is required ***)])
AC_CHECK_LIB(circmetrics, stats_add64, , [AC_MSG_ERROR(*** libcircmetrics is required ***)])
AC_CHECK_LIB(ssl, SSL_set_session, , [AC_MSG_ERROR(*** libssl is required ***)])
AC_CHECK_LIB(upb, upb_alloc_global, , [AC_MSG_ERROR(*** libupb is required ***)])

AC_CHECK_LIB(crypto, ERR_load_CRYPTO_strings, ,
	[
		AC_MSG_ERROR([libcrypto not found, but required])
	]
)

CPPFLAGS="$CPPFLAGS `pcre-config --cflags`"
LIBS="$LIBS `pcre-config --libs`"
AC_CHECK_LIB(pcre, pcre_compile, ,
	[
		AC_MSG_ERROR([libpcre not found, but required])
	]
)
AC_CHECK_LIB(circllhist, hist_alloc, ,
  [
    AC_MSG_ERROR([libcircllhist not found, but required])
  ]
)

AC_MSG_CHECKING([libxml2 cflags])
XML2CFLAGS=`xml2-config --cflags`
CFLAGS="$CFLAGS $XML2CFLAGS"
AC_MSG_RESULT([$XML2CFLAGS])

AC_MSG_CHECKING([libxml2 libs])
XML2LIBS=`xml2-config --libs`
LIBS="$LIBS $XML2LIBS"
AC_MSG_RESULT([$XML2LIBS])

AC_CHECK_FUNC(xmlParseDocument, , [AC_MSG_ERROR(*** libxml2 is required ***)])

AC_MSG_CHECKING([libxslt cflags])
XSLTCFLAGS=`xslt-config --cflags`
CFLAGS="$CFLAGS $XSLTCFLAGS"
AC_MSG_RESULT([$XSLTCFLAGS])

AC_MSG_CHECKING([libxslt libs])
XSLTLIBS=`xslt-config --libs`
LIBS="$LIBS $XSLTLIBS"
AC_MSG_RESULT([$XSLTLIBS])

AC_CHECK_FUNC(xsltParseStylesheetDoc, , [AC_MSG_ERROR(*** libxslt is required ***)])

SHCFLAGS="$PICFLAGS $CFLAGS"
SHLDFLAGS=`mtev-config --shldflags 2>/dev/null`
#SHLD=`mtev-config --shld 2>/dev/null`
MODLIBS=`mtev-config --libs 2>/dev/null`
SHCPPFLAGS="$PICFLAGS $CPPFLAGS"
AC_SUBST(SHCFLAGS)
AC_SUBST(SHCPPFLAGS)
AC_SUBST(LD)
AC_SUBST(SHLD)
AC_SUBST(SHLDFLAGS)
AC_SUBST(MODLIBS)
AC_SUBST(CXXFLAGS)

CFLAGS="$CFLAGS -Wall -Werror"
LIBS="-lstdc++ -lprotobuf -llmdb -lgrpc++ -lgrpc++_reflection -lgrpc -lgpr -lcares -laddress_sorting -labsl_strings -labsl_base -labsl_throw_delegate -labsl_str_format_internal -labsl_bad_optional_access -labsl_raw_logging_internal -labsl_int128 $LIBS -ldl"

AC_OUTPUT([
Makefile
src/Makefile
src/bloom/Makefile
src/proto/Makefile
src/modules/Makefile
])

AC_MSG_RESULT([
=== Compile-time Configuration ===

CFLAGS: $CFLAGS
CPPFLAGS: $CPPFLAGS
CXXFLAGS: $CXXFLAGS
LIBS: $LIBS

])
