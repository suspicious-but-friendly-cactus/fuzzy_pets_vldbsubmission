cmake_minimum_required(VERSION 3.10)

project(oprf C CXX)

# Targets oprf_server_(client|server)
foreach(_target
  oprf_server
  psi_server
  )
  add_executable(${_target}
    "${_target}.cpp")
  target_link_libraries(${_target}
    droidcrypto
    lowmc)
endforeach()
