# =============================================================================
#  VelocityProtoGen
#
#  Generates C++ + gRPC sources from a set of .proto files and bundles them
#  into a single static library target.
#
#  Usage:
#      velocity_add_proto_library(
#          NAME       velocity_proto
#          PROTO_ROOT ${CMAKE_SOURCE_DIR}/proto
#          PROTOS
#              ${CMAKE_SOURCE_DIR}/proto/common.proto
#              …
#      )
#
#  The resulting target exposes its generated header directory transitively,
#  so consumers `target_link_libraries(my_service PRIVATE velocity_proto)`
#  and `#include "telemetry.pb.h"` Just Works.
# =============================================================================

function(velocity_add_proto_library)
    set(_options)
    set(_oneValue NAME PROTO_ROOT)
    set(_multiValue PROTOS)
    cmake_parse_arguments(PG "${_options}" "${_oneValue}" "${_multiValue}" ${ARGN})

    if(NOT PG_NAME)
        message(FATAL_ERROR "velocity_add_proto_library: NAME is required")
    endif()
    if(NOT PG_PROTOS)
        message(FATAL_ERROR "velocity_add_proto_library: PROTOS is required")
    endif()
    if(NOT PG_PROTO_ROOT)
        set(PG_PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
    endif()

    # Resolve the protoc and grpc_cpp_plugin executables via the targets that
    # Conan exposes.
    if(NOT TARGET protobuf::protoc)
        message(FATAL_ERROR "protobuf::protoc target missing — ensure Protobuf is found before calling this.")
    endif()
    if(NOT TARGET gRPC::grpc_cpp_plugin)
        message(FATAL_ERROR "gRPC::grpc_cpp_plugin target missing — ensure gRPC is found.")
    endif()

    set(_out_dir "${CMAKE_BINARY_DIR}/proto-gen/cpp")
    file(MAKE_DIRECTORY ${_out_dir})

    set(_generated_srcs)
    set(_generated_hdrs)

    foreach(_proto IN LISTS PG_PROTOS)
        get_filename_component(_basename ${_proto} NAME_WE)
        set(_pb_h    ${_out_dir}/${_basename}.pb.h)
        set(_pb_cc   ${_out_dir}/${_basename}.pb.cc)
        set(_grpc_h  ${_out_dir}/${_basename}.grpc.pb.h)
        set(_grpc_cc ${_out_dir}/${_basename}.grpc.pb.cc)

        add_custom_command(
            OUTPUT  ${_pb_h} ${_pb_cc} ${_grpc_h} ${_grpc_cc}
            COMMAND $<TARGET_FILE:protobuf::protoc>
                ARGS
                    --proto_path=${PG_PROTO_ROOT}
                    --cpp_out=${_out_dir}
                    --grpc_out=${_out_dir}
                    --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
                    ${_proto}
            DEPENDS ${_proto} protobuf::protoc gRPC::grpc_cpp_plugin
            COMMENT "protoc → ${_basename}.pb.{h,cc} + grpc.pb.{h,cc}"
            VERBATIM
        )

        list(APPEND _generated_srcs ${_pb_cc} ${_grpc_cc})
        list(APPEND _generated_hdrs ${_pb_h}  ${_grpc_h})
    endforeach()

    add_library(${PG_NAME} STATIC ${_generated_srcs} ${_generated_hdrs})
    add_library(velocity::proto ALIAS ${PG_NAME})

    target_include_directories(${PG_NAME}
        PUBLIC
            $<BUILD_INTERFACE:${_out_dir}>
    )
    target_link_libraries(${PG_NAME}
        PUBLIC
            protobuf::libprotobuf
            gRPC::grpc++
            gRPC::grpc++_reflection
    )

    # Generated code intentionally fails some of our strict warnings.
    target_compile_options(${PG_NAME} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wno-shadow -Wno-conversion -Wno-sign-conversion -Wno-deprecated -Wno-unused-parameter -Wno-old-style-cast>
    )
endfunction()
