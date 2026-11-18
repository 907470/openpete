# Sources and libraries the openpete recorders add to the regtest binary.
target_sources(duckstation-regtest PRIVATE
  openpete_regtest.cpp
  openpete_regtest.h
  psxtrace_capture.cpp
  psxtrace_capture.h
  probe_capture.cpp
  probe_capture.h
)
target_link_libraries(duckstation-regtest PRIVATE minizip)
