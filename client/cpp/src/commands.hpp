// FlatBuffer command construction for the rana-socket client CLI. Each union
// variant has exactly one builder; validation of per-command arguments and the
// finished Command envelope happen here. No socket or CLI-frontend logic.

#pragma once

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <string>

// Validates per-command args and builds the finished Command flatbuffer into
// `fbb`. On bad/unknown command, returns false and (for usage errors) writes a
// diagnostic to stderr. `argv`/`argc` use the same indexing as main(): argv[3+]
// are the command's own arguments.
bool build_command(const std::string& cmd, int argc, char** argv,
                   flatbuffers::FlatBufferBuilder& fbb, uint64_t request_id,
                   std::string& err);
