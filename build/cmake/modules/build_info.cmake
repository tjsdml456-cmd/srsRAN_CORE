
execute_process(
COMMAND git rev-parse --abbrev-ref HEAD
WORKING_DIRECTORY "/home/user/srsRAN_main/srsRAN_Project"
OUTPUT_VARIABLE GIT_BRANCH
OUTPUT_STRIP_TRAILING_WHITESPACE
)

execute_process(
COMMAND git log -1 --format=%h
WORKING_DIRECTORY "/home/user/srsRAN_main/srsRAN_Project"
OUTPUT_VARIABLE GIT_COMMIT_HASH
OUTPUT_STRIP_TRAILING_WHITESPACE
)

message(STATUS "Generating build information")
configure_file(
  /home/user/srsRAN_main/srsRAN_Project/lib/support/versioning/hashes.h.in
  /home/user/srsRAN_main/srsRAN_Project/build/hashes.h
)
