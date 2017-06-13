#
# Generated Makefile - do not edit!
#
# Edit the Makefile in the project folder instead (../Makefile). Each target
# has a -pre and a -post target defined where you can add customized code.
#
# This makefile implements configuration specific macros and targets.


# Environment
MKDIR=mkdir
CP=cp
GREP=grep
NM=nm
CCADMIN=CCadmin
RANLIB=ranlib
CC=gcc
CCC=g++
CXX=g++
FC=gfortran
AS=as

# Macros
CND_PLATFORM=GNU-Linux
CND_DLIB_EXT=so
CND_CONF=Debug_x86_64
CND_DISTDIR=dist
CND_BUILDDIR=build

# Include project Makefile
include Makefile

# Object Directory
OBJECTDIR=${CND_BUILDDIR}/${CND_CONF}/${CND_PLATFORM}

# Object Files
OBJECTFILES= \
	${OBJECTDIR}/_ext/e17b0838/avltree.o \
	${OBJECTDIR}/_ext/e17b0838/base64.o \
	${OBJECTDIR}/_ext/e17b0838/crc32.o \
	${OBJECTDIR}/_ext/e17b0838/des.o \
	${OBJECTDIR}/_ext/e17b0838/logger.o \
	${OBJECTDIR}/_ext/e17b0838/md5.o \
	${OBJECTDIR}/_ext/e17b0838/object.o \
	${OBJECTDIR}/_ext/e17b0838/posix_ifos.o \
	${OBJECTDIR}/_ext/e17b0838/posix_naos.o \
	${OBJECTDIR}/_ext/e17b0838/posix_string.o \
	${OBJECTDIR}/_ext/e17b0838/posix_thread.o \
	${OBJECTDIR}/_ext/e17b0838/posix_time.o \
	${OBJECTDIR}/_ext/e17b0838/posix_wait.o \
	${OBJECTDIR}/_ext/5c0/fque.o \
	${OBJECTDIR}/_ext/5c0/io.o \
	${OBJECTDIR}/_ext/5c0/mxx.o \
	${OBJECTDIR}/_ext/5c0/ncb.o \
	${OBJECTDIR}/_ext/5c0/tcp.o \
	${OBJECTDIR}/_ext/5c0/tcpal.o \
	${OBJECTDIR}/_ext/5c0/tcpio.o \
	${OBJECTDIR}/_ext/5c0/udp.o \
	${OBJECTDIR}/_ext/5c0/udpio.o \
	${OBJECTDIR}/_ext/5c0/worker.o


# C Compiler Flags
CFLAGS=-m64 -D_POSIX_C_SOURCE=21000000L

# CC Compiler Flags
CCFLAGS=
CXXFLAGS=

# Fortran Compiler Flags
FFLAGS=

# Assembler Flags
ASFLAGS=

# Link Libraries and Options
LDLIBSOPTIONS=

# Build Targets
.build-conf: ${BUILD_SUBPROJECTS}
	"${MAKE}"  -f nbproject/Makefile-${CND_CONF}.mk /usr/local/lib64/nshost.so.8.1.1

/usr/local/lib64/nshost.so.8.1.1: ${OBJECTFILES}
	${MKDIR} -p /usr/local/lib64
	${LINK.c} -o /usr/local/lib64/nshost.so.8.1.1 ${OBJECTFILES} ${LDLIBSOPTIONS} -shared -fPIC

${OBJECTDIR}/_ext/e17b0838/avltree.o: ../../libnsp/com/avltree.c
	${MKDIR} -p ${OBJECTDIR}/_ext/e17b0838
	${RM} "$@.d"
	$(COMPILE.c) -g -Wall -I../../libnsp/icom -fPIC  -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/e17b0838/avltree.o ../../libnsp/com/avltree.c

${OBJECTDIR}/_ext/e17b0838/base64.o: ../../libnsp/com/base64.c
	${MKDIR} -p ${OBJECTDIR}/_ext/e17b0838
	${RM} "$@.d"
	$(COMPILE.c) -g -Wall -I../../libnsp/icom -fPIC  -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/e17b0838/base64.o ../../libnsp/com/base64.c

