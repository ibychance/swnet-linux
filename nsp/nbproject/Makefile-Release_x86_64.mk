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
CND_PLATFORM=None-Linux
CND_DLIB_EXT=so
CND_CONF=Release_x86_64
CND_DISTDIR=dist
CND_BUILDDIR=build

# Include project Makefile
include Makefile

# Object Directory
OBJECTDIR=${CND_BUILDDIR}/${CND_CONF}/${CND_PLATFORM}

# Object Files
OBJECTFILES= \
	${OBJECTDIR}/_ext/5624e7e1/avltree.o \
	${OBJECTDIR}/_ext/5624e7e1/base64.o \
	${OBJECTDIR}/_ext/5624e7e1/crc32.o \
	${OBJECTDIR}/_ext/5624e7e1/des.o \
	${OBJECTDIR}/_ext/5624e7e1/logger.o \
	${OBJECTDIR}/_ext/5624e7e1/md5.o \
	${OBJECTDIR}/_ext/5624e7e1/object.o \
	${OBJECTDIR}/_ext/5624e7e1/posix_ifos.o \
	${OBJECTDIR}/_ext/5624e7e1/posix_naos.o \
	${OBJECTDIR}/_ext/5624e7e1/posix_string.o \
	${OBJECTDIR}/_ext/5624e7e1/posix_thread.o \
	${OBJECTDIR}/_ext/5624e7e1/posix_time.o \
	${OBJECTDIR}/_ext/5624e7e1/posix_wait.o


# C Compiler Flags
CFLAGS=-m64 -D_POSIX_C_SOURCE=20170505L

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
	"${MAKE}"  -f nbproject/Makefile-${CND_CONF}.mk /usr/local/lib64/libnsp.a.7.3.0

/usr/local/lib64/libnsp.a.7.3.0: ${OBJECTFILES}
	${MKDIR} -p /usr/local/lib64
	${RM} /usr/local/lib64/libnsp.a.7.3.0
	${AR} -rv /usr/local/lib64/libnsp.a.7.3.0 ${OBJECTFILES} 
	$(RANLIB) /usr/local/lib64/libnsp.a.7.3.0

${OBJECTDIR}/_ext/5624e7e1/avltree.o: ../../com/avltree.c
	${MKDIR} -p ${OBJECTDIR}/_ext/5624e7e1
	${RM} "$@.d"
	$(COMPILE.c) -O3 -Wall -I../../icom -std=c89 -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/5624e7e1/avltree.o ../../com/avltree.c

${OBJECTDIR}/_ext/5624e7e1/base64.o: ../../com/base64.c
	${MKDIR} -p ${OBJECTDIR}/_ext/5624e7e1
	${RM} "$@.d"
	$(COMPILE.c) -O3 -Wall -I../../icom -std=c89 -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/5624e7e1/base64.o ../../com/base64.c

${OBJECTDIR}/_ext/5624e7e1/crc32.o: ../../com/crc32.c
	${MKDIR} -p ${OBJECTDIR}/_ext/5624e7e1
	${RM} "$@.d"
	$(COMPILE.c) -O3 -Wall -I../../icom -std=c89 -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/5624e7e1/crc32.o ../../com/crc32.c

${OBJECTDIR}/_ext/5624e7e1/des.o: ../../com/des.c
	${MKDIR} -p ${OBJECTDIR}/_ext/5624e7e1
	${RM} "$@.d"
	$(COMPILE.c) -O3 -Wall -I../../icom -std=c89 -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/5624e7e1/des.o ../../com/des.c

${OBJECTDIR}/_ext/5624e7e1/logger.o: ../../com/logger.c
	${MKDIR} -p ${OBJECTDIR}/_ext/5624e7e1
	${RM} "$@.d"
	$(COMPILE.c) -O3 -Wall -I../../icom -std=c89 -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/5624e7e1/logger.o ../../com/logger.c

${OBJECTDIR}/_ext/5624e7e1/md5.o: ../../com/md5.c
	${MKDIR} -p ${OBJECTDIR}/_ext/5624e7e1
	${RM} "$@.d"
	$(COMPILE.c) -O3 -Wall -I../../icom -std=c89 -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/5624e7e1/md5.o ../../com/md5.c

${OBJECTDIR}/_ext/5624e7e1/object.o: ../../com/object.c
	${MKDIR} -p ${OBJECTDIR}/_ext/5624e7e1
	${RM} "$@.d"
	$(COMPILE.c) -O3 -Wall -I../../icom -std=c89 -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/5624e7e1/object.o ../../com/object.c

${OBJECTDIR}/_ext/5624e7e1/posix_ifos.o: ../../com/posix_ifos.c
	${MKDIR} -p ${OBJECTDIR}/_ext/5624e7e1
	${RM} "$@.d"
	$(COMPILE.c) -O3 -Wall -I../../icom -std=c89 -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/5624e7e1/posix_ifos.o ../../com/posix_ifos.c

${OBJECTDIR}/_ext/5624e7e1/posix_naos.o: ../../com/posix_naos.c
	${MKDIR} -p ${OBJECTDIR}/_ext/5624e7e1
	${RM} "$@.d"
	$(COMPILE.c) -O3 -Wall -I../../icom -std=c89 -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/5624e7e1/posix_naos.o ../../com/posix_naos.c

${OBJECTDIR}/_ext/5624e7e1/posix_string.o: ../../com/posix_string.c
	${MKDIR} -p ${OBJECTDIR}/_ext/5624e7e1
	${RM} "$@.d"
	$(COMPILE.c) -O3 -Wall -I../../icom -std=c89 -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/5624e7e1/posix_string.o ../../com/posix_string.c

${OBJECTDIR}/_ext/5624e7e1/posix_thread.o: ../../com/posix_thread.c
	${MKDIR} -p ${OBJECTDIR}/_ext/5624e7e1
	${RM} "$@.d"
	$(COMPILE.c) -O3 -Wall -I../../icom -std=c89 -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/5624e7e1/posix_thread.o ../../com/posix_thread.c

${OBJECTDIR}/_ext/5624e7e1/posix_time.o: ../../com/posix_time.c
	${MKDIR} -p ${OBJECTDIR}/_ext/5624e7e1
	${RM} "$@.d"
	$(COMPILE.c) -O3 -Wall -I../../icom -std=c89 -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/5624e7e1/posix_time.o ../../com/posix_time.c

${OBJECTDIR}/_ext/5624e7e1/posix_wait.o: ../../com/posix_wait.c
	${MKDIR} -p ${OBJECTDIR}/_ext/5624e7e1
	${RM} "$@.d"
	$(COMPILE.c) -O3 -Wall -I../../icom -std=c89 -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/_ext/5624e7e1/posix_wait.o ../../com/posix_wait.c

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
