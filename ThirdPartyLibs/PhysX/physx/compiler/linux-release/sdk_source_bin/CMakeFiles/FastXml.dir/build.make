# CMAKE generated file: DO NOT EDIT!
# Generated by "Unix Makefiles" Generator, CMake Version 3.16

# Delete rule output on recipe failure.
.DELETE_ON_ERROR:


#=============================================================================
# Special targets provided by cmake.

# Disable implicit rules so canonical targets will work.
.SUFFIXES:


# Remove some rules from gmake that .SUFFIXES does not remove.
SUFFIXES =

.SUFFIXES: .hpux_make_needs_suffix_list


# Suppress display of executed commands.
$(VERBOSE).SILENT:


# A target that is always out of date.
cmake_force:

.PHONY : cmake_force

#=============================================================================
# Set environment variables for the build.

# The shell in which to execute make rules.
SHELL = /bin/sh

# The CMake executable.
CMAKE_COMMAND = /usr/bin/cmake

# The command to remove a file.
RM = /usr/bin/cmake -E remove -f

# Escaping for special characters.
EQUALS = =

# The top-level source directory on which CMake was run.
CMAKE_SOURCE_DIR = /home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/compiler/public

# The top-level build directory on which CMake was run.
CMAKE_BINARY_DIR = /home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/compiler/linux-release

# Include any dependencies generated for this target.
include sdk_source_bin/CMakeFiles/FastXml.dir/depend.make

# Include the progress variables for this target.
include sdk_source_bin/CMakeFiles/FastXml.dir/progress.make

# Include the compile flags for this target's objects.
include sdk_source_bin/CMakeFiles/FastXml.dir/flags.make

sdk_source_bin/CMakeFiles/FastXml.dir/home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/source/fastxml/src/PsFastXml.cpp.o: sdk_source_bin/CMakeFiles/FastXml.dir/flags.make
sdk_source_bin/CMakeFiles/FastXml.dir/home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/source/fastxml/src/PsFastXml.cpp.o: /home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/source/fastxml/src/PsFastXml.cpp
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/compiler/linux-release/CMakeFiles --progress-num=$(CMAKE_PROGRESS_1) "Building CXX object sdk_source_bin/CMakeFiles/FastXml.dir/home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/source/fastxml/src/PsFastXml.cpp.o"
	cd /home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/compiler/linux-release/sdk_source_bin && /usr/bin/clang++  $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -o CMakeFiles/FastXml.dir/home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/source/fastxml/src/PsFastXml.cpp.o -c /home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/source/fastxml/src/PsFastXml.cpp

sdk_source_bin/CMakeFiles/FastXml.dir/home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/source/fastxml/src/PsFastXml.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/FastXml.dir/home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/source/fastxml/src/PsFastXml.cpp.i"
	cd /home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/compiler/linux-release/sdk_source_bin && /usr/bin/clang++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/source/fastxml/src/PsFastXml.cpp > CMakeFiles/FastXml.dir/home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/source/fastxml/src/PsFastXml.cpp.i

sdk_source_bin/CMakeFiles/FastXml.dir/home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/source/fastxml/src/PsFastXml.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/FastXml.dir/home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/source/fastxml/src/PsFastXml.cpp.s"
	cd /home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/compiler/linux-release/sdk_source_bin && /usr/bin/clang++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/source/fastxml/src/PsFastXml.cpp -o CMakeFiles/FastXml.dir/home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/source/fastxml/src/PsFastXml.cpp.s

FastXml: sdk_source_bin/CMakeFiles/FastXml.dir/home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/source/fastxml/src/PsFastXml.cpp.o
FastXml: sdk_source_bin/CMakeFiles/FastXml.dir/build.make

.PHONY : FastXml

# Rule to build all files generated by this target.
sdk_source_bin/CMakeFiles/FastXml.dir/build: FastXml

.PHONY : sdk_source_bin/CMakeFiles/FastXml.dir/build

sdk_source_bin/CMakeFiles/FastXml.dir/clean:
	cd /home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/compiler/linux-release/sdk_source_bin && $(CMAKE_COMMAND) -P CMakeFiles/FastXml.dir/cmake_clean.cmake
.PHONY : sdk_source_bin/CMakeFiles/FastXml.dir/clean

sdk_source_bin/CMakeFiles/FastXml.dir/depend:
	cd /home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/compiler/linux-release && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" /home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/compiler/public /home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/source/compiler/cmake /home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/compiler/linux-release /home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/compiler/linux-release/sdk_source_bin /home/greg/Esenthel/ThirdPartyLibs/PhysX/physx/compiler/linux-release/sdk_source_bin/CMakeFiles/FastXml.dir/DependInfo.cmake --color=$(COLOR)
.PHONY : sdk_source_bin/CMakeFiles/FastXml.dir/depend