${OBJECTDIR}/_ext/e17b0838/crc32.o: ../../libnsp/com/crc32.c
	${MKDIR} -p ${OBJECTDIR}/_ext/e17b0838
	${RM} "$@.d"
	$(COMPILE.c) -g -Wall -I../../libnsp/icom -fPIC  -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/e17b0838/crc32.o ../../libnsp/com/crc32.c

${OBJECTDIR}/_ext/e17b0838/des.o: ../../libnsp/com/des.c
	${MKDIR} -p ${OBJECTDIR}/_ext/e17b0838
	${RM} "$@.d"
	$(COMPILE.c) -g -Wall -I../../libnsp/icom -fPIC  -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/e17b0838/des.o ../../libnsp/com/des.c

${OBJECTDIR}/_ext/e17b0838/logger.o: ../../libnsp/com/logger.c
	${MKDIR} -p ${OBJECTDIR}/_ext/e17b0838
	${RM} "$@.d"
	$(COMPILE.c) -g -Wall -I../../libnsp/icom -fPIC  -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/e17b0838/logger.o ../../libnsp/com/logger.c

${OBJECTDIR}/_ext/e17b0838/md5.o: ../../libnsp/com/md5.c
	${MKDIR} -p ${OBJECTDIR}/_ext/e17b0838
	${RM} "$@.d"
	$(COMPILE.c) -g -Wall -I../../libnsp/icom -fPIC  -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/e17b0838/md5.o ../../libnsp/com/md5.c

${OBJECTDIR}/_ext/e17b0838/object.o: ../../libnsp/com/object.c
	${MKDIR} -p ${OBJECTDIR}/_ext/e17b0838
	${RM} "$@.d"
	$(COMPILE.c) -g -Wall -I../../libnsp/icom -fPIC  -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/e17b0838/object.o ../../libnsp/com/object.c

${OBJECTDIR}/_ext/e17b0838/posix_ifos.o: ../../libnsp/com/posix_ifos.c
	${MKDIR} -p ${OBJECTDIR}/_ext/e17b0838
	${RM} "$@.d"
	$(COMPILE.c) -g -Wall -I../../libnsp/icom -fPIC  -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/e17b0838/posix_ifos.o ../../libnsp/com/posix_ifos.c

${OBJECTDIR}/_ext/e17b0838/posix_naos.o: ../../libnsp/com/posix_naos.c
	${MKDIR} -p ${OBJECTDIR}/_ext/e17b0838
	${RM} "$@.d"
	$(COMPILE.c) -g -Wall -I../../libnsp/icom -fPIC  -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/e17b0838/posix_naos.o ../../libnsp/com/posix_naos.c

${OBJECTDIR}/_ext/e17b0838/posix_string.o: ../../libnsp/com/posix_string.c
	${MKDIR} -p ${OBJECTDIR}/_ext/e17b0838
	${RM} "$@.d"
	$(COMPILE.c) -g -Wall -I../../libnsp/icom -fPIC  -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/e17b0838/posix_string.o ../../libnsp/com/posix_string.c

${OBJECTDIR}/_ext/e17b0838/posix_thread.o: ../../libnsp/com/posix_thread.c
	${MKDIR} -p ${OBJECTDIR}/_ext/e17b0838
	${RM} "$@.d"
	$(COMPILE.c) -g -Wall -I../../libnsp/icom -fPIC  -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/e17b0838/posix_thread.o ../../libnsp/com/posix_thread.c

${OBJECTDIR}/_ext/e17b0838/posix_time.o: ../../libnsp/com/posix_time.c
	${MKDIR} -p ${OBJECTDIR}/_ext/e17b0838
	${RM} "$@.d"
	$(COMPILE.c) -g -Wall -I../../libnsp/icom -fPIC  -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/e17b0838/posix_time.o ../../libnsp/com/posix_time.c

