#START-FILE-LIST
# File List auto generated by waf2cmake.py, do not modify manually.

# Converts the exceptions (which are disabled in cryengine) to assertion.
add_definitions(-DSPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS)

start_sources()
# -- spirv-cross.waf_files --
sources_platform(ALL)
add_sources("NoUberFile"
	SOURCE_GROUP "Root"
		"../../SDKs/spirv-cross/GLSL.std.450.h"
		"../../SDKs/spirv-cross/spirv_common.hpp"
		"../../SDKs/spirv-cross/spirv.hpp"
		"../../SDKs/spirv-cross/spirv_cross.hpp"
		"../../SDKs/spirv-cross/spirv_cross.cpp"
		"../../SDKs/spirv-cross/spirv_cfg.hpp"
		"../../SDKs/spirv-cross/spirv_cfg.cpp"
)

end_sources()
#END-FILE-LIST

CryEngineStaticModule(spirv-cross SOLUTION_FOLDER "Libs")

target_include_directories(${THIS_PROJECT} PUBLIC "${SDK_DIR}/spirv-cross")

if (WIN32 OR WIN64)
	target_compile_definitions(${THIS_PROJECT} PRIVATE -D_CRT_SECURE_NO_WARNINGS)
	target_compile_options(${THIS_PROJECT} PRIVATE
		/wd4530 # C++ exception handler used, but unwind semantics are not enabled. Specify /EHsc
		/wd4267 # 'initializing': conversion from 'size_t' to 'uint32_t', possible loss of data
		/wd4244 # '=': conversion from '__int64' to 'uint32_t', possible loss of data
		/WX-    # disabling warnings treated as errors
	)
elseif(LINUX OR ANDROID)
	target_compile_options(${THIS_PROJECT} PRIVATE
		-Wno-return-type
		-Werror
	)
endif()