${OBJECTDIR}/_ext/e17b0838/posix_wait.o: ../../libnsp/com/posix_wait.c
	${MKDIR} -p ${OBJECTDIR}/_ext/e17b0838
	${RM} "$@.d"
	$(COMPILE.c) -g -Wall -I../../libnsp/icom -fPIC  -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/e17b0838/posix_wait.o ../../libnsp/com/posix_wait.c

${OBJECTDIR}/_ext/5c0/fque.o: ../fque.c
	${MKDIR} -p ${OBJECTDIR}/_ext/5c0
	${RM} "$@.d"
	$(COMPILE.c) -g -Wall -I../../libnsp/icom -fPIC  -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/5c0/fque.o ../fque.c

${OBJECTDIR}/_ext/5c0/io.o: ../io.c
	${MKDIR} -p ${OBJECTDIR}/_ext/5c0
	${RM} "$@.d"
	$(COMPILE.c) -g -Wall -I../../libnsp/icom -fPIC  -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/5c0/io.o ../io.c

${OBJECTDIR}/_ext/5c0/mxx.o: ../mxx.c
	${MKDIR} -p ${OBJECTDIR}/_ext/5c0
	${RM} "$@.d"
	$(COMPILE.c) -g -Wall -I../../libnsp/icom -fPIC  -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/5c0/mxx.o ../mxx.c

${OBJECTDIR}/_ext/5c0/ncb.o: ../ncb.c
	${MKDIR} -p ${OBJECTDIR}/_ext/5c0
	${RM} "$@.d"
	$(COMPILE.c) -g -Wall -I../../libnsp/icom -fPIC  -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/5c0/ncb.o ../ncb.c

${OBJECTDIR}/_ext/5c0/tcp.o: ../tcp.c
	${MKDIR} -p ${OBJECTDIR}/_ext/5c0
	${RM} "$@.d"
	$(COMPILE.c) -g -Wall -I../../libnsp/icom -fPIC  -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/5c0/tcp.o ../tcp.c

${OBJECTDIR}/_ext/5c0/tcpal.o: ../tcpal.c
	${MKDIR} -p ${OBJECTDIR}/_ext/5c0
	${RM} "$@.d"
	$(COMPILE.c) -g -Wall -I../../libnsp/icom -fPIC  -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/5c0/tcpal.o ../tcpal.c

${OBJECTDIR}/_ext/5c0/tcpio.o: ../tcpio.c
	${MKDIR} -p ${OBJECTDIR}/_ext/5c0
	${RM} "$@.d"
	$(COMPILE.c) -g -Wall -I../../libnsp/icom -fPIC  -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/5c0/tcpio.o ../tcpio.c

${OBJECTDIR}/_ext/5c0/udp.o: ../udp.c
	${MKDIR} -p ${OBJECTDIR}/_ext/5c0
	${RM} "$@.d"
	$(COMPILE.c) -g -Wall -I../../libnsp/icom -fPIC  -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/5c0/udp.o ../udp.c

${OBJECTDIR}/_ext/5c0/udpio.o: ../udpio.c
	${MKDIR} -p ${OBJECTDIR}/_ext/5c0
	${RM} "$@.d"
	$(COMPILE.c) -g -Wall -I../../libnsp/icom -fPIC  -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/5c0/udpio.o ../udpio.c

${OBJECTDIR}/_ext/5c0/worker.o: ../worker.c
	${MKDIR} -p ${OBJECTDIR}/_ext/5c0
	${RM} "$@.d"
	$(COMPILE.c) -g -Wall -I../../libnsp/icom -fPIC  -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/5c0/worker.o ../worker.c

# Subprojects
.build-subprojects:

# Clean Targets
.clean-conf: ${CLEAN_SUBPROJECTS}
	${RM} -r ${CND_BUILDDIR}/${CND_CONF}

# Subprojects
.clean-subprojects:

# Enable dependency checking
.dep.inc: .depcheck-impl

include .dep.inc
